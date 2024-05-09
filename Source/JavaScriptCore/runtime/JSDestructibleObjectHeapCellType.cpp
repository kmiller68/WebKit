/*
 * Copyright (C) 2017-2023 Apple Inc. All rights reserved.
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
#include "JSDestructibleObjectHeapCellType.h"

#include "JSCJSValueInlines.h"
#include "JSDestructibleObject.h"
#include "MarkedBlockInlines.h"
#include <wtf/TZoneMallocInlines.h>

namespace JSC {

WTF_MAKE_TZONE_ALLOCATED_IMPL(JSDestructibleObjectHeapCellType);

struct JSDestructibleObjectDestroyFunc {
    WTF_FORBID_HEAP_ALLOCATION;
public:
    ALWAYS_INLINE DestructionResult operator()(VM&, JSCell* cell, DestructionConcurrency concurrency) const
    {
        return static_cast<JSDestructibleObject*>(cell)->classInfo()->methodTable.destroy(cell, concurrency);
    }
};

JSDestructibleObjectHeapCellType::JSDestructibleObjectHeapCellType()
    : HeapCellType(CellAttributes(NeedsDestruction, HeapCell::JSCell))
{
}

JSDestructibleObjectHeapCellType::~JSDestructibleObjectHeapCellType()
{
}

void JSDestructibleObjectHeapCellType::finishSweep(MarkedBlock::Handle& handle, FreeList* freeList, DestructionConcurrency concurrency) const
{
    if (concurrency == DestructionConcurrency::Mutator)
        handle.finishSweepKnowingHeapCellType<DestructionConcurrency::Mutator>(freeList, JSDestructibleObjectDestroyFunc());
    else
        handle.finishSweepKnowingHeapCellType<DestructionConcurrency::Concurrent>(freeList, JSDestructibleObjectDestroyFunc());
}

DestructionResult JSDestructibleObjectHeapCellType::destroy(VM& vm, JSCell* cell, DestructionConcurrency concurrency) const
{
    return JSDestructibleObjectDestroyFunc()(vm, cell, concurrency);
}

} // namespace JSC

