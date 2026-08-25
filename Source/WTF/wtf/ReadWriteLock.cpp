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
// Source/WTF/benchmarks/LockSpeedTest.cpp, at 8-12 readers and 1-2 writers with the writers sleeping
// between acquisitions, which is the shape this lock is expected to see. Measurements taken with
// writers hammering the lock instead point at very different values, so the writer sleep is not an
// incidental parameter - see ReadWriteLockSpinTuning.md.
//
// Neither side yields. Thread::yield() on Darwin is a thread_switch() Mach call with
// SWITCH_OPTION_DEPRESS, so it is a syscall that also depresses the caller's priority for up to a
// millisecond, and a spinning writer gains nothing for it.
//
// The writer's spin length barely matters here, because a writer registers on entry and so stops new
// readers immediately; what it then waits for is the readers already inside draining, which takes
// only a few microseconds. A short spin therefore measures the same as one thirty times longer and
// burns far less, so it is short.
//
// Reader spin tuning does not measurably move anything at this workload, because readers are almost
// never blocked by a writer - what limits them is contention on the reader count itself, which no
// spin setting affects. The values here are the ones that were best when writers do hammer the lock,
// since it costs nothing to keep them.
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
constexpr Tuning writerTuning { .spinLimit = 20, .pauseCount = 16, .yieldInterval = 0 };
// How deep into the writer's spin to announce ourselves in the waiting-writer count, which is what
// holds new readers out. Zero registers on entry.
//
// Zero measured best by a wide margin once writers arrive at a realistic rate rather than in a tight
// loop: with a writer sleeping 100us between acquisitions, registering immediately costs 1us to
// acquire against 37us at 128, because a deferred writer spins out its whole budget before it is
// allowed to stop new readers joining ahead of it. Reader throughput was unaffected either way.
//
// Deferring only helps when writers hammer the lock with no gap at all. In that case two writers keep
// a registration outstanding continuously, no reader ever reaches its fast path, and throughput
// collapses about tenfold - see ReadWriteLockSpinTuning.md. Deferring hides that, at the cost above.
// The real fix is for a writer not to block readers while it is merely queued behind another writer,
// which needs single-owner blocking rather than a count, and is a design change rather than a
// constant.
constexpr unsigned writerRegisterAfter = 0;
#elif CPU(ARM64) && OS(IOS_FAMILY)
// Not measured on this platform; these mirror the values LockAlgorithm uses here.
constexpr Tuning readerTuning { .spinLimit = 40, .pauseCount = 16, .yieldInterval = 4 };
constexpr Tuning writerTuning { .spinLimit = 40, .pauseCount = 16, .yieldInterval = 4 };
constexpr unsigned writerRegisterAfter = 4;
#else
// Likewise unmeasured, and pause is a much cheaper instruction outside ARM64, so keep the old
// sched-yield loop rather than porting conclusions that were drawn from isb costs.
constexpr Tuning readerTuning { .spinLimit = 40, .pauseCount = 0, .yieldInterval = 1 };
constexpr Tuning writerTuning { .spinLimit = 40, .pauseCount = 0, .yieldInterval = 1 };
constexpr unsigned writerRegisterAfter = 4;
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
// Release invariant: releasing the lock means clearing WriterHeldBit or giving up the last reader
// count. Every path that does either must decide from a has-parked bit it observed within the same
// read-modify-write, unless it is publishing the release while holding the bucket lock of the queue
// that bit belongs to. A releaser that instead decides from a value it loaded earlier can strand a
// thread that parked in between, leaving it asleep with the lock free and nobody left to hand it to.
// Both queues need checking, not just the one being woken: the reader and writer queues have
// separate bucket locks, so holding one says nothing about the other.
//
// The reader side satisfies this without a compare-and-swap at all. readUnlock() decrements with a
// single exchangeAdd and decides from the value that returns, which is by definition the state
// immediately before its own decrement.
//
// Deciding within the read-modify-write works because the has-parked bits, the held bit and the
// reader count share one word: a parking thread's exchangeOr of its has-parked bit and a releaser's
// RMW are two read-modify-writes on the same location, so they are totally ordered on every
// architecture. Either the releaser observes the bit and takes the slow path, or it wins and the
// parking thread's exchangeOr returns the released state, so it never parks. No fence substitutes
// for this, and none is needed; the atomicity of a single-location RMW is the whole mechanism.

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
    // readUnlock() has already given up our reader count, so there is nothing left here to release:
    // this is purely about waking whoever was parked behind us. That is why everything below
    // re-reads the state. Between that decrement and this call a writer can have acquired the lock,
    // or a new reader can have joined, and in either case handing the lock on becomes their job. The
    // parked bits stay set until someone actually dequeues, so passing the obligation along cannot
    // lose it.
    for (;;) {
        uint32_t state = m_state.load(std::memory_order_relaxed);

        // Somebody else owns the lock now and will hand it on when they release.
        if (state & (WriterHeldBit | ReaderCountMask))
            return;

        if (state & HasParkedWritersBit) {
            bool didWake = false;
            ParkingLot::unparkOne(
                writerParkingAddress(),
                [&](ParkingLot::UnparkResult result) -> intptr_t {
                    didWake = result.unparkedCount;
                    if (!result.unparkedCount) {
                        // The bit was stale. Heal it so the loop can move on to the readers.
                        m_state.exchangeAnd(~HasParkedWritersBit, std::memory_order_relaxed);
                        return BargingOpportunity;
                    }

                    if (result.timeToBeFair) {
                        // Hand the lock over directly: take WriterHeldBit and retire the woken
                        // writer's registration, since it returns without running the acquire that
                        // would otherwise have done so. Exact because we dequeued exactly one
                        // thread, and a parked writer always still holds its registration.
                        bool handedOff = m_state.transaction([&](uint32_t& bits) -> bool {
                            // Someone acquired between the load above and here, so there is no
                            // lock to give away; let the thread we woke contend for it instead.
                            if (bits & (WriterHeldBit | ReaderCountMask))
                                return false;
                            ASSERT(bits & WriterWaitCountMask);
                            bits -= WriterWaitCountUnit;
                            bits |= WriterHeldBit;
                            if (!result.mayHaveMoreThreads)
                                bits &= ~HasParkedWritersBit;
                            return true;
                        }, std::memory_order_release);
                        if (handedOff)
                            return DirectHandoff;
                    } else if (!result.mayHaveMoreThreads)
                        m_state.exchangeAnd(~HasParkedWritersBit, std::memory_order_relaxed);

                    // The lock is already free, so the writer we woke has to contend for it.
                    return BargingOpportunity;
                });
            if (didWake)
                return;
            continue;
        }

        if (state & HasParkedReadersBit) {
            // These readers parked on a writer, and no writer holds the lock now, so hand it to the
            // whole cohort at once rather than leaving them to race for it one at a time.
            bool didWake = false;
            ParkingLot::unparkCount(
                readerParkingAddress(), UINT32_MAX,
                [&](ParkingLot::UnparkResult result) -> intptr_t {
                    didWake = result.unparkedCount;
                    if (!result.unparkedCount) {
                        m_state.exchangeAnd(~HasParkedReadersBit, std::memory_order_relaxed);
                        return BargingOpportunity;
                    }

                    bool granted = m_state.transaction([&](uint32_t& bits) -> bool {
                        // A writer acquired since the load above. Note that new readers arriving
                        // are fine, since readers coexist; only a writer forces us to decline.
                        if (bits & WriterHeldBit)
                            return false;
                        // We asked for every waiter at this address, so the queue is drained.
                        // mayHaveMoreThreads is bucket-granular and would only leave the bit stale.
                        bits &= ~HasParkedReadersBit;
                        ASSERT((bits >> ReaderCountShift) + result.unparkedCount <= (ReaderCountMask >> ReaderCountShift));
                        bits += result.unparkedCount * ReaderCountUnit;
                        return true;
                    }, std::memory_order_release);
                    return granted ? DirectHandoff : BargingOpportunity;
                });
            if (didWake)
                return;
            continue;
        }

        return;
    }
}

void ReadWriteLock::writeLockSlow()
{
    unsigned spinCount = 0;
    // Whether we have announced ourselves in the waiting-writer count. Registering is what holds new
    // readers out, so we defer it until we are about to park rather than doing it on entry. A writer
    // that acquires within its spin never blocks a reader at all, which matters because readers yield
    // to the count rather than to the held bit: with two writers registering eagerly, the count is
    // never zero for long enough for any reader to use its fast path, and every operation on the lock
    // degenerates into a park/unpark handoff.
    //
    // The count stays exact in the sense reader liveness needs - it is still incremented once per
    // writer and retired only by whoever grants that writer the lock, so a nonzero count always has a
    // live writer behind it, and the readers it blocks are guaranteed to be woken when that writer
    // releases. Registration also strictly precedes parking, so every parked writer holds a
    // registration and the handoff paths can retire one on its behalf.
    bool registered = false;
    uint32_t state = m_state.load(std::memory_order_relaxed);

    for (;;) {
        // Retire our own registration, if we made one, as we take the lock. Note that this never
        // consults the waiting-writer count, so no value of it can prevent a writer from acquiring.
        if (!(state & (ReaderCountMask | WriterHeldBit))) {
            uint32_t desired = state | WriterHeldBit;
            if (registered) {
                ASSERT(state & WriterWaitCountMask);
                desired -= WriterWaitCountUnit;
            }
            uint32_t actual = m_state.compareExchangeStrong(state, desired, std::memory_order_acquire, std::memory_order_relaxed);
            if (actual == state)
                return;
            state = actual;
            continue;
        }

        if (!registered && spinCount >= ReadWriteLockInternal::writerRegisterAfter) {
            uint32_t previous = m_state.exchangeAdd(WriterWaitCountUnit, std::memory_order_relaxed);
            RELEASE_ASSERT((previous & WriterWaitCountMask) != WriterWaitCountMask, previous);
            registered = true;
            // Retry immediately: holding readers out may be all we needed, and if it is not, we go
            // back to spinning rather than straight to a park.
            state = previous + WriterWaitCountUnit;
            continue;
        }

        if (ReadWriteLockInternal::spinStep<ReadWriteLockInternal::writerTuning>(spinCount)) {
            state = m_state.load(std::memory_order_relaxed);
            continue;
        }

        ASSERT(registered);
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
            // The releasing thread set WriterHeldBit and retired our WriterWaitCountUnit for us.
            ASSERT(m_state.load(std::memory_order_relaxed) & WriterHeldBit);
            return;
        }

        state = m_state.load(std::memory_order_relaxed);
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
