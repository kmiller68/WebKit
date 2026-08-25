/*
 * Copyright (C) 2017-2026 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#include <wtf/Atomics.h>
#include <wtf/Lock.h>
#include <wtf/Nonmovable.h>
#include <wtf/ThreadSafetyAnalysis.h>

namespace WTF {

class ReadLockView;
class WriteLockView;

// A phase-fair, eventually writer-fair read-write lock, based on the algorithm from Spin-Based Reader-Writer Synchronization
// for Multiprocessor Real-Time Systems by Bjorn B. Brandenburg and James H. Anderson. See: https://www.cs.unc.edu/~anderson/papers/rtsj10-for-web.pdf,
// built on ParkingLot and WTF::Lock.
//
// It's easiest to read lock like this:
//     Locker locker { rwLock.read() };
//
// It's easiest to write lock like this:
//     Locker locker { rwLock.write() };
//
// Readers join unconditionally. m_rin counts readers that have entered and m_rout counts readers that
// have left, both monotonically, and a reader joins with a single fetch-and-add that cannot fail. That
// is the point of the two counters: with one reader count that must not rise while a writer wants the
// lock, admission has to be conditional and therefore a compare-and-swap, which fails whenever
// another reader is joining at the same time. Measured against such a design, this one is several
// times faster with eight or more readers, and the gap grows with reader count.
//
// A writer takes m_writerLock, which is what serialises writers against each other, then announces
// itself in m_rin's phase field, which stops readers arriving from that point on, and then waits for
// the readers already counted in m_rin to reach m_rout. Its phase is over when it clears that field.
//
// Fairness is phase-fair in both directions, and neither side can starve:
//  - A reader waits only for the one writer whose phase it collided with. Writers queued behind that
//    one are waiting on m_writerLock and are invisible to readers, so reader cost does not grow with
//    the number of waiting writers.
//  - A writer waits only for the readers that were already inside when it announced itself. Readers
//    arriving afterwards queue behind it.
//  - Writers are fair among themselves however WTF::Lock is, which is barging with ParkingLot's
//    eventual-fairness handoff.
//
// A read lock is not recursive: taking a second read lock on a thread that already holds one
// deadlocks if a writer announces itself in between.
class WTF_CAPABILITY_LOCK ReadWriteLock {
    WTF_MAKE_NONMOVABLE(ReadWriteLock);
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(ReadWriteLock);
public:
    constexpr ReadWriteLock() = default;

    void readLock() WTF_ACQUIRES_SHARED_LOCK()
    {
        // One unconditional read-modify-write, which cannot fail however many readers are joining at
        // once. The value it returns tells us whether a writer owns the current phase, and which
        // phase that is, atomically with our joining - both have to come from the same operation, or
        // a writer could claim a phase in between and we would wait on the wrong one.
        uint32_t previous = m_rin.exchangeAdd(ReaderUnit, std::memory_order_acquire);
        if (previous & WriterPresentBit) [[unlikely]]
            readLockSlow(previous & PhaseFieldMask);
    }

    bool tryReadLock() WTF_ACQUIRES_SHARED_LOCK_IF(true)
    {
        // Unlike readLock(), this must not join and then leave again when a writer turns out to own
        // the phase. A departure is indistinguishable from that of a reader the current writer is
        // draining, so backing out either satisfies that writer's count early, letting it in while a
        // reader is still inside, or pushes the count past the value it is waiting for and strands it
        // for good. So commit only when we can, which costs a compare-and-swap - acceptable on a path
        // that is not the common one, and it keeps readLock()'s fetch-and-add unconditional.
        uint32_t rin = m_rin.load(std::memory_order_relaxed);
        if (rin & WriterPresentBit)
            return false;
        return m_rin.compareExchangeWeak(rin, rin + ReaderUnit, std::memory_order_acquire);
    }

    void readUnlock() WTF_RELEASES_SHARED_LOCK()
    {
        // Also unconditional, and the value it returns is by definition the state immediately before
        // our own departure, which makes "we are the one the writer was waiting for" exact.
        uint32_t previous = m_rout.exchangeAdd(ReaderUnit, std::memory_order_release);
        if (previous & WriterDrainParkedBit) [[unlikely]] {
            // The parked bit lives in the word we just incremented, so an uncontended release costs
            // nothing beyond that single operation. Wake the writer only once the count it is waiting
            // for is actually reached, rather than on every departure.
            if (((previous + ReaderUnit) & ReaderCountMask) == m_drainTarget.load(std::memory_order_acquire))
                readUnlockSlow();
        }
    }

    // These three hold m_writerLock across the call boundary, which clang's thread-safety analysis
    // cannot express, so they opt out of it.
    void writeLock() WTF_ACQUIRES_LOCK() WTF_IGNORES_THREAD_SAFETY_ANALYSIS
    {
        m_writerLock.lock();
        uint32_t target = beginPhase();
        if ((m_rout.load(std::memory_order_acquire) & ReaderCountMask) != target) [[unlikely]]
            writeLockSlow(target);
    }

    bool tryWriteLock() WTF_ACQUIRES_LOCK_IF(true) WTF_IGNORES_THREAD_SAFETY_ANALYSIS
    {
        if (!m_writerLock.tryLock())
            return false;
        uint32_t target = beginPhase();
        if ((m_rout.load(std::memory_order_acquire) & ReaderCountMask) == target) [[likely]]
            return true;
        // Readers were already inside and we are not willing to wait for them, so give the phase back.
        // Readers that queued behind us in the meantime have to be released.
        endPhase();
        m_writerLock.unlock();
        return false;
    }

    void writeUnlock() WTF_RELEASES_LOCK() WTF_IGNORES_THREAD_SAFETY_ANALYSIS
    {
        endPhase();
        m_writerLock.unlock();
    }

    // WTF_RETURNS_LOCK declares that these denote the same capability as the lock itself, so that
    // data annotated WTF_GUARDED_BY_LOCK(theLock) is recognised as held inside
    // Locker { theLock.read() } as well as by theLock.readLock().
    ReadLockView& read() WTF_RETURNS_LOCK(*this);
    WriteLockView& write() WTF_RETURNS_LOCK(*this);

    // True once every thread has released the lock. Note that the reader counters are monotonic, so
    // being quiescent means they agree rather than that they are zero. Tests assert this to catch
    // state that leaks across acquisitions.
    bool isQuiescentForTesting() const
    {
        uint32_t rin = m_rin.load(std::memory_order_relaxed);
        uint32_t rout = m_rout.load(std::memory_order_relaxed);
        return !(rin & PhaseFieldMask)
            && (rin & ReaderCountMask) == (rout & ReaderCountMask)
            && !m_writerLock.isHeld();
    }

private:
    // m_rin: bits 31-8 readers entered, 7-4 phase id, 1 has parked readers, 0 writer present.
    // m_rout: bits 31-8 readers left, 0 has a parked writer draining.
    //
    // The reader counts are sequence numbers rather than gauges, so they wrap, and that is harmless:
    // a writer waits for equality between two counters that advance by the same unit on the same
    // modulus, so wrapping cancels. What it does require is that fewer than 2^24 readers are inside
    // at once, which also bounds how many can be inside for the equality to be unambiguous.
    static constexpr uint32_t WriterPresentBit = 1u << 0;
    static constexpr uint32_t HasParkedReadersBit = 1u << 1;
    static constexpr uint32_t PhaseIdShift = 4;
    static constexpr uint32_t PhaseIdMask = 0xF0;
    static constexpr uint32_t PhaseIdCount = 0xF;
    // The field a writer owns for the length of its phase, and which readers watch. Deliberately
    // excludes HasParkedReadersBit, so setting that does not look like a phase change.
    static constexpr uint32_t PhaseFieldMask = WriterPresentBit | PhaseIdMask;
    static constexpr uint32_t ReaderCountShift = 8;
    static constexpr uint32_t ReaderUnit = 1u << ReaderCountShift;
    static constexpr uint32_t ReaderCountMask = ~static_cast<uint32_t>(0xFF);
    static constexpr uint32_t WriterDrainParkedBit = 1u << 0;

    // Claim the phase and return the m_rout count to wait for. Holding m_writerLock is what makes the phase id safe to keep in a plain member and the addition safe: no other
    // writer can be between these two points, so the phase field is zero and cannot carry.
    // Must be called holding m_writerLock.
    uint32_t beginPhase()
    {
        // The id has to differ from the previous phase's, or a reader still waiting on that phase
        // would not notice the change. Four bits gives sixteen phases before a value repeats, which
        // has to outlast any reader that is descheduled mid-wait.
        m_phaseId = (m_phaseId + 1) & PhaseIdCount;
        uint32_t phase = WriterPresentBit | (m_phaseId << PhaseIdShift);
        ASSERT(!(m_rin.load(std::memory_order_relaxed) & PhaseFieldMask));
        return m_rin.exchangeAdd(phase, std::memory_order_acquire) & ReaderCountMask;
    }

    // End the phase, releasing any readers that queued behind us.
    // Must be called holding m_writerLock.
    void endPhase()
    {
        uint32_t rin = m_rin.exchangeAnd(~PhaseFieldMask, std::memory_order_release);
        ASSERT(rin & WriterPresentBit);
        if (rin & HasParkedReadersBit) [[unlikely]]
            writeUnlockSlow();
    }

    WTF_EXPORT_PRIVATE NEVER_INLINE void readLockSlow(uint32_t observedPhase);
    WTF_EXPORT_PRIVATE NEVER_INLINE void readUnlockSlow();
    WTF_EXPORT_PRIVATE NEVER_INLINE void writeLockSlow(uint32_t target);
    WTF_EXPORT_PRIVATE NEVER_INLINE void writeUnlockSlow();

    // Separate parking addresses for the two waits, since ParkingLot keys queues by address.
WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN
    void* readerParkingAddress() { return std::bit_cast<void*>(std::bit_cast<uint8_t*>(this)); }
    void* drainParkingAddress() { return std::bit_cast<void*>(std::bit_cast<uint8_t*>(this) + 1); }
WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

    // These three words are deliberately packed together rather than padded onto separate cache
    // lines. Giving m_rin and m_rout a line each measured about 50% more reader throughput and half
    // the write acquire latency under heavy contention, because a reader's join and its departure
    // then stop contending for one line. It is not done here because it would take the lock from 12
    // bytes to 128 or more, which is the wrong trade for something that may be embedded per object;
    // a caller that knows it has one heavily contended instance could reintroduce it.
    Atomic<uint32_t> m_rin { 0 };
    Atomic<uint32_t> m_rout { 0 };
    // The m_rout count the draining writer is waiting for. Only meaningful while WriterDrainParkedBit
    // is set in m_rout, and only one writer can be draining, since draining happens under
    // m_writerLock.
    Atomic<uint32_t> m_drainTarget { 0 };
    // Writer exclusion, kept off the words the readers use. Barging with eventual fairness is all
    // this needs, which is exactly what WTF::Lock is.
    Lock m_writerLock;
    // Guarded by m_writerLock, so it needs no atomicity of its own.
    uint32_t m_phaseId { 0 };
};

// Views of a ReadWriteLock that select a mode. Each is the same object as the lock it came from,
// viewed so that lock() and unlock() mean one mode's pair of operations, which is what lets Locker be
// used with either. Locker.h forward declares both and specialises Locker for them, the read side
// separately because acquiring shared cannot be expressed by a Locker that always acquires
// exclusively.
//
// The methods themselves opt out of thread-safety analysis: they declare their effect for callers,
// but the body upcasts to a different capability than the one declared, which the analysis reads as a
// mismatch.
class WTF_CAPABILITY_LOCK ReadLockView : public ReadWriteLock {
public:
    bool tryLock() WTF_ACQUIRES_SHARED_LOCK_IF(true) WTF_IGNORES_THREAD_SAFETY_ANALYSIS { return tryReadLock(); }
    void lock() WTF_ACQUIRES_SHARED_LOCK() WTF_IGNORES_THREAD_SAFETY_ANALYSIS { readLock(); }
    void unlock() WTF_RELEASES_SHARED_LOCK() WTF_IGNORES_THREAD_SAFETY_ANALYSIS { readUnlock(); }
};

class WTF_CAPABILITY_LOCK WriteLockView : public ReadWriteLock {
public:
    bool tryLock() WTF_ACQUIRES_LOCK_IF(true) WTF_IGNORES_THREAD_SAFETY_ANALYSIS { return tryWriteLock(); }
    void lock() WTF_ACQUIRES_LOCK() WTF_IGNORES_THREAD_SAFETY_ANALYSIS { writeLock(); }
    void unlock() WTF_RELEASES_LOCK() WTF_IGNORES_THREAD_SAFETY_ANALYSIS { writeUnlock(); }
};

inline ReadLockView& ReadWriteLock::read() WTF_IGNORES_THREAD_SAFETY_ANALYSIS { return *static_cast<ReadLockView*>(this); }
inline WriteLockView& ReadWriteLock::write() WTF_IGNORES_THREAD_SAFETY_ANALYSIS { return *static_cast<WriteLockView*>(this); }

} // namespace WTF

using WTF::ReadWriteLock;
