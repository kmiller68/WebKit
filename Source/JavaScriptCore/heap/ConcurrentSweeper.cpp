/*
 * Copyright (C) 2023-2024 Apple Inc. All rights reserved.
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
#include "ConcurrentSweeper.h"

#include "DeferGCInlines.h"
#include "HeapInlines.h"
#include "JITWorklist.h"
#include "MarkedBlock.h"
#include "VM.h"

#include <wtf/SimpleStats.h>

namespace JSC {

namespace ConcurrentSweeperInternal {
static constexpr bool verbose = false;
}

static Atomic<size_t> numBlocksSwept;

ConcurrentSweeper::ConcurrentSweeper(const AbstractLocker& locker, VM& vm, Box<Lock> lock, Ref<AutomaticThreadCondition>&& condition)
    : AutomaticThread(locker, WTFMove(lock), WTFMove(condition), ThreadType::GarbageCollection)
    , m_stringImplsBuffer(std::make_unique<StringImplBag::Node>(StringImplBag::Node::Data { }))
    , m_vm(vm)
{
}

ConcurrentSweeper::~ConcurrentSweeper()
{
}

Ref<ConcurrentSweeper> ConcurrentSweeper::create(VM& vm)
{
    Box<Lock> lock = Box<Lock>::create();
    Locker locker(*lock);
    return adoptRef(*new ConcurrentSweeper(locker, vm, lock, AutomaticThreadCondition::create()));
}

static inline bool shouldConcurrentlySweep(BlockDirectory* directory)
{
    return directory->destruction() == NeedsDestruction;
}

auto ConcurrentSweeper::poll(const AbstractLocker&) -> PollResult
{
    if (m_shouldStop)
        return PollResult::Stop;

    auto wait = [&] {
        // We're about to stop, make sure the mutator can see any strings.
        flushStringImplsToMainThreadDeref();
        m_isWorking = false;
        return PollResult::Wait;
    };

    if (auto* worklist = JITWorklist::existingGlobalWorklistOrNull()) {
        if (worklist->numberOfActiveThreads() >= 3)
            return wait();
    }

    m_isWorking = true;
    if (!m_priorityDirectoriesToSweep.isEmpty())
        return PollResult::Work;

    if (!m_currentDirectory && Options::collectContinuously())
        m_currentDirectory = m_vm.heap.objectSpace().firstDirectory();

    if (m_currentDirectory)
        return PollResult::Work;

    return wait();
}

auto ConcurrentSweeper::work() -> WorkResult
{
    Locker locker(m_rightToSweep);

    MarkedBlock::Handle* handle;
    if (m_priorityDirectoriesToSweep.peek()) {
        // TODO: Should this remember anything?
        unsigned unusedMarkedBlockIndex = 0;
        handle = m_priorityDirectoriesToSweep.peek()->data.findBlockToEagerlySweep(unusedMarkedBlockIndex);
        if (!handle) {
            m_priorityDirectoriesToSweep.dequeue();
            return WorkResult::Continue;
        }
    }
    else {
        handle = m_currentDirectory->findBlockToEagerlySweep(m_currentMarkedBlockIndex);
        if (!handle) {
            m_currentMarkedBlockIndex = 0;
            while (true) {
                m_currentDirectory = m_currentDirectory->nextDirectory();
                if (m_currentDirectory && m_currentDirectory->destruction() == NeedsMainThreadDestruction)
                    continue;
                return WorkResult::Continue;
            }
        }
    }

    handle->sweepConcurrently();
    if constexpr (ConcurrentSweeperInternal::verbose)
        numBlocksSwept.exchangeAdd(1, std::memory_order_relaxed);
    return WorkResult::Continue;
}

void ConcurrentSweeper::threadIsStopping(const AbstractLocker&)
{
    // This is unnecesssary but it doesn't hurt to be conservative.
    flushStringImplsToMainThreadDeref();
    dataLogLnIf(ConcurrentSweeperInternal::verbose, "Shutting down concurrent sweeper thread.");
}

void ConcurrentSweeper::maybeNotify()
{
    // Locker locker(lock());
    // m_hasNewWork = true;
    // condition().notifyAll(locker);

    size_t unsweptCount = 0;
    unsigned threshold = Options::concurrentSweeperUnsweptThreshold();
    for (auto* directory = m_vm.heap.objectSpace().firstDirectory(); directory; directory = directory->nextDirectory()) {
        if (directory->destruction() == NeedsMainThreadDestruction)
            continue;
        directory->assertIsMutatorOrMutatorIsStopped();
        unsweptCount += std::min(directory->unsweptBitsView().bitCount(), static_cast<size_t>(Options::concurrentSweeperThreshold()));
        if (unsweptCount >= threshold) {
            if constexpr (ConcurrentSweeperInternal::verbose) {
                dataLogLn("Hit threshold of ", threshold, " posting work: ", unsweptCount);
                size_t count = numBlocksSwept.exchange(0, std::memory_order_relaxed);
                dataLogLn("Swept ", count, " MarkedBlocks since last posting");
            }
            Locker locker(lock());
            m_currentDirectory = m_vm.heap.objectSpace().firstDirectory();
            condition().notifyAll(locker);
            return;
        }
    }
    dataLogLnIf(ConcurrentSweeperInternal::verbose, "Didn't hit threshold of ", threshold, " no work posted: ", unsweptCount);
}

void ConcurrentSweeper::clearStringImplsToMainThreadDerefSlow()
{
    SimpleStats stats;
    m_stringsToMainThreadDeref.consumeAll([&] (auto&& strings) ALWAYS_INLINE_LAMBDA {
        if constexpr (ConcurrentSweeperInternal::verbose)
            stats.add(strings.size());
    });
    dataLogLnIf(ConcurrentSweeperInternal::verbose, "Freed ", stats.sum(), " StringImpls on the mutator thread with ", stats.mean(), " StringImpls per bucket");
}

void ConcurrentSweeper::flushStringImplsToMainThreadDeref()
{
    if (!m_stringImplsBuffer->data.isEmpty()) {
        m_stringsToMainThreadDeref.add(WTFMove(m_stringImplsBuffer));
        m_stringImplsBuffer = std::make_unique<StringImplBag::Node>(StringImplBag::Node::Data { });
    }
}

void ConcurrentSweeper::shouldStop()
{
    Locker locker(lock());
    m_shouldStop = true;
    condition().notifyAll(locker);
}

} // namespace JSC
