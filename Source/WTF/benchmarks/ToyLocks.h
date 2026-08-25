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
    if (!strcmp(what, "sharedmutex") || !strcmp(what, "allrw"))
        Benchmark::template run<SharedMutexRWLock>("std::shared_mutex");
}

} // anonymous namespace

#endif // ToyLocks_h

