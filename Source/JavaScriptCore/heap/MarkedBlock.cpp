/*
 * Copyright (C) 2011-2024 Apple Inc. All rights reserved.
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

#include "config.h"
#include "MarkedBlock.h"

#include "AlignedMemoryAllocator.h"
#include "FreeListInlines.h"
#include "IsoCellSetInlines.h"
#include "JSCJSValueInlines.h"
#include "MarkedBlockInlines.h"
#include "SweepingScope.h"
#include <wtf/CommaPrinter.h>

namespace JSC {
namespace MarkedBlockInternal {
static constexpr bool verbose = true;
}

static constexpr bool computeBalance = false;
static size_t balance;

DEFINE_ALLOCATOR_WITH_HEAP_IDENTIFIER(MarkedBlock);
DEFINE_ALLOCATOR_WITH_HEAP_IDENTIFIER(MarkedBlockHandle);

MarkedBlock::Handle* MarkedBlock::tryCreate(JSC::Heap& heap, AlignedMemoryAllocator* alignedMemoryAllocator)
{
    if (computeBalance) {
        balance++;
        if (!(balance % 10))
            dataLog("MarkedBlock Balance: ", balance, "\n");
    }
    void* blockSpace = alignedMemoryAllocator->tryAllocateAlignedMemory(blockSize, blockSize);
    if (!blockSpace)
        return nullptr;
    if (scribbleFreeCells())
        scribble(blockSpace, blockSize);
    return new Handle(heap, alignedMemoryAllocator, blockSpace);
}

MarkedBlock::Handle::Handle(JSC::Heap& heap, AlignedMemoryAllocator* alignedMemoryAllocator, void* blockSpace)
    : m_alignedMemoryAllocator(alignedMemoryAllocator)
    , m_weakSet(heap.vm())
    , m_block(new (NotNull, blockSpace) MarkedBlock(heap.vm(), *this))
{
    heap.didAllocateBlock(blockSize);
}

MarkedBlock::Handle::~Handle()
{
    JSC::Heap& heap = *this->heap();
    if (computeBalance) {
        balance--;
        if (!(balance % 10))
            dataLog("MarkedBlock Balance: ", balance, "\n");
    }
    m_directory->removeBlock(this, BlockDirectory::WillDeleteBlock::Yes);
    m_block->~MarkedBlock();
    m_alignedMemoryAllocator->freeAlignedMemory(m_block);
    heap.didFreeBlock(blockSize);
}

MarkedBlock::MarkedBlock(VM& vm, Handle& handle)
{
    new (&header()) Header(vm, handle);
    dataLogLnIf(MarkedBlockInternal::verbose, RawPointer(this), ": Allocated.");
}

MarkedBlock::~MarkedBlock()
{
    dataLogLnIf(MarkedBlockInternal::verbose, RawPointer(this), ": Deallocated.");
    header().~Header();
}

MarkedBlock::Header::Header(VM& vm, Handle& handle)
    : m_handle(handle)
    , m_vm(&vm)
    , m_markingVersion(MarkedSpace::nullVersion)
    , m_newlyAllocatedVersion(MarkedSpace::nullVersion)
{
}

MarkedBlock::Header::~Header() = default;

void MarkedBlock::Handle::unsweepWithNoNewlyAllocated()
{
    RELEASE_ASSERT(m_isFreeListed);
    m_isFreeListed = false;
    m_directory->didFinishUsingBlock(this);
}

void MarkedBlock::Handle::stopAllocating(const FreeList& freeList)
{
    // dataLogLn(RawPointer(this), "stopAllocating: locking");
    Locker locker { blockHeader().m_lock };
    
    if (MarkedBlockInternal::verbose)
        dataLog(RawPointer(this), ": MarkedBlock::Handle::stopAllocating!\n");
    m_directory->assertIsMutatorOrMutatorIsStopped();

    if (!isFreeListed()) {
        if (MarkedBlockInternal::verbose)
            dataLog("There ain't no newly allocated.\n");
        // This means that we either didn't use this block at all for allocation since last GC, we
        // stopped right after consuming the last cell of the free list, or someone had already
        // done stopAllocating() before.
        ASSERT(freeList.allocationWillFail());
        directory()->didFinishUsingBlock(this);
        return;
    }
    
    if (MarkedBlockInternal::verbose)
        dataLog("Free list: ", freeList, "\n");
    
    // Roll back to a coherent state for Heap introspection. Cells newly
    // allocated from our free list are not currently marked, so we need another
    // way to tell what's live vs dead. 
    
    // WTF::dataFile().atomically([&] (PrintStream& out) {
    //     locker.assertLockIsHeld(blockHeader().m_lock);
    //     out.println(RawPointer(this), ": MarkedBlock::Handle::stopAllocating! before");
    //     blockHeader().dumpBits(out);
    // });

    ASSERT(m_directory->isEdenOnly(this) || !block().isNewlyAllocatedStale());

    // If we have the current allocation version then we could have:
    // swept, allocated, stopped, resumed, swept, allocated, and gotten here.
    // Thus we need to preserve the existing newlyAllocated because they would have
    // come from the current GC.
    // HeapVersion newlyAllocatedVersion = space()->newlyAllocatedVersion();
    // if (blockHeader().m_newlyAllocatedVersion != newlyAllocatedVersion) {
    //     // Make sure we're not blowing away anything keeping old objects alive.
    //     ASSERT(heap()->collectionScope() == CollectionScope::Eden || areMarksStale());
    //     blockHeader().m_newlyAllocated.clearAll();
    //     blockHeader().m_newlyAllocatedVersion = newlyAllocatedVersion;
    // }

    if (m_directory->isEdenOnly(this)) {
        {
            m_directory->releaseAssertAcquiredBitVectorLock();
            Locker locker { m_directory->bitvectorLock() };
            m_directory->setIsEdenOnly(this, false);
        }

        ASSERT_WITH_MESSAGE(freeList.hasOneInterval(), "An eden only block should only have one free list interval.");
        unsigned freeListStartAtom = block().atomNumber(freeList.peekNext());
        blockHeader().m_newlyAllocated.setEachNthBit(m_atomsPerCell, true, m_startAtom, freeListStartAtom);
        blockHeader().m_newlyAllocatedVersion = vm().heap.objectSpace().newlyAllocatedVersion();
    } else
        ASSERT(!block().isNewlyAllocatedStale());

    freeList.forEach([&] (HeapCell* cell) {
        locker.assertLockIsHeld(blockHeader().m_lock);
        if (m_attributes.destruction == NeedsDestruction)
            cell->zap(HeapCell::StopAllocating);
        blockHeader().m_newlyAllocated.clear(block().atomNumber(cell));
    });

    // TODO: Could this just be any bit after the first cell of the free list?
    // TODO: This could also do freeList.forEachInterval


    // WTF::dataFile().atomically([&] (PrintStream& out) {
    //     locker.assertLockIsHeld(blockHeader().m_lock);
    //     out.println(RawPointer(this), ": MarkedBlock::Handle::stopAllocating! after");
    //     blockHeader().dumpBits(out);
    // });

    // forEachCell(
    //     [&] (size_t, HeapCell* cell, HeapCell::Kind) -> IterationStatus {
    //         block().setNewlyAllocated(cell);
    //         return IterationStatus::Continue;
    //     });

    // freeList.forEach(
    //     [&] (HeapCell* cell) {
    //         if constexpr (MarkedBlockInternal::verbose)
    //             dataLog("Free cell: ", RawPointer(cell), "\n");
    //         if (m_attributes.destruction == NeedsDestruction)
    //             cell->zap(HeapCell::StopAllocating);
    //         block().clearNewlyAllocated(cell);
    //     });
    
    m_isFreeListed = false;
    directory()->didFinishUsingBlock(this);
}

void MarkedBlock::Handle::lastChanceToFinalize()
{
    // Concurrent sweeper is shut down at this point.
    m_directory->assertSweeperIsSuspended();
    m_directory->setIsDestructible(this, true);
    blockHeader().assertConcurrentWriteAccess();
    blockHeader().m_marks.clearAll();
    block().clearHasAnyMarked();
    blockHeader().m_markingVersion = heap()->objectSpace().markingVersion();
    m_weakSet.lastChanceToFinalize();
    blockHeader().m_newlyAllocated.clearAll();
    blockHeader().m_newlyAllocatedVersion = heap()->objectSpace().newlyAllocatedVersion();
    m_directory->setIsInUse(this, true);
    sweep(nullptr);
}

void MarkedBlock::Handle::resumeAllocating(FreeList& freeList)
{
    BlockDirectory* directory = this->directory();
    directory->assertSweeperIsSuspended();
    ASSERT(!directory->isInUse(this));
    directory->setIsInUse(this, true);
    {
        Locker locker { blockHeader().m_lock };
        
        if (MarkedBlockInternal::verbose)
            dataLog(RawPointer(this), ": MarkedBlock::Handle::resumeAllocating!\n");


        ASSERT(!isFreeListed());
        
        // TODO: is this right?
        if (!block().hasAnyNewlyAllocated() && !directory->isEdenOnly(this)) {
            if (MarkedBlockInternal::verbose)
                dataLog("There ain't no newly allocated.\n");
            // This means we had already exhausted the block when we stopped allocation.
            // TODO: Isn't this already clear?
            freeList.clear();
            return;
        }
    }

    // Re-create our free list from before stopping allocation. Note that this may return an empty
    // freelist, in which case the block will still be Marked!
    sweep(&freeList);
}

void MarkedBlock::aboutToMarkSlow(HeapVersion markingVersion)
{
    ASSERT(vm().heap.objectSpace().isMarking());
    Locker locker { header().m_lock };
    
    if (!areMarksStale(markingVersion))
        return;

    bool isEdenOnly;
    {
        BlockDirectory* directory = handle().directory();
        Locker bitLocker { directory->bitvectorLock() };
        isEdenOnly = directory->isEdenOnly(&handle());
    }

    if (isEdenOnly || !marksPreserveLivenessDuringMarking(markingVersion)) {
        if (MarkedBlockInternal::verbose)
            dataLog(RawPointer(this), ": Clearing marks without doing anything else.\n");
        // TODO: Update this comment.
        // Note that it's possible for this block to have newlyAllocated with an up-to-date
        // version! If it does, then we want to leave the newlyAllocated alone, since that means
        // that we had allocated in this previously empty block but did not fill it up, so
        // we created a newlyAllocated.
        header().m_marks.clearAll();
    } else {
        ASSERT(heap()->collectionScope() == CollectionScope::Full);
        HeapVersion newlyAllocatedVersion = space()->newlyAllocatedVersion();
        if (header().m_newlyAllocatedVersion == newlyAllocatedVersion) {
            // When do we get here? The block could not have been filled up. The newlyAllocated bits would
            // have had to be created since the end of the last collection. The only things that create
            // them are aboutToMarkSlow, lastChanceToFinalize, stopAllocating, and specializedSweep. If it had been
            // aboutToMarkSlow, then we shouldn't be here since the marks wouldn't be stale anymore. It
            // cannot be lastChanceToFinalize. So it must be stopAllocating or specializedSweep. That means that
            // we just computed the newlyAllocated bits just before the start of an increment.

            // TODO: Not anymore
            // When we are in that mode, it seems as if newlyAllocated should subsume marks.
            // WTF::dataFile().atomically([&] (PrintStream& out) {
            //     locker.assertLockIsHeld(header().m_lock);
            //     out.println(RawPointer(this), ": MarkedBlock newlyAllocated version (", header().m_newlyAllocatedVersion, ") up to date, merging.");
            //     dumpBits(out);
            //     out.println();
            // });
            header().m_newlyAllocated.mergeAndClear(header().m_marks);
        } else {
            ASSERT(!handle().isFreeListed());
            dataLogLnIf(MarkedBlockInternal::verbose, RawPointer(this), ": MarkedBlock newlyAllocated version (", header().m_newlyAllocatedVersion, ") stale, setting.");
            header().m_newlyAllocated.setAndClear(header().m_marks);
            header().m_newlyAllocatedVersion = newlyAllocatedVersion;
        }
    }
    ASSERT(header().m_marks.isEmpty());
    clearHasAnyMarked();
    WTF::storeStoreFence();
    header().m_markingVersion = markingVersion;
    
    // This means we're the first ones to mark any object in this block.
    BlockDirectory* directory = handle().directory();
    Locker bitLocker { directory->bitvectorLock() };
    directory->setIsMarkingNotEmpty(&handle(), true);
}

void MarkedBlock::resetAllocated()
{
    header().assertConcurrentWriteAccess();
    header().m_newlyAllocated.clearAll();
    header().m_newlyAllocatedVersion = MarkedSpace::nullVersion;
}

void MarkedBlock::resetMarks()
{
    // We want aboutToMarkSlow() to see what the mark bits were after the last collection. It uses
    // the version number to distinguish between the marks having already been stale before
    // beginMarking(), or just stale now that beginMarking() bumped the version. If we have a version
    // wraparound, then we will call this method before resetting the version to null. When the
    // version is null, aboutToMarkSlow() will assume that the marks were not stale as of before
    // beginMarking(). Hence the need to whip the marks into shape.
    header().assertConcurrentWriteAccess();
    if (areMarksStale())
        header().m_marks.clearAll();
    header().m_markingVersion = MarkedSpace::nullVersion;
}

#if ASSERT_ENABLED
void MarkedBlock::assertMarksNotStale() const
{
    header().assertConcurrentReadAccess();
    ASSERT(header().m_markingVersion == vm().heap.objectSpace().markingVersion());
}

void MarkedBlock::Header::assertConcurrentWriteAccess() const WTF_IGNORES_THREAD_SAFETY_ANALYSIS
{
    if (m_vm->heap.objectSpace().markingVersion() == m_markingVersion)
        return;
    ASSERT(!m_lock.isLocked());
}
#endif // ASSERT_ENABLED

bool MarkedBlock::areMarksStale()
{
    return areMarksStale(vm().heap.objectSpace().markingVersion());
}

bool MarkedBlock::Handle::areMarksStale()
{
    return m_block->areMarksStale();
}

bool MarkedBlock::isMarked(const void* p) const
{
    return isMarked(vm().heap.objectSpace().markingVersion(), p);
}

void MarkedBlock::Handle::didConsumeFreeList()
{
    Locker locker { blockHeader().m_lock };
    dataLogLnIf(MarkedBlockInternal::verbose, RawPointer(this), ": MarkedBlock::Handle::didConsumeFreeList! newlyAllocated version (", blockHeader().m_newlyAllocatedVersion, ")");
    ASSERT(isFreeListed());
    m_isFreeListed = false;

    // TODO: Delete this comment
    // If we have the current allocation version then we could have:
    // swept, allocated, stopped, resumed, swept, allocated, and gotten here.
    // Thus we need to preserve the existing newlyAllocated because they would have
    // come from the current GC.

#if ASSERT_ENABLED
    m_directory->assertIsMutatorOrMutatorIsStopped();
    if (space()->isMarking() && !m_directory->isEdenOnly(this)) {
        // This better be true because we did stopAllocating when we starting marking.
        ASSERT(blockHeader().m_newlyAllocatedVersion == space()->newlyAllocatedVersion());
        // If we had somehow finished consuming the free list right when we did stopAllocating then resumeAllocating would have
        ASSERT(!blockHeader().m_newlyAllocated.isEmpty());
        // If we have marked anything it should either be from an old generation (thus copied into newlyAllocated when we did stopAllocating) or actually new
        if (marksMode() == MarksNotStale && !blockHeader().m_newlyAllocated.subsumes(blockHeader().m_marks)) {
            block().dumpBits(WTF::dataFile());
            RELEASE_ASSERT_NOT_REACHED();
        }
    }
#endif

    m_directory->didFinishUsingBlock(this);
}

size_t MarkedBlock::markCount()
{
    header().assertConcurrentReadAccess();
    return areMarksStale() ? 0 : header().m_marks.count();
}

void MarkedBlock::clearHasAnyMarked()
{
    header().m_biasedMarkCount = header().m_markCountBias;
}

void MarkedBlock::noteMarkedSlow()
{
    BlockDirectory* directory = handle().directory();
    Locker locker { directory->bitvectorLock() };
    directory->setIsMarkingRetired(&handle(), true);
}

void MarkedBlock::Handle::removeFromDirectory()
{
    if (!m_directory)
        return;
    
    m_directory->removeBlock(this);
}

void MarkedBlock::Handle::didAddToDirectory(BlockDirectory* directory, unsigned index)
{
    ASSERT(m_index == std::numeric_limits<unsigned>::max());
    ASSERT(!m_directory);
    
    RELEASE_ASSERT(directory->subspace()->alignedMemoryAllocator() == m_alignedMemoryAllocator);
    
    m_index = index;
    m_directory = directory;
    blockHeader().m_subspace = directory->subspace();
    
    std::tie(m_atomsPerCell, m_startAtom) = atomsPerCellAndStartAtom(directory->cellSize());
    m_attributes = directory->attributes();

    if (!isJSCellKind(m_attributes.cellKind))
        RELEASE_ASSERT(m_attributes.destruction == DoesNotNeedDestruction);
    
    double markCountBias = -(Options::minMarkedBlockUtilization() * cellsPerBlock());
    
    // The mark count bias should be comfortably within this range.
    RELEASE_ASSERT(markCountBias > static_cast<double>(std::numeric_limits<int16_t>::min()));
    RELEASE_ASSERT(markCountBias < 0);
    
    // This means we haven't marked anything yet.
    blockHeader().m_biasedMarkCount = blockHeader().m_markCountBias = static_cast<int16_t>(markCountBias);
}

void MarkedBlock::Handle::didRemoveFromDirectory()
{
    ASSERT(m_index != std::numeric_limits<unsigned>::max());
    ASSERT(m_directory);
    
    m_index = std::numeric_limits<unsigned>::max();
    m_directory = nullptr;
    blockHeader().m_subspace = nullptr;
}

#if ASSERT_ENABLED
void MarkedBlock::assertValidCell(VM& vm, HeapCell* cell) const
{
    RELEASE_ASSERT(&vm == &this->vm());
    RELEASE_ASSERT(const_cast<MarkedBlock*>(this)->handle().cellAlign(cell) == cell);
}
#endif // ASSERT_ENABLED

void MarkedBlock::dumpBits(PrintStream& out) const WTF_IGNORES_THREAD_SAFETY_ANALYSIS
{
    CommaPrinter comma;
    handle().dumpState(out);
    out.println("\n\tHeap Markingversion: ", heap()->objectSpace().markingVersion(), " NewlyAllocatedVersion: ", heap()->objectSpace().newlyAllocatedVersion());
    out.print("\tmarks (", header().m_markingVersion, "):\t\t[");
    header().m_marks.dumpHex(out);
    out.print("]\n\tnewlyAllocated (", header().m_newlyAllocatedVersion, "):\t[");
    header().m_newlyAllocated.dumpHex(out);
    out.println("]");
}

void MarkedBlock::Handle::dumpState(PrintStream& out) const
{
    CommaPrinter comma;
    Locker locker { directory()->bitvectorLock() };
    directory()->forEachBitVectorWithName(
        [&](auto vectorRef, const char* name) {
            out.print(comma, name, ":"_s, vectorRef[index()] ? "YES"_s : "no"_s);
        });
}

Subspace* MarkedBlock::Handle::subspace() const
{
    return directory()->subspace();
}

void MarkedBlock::Handle::sweep(FreeList* freeList)
{
    SweepingScope sweepingScope(*heap());
    m_directory->assertIsMutatorOrMutatorIsStopped();
    ASSERT(m_directory->isInUse(this));

    SweepMode sweepMode = freeList ? SweepToFreeList : SweepOnly;
    bool needsDestruction = m_attributes.destruction == NeedsDestruction
        && m_directory->isDestructible(this);

    m_weakSet.sweep();

    if (sweepMode == SweepOnly && !needsDestruction) {
        // If we don't "release" our read access without locking then the ThreadSafetyAnalysis code gets upset with the locker below.
        m_directory->releaseAssertAcquiredBitVectorLock();
        Locker locker(m_directory->bitvectorLock());
        m_directory->setIsUnswept(this, false);
        return;
    }

    if (m_isFreeListed) {
        dataLog("FATAL: ", RawPointer(this), "->sweep: block is free-listed.\n");
        RELEASE_ASSERT_NOT_REACHED();
    }
    
    if (space()->isMarking())
        blockHeader().m_lock.lockWithoutAnalysis();
    
    subspace()->didBeginSweepingToFreeList(this);
    
    auto assertIsoCellSetsMatch = [&] ALWAYS_INLINE_LAMBDA {
#if ASSERT_ENABLED
        if (subspace()->isIsoSubspace() && freeList) {
            reinterpret_cast<IsoSubspace*>(subspace())->m_cellSets.forEach([&] (IsoCellSet* set) {
                freeList->forEach([&] (HeapCell* cell) {
                    ASSERT(!set->contains(cell));
                });
            });
        }
#endif
    };

    if (needsDestruction) {
        subspace()->finishSweep(*this, freeList);
        assertIsoCellSetsMatch();
        return;
    }
    
    // Handle the no-destructor specializations here, since we have the most of those. This
    // ensures that they don't get re-specialized for every destructor space.
    
    EmptyMode emptyMode = this->emptyMode();
    ScribbleMode scribbleMode = this->scribbleMode();
    NewlyAllocatedMode newlyAllocatedMode = this->newlyAllocatedMode();
    MarksMode marksMode = this->marksMode();
    
    auto trySpecialized = [&] () -> bool {
        if (sweepMode != SweepToFreeList)
            return false;
        if (scribbleMode != DontScribble)
            return false;
        if (newlyAllocatedMode != DoesNotHaveNewlyAllocated)
            return false;
        
        constexpr bool hasStaticAtomData = false;
        switch (emptyMode) {
        case IsEmpty:
            switch (marksMode) {
            case MarksNotStale:
                specializedSweep<true, SpecializedSweepData { IsEmpty, SweepToFreeList, BlockHasNoDestructors, DontScribble, DoesNotHaveNewlyAllocated, MarksNotStale, hasStaticAtomData, 0, 0 }>(freeList, IsEmpty, SweepToFreeList, BlockHasNoDestructors, DontScribble, DoesNotHaveNewlyAllocated, MarksNotStale, [] (VM&, JSCell*) { });
                return true;
            case MarksStale:
                specializedSweep<true, SpecializedSweepData { IsEmpty, SweepToFreeList, BlockHasNoDestructors, DontScribble, DoesNotHaveNewlyAllocated, MarksStale, hasStaticAtomData, 0, 0 }>(freeList, IsEmpty, SweepToFreeList, BlockHasNoDestructors, DontScribble, DoesNotHaveNewlyAllocated, MarksStale, [] (VM&, JSCell*) { });
                return true;
            }
            break;
        case NotEmpty:
            switch (marksMode) {
            case MarksNotStale:
                specializedSweep<true, SpecializedSweepData { NotEmpty, SweepToFreeList, BlockHasNoDestructors, DontScribble, DoesNotHaveNewlyAllocated, MarksNotStale, hasStaticAtomData, 0, 0 }>(freeList, IsEmpty, SweepToFreeList, BlockHasNoDestructors, DontScribble, DoesNotHaveNewlyAllocated, MarksNotStale, [] (VM&, JSCell*) { });
                return true;
            case MarksStale:
                specializedSweep<true, SpecializedSweepData { NotEmpty, SweepToFreeList, BlockHasNoDestructors, DontScribble, DoesNotHaveNewlyAllocated, MarksStale, hasStaticAtomData, 0, 0 }>(freeList, IsEmpty, SweepToFreeList, BlockHasNoDestructors, DontScribble, DoesNotHaveNewlyAllocated, MarksStale, [] (VM&, JSCell*) { });
                return true;
            }
            break;
        }
        
        return false;
    };
    
    if (trySpecialized()) {
        assertIsoCellSetsMatch();
        return;
    }

    // The template arguments don't matter because the first one is false.
    specializedSweep<false, SpecializedSweepData { }>(freeList, emptyMode, sweepMode, BlockHasNoDestructors, scribbleMode, newlyAllocatedMode, marksMode, [] (VM&, JSCell*) { });
    assertIsoCellSetsMatch();
}

bool MarkedBlock::Handle::isFreeListedCell(const void* target) const
{
    ASSERT(isFreeListed());
    return m_directory->isFreeListedCell(target);
}

} // namespace JSC

namespace WTF {

void printInternal(PrintStream& out, JSC::MarkedBlock::Handle::SweepMode mode)
{
    switch (mode) {
    case JSC::MarkedBlock::Handle::SweepToFreeList:
        out.print("SweepToFreeList");
        return;
    case JSC::MarkedBlock::Handle::SweepOnly:
        out.print("SweepOnly");
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

} // namespace WTF

