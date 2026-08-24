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

#include "config.h"
#include <wtf/ReadWriteLock.h>

#include <wtf/LockAlgorithmInlines.h>
#include <wtf/ParkingLot.h>
#include <limits>

namespace WTF {

// Both slow paths thread the last observed state through the loop by hand rather than using
// Atomic::transaction, which reloads on every attempt. Every RMW here hands back the value it
// saw — compareExchangeStrong returns the actual value on failure, exchangeOr and exchangeAdd
// return the previous one — so a retry costs no load at all. The one place a fresh load is
// wanted is the spin step, where re-reading is the whole point.

void ReadWriteLock::readLockSlow()
{
    unsigned spinCount = 0;

    for (;;) {
        uint32_t state = m_state.load(std::memory_order_relaxed);
        // Yield to writers that hold or are waiting for the lock. This is what keeps a
        // stream of readers from starving a writer.
        if (!(state & (WriterHeldBit | WriterWaitCountMask))) {
            ASSERT((state & ReaderCountMask) != ReaderCountMask);
            uint32_t actual = m_state.compareExchangeStrong(state, state + ReaderCountUnit, std::memory_order_acquire, std::memory_order_relaxed);
            if (actual == state)
                return;
            state = actual;
            continue;
        }

        if (!(state & HasParkedReadersBit) && spinCount < LockSpin::spinLimit) {
            LockSpin::spinStep(spinCount);
            state = m_state.load(std::memory_order_relaxed);
            continue;
        }

        if (!(state & HasParkedReadersBit))
            state = m_state.exchangeOr(HasParkedReadersBit, std::memory_order_relaxed) | HasParkedReadersBit;

        ParkingLot::ParkResult parkResult = ParkingLot::parkConditionally(
            readerParkingAddress(),
            [&]() -> bool {
                // Requiring HasParkedReadersBit is what makes it safe for a releasing writer
                // to clear that bit after finding this queue empty: either we observe the
                // clear and do not park, or our own set of the bit is what it observes.
                // Without this, we could park having already had our bit cleared, and nobody
                // would ever wake us.
                uint32_t currentState = m_state.load(std::memory_order_relaxed);
                return (currentState & (WriterHeldBit | WriterWaitCountMask))
                    && (currentState & HasParkedReadersBit);
            },
            []() { },
            ParkingLot::Time::infinity());

        // The releasing writer added our reader count on our behalf under the queue lock, so
        // we already hold a read lock and must not touch the state. Ordering comes from
        // ParkingLot's per-thread parkingLock, which the unparker takes before signalling.
        if (parkResult.token == ReadGranted)
            return;
    }
}

void ReadWriteLock::readUnlockSlow()
{
    bool shouldWakeWriter = false;

    m_state.transaction([&](uint32_t& bits) -> bool {
        ASSERT(bits & ReaderCountMask);
        bits -= ReaderCountUnit;
        shouldWakeWriter = !(bits & ReaderCountMask) && (bits & HasParkedWritersBit);
        return true;
    }, std::memory_order_release);

    if (!shouldWakeWriter)
        return;

    // The lock is already free, so the writer we wake has to contend for it. A stale
    // HasParkedWritersBit heals here: with nobody to unpark, the callback clears it.
    ParkingLot::unparkOne(
        writerParkingAddress(),
        [&](ParkingLot::UnparkResult result) -> intptr_t {
            if (!result.mayHaveMoreThreads) {
                m_state.exchangeAnd(~HasParkedWritersBit, std::memory_order_relaxed);
            }
            return BargingOpportunity;
        });
}

void ReadWriteLock::writeLockSlow()
{
    unsigned spinCount = 0;

    // Register as a waiting writer, which holds new readers out. The count is exact: it
    // is incremented once here and decremented only by whoever grants this writer the
    // lock, so it is precisely the number of writers between entry and acquisition. That
    // exactness is what reader liveness rests on — a nonzero count always has a live
    // writer behind it, so the readers it blocks are guaranteed to be woken when that
    // writer releases.
    uint32_t previousState = m_state.exchangeAdd(WriterWaitCountUnit, std::memory_order_relaxed);
    RELEASE_ASSERT((previousState & WriterWaitCountMask) != WriterWaitCountMask, previousState);
    uint32_t state = previousState + WriterWaitCountUnit;

    for (;; state = m_state.load(std::memory_order_relaxed)) {
        // Retire our own registration as we take the lock. Note that this never consults
        // the waiting-writer count, so no value of it can prevent a writer from acquiring.
        if (!(state & (ReaderCountMask | WriterHeldBit))) {
            ASSERT(state & WriterWaitCountMask);
            // We want the exchange to fail if we're in a reader phase.
            uint32_t desired = (state | WriterHeldBit) - WriterWaitCountUnit;
            uint32_t actual = m_state.compareExchangeStrong(state, desired, std::memory_order_acquire, std::memory_order_relaxed);
            if (actual == state)
                return;
            state = actual;
            continue;
        }

        if (!(state & HasParkedWritersBit) && spinCount < LockSpin::spinLimit) {
            LockSpin::spinStep(spinCount);
            continue;
        }

        if (!(state & HasParkedWritersBit))
            state = m_state.exchangeOr(HasParkedWritersBit, std::memory_order_relaxed) | HasParkedWritersBit;

        ParkingLot::ParkResult parkResult = ParkingLot::parkConditionally(
            writerParkingAddress(),
            [&]() -> bool {
                // HasParkedWritersBit is required for the same reason as on the reader side:
                // a releaser that clears it after finding this queue empty must not be able to
                // do so while we are on our way to parking.
                uint32_t currentState = m_state.load(std::memory_order_relaxed);
                return (currentState & (ReaderCountMask | WriterHeldBit))
                    && (currentState & HasParkedWritersBit);
            },
            []() { },
            ParkingLot::Time::infinity());

        if (parkResult.wasUnparked && parkResult.token == DirectHandoff) {
            // The releasing writer set WriterHeldBit and retired our registration for us.
            ASSERT(m_state.load(std::memory_order_relaxed) & WriterHeldBit);
            return;
        }

    }
}

void ReadWriteLock::writeUnlockSlow()
{
    for (;;) {
        uint32_t state = m_state.load(std::memory_order_relaxed);
        ASSERT(state & WriterHeldBit);

        if (state & HasParkedReadersBit) {
            // Hand the read lock to the whole parked cohort at once. Everything that decides
            // who owns what happens inside the callback, with the queue lock held, so the
            // count cannot change under us and no woken reader has to re-examine the state.
            bool granted = false;
            bool shouldWakeWriter = false;
            ParkingLot::unparkCount(
                readerParkingAddress(), std::numeric_limits<unsigned>::max(),
                [&](ParkingLot::UnparkResult result) -> intptr_t {
                    if (!result.unparkedCount) {
                        // HasParkedReadersBit was stale: a reader set it and then acquired
                        // without parking. Clear it, but keep holding the lock, because a
                        // release here would drop the wakeup of anyone on the writer queue.
                        m_state.exchangeAnd(~HasParkedReadersBit, std::memory_order_relaxed);
                        return BargingOpportunity;
                    }

                    granted = true;
                    m_state.transaction([&](uint32_t& bits) -> bool {
                        bits &= ~WriterHeldBit;
                        // We asked for every waiter at this address, so the queue is empty.
                        // mayHaveMoreThreads is bucket-granular and would only leave the bit
                        // stale here.
                        bits &= ~HasParkedReadersBit;
                        ASSERT((bits >> ReaderCountShift) + result.unparkedCount <= (ReaderCountMask >> ReaderCountShift));
                        bits += result.unparkedCount * ReaderCountUnit;
                        shouldWakeWriter = bits & HasParkedWritersBit;
                        return true;
                    }, std::memory_order_release);
                    return ReadGranted;
                });

            if (!granted)
                continue;

            // A writer may be parked behind the cohort we just granted. Nothing else would
            // wake it: the readers now holding the lock will only unpark a writer if the last
            // one out sees HasParkedWritersBit, and it was set before they were counted in.
            if (shouldWakeWriter) {
                ParkingLot::unparkOne(
                    writerParkingAddress(),
                    [&](ParkingLot::UnparkResult result) -> intptr_t {
                        if (!result.mayHaveMoreThreads)
                            m_state.exchangeAnd(~HasParkedWritersBit, std::memory_order_relaxed);
                        return BargingOpportunity;
                    });
            }
            return;
        }

        if (state & HasParkedWritersBit) {
            ParkingLot::unparkOne(
                writerParkingAddress(),
                [&](ParkingLot::UnparkResult result) -> intptr_t {
                    if (result.unparkedCount && result.timeToBeFair) {
                        // Hand the lock over directly: keep WriterHeldBit set and retire the
                        // woken writer's registration, since it returns without running the
                        // acquire transaction that would otherwise have done so. This is exact
                        // because we know we dequeued exactly one thread.
                        m_state.transactionRelaxed([&](uint32_t& bits) -> bool {
                            ASSERT(bits & WriterWaitCountMask);
                            bits -= WriterWaitCountUnit;
                            if (!result.mayHaveMoreThreads)
                                bits &= ~HasParkedWritersBit;
                            return true;
                        });
                        return DirectHandoff;
                    }

                    // Release and let the woken thread contend. A stale HasParkedWritersBit
                    // heals here, since nothing was dequeued when there was nobody to wake.
                    m_state.transaction([&](uint32_t& bits) -> bool {
                        bits &= ~WriterHeldBit;
                        if (!result.mayHaveMoreThreads)
                            bits &= ~HasParkedWritersBit;
                        return true;
                    }, std::memory_order_release);
                    return BargingOpportunity;
                });
            return;
        }

        // Nothing is parked, so release without waking anyone. This has to be a CAS from the
        // state observed above rather than a blind clear: a thread can set either parked bit
        // between that load and here, and releasing without noticing would drop its wakeup and
        // leave it asleep forever with the lock free. Retrying re-reads the bit and routes to
        // the branch that wakes it.
        if (m_state.compareExchangeWeak(state, state & ~WriterHeldBit, std::memory_order_release, std::memory_order_relaxed))
            return;
    }
}

} // namespace WTF
