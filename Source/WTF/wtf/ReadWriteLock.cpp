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
#include <limits>
#include <wtf/simde/simde.h>

namespace WTF {

// Wrapped in a namespace because this file is part of a unified source, where file-scope names would
// collide with those of the other files it is compiled with.
namespace ReadWriteLockInternal {

// A reader waits out one writer's phase; a writer waits for the readers already inside to finish their
// critical sections. Both spin before parking, and both budgets are measured rather than guessed - see
// ReadWriteLockSpinTuning.md.
//
// A budget is a cap on wasted time, not a cost: the loop exits the moment its condition flips, so the
// time actually spent is the smaller of the cap and the wait. A cap is therefore nearly free when it is
// larger than the wait, and only expensive when it is not - in which case the whole cap is burned and
// the caller parks anyway. That makes the useful question "is the cap bigger than what we are waiting
// for", and the answer wants to be yes:
//
//  - A reader is blocked for a whole phase, which is the writer's drain wait plus its critical section.
//  - A writer's drain is over once the readers already inside finish, so roughly one read critical
//    section, since readers run concurrently.
//
// One iteration is about 13ns, almost entirely the pause below, so 640 is a cap of about 8us. That
// covers both waits for critical sections up to a few microseconds, which is what these are tuned for.
// Callers holding either lock for far longer than that get no benefit and pay up to 15% more CPU;
// callers holding them very briefly are unaffected, because the cap is never spent.
//
// A cap that lands short of the wait is the worst case - it burns and then parks regardless - so these
// are deliberately well clear rather than marginal. Measured at a 570ns read critical section, raising
// the reader from 0.5us to 8us doubled read throughput and cut write acquire latency twelvefold, while
// intermediate values were worse than either end.
constexpr unsigned readerSpinLimit = 640;
constexpr unsigned writerSpinLimit = 640;
// Yields a blocked reader attempts after its pause budget is spent, before parking. These are a second
// phase rather than interleaved with the pauses the way LockAlgorithm does it, and the ordering is the
// point: nothing reaches them unless the pause spin already failed, so an acquire that completes
// promptly never yields and so never depresses its own priority. Interleaving measured 27% off read
// throughput under heavy contention for the same benefit, because then every waiter yields whether or
// not it was about to succeed.
//
// What they buy is the oversubscribed case, where blocked readers spinning are occupying the cores the
// readers inside the lock need in order to finish and let a writer in. Eight yields measured 37% lower
// write acquire latency with 28 readers on 12 performance cores, and nothing measurable anywhere else.
// Raising it further keeps improving that latency - 32 yields is twelve times better again - but starts
// costing oversubscribed read throughput, so this is the value that is free.
//
// Only readers yield. The same experiment on the writer's drain wait moved its latency by 13% against
// the reader side's 3x, which is what one would expect: a writer waits alone, so there is no crowd of
// spinners to get out of the way.
constexpr unsigned readerYieldLimit = 8;
// simde_mm_pause() is isb on ARM64, a full instruction-synchronization barrier and so much more
// expensive than x86's pause - about 13ns, which dominates a spin iteration. Only the product of this
// and a spin limit matters, so this stays at one and the limits carry the tuning.
constexpr unsigned pauseCount = 1;

// One iteration of a spin loop. Returns false once the caller has spun long enough and should park.
// Spin on plain loads at the call site: read-modify-write spinning ping-pongs the word between cores,
// which hurts exactly the contended case this has to survive.
ALWAYS_INLINE bool spinStep(unsigned& spinCount, unsigned limit)
{
    if (spinCount >= limit)
        return false;
    ++spinCount;
    for (unsigned i = 0; i < pauseCount; ++i)
        simde_mm_pause();
    return true;
}

} // namespace ReadWriteLockInternal

// Both parked bits live in the word their waiters watch, and that is what makes the handshakes safe
// without any extra ordering. A waiter sets the bit and then re-validates under the ParkingLot bucket
// lock, while the thread that would wake it clears the bit inside its unpark callback with that same
// lock held. So a waiter either observes the change and declines to park, or is already enqueued and
// gets dequeued. The one requirement is that a waker publishes its state change before unparking,
// which both paths below do.

void ReadWriteLock::readLockSlow(uint32_t observedPhase)
{
    // We are already counted in m_readersIn; all that remains is to wait out the writer whose phase we
    // collided with. Waiting for the phase field to *change* rather than to clear is what bounds this
    // to one writer: the next writer's phase carries a different id, so a queue of writers cannot
    // extend our wait.
    unsigned spinCount = 0;
    unsigned yieldCount = 0;
    for (;;) {
        uint32_t readersIn = m_readersIn.load(std::memory_order_acquire);
        if ((readersIn & s_phaseFieldMask) != observedPhase)
            return;

        if (ReadWriteLockInternal::spinStep(spinCount, ReadWriteLockInternal::readerSpinLimit))
            continue;

        // Give up the core before parking. See readerYieldLimit.
        if (yieldCount < ReadWriteLockInternal::readerYieldLimit) {
            ++yieldCount;
            Thread::yield();
            continue;
        }

        if (!(readersIn & s_hasParkedReadersBit))
            m_readersIn.exchangeOr(s_hasParkedReadersBit, std::memory_order_relaxed);

        ParkingLot::parkConditionally(
            readerParkingAddress(),
            [&]() -> bool {
                uint32_t currentReadersIn = m_readersIn.load(std::memory_order_relaxed);
                return (currentReadersIn & s_phaseFieldMask) == observedPhase && (currentReadersIn & s_hasParkedReadersBit);
            },
            []() { },
            ParkingLot::Time::infinity());
    }
}

void ReadWriteLock::readUnlockSlow()
{
    // We were the reader the draining writer was waiting for.
    ParkingLot::unparkOne(
        drainParkingAddress(),
        [&](ParkingLot::UnparkResult result) -> intptr_t {
            // Only one writer can be draining, so an empty queue means the bit was stale.
            if (!result.mayHaveMoreThreads)
                m_readersOut.exchangeAnd(~s_writerDrainParkedBit, std::memory_order_relaxed);
            return 0;
        });
}

void ReadWriteLock::writeLockSlow(uint32_t target)
{
    // We own the phase, so no more readers can join; wait for the ones already counted to leave.
    //
    // This is an equality test, which relies on m_readersOut never advancing past a target: it counts
    // departures, and the target is the number of joins at the moment we claimed the phase, so the
    // two meet exactly once. Anything that departs without a matching join before that moment would
    // break it, both by satisfying us early and by moving the count past a value we may not have
    // observed yet - which is why tryReadLock() commits rather than joining and backing out.
    unsigned spinCount = 0;
    for (;;) {
        uint32_t readersOut = m_readersOut.load(std::memory_order_acquire);
        if ((readersOut & s_readerCountMask) == target)
            return;

        if (ReadWriteLockInternal::spinStep(spinCount, ReadWriteLockInternal::writerSpinLimit))
            continue;

        // Publish what we are waiting for before advertising that we are waiting, so a reader that
        // observes the bit is guaranteed to see the target too. Both this and a reader's departure are
        // read-modify-writes on m_readersOut, so either the reader sees the bit and wakes us, or we observe
        // its departure above and never park.
        m_drainTarget.store(target, std::memory_order_relaxed);
        m_readersOut.exchangeOr(s_writerDrainParkedBit, std::memory_order_release);

        ParkingLot::parkConditionally(
            drainParkingAddress(),
            [&]() -> bool {
                uint32_t currentReadersOut = m_readersOut.load(std::memory_order_relaxed);
                return (currentReadersOut & s_readerCountMask) != target && (currentReadersOut & s_writerDrainParkedBit);
            },
            []() { },
            ParkingLot::Time::infinity());
    }
}

void ReadWriteLock::writeUnlockSlow()
{
    // Our phase is already over, so every one of these readers can proceed; none of them has to
    // re-examine anything but the phase field, which we have already changed.
    ParkingLot::unparkCount(
        readerParkingAddress(), std::numeric_limits<unsigned>::max(),
        [&](ParkingLot::UnparkResult) -> intptr_t {
            // We asked for every waiter at this address, so the queue is drained. mayHaveMoreThreads
            // is bucket-granular and would only leave the bit stale here.
            m_readersIn.exchangeAnd(~s_hasParkedReadersBit, std::memory_order_relaxed);
            return 0;
        });
}

} // namespace WTF
