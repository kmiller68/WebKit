/*
 * Copyright (C) 2017-2024 Apple Inc. All rights reserved.
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

#pragma once

#include "FreeList.h"
#include "MarkedBlock.h"

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {

ALWAYS_INLINE HeapCell* FreeList::peekNext() const
{
    ASSERT(allocationWillSucceed());
    if (m_intervalStart < m_intervalEnd)
        return reinterpret_cast<HeapCell*>(m_intervalStart);

    // It's an invariant of our allocator that we don't create empty intervals, so there
    // should always be enough space remaining to allocate a cell.
    return reinterpret_cast<HeapCell*>(nextInterval());
}

ALWAYS_INLINE HeapCell* FreeList::allocateWithCellSize(const Invocable<HeapCell*()> auto& slowPath, size_t cellSize)
{
    auto allocate = [&] ALWAYS_INLINE_LAMBDA {
        ASSERT(m_intervalStart + cellSize <= m_intervalEnd);
        HeapCell* result = reinterpret_cast<HeapCell*>(m_intervalStart);
        m_intervalStart += cellSize;
        return result;
    };

    if (LIKELY(m_intervalStart < m_intervalEnd))
        return allocate();
    
    FreeCell* cell = nextInterval();
    if (UNLIKELY(isSentinel(cell)))
        return slowPath();

    FreeCell::advance(m_secret, m_nextInterval, m_intervalStart, m_intervalEnd);
    
    // It's an invariant of our allocator that we don't create empty intervals, so there 
    // should always be enough space remaining to allocate a cell.
    return allocate();
}

void FreeList::forEach(const Invocable<void(HeapCell*)> auto& func, size_t cellSize) const
{
    FreeCell* cell = nextInterval();
    char* intervalStart = m_intervalStart;
    char* intervalEnd = m_intervalEnd;
    ASSERT(intervalEnd - intervalStart < (ptrdiff_t)(16 * KB));

    while (true) {
        for (; intervalStart < intervalEnd; intervalStart += cellSize)
            func(reinterpret_cast<HeapCell*>(intervalStart));

        // If we explore the whole interval and the cell is the sentinel value, though, we should
        // immediately exit so we don't decode anything out of bounds.
        if (isSentinel(cell))
            break;

        FreeCell::advance(m_secret, cell, intervalStart, intervalEnd);
    }
}

void FreeList::forEachInterval(const Invocable<void(char* start, char* end)> auto& func) const
{
    FreeCell* cell = nextInterval();
    char* intervalStart = m_intervalStart;
    char* intervalEnd = m_intervalEnd;
    ASSERT(intervalEnd - intervalStart < static_cast<ptrdiff_t>(16 * KB));

    if (intervalStart < intervalEnd)
        func(intervalStart, intervalEnd);

    while (!isSentinel(cell)) {
        FreeCell::advance(m_secret, cell, intervalStart, intervalEnd);

        ASSERT(intervalStart < intervalEnd);
        func(intervalStart, intervalEnd);
    }
}

} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
