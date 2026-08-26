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
// for Multiprocessor Real-Time Systems by Bjorn B. Brandenburg and James H. Anderson.
// See: https://www.cs.unc.edu/~anderson/papers/rtsj10-for-web.pdf,
//
// It's easiest to read lock like this:
//     Locker locker { rwLock.read() };
//
// It's easiest to write lock like this:
//     Locker locker { rwLock.write() };
//
// Readers join unconditionally. m_readersIn counts readers that have entered and m_readersOut counts readers that
// have left, both monotonically, and a reader joins with a single fetch-and-add that cannot fail. That
// is the point of the two counters: with one reader count that must not rise while a writer wants the
// lock, admission has to be conditional and therefore a compare-and-swap, which fails whenever
// another reader is joining at the same time. Measured against such a design, this one is several
// times faster with eight or more readers, and the gap grows with reader count.
//
// A writer takes m_writerLock, which is what serialises writers against each other, then announces
// itself in m_readersIn's phase field, which stops readers arriving from that point on, and then waits for
// the readers already counted in m_readersIn to reach m_readersOut. Its phase is over when it clears that field.
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
        uint32_t previous = m_readersIn.exchangeAdd(s_readerIncrement, std::memory_order_acquire);
        if (previous & s_writerPresentBit) [[unlikely]]
            readLockSlow(previous & s_phaseFieldMask);
    }

    bool tryReadLock() WTF_ACQUIRES_SHARED_LOCK_IF(true)
    {
        // Unlike readLock(), this must not join and then leave again when a writer turns out to own
        // the phase. A departure is indistinguishable from that of a reader the current writer is
        // draining, so backing out either satisfies that writer's count early, letting it in while a
        // reader is still inside, or pushes the count past the value it is waiting for and strands it
        // for good. So commit only when we can, which costs a compare-and-swap - acceptable on a path
        // that is not the common one, and it keeps readLock()'s fetch-and-add unconditional.
        uint32_t readersIn = m_readersIn.load(std::memory_order_relaxed);
        if (readersIn & s_writerPresentBit)
            return false;
        return m_readersIn.compareExchangeWeak(readersIn, readersIn + s_readerIncrement, std::memory_order_acquire);
    }

    void readUnlock() WTF_RELEASES_SHARED_LOCK()
    {
        // Also unconditional, and the value it returns is by definition the state immediately before
        // our own departure, which makes "we are the one the writer was waiting for" exact.
        uint32_t previous = m_readersOut.exchangeAdd(s_readerIncrement, std::memory_order_release);
        if (previous & s_writerDrainParkedBit) [[unlikely]] {
            // The parked bit lives in the word we just incremented, so an uncontended release costs
            // nothing beyond that single operation. Wake the writer only once the count it is waiting
            // for is actually reached, rather than on every departure.
            if (((previous + s_readerIncrement) & s_readerCountMask) == m_drainTarget.load(std::memory_order_acquire))
                readUnlockSlow();
        }
    }

    // These three hold m_writerLock across the call boundary, which clang's thread-safety analysis
    // cannot express, so they opt out of it.
    void writeLock() WTF_ACQUIRES_LOCK() WTF_IGNORES_THREAD_SAFETY_ANALYSIS
    {
        m_writerLock.lock();
        uint32_t target = beginPhase();
        if ((m_readersOut.load(std::memory_order_acquire) & s_readerCountMask) != target) [[unlikely]]
            writeLockSlow(target);
    }

    bool tryWriteLock() WTF_ACQUIRES_LOCK_IF(true) WTF_IGNORES_THREAD_SAFETY_ANALYSIS
    {
        if (!m_writerLock.tryLock())
            return false;
        uint32_t target = beginPhase();
        if ((m_readersOut.load(std::memory_order_acquire) & s_readerCountMask) == target) [[likely]]
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
        uint32_t readersIn = m_readersIn.load(std::memory_order_relaxed);
        uint32_t readersOut = m_readersOut.load(std::memory_order_relaxed);
        return !(readersIn & s_phaseFieldMask)
            && (readersIn & s_readerCountMask) == (readersOut & s_readerCountMask)
            && !m_writerLock.isHeld();
    }

private:
    // Upgrading is reached only through Locker { lock.read() }.tryUpgrade(lock), which is what tells
    // that locker its read lock is gone. Exposing this directly would let a caller upgrade out from
    // under a read Locker whose destructor then releases a read lock the thread no longer holds.
    friend class Locker<ReadLockView>;

    // Turn a read lock this thread already holds into a write lock without letting another writer in
    // between, so that whatever the caller read under the read lock is still true. On success the
    // caller holds the write lock. On failure it holds nothing at all: the read lock is gone too, so a
    // caller that still needs the data has to acquire again and re-examine it.
    //
    // Giving up the read lock on failure is also what makes both outcomes expressible to clang's
    // thread-safety analysis, which can say "acquires exclusive if this returned true" but has no way
    // to say "releases shared only if it returned false". It fits what this is for either way: read
    // cheaply, discover a write is needed, and upgrade if that is free rather than holding readers off
    // while queueing for the write lock.
    //
    // There is deliberately no blocking upgrade(). Waiting for m_writerLock would deadlock against a
    // writer that owns the phase, because that writer is waiting for this thread's read lock to be
    // released. For the same reason two threads upgrading at once can both keep failing, so a caller
    // that retries needs its own way out.
    bool tryUpgrade() WTF_RELEASES_SHARED_LOCK() WTF_ACQUIRES_LOCK_IF(true) WTF_IGNORES_THREAD_SAFETY_ANALYSIS
    {
        // This must not wait, because a writer holding m_writerLock may be draining, and what it is
        // waiting for is us. Taking it also establishes that no writer owns the phase.
        if (!m_writerLock.tryLock()) {
            // Release through the normal path, which is the one that can wake exactly that writer.
            readUnlock();
            return false;
        }
        uint32_t target = beginPhase();
        // Our own read lock is counted in m_readersIn and no readUnlock is coming for it, so count it
        // out here or the drain below would be waiting for us. Being counted out is also what makes
        // the read lock gone either way, and it is why the uncontended upgrade needs no waiting at
        // all: if we were the only reader, m_readersOut has already reached the target.
        m_readersOut.exchangeAdd(s_readerIncrement, std::memory_order_relaxed);
        if ((m_readersOut.load(std::memory_order_acquire) & s_readerCountMask) == target) [[likely]]
            return true;
        // Other readers were already inside and we are not willing to wait for them. Give the phase
        // back, releasing any readers that queued behind us.
        endPhase();
        m_writerLock.unlock();
        return false;
    }

    // m_readersIn: bits 31-8 readers entered, 7-4 phase id, 1 has parked readers, 0 writer present.
    // m_readersOut: bits 31-8 readers left, 0 has a parked writer draining.
    //
    // The reader counts are sequence numbers rather than gauges, so they wrap, and that is harmless:
    // a writer waits for equality between two counters that advance by the same unit on the same
    // modulus, so wrapping cancels. What it does require is that fewer than 2^24 readers are inside
    // at once, which also bounds how many can be inside for the equality to be unambiguous.
    //
    // FIXME: These could be uint16_t, which would halve the lock to 8 bytes. The two fields fail in
    // very different ways, so the bits are not interchangeable:
    //
    //  - Too few count bits is silent. If the number of readers in flight is ever an exact multiple
    //    of the field's modulus when a writer claims a phase, m_readersOut already equals the target and
    //    that writer proceeds with readers still inside. Nothing can detect it afterwards, because
    //    "none in flight" and "a full modulus in flight" are the same counter state, so it cannot be
    //    asserted on the writer side; only readLock() could bound it, and only by loading m_readersOut on
    //    its fast path. Note the bound is threads rather than cores: a reader stays counted while
    //    descheduled, and readers waiting in readLockSlow() are counted too.
    //  - Too few phase id bits is benign and self-correcting. The id is not part of exclusion at all;
    //    a reader returns when the phase field differs from what it saw, and a narrower id can only
    //    make that later, never earlier. It costs a reader that failed to look for an exact multiple
    //    of 2^bits phases one more phase of waiting, with probability around 1/2^bits per preemption.
    //
    // So bits should move from the id to the count, not the other way. A 3-bit id leaves 11 for the
    // count, or 2 bits leaves 12.
    static constexpr uint32_t s_writerPresentBit = 1u << 0;
    static constexpr uint32_t s_hasParkedReadersBit = 1u << 1;
    static constexpr uint32_t s_phaseIdShift = 4;
    static constexpr uint32_t s_phaseIdMask = 0xF0;
    static constexpr uint32_t s_phaseIdCount = 0xF;
    // The field a writer owns for the length of its phase, and which readers watch. Deliberately
    // excludes s_hasParkedReadersBit, so setting that does not look like a phase change.
    static constexpr uint32_t s_phaseFieldMask = s_writerPresentBit | s_phaseIdMask;
    static constexpr uint32_t s_readerCountShift = 8;
    static constexpr uint32_t s_readerIncrement = 1u << s_readerCountShift;
    static constexpr uint32_t s_readerCountMask = ~static_cast<uint32_t>(0xFF);
    static constexpr uint32_t s_writerDrainParkedBit = 1u << 0;

    // Claim the phase and return the m_readersOut count to wait for no
    // other writer can be between these two points, so the phase field is zero and cannot carry.
    uint32_t beginPhase() WTF_REQUIRES_LOCK(m_writerLock)
    {
        // The id has to differ from the previous phase's, or a reader still waiting on that phase
        // would not notice the change. Sixteen phases before a value repeats makes it very unlikely
        // that a reader descheduled mid-wait looks again after an exact multiple of them, which is
        // the case that costs it another phase of waiting.
        m_phaseId = static_cast<uint8_t>((m_phaseId + 1) & s_phaseIdCount);
        uint32_t phase = s_writerPresentBit | (static_cast<uint32_t>(m_phaseId) << s_phaseIdShift);
        ASSERT(!(m_readersIn.load(std::memory_order_relaxed) & s_phaseFieldMask));
        return m_readersIn.exchangeAdd(phase, std::memory_order_acquire) & s_readerCountMask;
    }

    // End the phase, releasing any readers that queued behind us.
    // Must be called holding m_writerLock.
    void endPhase() WTF_REQUIRES_LOCK(m_writerLock)
    {
        uint32_t readersIn = m_readersIn.exchangeAnd(~s_phaseFieldMask, std::memory_order_release);
        ASSERT(readersIn & s_writerPresentBit);
        if (readersIn & s_hasParkedReadersBit) [[unlikely]]
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
    // lines. Giving m_readersIn and m_readersOut a line each measured about 50% more reader throughput and half
    // the write acquire latency under heavy contention, because a reader's join and its departure
    // then stop contending for one line. It is not done here because it would take the lock from 16
    // bytes to 128 or more, which is the wrong trade for something that may be embedded per object;
    // a caller that knows it has one heavily contended instance could reintroduce it.
    Atomic<uint32_t> m_readersIn { 0 };
    Atomic<uint32_t> m_readersOut { 0 };
    // The m_readersOut count the draining writer is waiting for. Only meaningful while s_writerDrainParkedBit
    // is set in m_readersOut, and only one writer can be draining, since draining happens under
    // m_writerLock.
    Atomic<uint32_t> m_drainTarget { 0 };
    // Writer exclusion, kept off the words the readers use. Barging with eventual fairness is all
    // this needs, which is exactly what WTF::Lock is.
    Lock m_writerLock;
    // Guarded by m_writerLock, so it needs no atomicity of its own. Only the low four bits are ever
    // set, and a byte here shares the padding after m_writerLock rather than adding a word.
    uint8_t m_phaseId { 0 };
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

// Defined here rather than in Locker.h because it needs ReadLockView complete and the lock's own
// tryUpgrade(), which is private to everything but this. Opts out of the analysis because the
// capability it gives up belongs to the lock rather than to this locker.
template<typename T>
    requires (std::same_as<T, ReadLockView>)
bool Locker<T>::tryUpgrade(ReadWriteLock& lock) WTF_IGNORES_THREAD_SAFETY_ANALYSIS
{
    ASSERT(m_isLocked);
    ASSERT(&lock == static_cast<ReadWriteLock*>(&m_lock));
    // The read lock is given up whichever way the upgrade goes, so this locker has nothing left to
    // release in either case. Without this its destructor would count a departure that never had a
    // matching arrival, which is exactly what satisfies a later writer's drain target early.
    m_isLocked = false;
    return lock.tryUpgrade();
}

} // namespace WTF

using WTF::ReadWriteLock;
