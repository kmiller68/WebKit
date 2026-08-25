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
#include <limits>
#include <wtf/simde/simde.h>

namespace WTF {

// Wrapped in a namespace because this file is part of a unified source, where file-scope names would
// collide with those of the other files it is compiled with.
namespace ReadWriteLockInternal {

// Both waits here are for something short: a reader waits out one writer's critical section, and a
// writer waits for the readers already inside to finish theirs. So both spin briefly before parking
// rather than either parking immediately or spinning at length.
//
// These values are not the product of the search recorded in ReadWriteLockSpinTuning.md. That was
// done against a previous design whose reader admission was conditional, where the tuning interacted
// with reader starvation and mattered a great deal; here it is far less sensitive, because readers
// are only ever blocked by a writer that actually owns the phase. They are the values the benchmark
// used when measuring this design.
constexpr unsigned spinLimit = 40;
// simde_mm_pause() is isb on ARM64, a full instruction-synchronization barrier and so much more
// expensive than x86's pause; one per iteration is plenty.
constexpr unsigned pauseCount = 1;

// One iteration of a spin loop. Returns false once the caller has spun long enough and should park.
// Spin on plain loads at the call site: read-modify-write spinning ping-pongs the word between cores,
// which hurts exactly the contended case this has to survive.
ALWAYS_INLINE bool spinStep(unsigned& spinCount)
{
    if (spinCount >= spinLimit)
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
    // We are already counted in m_rin; all that remains is to wait out the writer whose phase we
    // collided with. Waiting for the phase field to *change* rather than to clear is what bounds this
    // to one writer: the next writer's phase carries a different id, so a queue of writers cannot
    // extend our wait.
    unsigned spinCount = 0;
    for (;;) {
        uint32_t rin = m_rin.load(std::memory_order_acquire);
        if ((rin & PhaseFieldMask) != observedPhase)
            return;

        if (ReadWriteLockInternal::spinStep(spinCount))
            continue;

        if (!(rin & HasParkedReadersBit))
            m_rin.exchangeOr(HasParkedReadersBit, std::memory_order_relaxed);

        ParkingLot::parkConditionally(
            readerParkingAddress(),
            [&]() -> bool {
                uint32_t currentRin = m_rin.load(std::memory_order_relaxed);
                return (currentRin & PhaseFieldMask) == observedPhase && (currentRin & HasParkedReadersBit);
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
                m_rout.exchangeAnd(~WriterDrainParkedBit, std::memory_order_relaxed);
            return 0;
        });
}

void ReadWriteLock::writeLockSlow(uint32_t target)
{
    // We own the phase, so no more readers can join; wait for the ones already counted to leave.
    //
    // This is an equality test, which relies on m_rout never advancing past a target: it counts
    // departures, and the target is the number of joins at the moment we claimed the phase, so the
    // two meet exactly once. Anything that departs without a matching join before that moment would
    // break it, both by satisfying us early and by moving the count past a value we may not have
    // observed yet - which is why tryReadLock() commits rather than joining and backing out.
    unsigned spinCount = 0;
    for (;;) {
        uint32_t rout = m_rout.load(std::memory_order_acquire);
        if ((rout & ReaderCountMask) == target)
            return;

        if (ReadWriteLockInternal::spinStep(spinCount))
            continue;

        // Publish what we are waiting for before advertising that we are waiting, so a reader that
        // observes the bit is guaranteed to see the target too. Both this and a reader's departure are
        // read-modify-writes on m_rout, so either the reader sees the bit and wakes us, or we observe
        // its departure above and never park.
        m_drainTarget.store(target, std::memory_order_relaxed);
        m_rout.exchangeOr(WriterDrainParkedBit, std::memory_order_release);

        ParkingLot::parkConditionally(
            drainParkingAddress(),
            [&]() -> bool {
                uint32_t currentRout = m_rout.load(std::memory_order_relaxed);
                return (currentRout & ReaderCountMask) != target && (currentRout & WriterDrainParkedBit);
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
            m_rin.exchangeAnd(~HasParkedReadersBit, std::memory_order_relaxed);
            return 0;
        });
}

} // namespace WTF
