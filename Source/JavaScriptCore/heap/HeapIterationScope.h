/*
 * Copyright (C) 2013-2024 Apple Inc. All rights reserved.
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
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#include "ConcurrentSweeper.h"
#include "Heap.h"
#include <wtf/Noncopyable.h>

namespace JSC {

class HeapIterationScope {
    WTF_FORBID_HEAP_ALLOCATION;
    WTF_MAKE_NONCOPYABLE(HeapIterationScope);
public:
    HeapIterationScope(Heap&);
    ~HeapIterationScope();

private:
    JSC::Heap& m_heap;
    bool m_didSuspendSweeper { false };
};

inline HeapIterationScope::HeapIterationScope(JSC::Heap& heap)
    : m_heap(heap)
{
    // FIXME: It would be nice to assert we're holding the API lock when iterating the heap so we know no other thread is mutating the heap
    // but adding `ASSERT_WITH_MESSAGE(heap.vm().currentThreadIsHoldingAPILock(), "Trying to iterate the JS heap without the API lock");`
    // causes spurious crashes since the only thing technically needed is just heap.hasAccess() but that doesn't verify this thread is
    // the one with access only that *some* thread has access.

    // While iterating the heap it's just simpler to assume the concurrent sweeper isn't running.
    if (m_heap.concurrentSweeper() && !m_heap.concurrentSweeper()->isSuspended()) {
        m_didSuspendSweeper = true;
        m_heap.concurrentSweeper()->suspendSweeping();
    }
    m_heap.willStartIterating();
}

inline HeapIterationScope::~HeapIterationScope()
{
    m_heap.didFinishIterating();
    if (m_didSuspendSweeper)
        m_heap.concurrentSweeper()->resumeSweeping();
}

} // namespace JSC
