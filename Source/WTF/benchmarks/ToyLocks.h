/*
 * Copyright (C) 2015-2017 Apple Inc. All rights reserved.
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

#ifndef ToyLocks_h
#define ToyLocks_h

#include <mutex>
#include <shared_mutex>
#include <thread>
#include <wtf/Atomics.h>
#include <wtf/Lock.h>
#include <wtf/ParkingLot.h>
#include <wtf/ReadWriteLock.h>
#include <wtf/Threading.h>
#include <wtf/WordLock.h>

#if __has_include(<os/lock.h>)
#include <os/lock.h>
#define HAS_UNFAIR_LOCK
#endif

#if defined(EXTRA_LOCKS) && EXTRA_LOCKS
#include <synchronic>
#endif

namespace {

unsigned toyLockSpinLimit = 40;

// Number of threads in each group that write. The rest read. Only the read-write lock
// benchmarks look at this.
unsigned toyLockWritersPerGroup = 1;

// Apple silicon uses 128-byte cache lines, so this is deliberately not 64. Used by the read-write
// locks to keep words that different threads hammer off each other's lines.
constexpr size_t toyLockCacheLineSize = 128;

// Tell the CPU we are in a spin loop, so it can stop speculating and let a sibling thread
// have the pipeline.
inline void spinLoopHint()
{
#if CPU(X86_64)
    asm volatile ("pause");
#elif CPU(ARM64)
    asm volatile ("isb sy");
#endif
}

// This is the old WTF::SpinLock class, included here so that we can still compare our new locks to a
// spinlock baseline.
class YieldSpinLock {
public:
    YieldSpinLock()
    {
        m_lock.store(0, std::memory_order_relaxed);
    }

    void lock()
    {
        while (!m_lock.compareExchangeWeak(0, 1, std::memory_order_acquire))
            Thread::yield();
    }

    void unlock()
    {
        m_lock.store(0, std::memory_order_release);
    }

    bool isLocked() const
    {
        return m_lock.load(std::memory_order_acquire);
    }

private:
    Atomic<unsigned> m_lock;
};

class PauseSpinLock {
public:
    PauseSpinLock()
    {
        m_lock.store(0, std::memory_order_relaxed);
    }

    void lock()
    {
        while (!m_lock.compareExchangeWeak(0, 1, std::memory_order_acquire))
            spinLoopHint();
    }

    void unlock()
    {
        m_lock.store(0, std::memory_order_release);
    }

    bool isLocked() const
    {
        return m_lock.load(std::memory_order_acquire);
    }

private:
    Atomic<unsigned> m_lock;
};

#if defined(EXTRA_LOCKS) && EXTRA_LOCKS
class TransactionalSpinLock {
public:
    TransactionalSpinLock()
    {
        m_lock = 0;
    }

    void lock()
    {
        for (;;) {
            unsigned char result;
            unsigned expected = 0;
            unsigned desired = 1;
            asm volatile (
                "xacquire; lock; cmpxchgl %3, %2\n\t"
                "sete %1"
                : "+a"(expected), "=q"(result), "+m"(m_lock)
                : "r"(desired)
                : "memory");
            if (result)
                return;
            Thread::yield();
        }
    }

    void unlock()
    {
        asm volatile (
            "xrelease; movl $0, %0"
            :
            : "m"(m_lock)
            : "memory");
    }

    bool isLocked() const
    {
        return m_lock;
    }

private:
    unsigned m_lock;
};

class SynchronicLock {
public:
    SynchronicLock()
        : m_locked(0)
    {
    }
    
    void lock()
    {
        for (;;) {
            int state = 0;
            if (m_locked.compare_exchange_weak(state, 1, std::memory_order_acquire))
                return;
            m_sync.wait_for_change(m_locked, state, std::memory_order_relaxed);
        }
    }
    
    void unlock()
    {
        m_sync.notify_one(m_locked, 0, std::memory_order_release);
    }
    
    bool isLocked()
    {
        return m_locked.load();
    }

private:
    std::atomic<int> m_locked;
    std::experimental::synchronic<int> m_sync;
};
#endif

template<typename StateType>
class BargingLock {
public:
    BargingLock()
    {
        m_state.store(0);
    }
    
    void lock()
    {
        if (m_state.compareExchangeWeak(0, isLockedBit, std::memory_order_acquire)) [[likely]]
            return;
        
        lockSlow();
    }
    
    void unlock()
    {
        if (m_state.compareExchangeWeak(isLockedBit, 0, std::memory_order_release)) [[likely]]
            return;
        
        unlockSlow();
    }
    
    bool isLocked() const
    {
        return m_state.load(std::memory_order_acquire) & isLockedBit;
    }
    
private:
    NEVER_INLINE void lockSlow()
    {
        for (unsigned i = toyLockSpinLimit; i--;) {
            StateType currentState = m_state.load();
            
            if (!(currentState & isLockedBit)
                && m_state.compareExchangeWeak(currentState, currentState | isLockedBit))
                return;
            
            if (currentState & hasParkedBit)
                break;
            
            Thread::yield();
        }
        
        for (;;) {
            StateType currentState = m_state.load();
            
            if (!(currentState & isLockedBit)
                && m_state.compareExchangeWeak(currentState, currentState | isLockedBit))
                return;
            
            m_state.compareExchangeWeak(isLockedBit, isLockedBit | hasParkedBit);
            
            ParkingLot::compareAndPark(&m_state, isLockedBit | hasParkedBit);
        }
    }
    
    NEVER_INLINE void unlockSlow()
    {
        ParkingLot::unparkOne(
            &m_state,
            [this] (ParkingLot::UnparkResult result) -> intptr_t {
                if (result.mayHaveMoreThreads)
                    m_state.store(hasParkedBit);
                else
                    m_state.store(0);
                return 0;
            });
    }
    
    static const StateType isLockedBit = 1;
    static const StateType hasParkedBit = 2;
    
    Atomic<StateType> m_state;
};

template<typename StateType>
class ThunderLock {
public:
    ThunderLock()
    {
        m_state.store(Unlocked);
    }
    
    void lock()
    {
        if (m_state.compareExchangeWeak(Unlocked, Locked, std::memory_order_acquire)) [[likely]]
            return;
        
        lockSlow();
    }
    
    void unlock()
    {
        if (m_state.compareExchangeWeak(Locked, Unlocked, std::memory_order_release)) [[likely]]
            return;
        
        unlockSlow();
    }
    
    bool isLocked() const
    {
        return m_state.load(std::memory_order_acquire) != Unlocked;
    }
    
private:
    NEVER_INLINE void lockSlow()
    {
        for (unsigned i = toyLockSpinLimit; i--;) {
            State currentState = m_state.load();
            
            if (currentState == Unlocked
                && m_state.compareExchangeWeak(Unlocked, Locked))
                return;
            
            if (currentState == LockedAndParked)
                break;
            
            Thread::yield();
        }
        
        for (;;) {
            if (m_state.compareExchangeWeak(Unlocked, Locked))
                return;
            
            m_state.compareExchangeWeak(Locked, LockedAndParked);
            ParkingLot::compareAndPark(&m_state, LockedAndParked);
        }
    }
    
    NEVER_INLINE void unlockSlow()
    {
        if (m_state.exchange(Unlocked) == LockedAndParked)
            ParkingLot::unparkAll(&m_state);
    }
    
    enum State : StateType {
        Unlocked,
        Locked,
        LockedAndParked
    };
    
    Atomic<State> m_state;
};

template<typename StateType>
class CascadeLock {
public:
    CascadeLock()
    {
        m_state.store(Unlocked);
    }
    
    void lock()
    {
        if (m_state.compareExchangeWeak(Unlocked, Locked, std::memory_order_acquire)) [[likely]]
            return;
        
        lockSlow();
    }
    
    void unlock()
    {
        if (m_state.compareExchangeWeak(Locked, Unlocked, std::memory_order_release)) [[likely]]
            return;
        
        unlockSlow();
    }
    
    bool isLocked() const
    {
        return m_state.load(std::memory_order_acquire) != Unlocked;
    }
    
private:
    NEVER_INLINE void lockSlow()
    {
        for (unsigned i = toyLockSpinLimit; i--;) {
            State currentState = m_state.load();
            
            if (currentState == Unlocked
                && m_state.compareExchangeWeak(Unlocked, Locked))
                return;
            
            if (currentState == LockedAndParked)
                break;
            
            Thread::yield();
        }
        
        State desiredState = Locked;
        for (;;) {
            if (m_state.compareExchangeWeak(Unlocked, desiredState))
                return;
            
            desiredState = LockedAndParked;
            m_state.compareExchangeWeak(Locked, LockedAndParked);
            ParkingLot::compareAndPark(&m_state, LockedAndParked);
        }
    }
    
    NEVER_INLINE void unlockSlow()
    {
        if (m_state.exchange(Unlocked) == LockedAndParked)
            ParkingLot::unparkOne(&m_state);
    }
    
    enum State : StateType {
        Unlocked,
        Locked,
        LockedAndParked
    };
    
    Atomic<State> m_state;
};

class HandoffLock {
public:
    HandoffLock()
    {
        m_state.store(0);
    }
    
    void lock()
    {
        if (m_state.compareExchangeWeak(0, isLockedBit, std::memory_order_acquire)) [[likely]]
            return;

        lockSlow();
    }

    void unlock()
    {
        if (m_state.compareExchangeWeak(isLockedBit, 0, std::memory_order_release)) [[likely]]
            return;

        unlockSlow();
    }

    bool isLocked() const
    {
        return m_state.load(std::memory_order_acquire) & isLockedBit;
    }
    
private:
    NEVER_INLINE void lockSlow()
    {
        for (;;) {
            unsigned state = m_state.load();
            
            if (!(state & isLockedBit)) {
                if (m_state.compareExchangeWeak(state, state | isLockedBit))
                    return;
                continue;
            }
            
            if (m_state.compareExchangeWeak(state, state + parkedCountUnit)) {
                bool result = ParkingLot::compareAndPark(&m_state, state + parkedCountUnit).wasUnparked;
                m_state.exchangeAdd(-parkedCountUnit);
                if (result)
                    return;
            }
        }
    }
    
    NEVER_INLINE void unlockSlow()
    {
        for (;;) {
            unsigned state = m_state.load();
            
            if (!(state >> parkedCountShift)) {
                RELEASE_ASSERT(state == isLockedBit);
                if (m_state.compareExchangeWeak(isLockedBit, 0))
                    return;
                continue;
            }
            
            if (ParkingLot::unparkOne(&m_state).unparkedCount) {
                // We unparked someone. There are now running and they hold the lock.
                return;
            }
            
            // Nobody unparked. Maybe there isn't anyone waiting. Just try again.
        }
    }
    
    static const unsigned isLockedBit = 1;
    static const unsigned parkedCountShift = 1;
    static const unsigned parkedCountUnit = 1 << parkedCountShift;
    
    Atomic<unsigned> m_state;
};

// Read-write locks. These have a different interface from the exclusive locks above, and are
// driven by their own benchmark, so they are dispatched by runEverythingRW() rather than
// runEverything().

// A phase-fair reader-writer lock, ported from the Rust `pflock` crate, which implements the
// design in Brandenburg and Anderson, "Spin-Based Reader-Writer Synchronization for
// Multiprocessor Real-Time Systems". Readers and writers alternate phases: a writer arriving
// during a read phase blocks readers that arrive after it, and a reader arriving during a write
// phase waits only for that one writer, not for any writers queued behind it.
//
// The rin word holds a reader count in its high bits plus two low bits describing the current
// write phase, so a reader learns whether a writer is present and which phase it is in with a
// single fetch-and-add. Readers then spin until either of those two bits changes, which is what
// bounds their wait to one writer.
//
// This spins rather than parking, so unlike the crate's version the accesses here carry the
// acquire and release ordering a lock needs. The original is relaxed throughout, which would
// let the critical section leak out of the lock on ARM64 and would not be comparable against
// locks that order correctly.
class PFLock {
public:
    void readLock()
    {
        // Take a reader ticket and read the current write phase.
        size_t phase = m_rin.exchangeAdd(readerIncrement, std::memory_order_acquire) & writerMask;

        // If a writer is present, wait for either the present bit or the phase bit to flip.
        // Writers queued behind that one cannot flip these bits, so this waits for one writer.
        while (phase && phase == (m_rin.load(std::memory_order_acquire) & writerMask))
            spinLoopHint();
    }

    void readUnlock()
    {
        m_rout.exchangeAdd(readerIncrement, std::memory_order_release);
    }

    void writeLock()
    {
        // Wait for our turn among writers.
        size_t writerTicket = m_win.exchangeAdd(1, std::memory_order_relaxed);
        while (writerTicket != m_wout.load(std::memory_order_acquire))
            spinLoopHint();

        // Announce ourselves in rin, which stops readers arriving after this point, and learn
        // how many readers came before us.
        size_t phase = writerPresentBit | (writerTicket & phaseIdBit);
        size_t readerTicket = m_rin.exchangeAdd(phase, std::memory_order_acquire);

        // Wait for exactly those readers to leave.
        while (readerTicket != m_rout.load(std::memory_order_acquire))
            spinLoopHint();
    }

    void writeUnlock()
    {
        // Clear the phase bits, releasing the readers that queued behind us.
        m_rin.exchangeAnd(phaseClearMask, std::memory_order_release);

        // Let the next writer take its turn. Only one writer is ever here.
        m_wout.exchangeAdd(1, std::memory_order_release);
    }

private:
    static constexpr size_t readerIncrement = 0x100;
    static constexpr size_t writerMask = 0x3;
    static constexpr size_t writerPresentBit = 0x2;
    static constexpr size_t phaseIdBit = 0x1;
    static constexpr size_t phaseClearMask = ~static_cast<size_t>(0xFF);

    Atomic<size_t> m_rin { 0 };
    Atomic<size_t> m_rout { 0 };
    Atomic<size_t> m_win { 0 };
    Atomic<size_t> m_wout { 0 };
};

// A phase-fair reader-writer lock with ticket-ordered writers, after Karl Malbrain's rwfutex. The
// original blocks on futexes; this parks on ParkingLot instead, and steals a bit in each word to
// record whether anyone is parked waiting on that word, so an uncontended unlock never calls into
// ParkingLot at all - which is the whole point, since a futex wake is a syscall.
//
// Four words, each with its own wait queue keyed on its own address:
//   m_rin      readers that have entered, plus the write-phase flags that readers watch
//   m_rout     readers that have left; a writer draining readers watches this
//   m_ticket   next writer ticket to hand out
//   m_serving  the ticket currently allowed to write; queued writers watch this
//
// Keeping each queue's parked bit in the word its waiters watch is what makes the handshake safe
// without any extra ordering: a waiter sets the bit and then re-validates, and the waker clears it
// inside the unpark callback with the bucket lock held, so a waiter either observes the clear and
// declines to park, or is already enqueued and gets dequeued.
//
// Readers join with a single unconditional fetch-add that can never fail, so concurrent readers never
// retry against each other - the property a single reader count cannot have. Note that a *waiting*
// reader has already counted itself into m_rin, which is why "m_rout has caught up with m_rin" is the
// condition for a reader phase being completely finished.
//
// Phase fairness: a reader waits only for the one writer currently in its phase, never for writers
// queued behind it, and a writer releasing the lock hands the next phase to the readers if any are
// waiting, leaving the last of those readers to hand it back to the writer queue.
class PFTLock {
public:
    void readLock()
    {
        uint32_t phase = m_rin.exchangeAdd(readerUnit, std::memory_order_acquire) & phaseMask;
        if (!phase)
            return;
        waitForPhaseChange(phase);
    }

    void readUnlock()
    {
        uint32_t previous = m_rout.exchangeAdd(readerUnit, std::memory_order_release);

        // The only thread that can be waiting on us is a writer draining readers, and its parked bit
        // lives in the word we just incremented, so an uncontended unlock is exactly one RMW with no
        // further loads and no ParkingLot call. When one is waiting, wake it only once its count is
        // actually reached: the original wakes it on every reader exit, which costs the writer a
        // park/unpark round trip per reader rather than one per acquisition.
        if (previous & writerDrainParkedBit) {
            if (((previous + readerUnit) & readerCountMask) == m_drainTarget.load(std::memory_order_acquire))
                wakeDrainingWriter();
        }
    }

    void writeLock()
    {
        uint32_t ticket = m_ticket.exchangeAdd(ticketUnit, std::memory_order_relaxed);
        waitForTurn(ticket);

        // Announce the write phase, which blocks readers arriving from here on, and learn how many
        // readers got in before us. The phase id alternates per ticket so that a reader can tell one
        // writer's phase from the next.
        uint32_t phase = writerPresentBit | ((ticket / ticketUnit) & phaseIdBit);
        uint32_t enteredReaders = m_rin.exchangeAdd(phase, std::memory_order_acquire);
        waitForReaders(enteredReaders);
    }

    void writeUnlock()
    {
        // End our phase, which releases the readers that queued behind us, then advance the ticket.
        uint32_t rin = m_rin.exchangeAnd(~phaseMask, std::memory_order_release);
        uint32_t serving = m_serving.exchangeAdd(ticketUnit, std::memory_order_release);

        // Wake both queues. The original defers the writer wake, handing readers a whole phase and
        // leaving the last reader out to wake the writer queue; that needs the readers to check the
        // ticket on every unlock, and it goes badly wrong if the check is narrowed to the last reader,
        // since a waiting reader has already counted itself into m_rin and so the reader count rarely
        // drains to zero. Waking the next writer here instead is still phase-fair - it will set its
        // phase bits and block readers arriving after it, while waiting only for the readers already
        // counted in - and it keeps readUnlock down to a single RMW.
        if (rin & readersParkedBit)
            wakeReaders();
        if (serving & writersParkedBit)
            wakeQueuedWriters();
    }

private:
    // m_rin and m_rout count readers above the flag byte, as in the original, so the flags below can
    // grow without disturbing the counts.
    static constexpr uint32_t phaseIdBit = 1;
    static constexpr uint32_t writerPresentBit = 2;
    static constexpr uint32_t phaseMask = phaseIdBit | writerPresentBit;
    static constexpr uint32_t readersParkedBit = 4;
    static constexpr uint32_t writerDrainParkedBit = 1;
    static constexpr uint32_t writersParkedBit = 1;
    static constexpr uint32_t readerUnit = 1 << 8;
    static constexpr uint32_t readerCountMask = ~static_cast<uint32_t>(0xFF);
    // Two, so that the ticket counters step over writersParkedBit in m_serving.
    static constexpr uint32_t ticketUnit = 2;

    NEVER_INLINE void waitForPhaseChange(uint32_t phase)
    {
        unsigned spinCount = 0;
        for (;;) {
            uint32_t rin = m_rin.load(std::memory_order_acquire);
            if ((rin & phaseMask) != phase)
                return;
            if (spinCount < toyLockSpinLimit) {
                ++spinCount;
                spinLoopHint();
                continue;
            }
            if (!(rin & readersParkedBit))
                m_rin.exchangeOr(readersParkedBit, std::memory_order_relaxed);
            ParkingLot::parkConditionally(
                &m_rin,
                [&]() -> bool {
                    uint32_t current = m_rin.load(std::memory_order_relaxed);
                    return (current & phaseMask) == phase && (current & readersParkedBit);
                },
                []() { }, ParkingLot::Time::infinity());
        }
    }

    NEVER_INLINE void waitForTurn(uint32_t ticket)
    {
        unsigned spinCount = 0;
        for (;;) {
            uint32_t serving = m_serving.load(std::memory_order_acquire);
            if ((serving & ~writersParkedBit) == ticket)
                return;
            if (spinCount < toyLockSpinLimit) {
                ++spinCount;
                spinLoopHint();
                continue;
            }
            if (!(serving & writersParkedBit))
                m_serving.exchangeOr(writersParkedBit, std::memory_order_relaxed);
            ParkingLot::parkConditionally(
                &m_serving,
                [&]() -> bool {
                    uint32_t current = m_serving.load(std::memory_order_relaxed);
                    return (current & ~writersParkedBit) != ticket && (current & writersParkedBit);
                },
                []() { }, ParkingLot::Time::infinity());
        }
    }

    NEVER_INLINE void waitForReaders(uint32_t enteredReaders)
    {
        unsigned spinCount = 0;
        uint32_t target = enteredReaders & readerCountMask;
        for (;;) {
            uint32_t rout = m_rout.load(std::memory_order_acquire);
            if ((rout & readerCountMask) == target)
                return;
            if (spinCount < toyLockSpinLimit) {
                ++spinCount;
                spinLoopHint();
                continue;
            }
            // Publish what we are waiting for before advertising that we are waiting, so a reader that
            // observes the bit is guaranteed to see the target as well. Only one writer can be draining
            // at a time, since the ticket serialises them, so this needs no other protection. Both this
            // and the reader's decrement are RMWs on m_rout, so either the reader sees the bit and
            // wakes us, or we see its decrement below and never park.
            m_drainTarget.store(target, std::memory_order_relaxed);
            m_rout.exchangeOr(writerDrainParkedBit, std::memory_order_release);

            ParkingLot::parkConditionally(
                &m_rout,
                [&]() -> bool {
                    uint32_t current = m_rout.load(std::memory_order_relaxed);
                    return (current & readerCountMask) != target && (current & writerDrainParkedBit);
                },
                []() { }, ParkingLot::Time::infinity());
        }
    }

    NEVER_INLINE void wakeDrainingWriter()
    {
        ParkingLot::unparkOne(
            &m_rout,
            [&](ParkingLot::UnparkResult result) -> intptr_t {
                if (!result.mayHaveMoreThreads)
                    m_rout.exchangeAnd(~writerDrainParkedBit, std::memory_order_relaxed);
                return 0;
            });
    }

    NEVER_INLINE void wakeReaders()
    {
        ParkingLot::unparkCount(
            &m_rin, UINT32_MAX,
            [&](ParkingLot::UnparkResult) -> intptr_t {
                m_rin.exchangeAnd(~readersParkedBit, std::memory_order_relaxed);
                return 0;
            });
    }

    // Wakes every queued writer, since only the one holding the current ticket can proceed and the
    // rest have to re-check and re-park. The original does the same with FUTEX_WAKE_BITSET INT_MAX.
    NEVER_INLINE void wakeQueuedWriters()
    {
        ParkingLot::unparkCount(
            &m_serving, UINT32_MAX,
            [&](ParkingLot::UnparkResult) -> intptr_t {
                m_serving.exchangeAnd(~writersParkedBit, std::memory_order_relaxed);
                return 0;
            });
    }

    // Each of these is hammered by a different set of threads, so they get their own lines: readers
    // read-modify-write m_rin to join and m_rout to leave, and writers own everything else. Packed
    // together they were one line, which serialised all of it.
    alignas(toyLockCacheLineSize) Atomic<uint32_t> m_rin { 0 };
    alignas(toyLockCacheLineSize) Atomic<uint32_t> m_rout { 0 };
    alignas(toyLockCacheLineSize) Atomic<uint32_t> m_ticket { 0 };
    Atomic<uint32_t> m_serving { 0 };
    // The m_rout count the draining writer is waiting for. Only meaningful while
    // writerDrainParkedBit is set in m_rout.
    Atomic<uint32_t> m_drainTarget { 0 };
};

// PF-T with the strict writer ordering replaced by a barging, eventually fair writer mutex.
//
// PFTLock's ticket/serving pair means a writer can do nothing until its turn comes up even when the
// lock is free, so every acquisition costs a park/unpark. The obvious replacement - have writers
// compete for the write phase by compare-and-swapping PRES into m_rin - measured far worse, because
// m_rin is the word readers hammer with their joins and departures, so the writer's compare-and-swap
// loses that race repeatedly. Its acquire latency got *worse* as writes got rarer (2.5us at no sleep
// up to 21.9us at 1ms), since rarer writes mean livelier readers.
//
// So writer exclusion lives in its own word instead, where readers never touch it, and WTF::Lock is
// exactly the right thing: barging with ParkingLot's eventual fairness. Claiming the phase then goes
// back to being the unconditional fetch-add that PFT uses and cannot fail.
//
// Holding the writer mutex across the release is also what "reserve the next phase" degenerates to,
// so the phaseReservedBit machinery from the compare-and-swap version is gone. Phase fairness still
// holds: the readers we release on unlock have already counted themselves into m_rin, so a successor
// writer's drain target includes them and it must wait them out before entering.
class PFBLock {
public:
    void readLock()
    {
        uint32_t previous = m_rin.exchangeAdd(readerUnit, std::memory_order_acquire);
        if (previous & writerPresentBit)
            waitForPhaseChange(previous & readerWaitMask);
    }

    void readUnlock()
    {
        uint32_t previous = m_rout.exchangeAdd(readerUnit, std::memory_order_release);
        if (previous & writerDrainParkedBit) {
            if (((previous + readerUnit) & readerCountMask) == m_drainTarget.load(std::memory_order_acquire))
                wakeDrainingWriter();
        }
    }

    void writeLock()
    {
        m_writerLock.lock();
        // Only the holder of m_writerLock touches this, so it needs no atomicity of its own. The id
        // has to differ from the previous phase's, or a reader waiting on that phase would not notice
        // the change; four bits gives sixteen phases before a value repeats.
        m_phaseId = (m_phaseId + 1) & phaseIdCount;
        // The phase field is zero here, because the previous writer cleared it and no other writer can
        // be between these two points, so adding is safe and cannot carry into the parked bit.
        uint32_t phase = writerPresentBit | (m_phaseId << phaseIdShift);
        uint32_t enteredReaders = m_rin.exchangeAdd(phase, std::memory_order_acquire);
        waitForReaders(enteredReaders & readerCountMask);
    }

    void writeUnlock()
    {
        uint32_t rin = m_rin.exchangeAnd(~phaseFieldMask, std::memory_order_release);
        if (rin & readersParkedBit)
            wakeReaders();
        m_writerLock.unlock();
    }

private:
    static constexpr uint32_t writerPresentBit = 1 << 0;
    static constexpr uint32_t readersParkedBit = 1 << 1;
    static constexpr uint32_t phaseIdShift = 4;
    static constexpr uint32_t phaseIdMask = 0xF0;
    static constexpr uint32_t phaseIdCount = 0xF;
    // The whole phase field, cleared on release. Deliberately excludes readersParkedBit.
    static constexpr uint32_t phaseFieldMask = writerPresentBit | phaseIdMask;
    // What a reader compares to decide whether the phase it was blocked by is over.
    static constexpr uint32_t readerWaitMask = phaseFieldMask;
    static constexpr uint32_t readerUnit = 1 << 8;
    static constexpr uint32_t readerCountMask = ~static_cast<uint32_t>(0xFF);
    static constexpr uint32_t writerDrainParkedBit = 1 << 0;

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN
    void* readerParkingAddress() { return std::bit_cast<uint8_t*>(this); }
    void* drainParkingAddress() { return std::bit_cast<uint8_t*>(this) + 1; }
WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

    NEVER_INLINE void waitForPhaseChange(uint32_t observed)
    {
        unsigned spinCount = 0;
        for (;;) {
            uint32_t rin = m_rin.load(std::memory_order_acquire);
            if ((rin & readerWaitMask) != observed)
                return;
            if (spinCount < toyLockSpinLimit) {
                ++spinCount;
                spinLoopHint();
                continue;
            }
            if (!(rin & readersParkedBit))
                m_rin.exchangeOr(readersParkedBit, std::memory_order_relaxed);
            ParkingLot::parkConditionally(
                readerParkingAddress(),
                [&]() -> bool {
                    uint32_t current = m_rin.load(std::memory_order_relaxed);
                    return (current & readerWaitMask) == observed && (current & readersParkedBit);
                },
                []() { }, ParkingLot::Time::infinity());
        }
    }

    NEVER_INLINE void waitForReaders(uint32_t target)
    {
        unsigned spinCount = 0;
        for (;;) {
            uint32_t rout = m_rout.load(std::memory_order_acquire);
            if ((rout & readerCountMask) == target)
                return;
            if (spinCount < toyLockSpinLimit) {
                ++spinCount;
                spinLoopHint();
                continue;
            }
            // Publish the target before advertising that we are waiting, so a reader that observes the
            // bit is guaranteed to see the target too. Only the writer holding m_writerLock drains.
            m_drainTarget.store(target, std::memory_order_relaxed);
            m_rout.exchangeOr(writerDrainParkedBit, std::memory_order_release);
            ParkingLot::parkConditionally(
                drainParkingAddress(),
                [&]() -> bool {
                    uint32_t current = m_rout.load(std::memory_order_relaxed);
                    return (current & readerCountMask) != target && (current & writerDrainParkedBit);
                },
                []() { }, ParkingLot::Time::infinity());
        }
    }

    NEVER_INLINE void wakeReaders()
    {
        ParkingLot::unparkCount(
            readerParkingAddress(), UINT32_MAX,
            [&](ParkingLot::UnparkResult) -> intptr_t {
                m_rin.exchangeAnd(~readersParkedBit, std::memory_order_relaxed);
                return 0;
            });
    }

    NEVER_INLINE void wakeDrainingWriter()
    {
        ParkingLot::unparkOne(
            drainParkingAddress(),
            [&](ParkingLot::UnparkResult result) -> intptr_t {
                if (!result.mayHaveMoreThreads)
                    m_rout.exchangeAnd(~writerDrainParkedBit, std::memory_order_relaxed);
                return 0;
            });
    }

    // Own cache lines: readers read-modify-write m_rin to join and m_rout to leave, and writers own
    // the rest. Packed together they were a single line, which serialised all of it.
    alignas(toyLockCacheLineSize) Atomic<uint32_t> m_rin { 0 };
    alignas(toyLockCacheLineSize) Atomic<uint32_t> m_rout { 0 };
    alignas(toyLockCacheLineSize) Atomic<uint32_t> m_drainTarget { 0 };
    // Barging with eventual fairness, which is all writer exclusion needs, and it keeps writers off
    // the word the readers are using.
    Lock m_writerLock;
    uint32_t m_phaseId { 0 };
};


// Adapts std::shared_mutex to the interface the read-write benchmark uses.
class SharedMutexRWLock {
public:
    void readLock() { m_lock.lock_shared(); }
    void readUnlock() { m_lock.unlock_shared(); }
    void writeLock() { m_lock.lock(); }
    void writeUnlock() { m_lock.unlock(); }

private:
    std::shared_mutex m_lock;
};

#ifdef HAS_UNFAIR_LOCK
class UnfairLock {
    os_unfair_lock l = OS_UNFAIR_LOCK_INIT;
public:
    void lock()
    {
        os_unfair_lock_lock(&l);
    }
    void unlock()
    {
        os_unfair_lock_unlock(&l);
    }
};
#endif

template<typename Benchmark>
void runEverything(const char* what)
{
    if (!strcmp(what, "yieldspinlock") || !strcmp(what, "all"))
        Benchmark::template run<YieldSpinLock>("YieldSpinLock");
    if (!strcmp(what, "pausespinlock") || !strcmp(what, "all"))
        Benchmark::template run<PauseSpinLock>("PauseSpinLock");
#if defined(EXTRA_LOCKS) && EXTRA_LOCKS
    if (!strcmp(what, "transactionalspinlock") || !strcmp(what, "all"))
        Benchmark::template run<TransactionalSpinLock>("TransactionalSpinLock");
    if (!strcmp(what, "synchroniclock") || !strcmp(what, "all"))
        Benchmark::template run<SynchronicLock>("SynchronicLock");
#endif
    if (!strcmp(what, "wordlock") || !strcmp(what, "all"))
        Benchmark::template run<WordLock>("WTFWordLock");
    if (!strcmp(what, "lock") || !strcmp(what, "all"))
        Benchmark::template run<Lock>("WTFLock");
    if (!strcmp(what, "barginglock") || !strcmp(what, "all"))
        Benchmark::template run<BargingLock<uint8_t>>("ByteBargingLock");
    if (!strcmp(what, "bargingwordlock") || !strcmp(what, "all"))
        Benchmark::template run<BargingLock<uint32_t>>("WordBargingLock");
    if (!strcmp(what, "thunderlock") || !strcmp(what, "all"))
        Benchmark::template run<ThunderLock<uint8_t>>("ByteThunderLock");
    if (!strcmp(what, "thunderwordlock") || !strcmp(what, "all"))
        Benchmark::template run<ThunderLock<uint32_t>>("WordThunderLock");
    if (!strcmp(what, "cascadelock") || !strcmp(what, "all"))
        Benchmark::template run<CascadeLock<uint8_t>>("ByteCascadeLock");
    if (!strcmp(what, "cascadewordlock") || !strcmp(what, "all"))
        Benchmark::template run<CascadeLock<uint32_t>>("WordCascadeLock");
    if (!strcmp(what, "handofflock") || !strcmp(what, "all"))
        Benchmark::template run<HandoffLock>("HandoffLock");
#ifdef HAS_UNFAIR_LOCK
    if (!strcmp(what, "unfairlock") || !strcmp(what, "all"))
        Benchmark::template run<UnfairLock>("UnfairLock");
#endif
    if (!strcmp(what, "mutex") || !strcmp(what, "all"))
        Benchmark::template run<std::mutex>("std::mutex");
}

// The read-write locks are driven by a benchmark that mixes readers and writers, so they get
// their own dispatcher. The names here are disjoint from the ones above, which lets both
// dispatchers be called with the same argument.
template<typename Benchmark>
void runEverythingRW(const char* what)
{
    if (!strcmp(what, "readwritelock") || !strcmp(what, "allrw"))
        Benchmark::template run<ReadWriteLock>("WTFReadWriteLock");
    if (!strcmp(what, "pflock") || !strcmp(what, "allrw"))
        Benchmark::template run<PFLock>("PFLock");
    if (!strcmp(what, "pftlock") || !strcmp(what, "allrw"))
        Benchmark::template run<PFTLock>("PFTLock");
    if (!strcmp(what, "pfblock") || !strcmp(what, "allrw"))
        Benchmark::template run<PFBLock>("PFBLock");
    if (!strcmp(what, "sharedmutex") || !strcmp(what, "allrw"))
        Benchmark::template run<SharedMutexRWLock>("std::shared_mutex");
}

} // anonymous namespace

#endif // ToyLocks_h

