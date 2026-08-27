/*
 * Copyright (C) 2026 Apple Inc. All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"

#include <wtf/Atomics.h>
#include <wtf/Lock.h>
#include <wtf/MonotonicTime.h>
#include <wtf/ReadWriteLock.h>
#include <wtf/Threading.h>
#include <wtf/Vector.h>
#include <array>

namespace TestWebKitAPI {

namespace {

constexpr Seconds testTimeout = 30_s;

// Every test here can hang rather than fail if the lock is broken, so each one runs its
// threads with a deadline and reports how far each thread got. A hang shows up as a
// failed expectation on the per-thread progress counters instead of a stuck test binary.
class DeadlineGuard {
public:
    bool expired() const { return MonotonicTime::now() > m_deadline; }

private:
    MonotonicTime m_deadline { MonotonicTime::now() + testTimeout };
};

template<typename Body>
Vector<Ref<Thread>> spawn(unsigned count, Body body)
{
    Vector<Ref<Thread>> threads;
    for (unsigned i = 0; i < count; ++i) {
        threads.append(Thread::create("ReadWriteLock test"_s, [i, body] {
            body(i);
        }));
    }
    return threads;
}

void join(Vector<Ref<Thread>>& threads)
{
    for (auto& thread : threads)
        thread->waitForCompletion();
}

} // anonymous namespace

// Three or more writers contending is the case that livelocked the previous
// implementation: every writer spun forever at 100% CPU with the lock free.
TEST(WTF_ReadWriteLock, ManyWritersMakeProgress)
{
    constexpr unsigned writerCount = 8;
    constexpr unsigned acquisitionsPerWriter = 1000;

    ReadWriteLock lock;
    DeadlineGuard deadline;
    unsigned counter = 0;
    std::array<Atomic<unsigned>, writerCount> progress { };

    auto threads = spawn(writerCount, [&](unsigned index) {
        for (unsigned i = 0; i < acquisitionsPerWriter && !deadline.expired(); ++i) {
            Locker locker { lock.write() };
            ++counter;
            progress[index].store(i + 1, std::memory_order_relaxed);
        }
    });
    join(threads);

    for (unsigned i = 0; i < writerCount; ++i)
        EXPECT_EQ(acquisitionsPerWriter, progress[i].load(std::memory_order_relaxed));
    EXPECT_EQ(writerCount * acquisitionsPerWriter, counter);
    EXPECT_TRUE(lock.isQuiescentForTesting());
}

// A stream of writers must not starve readers. The previous implementation checked for
// parked writers first and unconditionally, so with several writers always queued the
// reader wake path was never reached.
TEST(WTF_ReadWriteLock, WritersDoNotStarveReaders)
{
    constexpr unsigned writerCount = 4;
    constexpr unsigned readerCount = 4;
    constexpr unsigned readsPerReader = 100;

    ReadWriteLock lock;
    DeadlineGuard deadline;
    Atomic<bool> readersDone { false };
    unsigned counter = 0;
    std::array<Atomic<unsigned>, readerCount> readsCompleted { };

    auto writers = spawn(writerCount, [&](unsigned) {
        while (!readersDone.load(std::memory_order_relaxed) && !deadline.expired()) {
            Locker locker { lock.write() };
            ++counter;
        }
    });

    auto readers = spawn(readerCount, [&](unsigned index) {
        for (unsigned i = 0; i < readsPerReader && !deadline.expired(); ++i) {
            Locker locker { lock.read() };
            readsCompleted[index].store(i + 1, std::memory_order_relaxed);
        }
    });

    join(readers);
    readersDone.store(true, std::memory_order_relaxed);
    join(writers);

    for (unsigned i = 0; i < readerCount; ++i)
        EXPECT_EQ(readsPerReader, readsCompleted[i].load(std::memory_order_relaxed));
    EXPECT_TRUE(lock.isQuiescentForTesting());
}

// The mirror case: a stream of readers must not starve a writer. A writer registers
// itself before inspecting the lock, which holds new readers out.
TEST(WTF_ReadWriteLock, ReadersDoNotStarveWriter)
{
    constexpr unsigned readerCount = 4;
    constexpr unsigned writesToComplete = 100;

    ReadWriteLock lock;
    DeadlineGuard deadline;
    Atomic<bool> writerDone { false };
    Atomic<unsigned> writesCompleted { 0 };

    auto readers = spawn(readerCount, [&](unsigned) {
        while (!writerDone.load(std::memory_order_relaxed) && !deadline.expired()) {
            Locker locker { lock.read() };
        }
    });

    auto writer = spawn(1, [&](unsigned) {
        for (unsigned i = 0; i < writesToComplete && !deadline.expired(); ++i) {
            Locker locker { lock.write() };
            writesCompleted.store(i + 1, std::memory_order_relaxed);
        }
    });

    join(writer);
    writerDone.store(true, std::memory_order_relaxed);
    join(readers);

    EXPECT_EQ(writesToComplete, writesCompleted.load(std::memory_order_relaxed));
    EXPECT_TRUE(lock.isQuiescentForTesting());
}

// Several writers plus readers, so that the writer-to-writer direct handoff path runs
// with other writers still parked behind it. That handoff hands ownership over without
// the woken writer running its own acquire path, so its bookkeeping is the easiest thing
// to get wrong; if it leaks, a later reader or writer hangs.
TEST(WTF_ReadWriteLock, WriterHandoffWithWritersAndReadersQueued)
{
    constexpr unsigned writerCount = 6;
    constexpr unsigned readerCount = 6;
    constexpr unsigned iterations = 500;

    ReadWriteLock lock;
    DeadlineGuard deadline;
    unsigned counter = 0;
    std::array<Atomic<unsigned>, writerCount + readerCount> progress { };

    auto writers = spawn(writerCount, [&](unsigned index) {
        for (unsigned i = 0; i < iterations && !deadline.expired(); ++i) {
            Locker locker { lock.write() };
            ++counter;
            progress[index].store(i + 1, std::memory_order_relaxed);
        }
    });

    auto readers = spawn(readerCount, [&](unsigned index) {
        for (unsigned i = 0; i < iterations && !deadline.expired(); ++i) {
            Locker locker { lock.read() };
            progress[writerCount + index].store(i + 1, std::memory_order_relaxed);
        }
    });

    join(writers);
    join(readers);

    for (unsigned i = 0; i < writerCount + readerCount; ++i)
        EXPECT_EQ(iterations, progress[i].load(std::memory_order_relaxed));
    EXPECT_EQ(writerCount * iterations, counter);
    EXPECT_TRUE(lock.isQuiescentForTesting());
}

// Once a burst of writers has finished and every thread has gone away, a lone reader
// must still be able to acquire. This catches state left behind on behalf of a writer
// that no longer exists, which is invisible while any writer is still running.
TEST(WTF_ReadWriteLock, LoneReaderAfterWriterBurst)
{
    constexpr unsigned writerCount = 4;
    constexpr unsigned iterations = 2000;

    ReadWriteLock lock;
    DeadlineGuard deadline;
    unsigned counter = 0;

    auto writers = spawn(writerCount, [&](unsigned) {
        for (unsigned i = 0; i < iterations && !deadline.expired(); ++i) {
            Locker locker { lock.write() };
            ++counter;
        }
    });
    join(writers);

    EXPECT_TRUE(lock.isQuiescentForTesting());

    Atomic<bool> acquired { false };
    auto reader = spawn(1, [&](unsigned) {
        Locker locker { lock.read() };
        acquired.store(true, std::memory_order_relaxed);
    });
    join(reader);

    EXPECT_TRUE(acquired.load(std::memory_order_relaxed));
    EXPECT_TRUE(lock.isQuiescentForTesting());
}

// Mutual exclusion: a writer must never overlap a reader or another writer.
TEST(WTF_ReadWriteLock, MutualExclusion)
{
    constexpr unsigned writerCount = 4;
    constexpr unsigned readerCount = 4;
    constexpr unsigned iterations = 500;

    ReadWriteLock lock;
    DeadlineGuard deadline;
    Atomic<int> readersInside { 0 };
    Atomic<int> writersInside { 0 };
    Atomic<unsigned> violations { 0 };

    auto writers = spawn(writerCount, [&](unsigned) {
        for (unsigned i = 0; i < iterations && !deadline.expired(); ++i) {
            Locker locker { lock.write() };
            writersInside.exchangeAdd(1);
            if (writersInside.load() != 1 || readersInside.load())
                violations.exchangeAdd(1);
            Thread::yield();
            if (writersInside.load() != 1 || readersInside.load())
                violations.exchangeAdd(1);
            writersInside.exchangeAdd(-1);
        }
    });

    auto readers = spawn(readerCount, [&](unsigned) {
        for (unsigned i = 0; i < iterations && !deadline.expired(); ++i) {
            Locker locker { lock.read() };
            readersInside.exchangeAdd(1);
            if (writersInside.load())
                violations.exchangeAdd(1);
            Thread::yield();
            if (writersInside.load())
                violations.exchangeAdd(1);
            readersInside.exchangeAdd(-1);
        }
    });

    join(writers);
    join(readers);

    EXPECT_EQ(0u, violations.load());
    EXPECT_TRUE(lock.isQuiescentForTesting());
}

// Readers really are concurrent: with no writers present, several readers must be able
// to hold the lock at the same time.
TEST(WTF_ReadWriteLock, ReadersRunConcurrently)
{
    constexpr unsigned readerCount = 4;

    ReadWriteLock lock;
    DeadlineGuard deadline;
    Atomic<unsigned> arrived { 0 };
    Atomic<unsigned> inside { 0 };
    Atomic<unsigned> maxObserved { 0 };

    auto readers = spawn(readerCount, [&](unsigned) {
        Locker locker { lock.read() };
        unsigned current = inside.exchangeAdd(1) + 1;
        for (;;) {
            unsigned previous = maxObserved.load();
            if (current <= previous || maxObserved.compareExchangeWeak(previous, current))
                break;
        }
        // Wait on a count that only ever grows. Waiting on `inside` would hang until the
        // deadline, because the last reader to arrive decrements it again on its way out
        // before the others have observed it.
        arrived.exchangeAdd(1);
        while (arrived.load() < readerCount && !deadline.expired())
            Thread::yield();
        inside.exchangeAdd(-1);
    });
    join(readers);

    EXPECT_EQ(readerCount, maxObserved.load());
    EXPECT_TRUE(lock.isQuiescentForTesting());
}

// Try-locks make a single attempt and so may fail spuriously when the underlying CAS loses
// its reservation. A failure to acquire is therefore only meaningful if it persists, which
// is what this retries for. A *refusal* needs no retry: the try-lock returns before
// attempting any CAS when the lock is unavailable, so that answer is exact.
template<typename TryLock>
bool eventuallyAcquires(const TryLock& tryLock, DeadlineGuard& deadline)
{
    while (!deadline.expired()) {
        if (tryLock())
            return true;
        Thread::yield();
    }
    return false;
}

TEST(WTF_ReadWriteLock, TryLockUncontended)
{
    ReadWriteLock lock;
    DeadlineGuard deadline;

    EXPECT_TRUE(eventuallyAcquires([&] { return lock.tryWriteLock(); }, deadline));
    // A write lock excludes everything, including other try-lockers.
    EXPECT_FALSE(lock.tryWriteLock());
    EXPECT_FALSE(lock.tryReadLock());
    lock.writeUnlock();
    EXPECT_TRUE(lock.isQuiescentForTesting());

    // Readers do not conflict, so a try-read must succeed alongside another reader.
    EXPECT_TRUE(eventuallyAcquires([&] { return lock.tryReadLock(); }, deadline));
    EXPECT_TRUE(eventuallyAcquires([&] { return lock.tryReadLock(); }, deadline));
    EXPECT_FALSE(lock.tryWriteLock());
    lock.readUnlock();
    EXPECT_FALSE(lock.tryWriteLock());
    lock.readUnlock();
    EXPECT_TRUE(lock.isQuiescentForTesting());

    EXPECT_TRUE(eventuallyAcquires([&] { return lock.tryWriteLock(); }, deadline));
    lock.writeUnlock();
    EXPECT_TRUE(lock.isQuiescentForTesting());
}

// A try-read must refuse while a writer is merely waiting, not only while one is holding.
// Barging past a registered writer would be a hole in the mechanism that stops a stream of
// readers from starving writers.
TEST(WTF_ReadWriteLock, TryReadLockYieldsToWaitingWriter)
{
    ReadWriteLock lock;
    DeadlineGuard deadline;
    Atomic<bool> writerHasLock { false };

    lock.readLock();

    auto writer = spawn(1, [&](unsigned) {
        Locker locker { lock.write() };
        writerHasLock.store(true, std::memory_order_relaxed);
    });

    // The writer registers itself before blocking, which becomes observable as try-read
    // starting to refuse even though only a reader holds the lock.
    bool refusedWhileWriterWaited = false;
    while (!deadline.expired()) {
        if (!lock.tryReadLock()) {
            refusedWhileWriterWaited = true;
            break;
        }
        // We got in, so the writer has not registered yet. Undo and look again.
        lock.readUnlock();
        Thread::yield();
    }

    EXPECT_TRUE(refusedWhileWriterWaited);
    EXPECT_FALSE(writerHasLock.load(std::memory_order_relaxed));

    lock.readUnlock();
    join(writer);

    EXPECT_TRUE(writerHasLock.load(std::memory_order_relaxed));
    EXPECT_TRUE(lock.isQuiescentForTesting());
}

// A reader that is slow to be scheduled after being woken must not lose its turn. With two
// readers parked and a writer looping, the writer's release wakes both, but the first reader
// can complete its whole critical section and the writer can re-acquire before the second
// reader ever runs. If waking only granted permission rather than the lock itself, the second
// reader would find that permission withdrawn, re-park, and repeat forever.
TEST(WTF_ReadWriteLock, WokenReaderKeepsItsTurnWhenDescheduled)
{
    constexpr unsigned readerCount = 2;
    constexpr unsigned readsPerReader = 200;

    ReadWriteLock lock;
    DeadlineGuard deadline;
    Atomic<bool> readersDone { false };
    Atomic<unsigned> writesCompleted { 0 };
    std::array<Atomic<unsigned>, readerCount> readsCompleted { };

    // One writer, looping as tightly as it can, so that it is always either holding the lock
    // or registered as waiting for it.
    auto writer = spawn(1, [&](unsigned) {
        while (!readersDone.load(std::memory_order_relaxed) && !deadline.expired()) {
            Locker locker { lock.write() };
            writesCompleted.exchangeAdd(1);
        }
    });

    auto readers = spawn(readerCount, [&](unsigned index) {
        // Don't start until the writer is genuinely running, otherwise the readers can finish
        // before it ever contends and the test proves nothing.
        while (writesCompleted.load(std::memory_order_relaxed) < 100 && !deadline.expired())
            Thread::yield();

        for (unsigned i = 0; i < readsPerReader && !deadline.expired(); ++i) {
            {
                Locker locker { lock.read() };
                // Hold long enough that the other reader in the cohort is likely still
                // descheduled when this one releases, which is the shape of the race.
                Thread::yield();
            }
            readsCompleted[index].store(i + 1, std::memory_order_relaxed);
        }
    });

    join(readers);
    readersDone.store(true, std::memory_order_relaxed);
    join(writer);

    // If this is 0 the writer never contended and the test was vacuous.
    EXPECT_GT(writesCompleted.load(std::memory_order_relaxed), 100u);
    for (unsigned i = 0; i < readerCount; ++i)
        EXPECT_EQ(readsPerReader, readsCompleted[i].load(std::memory_order_relaxed));
    EXPECT_TRUE(lock.isQuiescentForTesting());
}

// Uncontended acquisition must leave no residue, so that the inline fast paths keep
// working rather than silently degrading to the out-of-line ones.
TEST(WTF_ReadWriteLock, UncontendedQuiescence)
{
    ReadWriteLock lock;

    for (unsigned i = 0; i < 100; ++i) {
        {
            Locker locker { lock.write() };
        }
        EXPECT_TRUE(lock.isQuiescentForTesting());
        {
            Locker locker { lock.read() };
        }
        EXPECT_TRUE(lock.isQuiescentForTesting());
    }
}

// A lone reader upgrading has nothing to wait for, and what it gets is a real write lock.
TEST(WTF_ReadWriteLock, TryUpgradeUncontended)
{
    ReadWriteLock lock;

    {
        Locker readLocker { lock.read() };
        EXPECT_TRUE(readLocker.tryUpgrade(lock));
        Locker writeLocker { AdoptLock, lock.write() };

        // Whatever we hold now excludes everything, which is what distinguishes it from the read
        // lock we started with.
        EXPECT_FALSE(lock.tryReadLock());
        EXPECT_FALSE(lock.tryWriteLock());
    }
    EXPECT_TRUE(lock.isQuiescentForTesting());
}

// An upgrade cannot go through while another reader is still inside, and failing must leave the
// lock exactly as free as it would be had the upgrader simply released its read lock.
// An upgrade with another reader inside waits for it, the same way an ordinary write acquisition
// does. The other read lock is on a second thread because the read lock is not recursive: a thread
// that holds two of them and upgrades one would wait for a departure only it can make.
TEST(WTF_ReadWriteLock, TryUpgradeWaitsForReaderToLeave)
{
    constexpr Seconds readerHold = 50_ms;

    ReadWriteLock lock;
    DeadlineGuard deadline;
    Atomic<bool> otherReaderInside { false };

    auto otherReader = spawn(1, [&](unsigned) {
        Locker readLocker { lock.read() };
        otherReaderInside.store(true, std::memory_order_release);
        sleep(readerHold);
    });

    while (!otherReaderInside.load(std::memory_order_acquire) && !deadline.expired())
        Thread::yield();
    EXPECT_TRUE(otherReaderInside.load(std::memory_order_acquire));

    auto startTime = MonotonicTime::now();
    {
        Locker readLocker { lock.read() };
        EXPECT_TRUE(readLocker.tryUpgrade(lock));
        Locker writeLocker { AdoptLock, lock.write() };

        // The other reader has necessarily left, because this is exclusive now.
        EXPECT_FALSE(lock.tryReadLock());
    }
    // The upgrade could only complete once the other reader departed, so it has to have spent most
    // of the hold waiting. Without that this would also pass if the upgrade had simply failed to
    // notice the other reader.
    EXPECT_GT(MonotonicTime::now() - startTime, readerHold / 2);

    join(otherReader);
    EXPECT_TRUE(lock.isQuiescentForTesting());
}

// The case that would deadlock a blocking upgrade: the writer we would have to queue behind is
// itself waiting for our read lock. Failing has to release that read lock through the path that
// wakes such a writer, or the writer never runs.
TEST(WTF_ReadWriteLock, TryUpgradeReleasesReadLockForWaitingWriter)
{
    ReadWriteLock lock;
    DeadlineGuard deadline;
    Atomic<bool> writerHasLock { false };

    {
        Locker readLocker { lock.read() };

        auto writer = spawn(1, [&](unsigned) {
            Locker locker { lock.write() };
            writerHasLock.store(true, std::memory_order_relaxed);
        });

        // Wait until the writer owns the phase, which is observable as try-read beginning to refuse.
        // From that point it also holds the writer exclusion, so the upgrade below cannot get it.
        bool writerRegistered = false;
        while (!deadline.expired()) {
            if (!lock.tryReadLock()) {
                writerRegistered = true;
                break;
            }
            lock.readUnlock();
            Thread::yield();
        }
        EXPECT_TRUE(writerRegistered);
        EXPECT_FALSE(writerHasLock.load(std::memory_order_relaxed));

        EXPECT_FALSE(readLocker.tryUpgrade(lock));

        // The read locker is still in scope and we release nothing explicitly, so if the writer
        // completes it can only be because the failed upgrade gave up our read lock for us.
        join(writer);
        EXPECT_TRUE(writerHasLock.load(std::memory_order_relaxed));
    }
    EXPECT_TRUE(lock.isQuiescentForTesting());
}

// The point of upgrading rather than releasing and re-acquiring: no writer may run in between, so
// what was read under the read lock is still true once the write lock is held.
// There is one upgrader rather than several because an upgrade fails when it cannot take the writer
// exclusion, so concurrent upgraders would spend the test losing it to each other. The writers pause
// between acquisitions for the same reason: looping as fast as they can, they hold that exclusion
// essentially all the time and no upgrade ever gets a chance.
TEST(WTF_ReadWriteLock, TryUpgradeAdmitsNoInterveningWriter)
{
    constexpr unsigned writerCount = 2;
    constexpr unsigned iterations = 500;

    ReadWriteLock lock;
    DeadlineGuard deadline;
    unsigned counter = 0;
    Atomic<bool> upgraderDone { false };
    Atomic<unsigned> staleReads { 0 };
    Atomic<unsigned> upgradesCompleted { 0 };
    Atomic<int> readersInside { 0 };
    Atomic<int> writersInside { 0 };
    Atomic<unsigned> violations { 0 };

    auto writers = spawn(writerCount, [&](unsigned) {
        while (!upgraderDone.load(std::memory_order_relaxed) && !deadline.expired()) {
            {
                Locker locker { lock.write() };
                writersInside.exchangeAdd(1);
                if (writersInside.load() != 1 || readersInside.load())
                    violations.exchangeAdd(1);
                ++counter;
                writersInside.exchangeAdd(-1);
            }
            sleep(100_us);
        }
    });

    auto upgrader = spawn(1, [&](unsigned) {
        for (unsigned i = 0; i < iterations && !deadline.expired(); ++i) {
            Locker readLocker { lock.read() };
            readersInside.exchangeAdd(1);
            if (writersInside.load())
                violations.exchangeAdd(1);
            unsigned observed = counter;
            readersInside.exchangeAdd(-1);

            // Whether this succeeds or not, the read lock is gone once it returns.
            if (!readLocker.tryUpgrade(lock))
                continue;
            Locker writeLocker { AdoptLock, lock.write() };

            writersInside.exchangeAdd(1);
            if (writersInside.load() != 1 || readersInside.load())
                violations.exchangeAdd(1);
            // A writer running between the read above and the upgrade would have changed this, which
            // is exactly what upgrading is supposed to rule out.
            if (counter != observed)
                staleReads.exchangeAdd(1);
            ++counter;
            writersInside.exchangeAdd(-1);
            upgradesCompleted.exchangeAdd(1);
        }
    });

    join(upgrader);
    upgraderDone.store(true, std::memory_order_relaxed);
    join(writers);

    EXPECT_EQ(0u, staleReads.load());
    EXPECT_EQ(0u, violations.load());
    // If nothing ever upgraded the test proved nothing.
    EXPECT_GT(upgradesCompleted.load(), 0u);
    EXPECT_TRUE(lock.isQuiescentForTesting());
}

// A scope that holds both the read locker and the write locker it produced must release exactly
// once: the write locker owns the write lock, and the read locker owns nothing by then.
TEST(WTF_ReadWriteLock, TryUpgradeThroughLockerReleasesExactlyOnce)
{
    ReadWriteLock lock;
    DeadlineGuard deadline;

    for (unsigned i = 0; i < 100; ++i) {
        {
            Locker readLocker { lock.read() };
            EXPECT_TRUE(readLocker.tryUpgrade(lock));
            Locker writeLocker { AdoptLock, lock.write() };
        }
        // Under-releasing would leave the write lock held and fail here; over-releasing would trip
        // the assertion in endPhase().
        EXPECT_TRUE(lock.isQuiescentForTesting());
        EXPECT_TRUE(eventuallyAcquires([&] { return lock.tryWriteLock(); }, deadline));
        lock.writeUnlock();
        EXPECT_TRUE(lock.isQuiescentForTesting());
    }
}

} // namespace TestWebKitAPI
