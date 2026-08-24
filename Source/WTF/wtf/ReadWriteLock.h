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
#include <wtf/Nonmovable.h>

namespace WTF {

// A read-write lock built directly on ParkingLot, in the style of WTF::Lock.
//
// It's easiest to read lock like this:
//     Locker locker { rwLock.read() };
//
// It's easiest to write lock like this:
//     Locker locker { rwLock.write() };
//
// Fairness. Readers yield to writers: a reader that arrives while any writer is
// waiting parks rather than joining. That alone would starve readers under a stream of
// writers, so a writer releasing the lock hands the read lock directly to every reader it
// wakes - it adds their count to the state on their behalf, under the queue lock, and tells
// them so with a token. They therefore wake up already holding the lock, and cannot lose it
// to a writer that acquires before they are scheduled. Granting rather than merely permitting
// is what makes this bounded: a permitted reader that is slow to wake would find the
// permission withdrawn and re-park, and a writer stream could repeat that indefinitely.
// Writers are kept fair among themselves by ParkingLot's eventual-fairness mechanism, the
// same way WTF::Lock does it.
//
// A read lock is not recursive: taking a second read lock on a thread that already
// holds one deadlocks if a writer is waiting in between.
class ReadWriteLock {
    WTF_MAKE_NONMOVABLE(ReadWriteLock);
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(ReadWriteLock);
public:
    constexpr ReadWriteLock() = default;

    bool tryReadLock()
    {
        uint32_t state = m_state.load(std::memory_order_relaxed);
        if (!(state & (WriterHeldBit | WriterWaitCountMask))) [[likely]] {
            if (m_state.compareExchangeWeak(state, state + ReaderCountUnit, std::memory_order_acquire, std::memory_order_relaxed)) [[likely]]
                return true;
        }
        return false;
    }

    void readLock()
    {
        if (!tryReadLock())
            readLockSlow();
    }

    void readUnlock()
    {
        uint32_t state = m_state.load(std::memory_order_relaxed);
        ASSERT(state & ReaderCountMask);
        // Only the last reader out has anything beyond a decrement to do: it wakes a
        // writer, if one is waiting.
        bool isLastReaderWithWork = (state & ReaderCountMask) == ReaderCountUnit
            && (state & HasParkedWritersBit);
        if (!isLastReaderWithWork) [[likely]] {
            if (m_state.compareExchangeWeak(state, state - ReaderCountUnit, std::memory_order_release)) [[likely]]
                return;
        }
        readUnlockSlow();
    }

    bool tryWriteLock()
    {
        return m_state.compareExchangeWeak(0u, WriterHeldBit, std::memory_order_acquire);
    }

    void writeLock()
    {
        if (!tryWriteLock())
            writeLockSlow();
    }

    void writeUnlock()
    {
        // This deliberately requires the state to be exactly WriterHeldBit, so that a
        // release can never skip writeUnlockSlow while anyone is parked. Do not widen it
        // to ignore the parked bits.
        if (m_state.compareExchangeWeak(WriterHeldBit, 0u, std::memory_order_release)) [[likely]]
            return;
        writeUnlockSlow();
    }

    class ReadLock;
    class WriteLock;

    ReadLock& read();
    WriteLock& write();

    // A lock that every thread has released must read back as zero. Tests assert this to
    // catch state that leaks across acquisitions.
    uint32_t stateForTesting() const { return m_state.load(std::memory_order_relaxed); }

private:
    // State encoding in a 32-bit word. The fields are ordered so that the masks the hot
    // paths test are contiguous runs of bits, and therefore single-instruction immediates:
    // ReaderCountMask | WriterHeldBit and WriterHeldBit | WriterWaitCountMask.
    //
    // Bits 31-16 (16 bits): reader count
    // Bit      15:          writer held
    // Bits 14-2  (13 bits): waiting-writer count
    // Bit       1:          has parked writers
    // Bit       0:          has parked readers
    static constexpr uint32_t ReaderCountShift = 16;
    static constexpr uint32_t ReaderCountUnit = 1u << ReaderCountShift;
    static constexpr uint32_t ReaderCountMask = 0xFFFF0000;
    static constexpr uint32_t WriterHeldBit = 1u << 15;
    static constexpr uint32_t WriterWaitCountShift = 2;
    static constexpr uint32_t WriterWaitCountUnit = 1u << WriterWaitCountShift;
    static constexpr uint32_t WriterWaitCountMask = 0x00007FFC;
    static constexpr uint32_t HasParkedWritersBit = 1u << 1;
    static constexpr uint32_t HasParkedReadersBit = 1u << 0;

    // Tokens for ParkingLot handoff. BargingOpportunity and DirectHandoff match the Lock
    // pattern; ReadGranted says the releasing writer already counted this reader in, so it
    // holds a read lock and must not touch the state.
    enum Token : intptr_t {
        BargingOpportunity = 0,
        DirectHandoff = 1,
        ReadGranted = 2
    };

    WTF_EXPORT_PRIVATE NEVER_INLINE void readLockSlow();
    WTF_EXPORT_PRIVATE NEVER_INLINE void readUnlockSlow();
    WTF_EXPORT_PRIVATE NEVER_INLINE void writeLockSlow();
    WTF_EXPORT_PRIVATE NEVER_INLINE void writeUnlockSlow();

    // Separate parking addresses for readers and writers to enable selective wakeup.
    // ParkingLot uses the address as a key, so different addresses create separate queues.
WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN
    void* readerParkingAddress() { return std::bit_cast<void*>(std::bit_cast<uint8_t*>(this)); }
    void* writerParkingAddress() { return std::bit_cast<void*>((std::bit_cast<uint8_t*>(this) + 1)); }
WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

    Atomic<uint32_t> m_state { 0 };
};

class ReadWriteLock::ReadLock : public ReadWriteLock {
public:
    bool tryLock() { return tryReadLock(); }
    void lock() { readLock(); }
    void unlock() { readUnlock(); }
};

class ReadWriteLock::WriteLock : public ReadWriteLock {
public:
    bool tryLock() { return tryWriteLock(); }
    void lock() { writeLock(); }
    void unlock() { writeUnlock(); }
};

inline ReadWriteLock::ReadLock& ReadWriteLock::read() { return *static_cast<ReadLock*>(this); }
inline ReadWriteLock::WriteLock& ReadWriteLock::write() { return *static_cast<WriteLock*>(this); }

} // namespace WTF

using WTF::ReadWriteLock;
