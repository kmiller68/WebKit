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

#include <wtf/ParkingLot.h>
#include <wtf/Threading.h>
#include <wtf/simde/simde.h>

namespace WTF {

// Wrapped in a namespace because this file is part of a unified source, where file-scope names
// would collide with those of the other files it is compiled with.
namespace ReadWriteLockInternal {

// This lock spins on its own schedule rather than sharing WTF::Lock's, and readers and writers do
// not share one either. The values below were measured with the read-write configurations of
// Source/WTF/benchmarks/LockSpeedTest.cpp; what follows is what the measurements showed, since most
// of it is not what you would guess.
//
// Writers do not yield. Thread::yield() on Darwin is a thread_switch() Mach call with
// SWITCH_OPTION_DEPRESS, so it is a syscall that also depresses the caller's priority for up to a
// millisecond, and a spinning writer gains nothing for it: dropping it raised throughput 8-9% with
// one writer and with four, and never measured worse.
//
// Readers do yield, because they must not win too often. A reader that spins long enough to
// acquire on the fast path never parks, and so never sets HasParkedReadersBit; once readers do park,
// every writer release goes through the cohort grant in writeUnlockSlow, which hands the read lock
// to all parked readers at once and leaves the writer to re-register and wait out the whole cohort
// behind them. One writer turn per cohort is a ~50x writer penalty. Every setting that made readers
// more successful - not yielding, or a shorter spinLimit - therefore cost writers several times what
// it gained readers, and cut total throughput by a third or more.
//
// What tunes the wait is total spin time, spinLimit * pauseCount, more than either factor alone:
// halving one and doubling the other measured the same. Too short is bad for everyone (a quarter of
// the time-to-park cost 20-40% throughput everywhere), and past a point more only shifts the split
// further towards writers.
//
// Note that yieldInterval counts iterations, not time, so it is not independent of pauseCount:
// shortening an iteration makes yields more frequent in wall-clock terms. That coupling is sharp
// enough to matter - cutting pauseCount alone, leaving yieldInterval nominally unchanged, cost 40%
// of total throughput.
struct Tuning {
    // How many iterations before giving up and parking.
    unsigned spinLimit;
    // Pauses per iteration. simde_mm_pause() is isb on ARM64, a full instruction-synchronization
    // barrier, and so much more expensive than x86's pause.
    unsigned pauseCount;
    // Yield every nth iteration, or never if zero.
    unsigned yieldInterval;
};

#if CPU(ARM64) && OS(MACOS)
constexpr Tuning readerTuning { .spinLimit = 80, .pauseCount = 8, .yieldInterval = 16 };
constexpr Tuning writerTuning { .spinLimit = 80, .pauseCount = 12, .yieldInterval = 0 };
#elif CPU(ARM64) && OS(IOS_FAMILY)
// Not measured on this platform; these mirror the values LockAlgorithm uses here.
constexpr Tuning readerTuning { .spinLimit = 40, .pauseCount = 16, .yieldInterval = 4 };
constexpr Tuning writerTuning { .spinLimit = 40, .pauseCount = 16, .yieldInterval = 4 };
#else
// Likewise unmeasured, and pause is a much cheaper instruction outside ARM64, so keep the old
// sched-yield loop rather than porting conclusions that were drawn from isb costs.
constexpr Tuning readerTuning { .spinLimit = 40, .pauseCount = 0, .yieldInterval = 1 };
constexpr Tuning writerTuning { .spinLimit = 40, .pauseCount = 0, .yieldInterval = 1 };
#endif

// One iteration of the spin loop. Returns false once the caller has spun long enough and should
// park instead. Spin on plain loads at the call site: read-modify-write spinning ping-pongs the
// lock's cache line between cores, which hurts exactly the contended case this has to survive.
template<const Tuning& tuning>
ALWAYS_INLINE bool spinStep(unsigned& spinCount)
{
    if (spinCount >= tuning.spinLimit)
        return false;
    ++spinCount;
    // Checked after incrementing so that the first few spins do not yield. This makes it more
    // likely we acquire without having depressed our own priority beforehand.
    if constexpr (tuning.yieldInterval) {
        if (!(spinCount % tuning.yieldInterval))
            Thread::yield();
    }
    for (unsigned i = 0; i < tuning.pauseCount; ++i)
        simde_mm_pause();
    return true;
}

} // namespace ReadWriteLockInternal

// Both slow paths thread the last observed state through the loop by hand rather than using
// Atomic::transaction, which reloads on every attempt. Every RMW here hands back the value it
// saw — compareExchangeStrong returns the actual value on failure, exchangeOr and exchangeAdd
// return the previous one — so a retry costs no load at all. The one place a fresh load is
// wanted is the spin step, where re-reading is the whole point.
//
// Release invariant: releasing the lock means clearing WriterHeldBit or giving up the last
// reader count. Every path that does either must decide from a has-parked bit it observed within
// the same compare-and-swap, unless it is publishing the release while holding the bucket lock of
// the queue that bit belongs to. A releaser that instead decides from a value it loaded earlier
// can strand a thread that parked in between, leaving it asleep with the lock free and nobody
// left to hand it to. Both queues need checking, not just the one being woken: the reader and
// writer queues have separate bucket locks, so holding one says nothing about the other.
//
// Deciding within the compare-and-swap works because the has-parked bits, the held bit and the
// reader count share one word: a parking thread's exchangeOr of its has-parked bit and a
// releaser's compare-and-swap are two read-modify-writes on the same location, so they are
// totally ordered on every architecture. Either the releaser observes the bit and declines to
// release, or it wins and the parking thread's exchangeOr returns the released state, so it never
// parks. No fence substitutes for this, and none is needed; the atomicity of a single-location
// RMW is the whole mechanism.

void ReadWriteLock::readLockSlow()
{
    unsigned spinCount = 0;

    for (;;) {
        uint32_t state = m_state.load(std::memory_order_relaxed);
        // Yield to writers that hold or are waiting for the lock. This is what keeps a
        // stream of readers from starving a writer.
        if (!(state & (WriterHeldBit | WriterWaitCountMask))) {
            ASSERT((state & ReaderCountMask) != ReaderCountMask);
            uint32_t actual = m_state.compareExchangeStrong(state, state + ReaderCountUnit, std::memory_order_acquire);
            if (actual == state)
                return;
            state = actual;
            continue;
        }

        if (ReadWriteLockInternal::spinStep<ReadWriteLockInternal::readerTuning>(spinCount))
            continue;

        if (!(state & HasParkedReadersBit))
            state = m_state.exchangeOr(HasParkedReadersBit) | HasParkedReadersBit;

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

        if (parkResult.wasUnparked && parkResult.token == DirectHandoff)
            return;
    }
}

void ReadWriteLock::readUnlockSlow()
{
    for (;;) {
        uint32_t state = m_state.load(std::memory_order_relaxed);
        ASSERT(!(state & WriterHeldBit));
        ASSERT(state & ReaderCountMask);

        // Only the last reader out hands the lock on, and we can get here while others still
        // hold it. readUnlock() falls through on any failed exchange, and its exchange can fail
        // because a new reader joined: readers are admitted whenever no writer holds or is
        // registered, which is exactly the situation when the parked bit belongs to readers
        // rather than to a writer. Waking anyone then would transfer ownership out from under
        // the readers that are still here.
        bool isLastReader = (state & ReaderCountMask) == ReaderCountUnit;

        if (isLastReader && (state & HasParkedWritersBit)) {
            bool didRelease = false;
            ParkingLot::unparkOne(
                writerParkingAddress(),
                [&](ParkingLot::UnparkResult result) -> intptr_t {
                    didRelease = result.unparkedCount;
                    m_state.transaction([&](uint32_t& bits) -> bool {
                        // Only the unlocker can clear either of the Parked bits and only in the respective unpark callback.
                        ASSERT(bits & HasParkedWritersBit);
                        ASSERT(!(bits & WriterHeldBit));
                        if (!result.unparkedCount) {
                            // HasParkedWritersBit was stale. Heal it but keep our reader count so
                            // that the loop can re-dispatch to whoever is really parked. Giving up
                            // the lock here would strand a parked reader whenever the writer
                            // registration the bit stood for is also gone.
                            bits &= ~HasParkedWritersBit;
                            return true;
                        }
                        if (!result.mayHaveMoreThreads)
                            bits &= ~HasParkedWritersBit;
                        if (result.timeToBeFair) {
                            // It's one less operation to keep everything consist here.
                            // timeToBeFair implies we dequeued a thread, and a parked writer
                            // always still holds its registration, so this cannot underflow
                            // into WriterHeldBit.
                            ASSERT(bits & WriterWaitCountMask);
                            bits -= WriterWaitCountUnit;
                            bits |= WriterHeldBit;
                        }
                        bits -= ReaderCountUnit;
                        return true;
                    }, std::memory_order_release);
                    return result.timeToBeFair ? DirectHandoff : BargingOpportunity;
                });
            if (didRelease)
                return;
            continue;
        }

        if (isLastReader && (state & HasParkedReadersBit)) {
            // These readers parked on a waiting writer, since no writer can have held the lock
            // while we held it for reading. Hand them the lock rather than leaving them to wait
            // on a registration whose owner may still be spinning: new readers stay blocked by
            // that registration, so this cannot starve the writer.
            bool didRelease = false;
            ParkingLot::unparkCount(
                readerParkingAddress(), UINT32_MAX,
                [&](ParkingLot::UnparkResult result) -> intptr_t {
                    didRelease = m_state.transaction([&](uint32_t& bits) -> bool {
                        // A writer can have parked since the load above, and we hold the reader
                        // queue's bucket lock rather than the writer's, so re-reading here is the
                        // only thing that can see it. Decline and let the loop take the branch
                        // above: giving up the last reader count now would leave that writer
                        // asleep with the lock free and nobody left to wake it.
                        if (bits & HasParkedWritersBit)
                            return false;
                        ASSERT(bits & HasParkedReadersBit);
                        // We asked for every waiter at this address, so the queue is drained.
                        // mayHaveMoreThreads is bucket-granular and would only leave the bit
                        // stale here.
                        bits &= ~HasParkedReadersBit;
                        // Count the woken cohort in on their behalf, then drop our own count.
                        // Note that a new reader can have joined since the load above, so this
                        // is not necessarily a transfer of the last count.
                        ASSERT((bits >> ReaderCountShift) + result.unparkedCount <= (ReaderCountMask >> ReaderCountShift));
                        bits += (result.unparkedCount - 1) * ReaderCountUnit;
                        return true;
                    }, std::memory_order_release);
                    // Having declined, we own nothing to give away, so anyone we dequeued has to
                    // contend for the lock again rather than wake up believing they hold it.
                    return didRelease && result.unparkedCount ? DirectHandoff : BargingOpportunity;
                });
            if (didRelease)
                return;
            continue;
        }

        // If there are writers waiting but not parked they'll end up with the lock. Readers can't acquire the lock
        // while there's pending writers.
        if (m_state.compareExchangeWeak(state, state - ReaderCountUnit, std::memory_order_release))
            return;
    }
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

        if (ReadWriteLockInternal::spinStep<ReadWriteLockInternal::writerTuning>(spinCount))
            continue;

        state = m_state.exchangeOr(HasParkedWritersBit) | HasParkedWritersBit;

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
            // The releasing writer set WriterHeldBit and retired our WriterWaitCountUnit for us.
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
            // who owns what happens inside the callback, with the read queue lock held, so the
            // count cannot change under us and no woken reader has to re-examine the state.
            bool didHandoffLock = false;
            ParkingLot::unparkCount(
                readerParkingAddress(), UINT32_MAX,
                [&](ParkingLot::UnparkResult result) -> intptr_t {
                    // In this lambda we're holding the reader's parking lock, so a reader racing to park
                    // will either:
                    // 1) Get the lock first and have been dequeued
                    // 2) Have set the HasParkedReadersBit but will fail their parkConditionally validation and repark.
                    if (!result.unparkedCount) {
                        // Most likely someone is racing to park and lost the race, nothing we can do about it here.
                        m_state.exchangeAnd(~HasParkedReadersBit, std::memory_order_relaxed);
                        return BargingOpportunity;
                    }

                    didHandoffLock = true;
                    // After this transaction the readers will now hold the lock.
                    m_state.transaction([&](uint32_t& bits) -> bool {
                        bits &= ~WriterHeldBit;
                        // We asked for every waiter at this address, so the queue is empty.
                        // mayHaveMoreThreads is bucket-granular and would only leave the bit
                        // stale here.
                        bits &= ~HasParkedReadersBit;
                        ASSERT((bits >> ReaderCountShift) + result.unparkedCount <= (ReaderCountMask >> ReaderCountShift));
                        bits += result.unparkedCount * ReaderCountUnit;
                        return true;
                    }, std::memory_order_release);
                    return DirectHandoff;
                });

            if (didHandoffLock)
                return;
        }

        if (state & HasParkedWritersBit) {
            bool done = false;
            ParkingLot::unparkOne(
                writerParkingAddress(),
                [&](ParkingLot::UnparkResult result) -> intptr_t {
                    if (result.unparkedCount && result.timeToBeFair) {
                        // Hand the lock over directly: keep WriterHeldBit set and retire the
                        // woken writer's registration, since it returns without running the
                        // acquire transaction that would otherwise have done so. This is exact
                        // because we know we dequeued exactly one thread. A reader that parked
                        // since the load above stays parked, which is correct: the lock is never
                        // released here, so granting that reader is the successor's job.
                        m_state.transactionRelaxed([&](uint32_t& bits) -> bool {
                            ASSERT(bits & WriterWaitCountMask);
                            bits -= WriterWaitCountUnit;
                            if (!result.mayHaveMoreThreads)
                                bits &= ~HasParkedWritersBit;
                            return true;
                        });
                        done = true;
                        return DirectHandoff;
                    }

                    // Release and let the woken thread contend. Declining while
                    // HasParkedReadersBit is set is what upholds the release invariant above:
                    // our snapshot of that bit predates this callback, and the reader queue's
                    // bucket lock is not the one held here, so the compare-and-swap is the only
                    // thing standing between a reader that parked in the window and a lost wake.
                    done = m_state.transaction([&](uint32_t& bits) -> bool {
                        if (bits & HasParkedReadersBit)
                            return false;
                        bits &= ~(WriterHeldBit | HasParkedWritersBit);
                        // This can conservatively return true.
                        if (result.mayHaveMoreThreads)
                            bits |= HasParkedWritersBit;
                        return true;
                    }, std::memory_order_release);
                    return BargingOpportunity;
                });
            if (done)
                return;

            continue;
        }

        // Nothing is parked, so release without waking anyone. This has to be a CAS from the
        // state observed above rather than a blind clear: a thread can set either parked bit
        // between that load and here, and releasing without noticing would drop its wakeup and
        // leave it asleep forever with the lock free. Retrying re-reads the bit and routes to
        // the branch that wakes it.
        if (m_state.compareExchangeWeak(state, state & ~WriterHeldBit, std::memory_order_release))
            return;
    }
}

} // namespace WTF
