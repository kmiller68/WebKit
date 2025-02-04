/*
 * Copyright (C) 2011-2025 Apple Inc. All rights reserved.
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
#include "FreeList.h"
#include "IsoCellSetInlines.h"
#include "JSCJSValueInlines.h"
#include "MarkedBlockInlines.h"
#include "SweepingScope.h"
#include "VMInspector.h"
#include <wtf/CommaPrinter.h>

#if PLATFORM(COCOA)
#include <wtf/cocoa/CrashReporter.h>
#endif

#include <wtf/SimpleStats.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace MarkedBlockInternal {
#ifndef NDEBUG
static constexpr bool verbose = true;
#else
static constexpr bool verbose = false;
#endif
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
{
}

MarkedBlock::Header::~Header() = default;

void MarkedBlock::Handle::unsweepWithNoNewlyAllocated()
{
    dataLogLnIf(MarkedBlockInternal::verbose, RawPointer(this), "/", RawPointer(&blockHeader()), ": unsweepWithNoNewlyAllocated");
    Locker locker { blockHeader().m_lock };
    RELEASE_ASSERT(isFreeListed());
    // TODO: Re-cache the free list.
    clearCachedFreeList();
    // TODO: Should this retire the block?
    Locker bitLocker { m_directory->bitvectorLock() };
    ASSERT(m_directory->isFreeListed(this));
    m_directory->setIsFreeListed(this, false);
    m_directory->didFinishUsingBlock(locker, this);
}

JS_EXPORT_PRIVATE extern SimpleStats stopAllocatingStats;
SimpleStats stopAllocatingStats { };

void MarkedBlock::Handle::stopAllocating(const FreeList& freeList)
{
    Locker locker { blockHeader().m_lock };
    
    dataLogLnIf(MarkedBlockInternal::verbose, RawPointer(this), "/", RawPointer(&blockHeader()), ": MarkedBlock::Handle::stopAllocating! Heap: (", space()->markingVersion(), ", ", space()->newlyAllocatedVersion(), ") Block: (", m_freeListStatus, ", ", blockHeader().m_newlyAllocatedVersion, ")");
    m_directory->assertIsMutatorOrMutatorIsStopped();
    ASSERT(!m_directory->isAllocated(this));
    ASSERT(!m_directory->isEmpty(this));

    WTF::BitSet<atomsPerBlock> newlyAllocatedBefore;

    if (m_freeListStatus == NotFreeListed) {
        if (MarkedBlockInternal::verbose)
            dataLog("There ain't no newly allocated.\n");
        // This means that we either didn't use this block at all for allocation since last GC, we
        // stopped right after consuming the last cell of the free list, or someone had already
        // done stopAllocating() before.
        ASSERT(freeList.allocationWillFail());
        ASSERT(!m_directory->isFreeListed(this));
        directory()->didFinishUsingBlock(this);
        return;
    }
    
    if constexpr (MarkedBlockInternal::verbose) {
        WTF::dataFile().atomically([&] (PrintStream& out) {
            out.println(RawPointer(this), "/", RawPointer(&blockHeader()), ": Free list: ", freeList);
            block().dumpBits(out);
        });
    }

    // Roll back to a coherent state for heap introspection.
    HeapVersion newlyAllocatedVersion = space()->newlyAllocatedVersion();

    FreeListStatus originalFreeListStatus = m_freeListStatus;
    if (originalFreeListStatus == FreeListedAndAllocating) {
        // stopAllocatingStats.add(0);
        // Cells newly allocated from our free list are not currently marked, so we need another
        // way to tell what's live vs dead. Since we sweept this cycle we know every cell in
        // this block not on the free list is live so we just update that information.

        // TODO: Do this in one pass. Maybe we can just fill with all 1s?
        ASSERT(blockHeader().m_recordedFreeListVersion == nullHeapVersion);
        blockHeader().m_newlyAllocated.clearAll();
        blockHeader().m_newlyAllocated.setEachNthBit(m_atomsPerCell, true, m_startAtom);
    } else {
        // stopAllocatingStats.add(1);
        ASSERT(originalFreeListStatus == FreeListedRecordedAndAllocating);
        ASSERT(blockHeader().m_recordedFreeListVersion != nullHeapVersion);

        if (blockHeader().m_newlyAllocatedVersion == newlyAllocatedVersion) {
            newlyAllocatedBefore = blockHeader().m_newlyAllocated;
            blockHeader().m_newlyAllocated.merge(blockHeader().m_recordedFreeList);
        } else
            blockHeader().m_newlyAllocated = blockHeader().m_recordedFreeList;

        // TODO: Re-cache the free list here rather than destroying it.
        // blockHeader().m_recordedFreeList.clearRange(0, block().atomNumber(freeList.peekNext()));

        // TODO: Maybe this should do an aboutToMarkSlow for full GCs since we're looking at marks anyway?
        // TODO: Remove this when we fix IsoSubSets
        // if (marksMode() == MarksNotStale)
        //     blockHeader().m_newlyAllocated.merge(blockHeader().m_marks);

        // FIXME: We could have specializedSweep zap every cell on the free list and isLive could
        // tell if an isNewlyAllocated cell is on the free list in constant time by checking if
        // the cell is zapped. Probably need to fix IsoCellSet::sweepToFreeList in that case.
        blockHeader().m_recordedFreeListVersion = nullHeapVersion;
    }

    // TODO: This could do freeList.forEachInterval
    freeList.forEach([&] (HeapCell* cell) {
        // TODO: I think this would require zapping every cell on MarkedBlock allocation when !DoesNotNeedDestruction otherwise the stale bits could look like an undestructed object.
        // RELEASE_ASSERT(m_attributes.destruction == DoesNotNeedDestruction || cell->isZapped());
        if (m_attributes.destruction != DoesNotNeedDestruction)
            cell->zap(HeapCell::StopAllocating);
        locker.assertIsHolding(block().header().m_lock);
        block().clearNewlyAllocated(cell);
    }, cellSize());
    blockHeader().m_newlyAllocatedVersion = newlyAllocatedVersion;

    if constexpr (MarkedBlockInternal::verbose) {
        WTF::dataFile().atomically([&] (PrintStream& out) {
            out.println(RawPointer(this), "/", RawPointer(&blockHeader()), ": MarkedBlock::Handle::stopAllocating! now");
            block().dumpBits(out);
        });
    }

    if (space()->isMarking()) {
        // If we have marked anything it should either be from an old generation (thus copied into newlyAllocated when we did stopAllocating) or actually new
        if (!areMarksStale() && heap()->collectionScope() == CollectionScope::Full && !blockHeader().m_newlyAllocated.subsumes(blockHeader().m_marks)) {
            WTF::dataFile().atomically([&](PrintStream& out) {
                out.println(RawPointer(this), "/", RawPointer(&blockHeader()), ": newlyAllocated doesn't subsume non-stale marks at stopAllocating (", originalFreeListStatus, ") is marking: ", space()->isMarking(), " ", blockHeader().m_aboutToMarkSlowCase);
                block().dumpBits(out);
                out.print("\tnewlyAllocatedBefore:\t\t[");
                newlyAllocatedBefore.dumpHex(out);
                out.println("]");
            });
            RELEASE_ASSERT_NOT_REACHED();
        }
    }

    // ASSERT(marksMode() == MarksStale || blockHeader().m_newlyAllocated.subsumes(blockHeader().m_marks));

    m_freeListStatus = NotFreeListed;
    m_directory->releaseAssertAcquiredBitVectorLock();
    Locker bitLocker(m_directory->bitvectorLock());
    m_directory->setIsFreeListed(this, false);
    m_directory->didFinishUsingBlock(bitLocker, this);
}

void MarkedBlock::Handle::lastChanceToFinalize()
{
    // Concurrent sweeper is shut down at this point.
    m_directory->assertSweeperIsSuspended();
    m_directory->setIsAllocated(this, false);
    if (m_attributes.destruction != DoesNotNeedDestruction) {
        m_cachedFreeList.forEach([&](HeapCell* cell) {
                cell->zap(HeapCell::LastChanceToFinalize);
        }, cellSize());
        m_directory->setIsDestructible(this, true);
    }
    blockHeader().assertConcurrentWriteAccess();
    blockHeader().m_marks.clearAll();
    block().clearHasAnyMarked();
    blockHeader().m_markingVersion = heap()->objectSpace().markingVersion();
    m_weakSet.lastChanceToFinalize();
    blockHeader().m_newlyAllocated.clearAll();
    blockHeader().m_newlyAllocatedVersion = heap()->objectSpace().newlyAllocatedVersion();
    blockHeader().m_recordedFreeListVersion = nullHeapVersion;
    m_directory->startUsingBlock(NoLockingNecessary, this);
    sweep(nullptr);
}

void MarkedBlock::Handle::resumeAllocating(FreeList& freeList)
{
    BlockDirectory* directory = this->directory();
    directory->assertSweeperIsSuspended();
    directory->startUsingBlock(NoLockingNecessary, this);
    dataLogLnIf(MarkedBlockInternal::verbose, RawPointer(this), "/", RawPointer(&blockHeader()), ": MarkedBlock::Handle::resumeAllocating! (", space()->markingVersion(), ", ", space()->newlyAllocatedVersion(), ")");

    {
        Locker locker { blockHeader().m_lock };
        
        ASSERT(!directory->isAllocated(this));
        ASSERT(!isFreeListed());
        
        // TODO: is this right?
        // if (!block().hasAnyNewlyAllocated()) {
        //     if (MarkedBlockInternal::verbose)
        //         dataLog("There ain't no newly allocated.\n");
        //     // This means we had already exhausted the block when we stopped allocation.
        //     // TODO: Isn't this already clear?
        //     m_freeListStatus = NotFreeListed;
        //     directory->setIsFreeListed(this, false);
        //     freeList.clear();
        //     return;
        // }
    }

    // Re-create our free list from before stopping allocation. Note that this may return an empty
    // freelist, in which case the block will still be Marked!
    sweep(&freeList);
}

#if ENABLE(MARKEDBLOCK_TEST_DUMP_INFO)

inline void MarkedBlock::setupTestForDumpInfoAndCrash()
{
    static std::atomic<uint64_t> count = 0;
    char* blockMem = std::bit_cast<char*>(this);

    // Option set to 0 disables testing.
    if (++count == Options::markedBlockDumpInfoCount()) {
        memset(&header(), 0, sizeof(uintptr_t));
        switch (Options::markedBlockDumpInfoCount() & 0xf) {
        case 1: // Test null VM pointer.
            dataLogLn("Zeroing MarkedBlock::Header::m_vm");
            *const_cast<VM**>(&header().m_vm) = nullptr;
            break;
        case 2: // Test non-null invalid VM pointer.
            dataLogLn("Corrupting MarkedBlock::Header::m_vm");
            *const_cast<VM**>(&header().m_vm) = std::bit_cast<VM*>(0xdeadbeefdeadbeef);
            break;
        case 3: // Test contiguous and total zero byte counts: start and end zeroed.
            dataLogLn("Zeroing start and end of MarkedBlock");
            memset(blockMem, 0, blockSize / 4);
            memset(blockMem + 3 * blockSize / 4, 0, blockSize / 4);
            break;
        case 4: // Test contiguous and total zero byte counts: entire block zeroed.
            dataLogLn("Zeroing MarkedBlock");
            memset(blockMem, 0, blockSize);
            break;
        }
    }
}

#else

inline void MarkedBlock::setupTestForDumpInfoAndCrash() { }

#endif // ENABLE(MARKEDBLOCK_TEST_DUMP_INFO)

void MarkedBlock::aboutToMarkSlow(HeapVersion markingVersion, HeapCell* cell)
{
    constexpr bool verbose = false || MarkedBlockInternal::verbose;
    ASSERT(vm().heap.objectSpace().isMarking());
    setupTestForDumpInfoAndCrash();

    Locker locker { header().m_lock };
    
    if (!areMarksStale(markingVersion))
        return;

    Handle* handle = header().handlePointerForNullCheck();
    if (UNLIKELY(!handle))
        dumpInfoAndCrashForInvalidHandleV2(locker, cell);

    BlockDirectory* directory = handle->directory();
    bool isAllocated;
    {
        Locker bitLocker { directory->bitvectorLock() };
        isAllocated = directory->isAllocated(handle);
    }

    HeapVersion newlyAllocatedVersion = space()->newlyAllocatedVersion();
    if (isAllocated || !staleMarksConveyLivenessDuringMarking(markingVersion)) {
        dataLogLnIf(verbose, RawPointer(handle), "/", RawPointer(this), ": MarkedBlock::aboutToMarkSlow! Clearing marks without doing anything else. (Marking: ", markingVersion, ", NA: ", newlyAllocatedVersion, ")");
        // We already know that the block is full and is already recognized as such, or that the
        // block did not survive the previous GC (since the marks are stale). So, we can clear
        // mark bits the old fashioned way. Note that it's possible for such a block to have
        // newlyAllocated with an up-to-date version! If it does, then we want to leave the
        // newlyAllocated alone, since that means that we had allocated in this previously empty
        // block but did not fill it up, so we created a newlyAllocated.
        header().m_marks.clearAll();
        header().m_aboutToMarkSlowCase = MarkedBlock::Header::AllocatedOrAllDead;
    } else {
        ASSERT(heap()->collectionScope() == CollectionScope::Full);
        if (header().m_newlyAllocatedVersion == newlyAllocatedVersion) {
            // TODO: Correct this comment.
            // When do we get here? The block could not have been filled up. The newlyAllocated bits would
            // have had to be created since the end of the last collection. The only things that create
            // them are aboutToMarkSlow, lastChanceToFinalize, stopAllocating, and didConsumeFreeList. If it had
            // been aboutToMarkSlow, then we shouldn't be here since the marks wouldn't be stale anymore. It
            // cannot be lastChanceToFinalize. So it must be stopAllocating or didConsumeFreeList. That means that
            // we just computed the newlyAllocated bits just before the start of an increment. In the former case
            // it seems as if newlyAllocated should subsume marks however in the latter it wouldn't.

            if constexpr (verbose) {
                WTF::dataFile().atomically([&] (PrintStream& out) {
                    locker.assertIsHolding(header().m_lock);
                    out.println(RawPointer(handle), "/", RawPointer(this), ": MarkedBlock::aboutToMarkSlow! newlyAllocated version (", header().m_newlyAllocatedVersion, ") up to date or using recorded free list (", header().m_recordedFreeListVersion, "), merging.");
                    dumpBits(out);
                    out.println();
                });
            }
            // TODO: Remove this once we start caching free lists.
            ASSERT(header().m_recordedFreeListVersion == nullHeapVersion || header().m_recordedFreeListVersion == newlyAllocatedVersion);
            header().m_newlyAllocated.mergeAndClear(header().m_marks);
            header().m_aboutToMarkSlowCase = MarkedBlock::Header::NewlyAllocatedUpToDate;
        } else {
            if constexpr (verbose) {
                WTF::dataFile().atomically([&] (PrintStream& out) {
                    locker.assertIsHolding(header().m_lock);
                    out.println(RawPointer(handle), "/", RawPointer(this), ": MarkedBlock::aboutToMarkSlow! newlyAllocated version (", newlyAllocatedVersion, ", ", header().m_newlyAllocatedVersion, ") stale, setting.");
                    dumpBits(out);
                    out.println();
                });
            }
            header().m_aboutToMarkSlowCase = header().m_marks.isEmpty() ? MarkedBlock::Header::NewlyAllocatedStaleMarksEmpty : MarkedBlock::Header::NewlyAllocatedStaleMarksNonEmpty;
            header().m_newlyAllocated.setAndClear(header().m_marks);
        }
        header().m_newlyAllocatedVersion = newlyAllocatedVersion;
    }

    ASSERT(header().m_marks.isEmpty());
    clearHasAnyMarked();
    WTF::storeStoreFence();
    header().m_markingVersion = markingVersion;
    
    // This means we're the first ones to mark any object in this block.
    Locker bitLocker { directory->bitvectorLock() };
    directory->setIsMarkingNotEmpty(handle, true);
}

void MarkedBlock::resetAllocated()
{
    header().assertConcurrentWriteAccess();
    header().m_newlyAllocated.clearAll();
    header().m_recordedFreeList.clearAll();
    header().m_newlyAllocatedVersion = nullHeapVersion;
    header().m_recordedFreeListVersion = nullHeapVersion;
    ASSERT(!handle().isAllocating());
    handle().m_freeListStatus = Handle::NotFreeListed;
    handle().directory()->assertSweeperIsSuspended();
    handle().directory()->setIsFreeListed(&handle(), false);
    handle().m_cachedFreeList.clear();
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
    header().m_markingVersion = nullHeapVersion;
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

JS_EXPORT_PRIVATE extern SimpleStats didConsumeFreeListStats;
SimpleStats didConsumeFreeListStats { };

void MarkedBlock::Handle::didConsumeFreeList()
{
    Locker locker { blockHeader().m_lock };
    dataLogLnIf(MarkedBlockInternal::verbose, RawPointer(this), "/", RawPointer(&blockHeader()), ": MarkedBlock::Handle::didConsumeFreeList! newlyAllocated version (", blockHeader().m_newlyAllocatedVersion, "), free list status ", m_freeListStatus, " version (", blockHeader().m_recordedFreeListVersion, ")");

    // TODO: Delete this comment
    // If we have the current allocation version then we could have:
    // swept, allocated, stopped, resumed, swept, allocated, and gotten here.
    // Thus we need to preserve the existing newlyAllocated because they would have
    // come from the current GC.
    ASSERT(isFreeListed());
    ASSERT_IMPLIES(m_freeListStatus == FreeListedAndAllocating, blockHeader().m_recordedFreeListVersion == nullHeapVersion);
    bool isAllocated = m_freeListStatus == FreeListedAndAllocating || blockHeader().m_recordedFreeListVersion == space()->newlyAllocatedVersion();

    // didConsumeFreeListStats.add(!isAllocated);
    if (!isAllocated) {
        if (newlyAllocatedMode() == DoesNotHaveNewlyAllocated)
            blockHeader().m_newlyAllocated = blockHeader().m_recordedFreeList;
        else
            blockHeader().m_newlyAllocated.merge(blockHeader().m_recordedFreeList);
        blockHeader().m_newlyAllocatedVersion = space()->newlyAllocatedVersion();
    }

#if ASSERT_ENABLED
    if (space()->isMarking() && !isAllocated) {
        // If we have marked anything it should either be from an old generation (thus copied into newlyAllocated when we did stopAllocating) or actually new
        if (!areMarksStale() && heap()->collectionScope() == CollectionScope::Full && !blockHeader().m_newlyAllocated.subsumes(blockHeader().m_marks)) {
            WTF::dataFile().atomically([&](PrintStream& out) {
                out.println(RawPointer(this), "/", RawPointer(&blockHeader()), ": newlyAllocated doesn't subsume non-stale marks at didConsumeFreeList.");
                block().dumpBits(out);
            });
            RELEASE_ASSERT_NOT_REACHED();
        }
    }
#endif

    m_freeListStatus = NotFreeListed;
    blockHeader().m_recordedFreeListVersion = nullHeapVersion;
    Locker bitLocker(m_directory->bitvectorLock());

    ASSERT(m_cachedFreeList.isClear());
    ASSERT(!m_directory->isAllocated(this));
    m_directory->setIsAllocated(this, isAllocated);
    m_directory->setIsFreeListed(this, false);
    m_directory->didFinishUsingBlock(bitLocker, this);
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
    dataLogLnIf(MarkedBlockInternal::verbose, RawPointer(&blockHeader()), ": didAddToDirectory ", RawPointer(directory), " (", directory->subspace()->name(), ")");
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
    dataLogLnIf(MarkedBlockInternal::verbose, RawPointer(&blockHeader()), ": didRemoveFromDirectory ", RawPointer(m_directory), " (", m_directory->subspace()->name(), ")");
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
    out.println("\n\tHeap MarkingVersion: ", heap()->objectSpace().markingVersion(), " NewlyAllocatedVersion: ", heap()->objectSpace().newlyAllocatedVersion());
    out.print("\tmarks (", header().m_markingVersion, "):\t\t\t[");
    header().m_marks.dumpHex(out);
    out.print("]\n\tnewlyAllocated (", header().m_newlyAllocatedVersion, "):\t\t[");
    header().m_newlyAllocated.dumpHex(out);
    out.print("]\n\t", handle().m_freeListStatus, " (", header().m_recordedFreeListVersion, "):\t[");
    header().m_recordedFreeList.dumpHex(out);
    out.println("]");
}

void MarkedBlock::Handle::dumpState(PrintStream& out) const WTF_IGNORES_THREAD_SAFETY_ANALYSIS
{
    CommaPrinter comma;
    // TODO: This is really easy to deadlock with prints under the lock... Maybe we should require holding the bitvector lock to call this...
    // Locker locker { directory()->bitvectorLock() };
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

    SweepMode sweepMode = this->sweepMode(freeList);
    bool needsDestruction = m_attributes.destruction != DoesNotNeedDestruction
        && m_directory->isDestructible(this);

    // // TODO: Is this the right spot for this?
    ConcurrentSweeper* concurrentSweeper = vm().heap.concurrentSweeper();
    if (concurrentSweeper)
        concurrentSweeper->clearStringImplsToMainThreadDeref();

    if (UNLIKELY(isAllocated())) {
        WTF::dataFile().atomically([&] (PrintStream& out) {
            out.println("FATAL: ", RawPointer(this), "->sweep: block is allocated.");
            dumpState(out);
            out.println();
        });
        RELEASE_ASSERT_NOT_REACHED();
    }

    if (UNLIKELY(isAllocating())) {
        dataLogLn("FATAL: ", RawPointer(this), "->sweep: block is allocating (", m_freeListStatus, ").");
        RELEASE_ASSERT_NOT_REACHED();
    }

    m_weakSet.sweep();

    if (sweepMode == SweepOnly && !needsDestruction) {
        // If we don't "release" our read access without locking then the ThreadSafetyAnalysis code gets upset with the locker below.
        m_directory->releaseAssertAcquiredBitVectorLock();
        Locker locker(m_directory->bitvectorLock());
        m_directory->setIsUnswept(this, false);
        return;
    }

    if (space()->isMarking())
        blockHeader().m_lock.lockWithoutAnalysis();
    
    subspace()->didBeginSweepingToFreeList(this);
    
    auto validateSweep = [&] ALWAYS_INLINE_LAMBDA {
#if ASSERT_ENABLED
        if (subspace()->isIsoSubspace() && freeList) {
            reinterpret_cast<IsoSubspace*>(subspace())->m_cellSets.forEach([&] (IsoCellSet* set) {
                freeList->forEach([&] (HeapCell* cell) {
                    ASSERT(!set->contains(cell));
                }, cellSize());
            });
        }

        if (freeList) {
            m_weakSet.forEachBlock([&](WeakBlock& block) {
                block.validate(freeList);
            });
        }
#endif
    };

    if (needsDestruction) {
        subspace()->finishSweep(*this, freeList);
        validateSweep();
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
        validateSweep();
        return;
    }

    // The template arguments don't matter because the first one is false.
    specializedSweep<false, SpecializedSweepData { }>(freeList, emptyMode, sweepMode, BlockHasNoDestructors, scribbleMode, newlyAllocatedMode, marksMode, [] (VM&, JSCell*) { });
    validateSweep();
}

void MarkedBlock::Handle::sweepConcurrently()
{
#if ASSERT_ENABLED
    {
        Locker locker(m_directory->bitvectorLock());
        ASSERT(m_attributes.destruction != NeedsMainThreadDestruction);
        ASSERT(m_directory->isInUse(this));

        if (UNLIKELY(m_directory->isAllocated(this))) {
            WTF::dataFile().atomically([&] (PrintStream& out) {
                out.println("FATAL: ", RawPointer(this), "->sweepConcurrently: block is allocated.");
                dumpState(out);
                out.println();
            });
            RELEASE_ASSERT_NOT_REACHED();
        }
    }
#endif

    if (UNLIKELY(isAllocating())) {
        dataLogLn("FATAL: ", RawPointer(this), "->sweepConcurrently: block is allocating (", m_freeListStatus, ").");
        RELEASE_ASSERT_NOT_REACHED();
    }

    m_weakSet.sweep();

    if (space()->isMarking())
        blockHeader().m_lock.lockWithoutAnalysis();

    subspace()->didBeginSweepingToFreeListConcurrently(this);
    ASSERT(!isAllocating());
    {
        // TODO: Figure out a better mechanism for free list status...
        if (isFreeListed())
            clearCachedFreeList();
        Locker locker(m_directory->bitvectorLock());
        m_directory->setIsFreeListed(this, false);
    }

    subspace()->finishSweepConcurrently(*this);

#if ASSERT_ENABLED
    m_weakSet.forEachBlock([&](WeakBlock& block) {
        block.validate(freeList);
    });
#endif

    if (m_cachedFreeList.allocationWillFail()) {
        m_cachedFreeList.clear();
        unsweepWithNoNewlyAllocated();
        ASSERT(!isFreeListed());
        return;
    }

    ASSERT(!m_cachedFreeList.isClear());
    ASSERT(freeListStatus() == FreeListedWithRecording);
    Locker locker(m_directory->bitvectorLock());
    ASSERT(m_directory->isFreeListed(this));
    m_directory->didFinishUsingBlock(locker, this);
}

NO_RETURN_DUE_TO_CRASH NEVER_INLINE void MarkedBlock::dumpInfoAndCrashForInvalidHandleV2(AbstractLocker&, HeapCell* heapCell)
{
    VM* blockVM = header().m_vm;
    VM* actualVM = nullptr;
    bool isBlockVMValid = false;
    bool isBlockInSet = false;
    bool isBlockInDirectory = false;
    bool foundInBlockVM = false;
    size_t contiguousZeroBytesHeadOfBlock = 0;
    size_t totalZeroBytesInBlock = 0;
    uint64_t cellFirst8Bytes = 0;
    unsigned subspaceHash = 0;
    MarkedBlock::Handle* handle = nullptr;

    if (heapCell) {
        uint64_t* p = std::bit_cast<uint64_t*>(heapCell);
        cellFirst8Bytes = *p;
    }

    auto updateCrashLogMsg = [&](int line) {
#if PLATFORM(COCOA)
        StringPrintStream out;
        out.printf("INVALID HANDLE [%d]: markedBlock=%p; heapCell=%p; cellFirst8Bytes=%#llx; subspaceHash=%#x; contiguousZeros=%lu; totalZeros=%lu; blockVM=%p; actualVM=%p; isBlockVMValid=%d; isBlockInSet=%d; isBlockInDir=%d; foundInBlockVM=%d;",
            line, this, heapCell, cellFirst8Bytes, subspaceHash, contiguousZeroBytesHeadOfBlock, totalZeroBytesInBlock, blockVM, actualVM, isBlockVMValid, isBlockInSet, isBlockInDirectory, foundInBlockVM);
        auto message = out.toCString();
        WTF::setCrashLogMessage(message.data());
        dataLogLn(message.data());
#else
        UNUSED_PARAM(line);
#endif
    };
    updateCrashLogMsg(__LINE__);

    char* blockStart = std::bit_cast<char*>(this);
    bool sawNonZero = false;
    for (auto mem = blockStart; mem < blockStart + MarkedBlock::blockSize; mem++) {
        // Exclude the MarkedBlock::Header::m_lock from the zero scan since taking the lock writes a non-zero value.
        auto isMLockBytes = [blockStart](char* p) ALWAYS_INLINE_LAMBDA {
            constexpr size_t lockOffset = offsetOfHeader + OBJECT_OFFSETOF(MarkedBlock::Header, m_lock);
            size_t offset = p - blockStart;
            return lockOffset <= offset && offset < lockOffset + sizeof(MarkedBlock::Header::m_lock);
        };
        bool byteIsZero = !*mem;
        if (byteIsZero || isMLockBytes(mem)) {
            totalZeroBytesInBlock++;
            if (!sawNonZero)
                contiguousZeroBytesHeadOfBlock++;
        } else
            sawNonZero = true;
    }
    updateCrashLogMsg(__LINE__);

    VMInspector::forEachVM([&](VM& vm) {
        if (blockVM == &vm) {
            isBlockVMValid = true;
            return IterationStatus::Done;
        }
        return IterationStatus::Continue;
    });
    updateCrashLogMsg(__LINE__);

    if (isBlockVMValid) {
        MarkedSpace& objectSpace = blockVM->heap.objectSpace();
        isBlockInSet = objectSpace.blocks().set().contains(this);
        handle = objectSpace.findMarkedBlockHandleDebug(this);
        isBlockInDirectory = !!handle;
        foundInBlockVM = isBlockInSet || isBlockInDirectory;
        updateCrashLogMsg(__LINE__);
    }

    if (!foundInBlockVM) {
        // Search all VMs to see if this block belongs to any VM.
        VMInspector::forEachVM([&](VM& vm) {
            MarkedSpace& objectSpace = vm.heap.objectSpace();
            isBlockInSet = objectSpace.blocks().set().contains(this);
            handle = objectSpace.findMarkedBlockHandleDebug(this);
            isBlockInDirectory = !!handle;
            // Either of them is true indicates that the block belongs to the VM.
            if (isBlockInSet || isBlockInDirectory) {
                actualVM = &vm;
                updateCrashLogMsg(__LINE__);
                return IterationStatus::Done;
            }
            return IterationStatus::Continue;
        });
    }
    updateCrashLogMsg(__LINE__);

    if (handle && handle->directory() && handle->directory()->subspace())
        subspaceHash = handle->directory()->subspace()->nameHash();
    updateCrashLogMsg(__LINE__);

    uint64_t bitfield = 0xab00ab01ab020000;
    if (!isBlockVMValid)
        bitfield |= 1 << 7;
    if (!isBlockInSet)
        bitfield |= 1 << 6;
    if (!isBlockInDirectory)
        bitfield |= 1 << 5;
    if (!foundInBlockVM)
        bitfield |= 1 << 4;

    static_assert(MarkedBlock::blockSize < (1ull << 32));
    uint64_t zeroCounts = contiguousZeroBytesHeadOfBlock | (static_cast<uint64_t>(totalZeroBytesInBlock) << 32);

    CRASH_WITH_INFO(heapCell, cellFirst8Bytes, zeroCounts, bitfield, subspaceHash, blockVM, actualVM);
}

JS_EXPORT_PRIVATE extern SimpleStats stealFreeListOrSweepStats;
SimpleStats stealFreeListOrSweepStats { };

void MarkedBlock::Handle::stealFreeListOrSweep(FreeList& freeList)
{
    m_directory->assertIsMutatorOrMutatorIsStopped();
    ASSERT(m_directory->isInUse(this));
    ASSERT(!isAllocating());
    if (UNLIKELY(Options::mainThreadRecordsFreeList()) && !isFreeListed()) {
        sweep(&m_cachedFreeList);
        ASSERT(freeListStatus() == FreeListedWithRecording);
        ASSERT(m_directory->isFreeListed(this));
    }

    // if (m_attributes.destruction != NeedsMainThreadDestruction)
    //     stealFreeListOrSweepStats.add(freeListStatus() == FreeListedWithRecording);
    if (freeListStatus() == FreeListedWithRecording) {
        freeList.stealFrom(cachedFreeList());
        ASSERT(cachedFreeList().isClear());
        ASSERT(m_directory->isFreeListed(this));
        m_freeListStatus = FreeListedRecordedAndAllocating;
        return;
    }

    sweep(&freeList);
}

} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
