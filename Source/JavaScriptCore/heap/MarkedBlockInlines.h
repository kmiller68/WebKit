/*
 * Copyright (C) 2016-2025 Apple Inc. All rights reserved.
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

#include "BlockDirectory.h"
#include "JSCast.h"
#include "MarkedBlock.h"
#include "MarkedSpace.h"
#include "Scribble.h"
#include "SuperSampler.h"
#include "VM.h"

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {

inline unsigned MarkedBlock::Handle::cellsPerBlock() const
{
    return MarkedSpace::blockPayload / cellSize();
}

inline bool MarkedBlock::isNewlyAllocatedStale() const
{
    header().assertConcurrentReadAccess();
    return header().m_newlyAllocatedVersion != space()->newlyAllocatedVersion();
}

inline bool MarkedBlock::hasAnyNewlyAllocated()
{
    return !isNewlyAllocatedStale();
}

inline JSC::Heap* MarkedBlock::heap() const
{
    return &vm().heap;
}

inline MarkedSpace* MarkedBlock::space() const
{
    return &heap()->objectSpace();
}

inline MarkedSpace* MarkedBlock::Handle::space() const
{
    return &heap()->objectSpace();
}

inline bool MarkedBlock::staleMarksConveyLivenessDuringMarking(HeapVersion markingVersion)
{
    header().assertConcurrentReadAccess();
    return staleMarksConveyLivenessDuringMarking(header().m_markingVersion, markingVersion);
}

inline bool MarkedBlock::staleMarksConveyLivenessDuringMarking(HeapVersion myMarkingVersion, HeapVersion markingVersion)
{
    // This function tells you if an old generation object should be considered "live" even though it
    // may not survive this GC.
    // This returns true if any of these is true:
    // - We just created the block and so the bits are clear already.
    // - This block has objects marked during the last GC, and so its version was up-to-date just
    //   before the current collection did beginMarking(). This means that any objects that have
    //   their mark bit set are valid objects that were never deleted, and so are candidates for
    //   marking in any conservative scan. Using our jargon, they are "live".
    // - We did ~2^32 collections and rotated the version back to null, so we needed to hard-reset
    //   everything. If the marks had been stale, we would have cleared them. So, we can be sure that
    //   any set mark bit reflects objects marked during last GC, i.e. "live" objects.
    // It would be absurd to use this method when not collecting, since this special "one version
    // back" state only makes sense when we're in a concurrent collection and have to be
    // conservative.
    ASSERT(space()->isMarking());
    // This method makes no sense if the marking version is not stale.
    ASSERT(myMarkingVersion != markingVersion);
    if (heap()->collectionScope() == CollectionScope::Eden)
        return false;
    return myMarkingVersion == nullHeapVersion
        || MarkedSpace::nextVersion(myMarkingVersion) == markingVersion;
}

inline bool MarkedBlock::Handle::isAllocated()
{
    m_directory->assertIsMutatorOrMutatorIsStopped();
    return m_directory->isAllocated(this);
}

ALWAYS_INLINE bool MarkedBlock::Handle::isLive(HeapVersion markingVersion, HeapVersion newlyAllocatedVersion, bool isMarking, const HeapCell* cell)
{
    ASSERT(!isAllocating());
    m_directory->assertIsMutatorOrMutatorIsStopped();
    ASSERT(!m_directory->isInUse(this));
    if (m_directory->isAllocated(this))
        return true;

    // We need to do this while holding the lock because marks might be stale. In that case, newly
    // allocated will not yet be valid. Consider this interleaving.
    //
    // One thread is doing this:
    //
    // 1) IsLiveChecksNewlyAllocated: We check if newly allocated is valid. If it is valid, and the bit is
    //    set, we return true. Let's assume that this executes atomically. It doesn't have to in general,
    //    but we can assume that for the purpose of seeing this bug.
    //
    // 2) IsLiveChecksMarks: Having failed that, we check the mark bits. This step implies the rest of
    //    this function. It happens under a lock so it's atomic.
    //
    // Another thread is doing:
    //
    // 1) AboutToMarkSlow: This is the entire aboutToMarkSlow function, and let's say it's atomic. It
    //    sorta is since it holds a lock, but that doesn't actually make it atomic with respect to
    //    IsLiveChecksNewlyAllocated, since that does not hold a lock in our scenario.
    //
    // The harmful interleaving happens if we start out with a block that has stale mark bits that
    // nonetheless convey liveness during marking (the off-by-one version trick). The interleaving is
    // just:
    //
    // IsLiveChecksNewlyAllocated AboutToMarkSlow IsLiveChecksMarks
    //
    // We started with valid marks but invalid newly allocated. So, the first part doesn't think that
    // anything is live, but dutifully drops down to the marks step. But in the meantime, we clear the
    // mark bits and transfer their contents into newlyAllocated. So IsLiveChecksMarks also sees nothing
    // live. Ooops!
    //
    // Fortunately, since this is just a read critical section, we can use a CountingLock.
    //
    // Probably many users of CountingLock could use its lambda-based and locker-based APIs. But here, we
    // need to ensure that everything is ALWAYS_INLINE. It's hard to do that when using lambdas. It's
    // more reliable to write it inline instead. Empirically, it seems like how inline this is has some
    // impact on perf - around 2% on splay if you get it wrong.
    //
    // FIXME: We could try the lambda-based API with ALWAYS_INLINE_LAMBDA.

// FIXME: Make this work with thread safety analysis. The problem is that it can't see through Dependency.
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wthread-safety-analysis"
#pragma clang diagnostic ignored "-Wthread-safety-precise"
#endif


    MarkedBlock& block = this->block();
    MarkedBlock::Header& header = block.header();
    size_t atomNumber = block.atomNumber(cell);
    auto count = header.m_lock.tryOptimisticFencelessRead();
    if (count.value) {
        Dependency fenceBefore = Dependency::fence(count.input);
        MarkedBlock& fencedBlock = *fenceBefore.consume(&block);
        MarkedBlock::Header& fencedHeader = fencedBlock.header();
        MarkedBlock::Handle* fencedThis = fenceBefore.consume(this);

        ASSERT_UNUSED(fencedThis, !fencedThis->isAllocating());

        HeapVersion myNewlyAllocatedVersion = fencedHeader.m_newlyAllocatedVersion;
        if (myNewlyAllocatedVersion == newlyAllocatedVersion) {
            bool result = fencedHeader.m_newlyAllocated.get(atomNumber);
            // TODO: If the validate fails maybe we should just go straight to locking path.
            if (result && header.m_lock.fencelessValidate(count.value, Dependency::fence(result)))
                return result;
        }

        HeapVersion myMarkingVersion = fencedHeader.m_markingVersion;
        if (myMarkingVersion != markingVersion && (!isMarking || !fencedBlock.staleMarksConveyLivenessDuringMarking(myMarkingVersion, markingVersion))) {
            if (header.m_lock.fencelessValidate(count.value, Dependency::fence(myMarkingVersion)))
                return false;
        } else {
            bool result = fencedHeader.m_marks.get(block.atomNumber(cell));
            if (header.m_lock.fencelessValidate(count.value, Dependency::fence(result)))
                return result;
        }
    }
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

    Locker locker { header.m_lock };

    ASSERT(!isAllocating());

    HeapVersion myNewlyAllocatedVersion = header.m_newlyAllocatedVersion;
    if (myNewlyAllocatedVersion == newlyAllocatedVersion && header.m_newlyAllocated.get(atomNumber))
        return true;

    if (block.areMarksStale(markingVersion)) {
        if (!isMarking)
            return false;
        if (!block.staleMarksConveyLivenessDuringMarking(markingVersion))
            return false;
    }

    return header.m_marks.get(atomNumber);
}

inline bool MarkedBlock::Handle::isLive(const HeapCell* cell)
{
    return isLive(space()->markingVersion(), space()->newlyAllocatedVersion(), space()->isMarking(), cell);
}

inline bool MarkedBlock::Handle::isPendingDestruction(const HeapCell* cell)
{
    // If the block is freelisted then either:
    // (1) The cell is not on the FreeList, in which case it is newly allocated or
    // (2) The cell is on the FreeList, in which case it has already been swept.
    // In either case, the destructor is not pending.

    // TODO: ASSERT this cell type isn't concurrently sweepable. The only user of this is CodeBlock
    // if/when we want CodeBlocks to be concurrently sweepable we'd need to make watchpoint's m_owner
    // be a JSC::Weak.
    if (isFreeListed())
        return false;

    return !isLive(cell);
}

ALWAYS_INLINE bool MarkedBlock::Handle::isLiveForConservativeRoots(HeapVersion markingVersion, HeapVersion newlyAllocatedVersion, const void* p)
{
    ASSERT(vm().heap.objectSpace().isMarking());
    constexpr bool isMarking = true;

    constexpr bool verbose = false;
    if (!block().isAtom(p)) {
        dataLogLnIf(verbose, RawPointer(this), "/", RawPointer(&block()), ": MarkedBlock::Handle::isLiveForConservativeRoots! ", RawPointer(p), " is not an atom");
        return false;
    }
    bool result = isLive(markingVersion, newlyAllocatedVersion, isMarking, reinterpret_cast<const HeapCell*>(p));
    dataLogLnIf(verbose, RawPointer(this), "/", RawPointer(&block()), ": MarkedBlock::Handle::isLiveForConservativeRoots! ", RawPointer(p), " (", block().atomNumber(p), " ", marksMode(), " ", newlyAllocatedMode(), ") ", result);
    return result;
}

inline bool MarkedBlock::Handle::areMarksStaleForSweep()
{
    return marksMode() == MarksStale;
}

// The following has to be true for specialization to kick in:
//
// sweepMode == SweepToFreeList
// scribbleMode == DontScribble
// newlyAllocatedMode == DoesNotHaveNewlyAllocated
// destructionMode != BlockHasDestructorsAndCollectorIsRunning
//
// emptyMode = IsEmpty
//     destructionMode = DoesNotNeedDestruction
//         marksMode = MarksNotStale (1)
//         marksMode = MarksStale (2)
// emptyMode = NotEmpty
//     destructionMode = DoesNotNeedDestruction
//         marksMode = MarksNotStale (3)
//         marksMode = MarksStale (4)
//     destructionMode = NeedsDestruction || NeedsMainThreadDestruction
//         marksMode = MarksNotStale (5)
//         marksMode = MarksStale (6)
//
// Only the DoesNotNeedDestruction one should be specialized by MarkedBlock.

template<size_t storageSize, bool alwaysFitsOnStack>
class DeadCellStorage {
public:
    DeadCellStorage() = default;
    void append(MarkedBlock::AtomNumberType cell) { return m_deadCells.append(cell); }
    std::span<const MarkedBlock::AtomNumberType> span() const { return m_deadCells.span(); }
private:
    Vector<MarkedBlock::AtomNumberType, storageSize> m_deadCells;
};

template<size_t storageSize>
class DeadCellStorage<storageSize, true> {
public:
    DeadCellStorage() = default;
    void append(MarkedBlock::AtomNumberType cell) { m_deadCells[m_size++] = cell; }
    std::span<const MarkedBlock::AtomNumberType> span() const { return { m_deadCells.data(), m_size }; }
private:
    std::array<MarkedBlock::AtomNumberType, storageSize> m_deadCells;
    size_t m_size { 0 };
};

template<bool specialize, MarkedBlock::Handle::SpecializedSweepData specializedSweepData>
void MarkedBlock::Handle::specializedSweep(FreeList* freeList, MarkedBlock::Handle::EmptyMode emptyMode, MarkedBlock::Handle::SweepMode sweepMode, MarkedBlock::Handle::SweepDestructionMode destructionMode, MarkedBlock::Handle::ScribbleMode scribbleMode, MarkedBlock::Handle::NewlyAllocatedMode newlyAllocatedMode, MarkedBlock::Handle::MarksMode marksMode, const auto& destroyFunc)
{
#ifndef NDEBUG
    constexpr bool verbose = true;
#else
    constexpr bool verbose = false;
#endif
    size_t atomsPerCell = m_atomsPerCell;
    size_t startAtom = m_startAtom;

    if constexpr (specialize) {
        emptyMode = specializedSweepData.emptyMode;
        sweepMode = specializedSweepData.sweepMode;
        destructionMode = specializedSweepData.destructionMode;
        scribbleMode = specializedSweepData.scribbleMode;
        newlyAllocatedMode = specializedSweepData.newlyAllocatedMode;
        marksMode = specializedSweepData.marksMode;
        if constexpr (specializedSweepData.hasAtomSizeData) {
            ASSERT(atomsPerCell == specializedSweepData.atomsPerCell);
            ASSERT(startAtom == specializedSweepData.startAtom);
            atomsPerCell = specializedSweepData.atomsPerCell;
            startAtom = specializedSweepData.startAtom;
        }
    }

    ASSERT(!(destructionMode == BlockHasNoDestructors && sweepMode == SweepOnly));

    SuperSamplerScope superSamplerScope(false);

    MarkedBlock& block = this->block();
    MarkedBlock::Header& header = block.header();
    unsigned cellSize = atomsPerCell * atomSize;
    bool isMarking = space()->isMarking();
    header.m_lock.assertIsHeldWhen(isMarking);

    if constexpr (verbose) {
        WTF::dataFile().atomically([&] (PrintStream& out) {
            out.println(RawPointer(this), "/", RawPointer(&block), ": MarkedBlock::Handle::specializedSweep! (", subspace()->name(), ", ", sweepMode, ", ", newlyAllocatedMode, ", ", marksMode, ", ", destructionMode, ")");
            block.dumpBits(out);
        });
    }

    VM& vm = this->vm();
    uint64_t secret = vm.heapRandom().getUint64();
    auto destroy = [&] (void* cell) ALWAYS_INLINE_LAMBDA {
        JSCell* jsCell = static_cast<JSCell*>(cell);
        if (!jsCell->isZapped()) {
            destroyFunc(vm, jsCell);
            jsCell->zap(HeapCell::Destruction);
        }
    };

    auto setBits = [&] (bool isEmpty) ALWAYS_INLINE_LAMBDA {
        // TODO: Probably not useful in general
// #if ASSERT_ENABLED
//         if (sweepMode != SweepOnly) {
//             freeList->forEach([&] (HeapCell* cell) {
//                 ASSERT(block.isAtom(cell));
//                 if (destructionMode == BlockHasDestructors)
//                     ASSERT(cell->isZapped());
//                 if (sweepMode == SweepToFreeListAndRecord)
//                     ASSERT(header.m_freeList.get(block.atomNumber(cell)));
//             });
//         }
// #endif
        Locker locker { m_directory->bitvectorLock() };
        m_directory->setIsUnswept(this, false);
        m_directory->setIsDestructible(this, false);
        m_directory->setIsFreeListed(this, sweepMode != SweepOnly);
        m_directory->setIsEmpty(this, sweepMode == SweepOnly && isEmpty);
        ASSERT(m_directory->isFreeListed(this) == isFreeListed());
    };

    ASSERT(!isAllocating());

    switch (sweepMode) {
    case SweepOnly:
        m_freeListStatus = NotFreeListed;
        header.m_recordedFreeListVersion = nullHeapVersion;
        m_cachedFreeList.clear();
        break;
    case SweepToFreeListAndRecord:
        m_freeListStatus = FreeListedWithRecording;
        header.m_recordedFreeListVersion = space()->newlyAllocatedVersion();
        ASSERT(freeList == &m_cachedFreeList);
        break;
    case SweepToFreeList:
        m_freeListStatus = FreeListedAndAllocating;
        header.m_recordedFreeListVersion = nullHeapVersion;
        m_cachedFreeList.clear();
        break;
    }

    if (emptyMode == IsEmpty) {
#if ASSERT_ENABLED
        if (UNLIKELY(newlyAllocatedMode == HasNewlyAllocated && !header.m_newlyAllocated.isEmpty())) {
            WTF::dataFile().atomically(
                [&] (PrintStream& out) {
                    header.assertConcurrentReadAccess();
                    out.print(RawPointer(this), "/", RawPointer(&block), ": newly allocated not empty!\n");
                    out.print("Block lock is held: ", header.m_lock.isHeld(), "\n");
                    block.dumpBits(out);
                    UNREACHABLE_FOR_PLATFORM();
                });
        }

        // This is an incredibly powerful assertion that checks the sanity of our block bits.
        if (UNLIKELY(marksMode == MarksNotStale && !header.m_marks.isEmpty())) {
            WTF::dataFile().atomically(
                [&] (PrintStream& out) {
                    header.assertConcurrentReadAccess();
                    out.print(RawPointer(this), "/", RawPointer(&block), ": marks not empty!\n");
                    out.print("Block lock is held: ", header.m_lock.isHeld(), "\n");
                    block.dumpBits(out);
                    UNREACHABLE_FOR_PLATFORM();
                });
        }
#endif

        char* payloadEnd = std::bit_cast<char*>(block.atoms() + numberOfAtoms);
        char* payloadBegin = std::bit_cast<char*>(block.atoms() + startAtom);
        RELEASE_ASSERT(static_cast<size_t>(payloadEnd - payloadBegin) <= payloadSize, payloadBegin, payloadEnd, &block, cellSize, startAtom);

        if (isMarking)
            header.m_lock.unlockWithoutAnalysis();
        if (destructionMode != BlockHasNoDestructors) {
            for (char* cell = payloadBegin; cell < payloadEnd; cell += cellSize)
                destroy(cell);
        }

        // TODO: Could we do this after releasing the lock? The block is empty so I don't think anyone's looking at the newlyAllocated bits. But that also means no one wants the lock.
        if (sweepMode == SweepToFreeListAndRecord) {
            // TODO: Do these in one pass.
            header.m_recordedFreeList.clearAll();
            header.m_recordedFreeList.setEachNthBit(atomsPerCell, true, startAtom);
        }
        if (sweepMode != SweepOnly) {
            if (UNLIKELY(scribbleMode == Scribble))
                scribble(payloadBegin, payloadEnd - payloadBegin);
            FreeCell* interval = reinterpret_cast_ptr<FreeCell*>(payloadBegin);
            interval->makeLast(payloadEnd - payloadBegin, secret);
            freeList->initialize(interval, secret, payloadEnd - payloadBegin);
        }
        setBits(true);

        if constexpr (verbose) {
            WTF::dataFile().atomically([&] (PrintStream& out) {
                out.println(RawPointer(this), "/", RawPointer(&block), ": Quickly swept block with cell size ", cellSize, " and attributes ", m_attributes, ": ", pointerDump(freeList));
                block.dumpBits(out);
            });
        }
        return;
    }

    // This produces a free list that is ordered in reverse through the block.
    // This is fine, since the allocation code makes no assumptions about the
    // order of the free list.
    size_t freedBytes = 0;
    bool isEmpty = true;
    FreeCell* head = nullptr;
    size_t currentIntervalSize = 0;
    size_t previousDeadCell = 0;

    // We try to allocate the deadCells vector entirely on the stack if possible.
    // Otherwise, we use the maximum permitted space (currently 8kB) to store as
    // many elements as possible. If we know that all the atoms in the block will
    // fit in the stack buffer, however, we can use unchecked append instead of
    // checked.
    constexpr size_t maxDeadCellBufferBytes = 8 * KB; // Arbitrary limit of 8kB for stack buffer.
    constexpr size_t deadCellBufferBytes = std::min(atomsPerBlock * sizeof(AtomNumberType), maxDeadCellBufferBytes);
    static_assert(deadCellBufferBytes <= maxDeadCellBufferBytes);
    constexpr bool deadCellsAlwaysFitsOnStack = (deadCellBufferBytes / sizeof(AtomNumberType)) <= atomsPerBlock;
    DeadCellStorage<deadCellBufferBytes / sizeof(AtomNumberType), deadCellsAlwaysFitsOnStack> deadCells;

    auto handleDeadCell = [&] (size_t i) {
        HeapCell* cell = reinterpret_cast_ptr<HeapCell*>(&block.atoms()[i]);
        if (destructionMode != BlockHasNoDestructors)
            destroy(cell);
        if (sweepMode != SweepOnly) {
            if (UNLIKELY(scribbleMode == Scribble))
                scribble(cell, cellSize);

            if (sweepMode == SweepToFreeListAndRecord)
                header.m_recordedFreeList.set(i, true);

            // The following check passing implies there was at least one live cell
            // between us and the last dead cell, meaning that the previous dead
            // cell is the start of its interval.
            if (i + atomsPerCell < previousDeadCell) {
                size_t intervalLength = currentIntervalSize * atomSize;
                FreeCell* cell = reinterpret_cast_ptr<FreeCell*>(&block.atoms()[previousDeadCell]);
                if (LIKELY(head))
                    cell->setNext(head, intervalLength, secret);
                else
                    cell->makeLast(intervalLength, secret);
                freedBytes += intervalLength;
                head = cell;
                currentIntervalSize = 0;
            }
            currentIntervalSize += atomsPerCell;
            previousDeadCell = i;
        }
    };

    auto checkForFinalInterval = [&] () {
        if (sweepMode != SweepOnly && currentIntervalSize) {
            size_t intervalLength = currentIntervalSize * atomSize;
            FreeCell* cell = reinterpret_cast_ptr<FreeCell*>(&block.atoms()[previousDeadCell]);

            if (LIKELY(head))
                cell->setNext(head, intervalLength, secret);
            else
                cell->makeLast(intervalLength, secret);
            freedBytes += intervalLength;
            head = cell;
        }
    };

    // if (newlyAllocatedMode == DoesNotHaveNewlyAllocated && sweepMode == SweepToFreeListAndRecord && emptyMode == NotEmpty) {
    //     // TODO: Make 100% sure the else branch is necessary.
    //     if (marksMode == MarksStale)
    //         header.m_newlyAllocated.clearAll();
    //     else
    //         header.m_newlyAllocated = header.m_marks;
    //     header.m_newlyAllocatedVersion = space()->newlyAllocatedVersion();
    // } else
    //     ASSERT((newlyAllocatedMode == HasNewlyAllocated) == (header.m_newlyAllocatedVersion == space()->newlyAllocatedVersion()));

    // Note: we don't want to update the newly allocated version since that would confuse isLive.
    if (sweepMode == SweepToFreeListAndRecord)
        header.m_recordedFreeList.clearAll();

    for (int i = endAtom - atomsPerCell; i >= static_cast<int>(startAtom); i -= atomsPerCell) {
        if (emptyMode == NotEmpty
            && ((marksMode == MarksNotStale && header.m_marks.get(i))
                || (newlyAllocatedMode == HasNewlyAllocated && header.m_newlyAllocated.get(i)))) {
            isEmpty = false;
            continue;
        }

        if (destructionMode == BlockHasDestructorsAndCollectorIsRunning)
            deadCells.append(i);
        else
            handleDeadCell(i);
    }
    if (destructionMode != BlockHasDestructorsAndCollectorIsRunning)
        checkForFinalInterval(); // We need this to handle the first interval in the block, since it has no dead cells before it.

    if (space()->isMarking())
        header.m_lock.unlockWithoutAnalysis();

    if (destructionMode == BlockHasDestructorsAndCollectorIsRunning) {
        for (size_t i : deadCells.span())
            handleDeadCell(i);
        checkForFinalInterval();
    }

    if (sweepMode != SweepOnly)
        freeList->initialize(head, secret, freedBytes);
    setBits(isEmpty);

    if constexpr (verbose) {
        WTF::dataFile().atomically([&] (PrintStream& out) {
            out.println(RawPointer(this), "/", RawPointer(&block), ": Slowly swept block with cell size ", cellSize, " and attributes ", m_attributes, ": empty=", isEmpty, " ", pointerDump(freeList));
            block.dumpBits(out);
        });
    }
}

template<bool hasSizeData, unsigned atomsPerCell, unsigned startAtom>
void MarkedBlock::Handle::finishSweepKnowingHeapCellType(FreeList* freeList, const auto& destroyFunc)
{
    SweepDestructionMode destructionMode = this->sweepDestructionMode();
    EmptyMode emptyMode = this->emptyMode();
    ScribbleMode scribbleMode = this->scribbleMode();
    NewlyAllocatedMode newlyAllocatedMode = this->newlyAllocatedMode();
    MarksMode marksMode = this->marksMode();
    SweepMode sweepMode = this->sweepMode(freeList);

    ASSERT(newlyAllocatedMode == HasNewlyAllocated || marksMode == MarksNotStale || emptyMode == IsEmpty);

    // FIXME: It feels like we should have a cleaner way to express this but it's probably not worth the effort since
    // we only specialize in two places.
    auto trySpecialized = [&] () -> bool {
        if (scribbleMode != DontScribble)
            return false;
        if (newlyAllocatedMode != DoesNotHaveNewlyAllocated)
            return false;
        if (destructionMode != BlockHasDestructors)
            return false;

        switch (emptyMode) {
        case IsEmpty:
            switch (sweepMode) {
            case SweepOnly:
                switch (marksMode) {
                case MarksNotStale:
                    specializedSweep<true, SpecializedSweepData { IsEmpty, SweepOnly, BlockHasDestructors, DontScribble, DoesNotHaveNewlyAllocated, MarksNotStale, hasSizeData, atomsPerCell, startAtom }>(freeList, IsEmpty, SweepOnly, BlockHasDestructors, DontScribble, DoesNotHaveNewlyAllocated, MarksNotStale, destroyFunc);
                    return true;
                case MarksStale:
                    specializedSweep<true, SpecializedSweepData { IsEmpty, SweepOnly, BlockHasDestructors, DontScribble, DoesNotHaveNewlyAllocated, MarksStale, hasSizeData, atomsPerCell, startAtom }>(freeList, IsEmpty, SweepOnly, BlockHasDestructors, DontScribble, DoesNotHaveNewlyAllocated, MarksStale, destroyFunc);
                    return true;
                }
                RELEASE_ASSERT_NOT_REACHED();
            case SweepToFreeList:
                switch (marksMode) {
                case MarksNotStale:
                    specializedSweep<true, SpecializedSweepData { IsEmpty, SweepToFreeList, BlockHasDestructors, DontScribble, DoesNotHaveNewlyAllocated, MarksNotStale, hasSizeData, atomsPerCell, startAtom }>(freeList, IsEmpty, SweepToFreeList, BlockHasDestructors, DontScribble, DoesNotHaveNewlyAllocated, MarksNotStale, destroyFunc);
                    return true;
                case MarksStale:
                    specializedSweep<true, SpecializedSweepData { IsEmpty, SweepToFreeList, BlockHasDestructors, DontScribble, DoesNotHaveNewlyAllocated, MarksStale, hasSizeData, atomsPerCell, startAtom }>(freeList, IsEmpty, SweepToFreeList, BlockHasDestructors, DontScribble, DoesNotHaveNewlyAllocated, MarksStale, destroyFunc);
                    return true;
                }
            case SweepToFreeListAndRecord:
                switch (marksMode) {
                case MarksNotStale:
                    specializedSweep<true, SpecializedSweepData { IsEmpty, SweepToFreeListAndRecord, BlockHasDestructors, DontScribble, DoesNotHaveNewlyAllocated, MarksNotStale, hasSizeData, atomsPerCell, startAtom }>(freeList, IsEmpty, SweepToFreeListAndRecord, BlockHasDestructors, DontScribble, DoesNotHaveNewlyAllocated, MarksNotStale, destroyFunc);
                    return true;
                case MarksStale:
                    specializedSweep<true, SpecializedSweepData { IsEmpty, SweepToFreeListAndRecord, BlockHasDestructors, DontScribble, DoesNotHaveNewlyAllocated, MarksStale, hasSizeData, atomsPerCell, startAtom }>(freeList, IsEmpty, SweepToFreeListAndRecord, BlockHasDestructors, DontScribble, DoesNotHaveNewlyAllocated, MarksStale, destroyFunc);
                    return true;
                }
            }
            RELEASE_ASSERT_NOT_REACHED();
        case NotEmpty:
            switch (sweepMode) {
            case SweepOnly:
                switch (marksMode) {
                case MarksNotStale:
                    specializedSweep<true, SpecializedSweepData { NotEmpty, SweepOnly, BlockHasDestructors, DontScribble, DoesNotHaveNewlyAllocated, MarksNotStale, hasSizeData, atomsPerCell, startAtom }>(freeList, NotEmpty, SweepOnly, BlockHasDestructors, DontScribble, DoesNotHaveNewlyAllocated, MarksNotStale, destroyFunc);
                    return true;
                case MarksStale:
                    specializedSweep<true, SpecializedSweepData { NotEmpty, SweepOnly, BlockHasDestructors, DontScribble, DoesNotHaveNewlyAllocated, MarksStale, hasSizeData, atomsPerCell, startAtom }>(freeList, NotEmpty, SweepOnly, BlockHasDestructors, DontScribble, DoesNotHaveNewlyAllocated, MarksStale, destroyFunc);
                    return true;
                }
                RELEASE_ASSERT_NOT_REACHED();
            case SweepToFreeList:
                switch (marksMode) {
                case MarksNotStale:
                    specializedSweep<true, SpecializedSweepData { NotEmpty, SweepToFreeList, BlockHasDestructors, DontScribble, DoesNotHaveNewlyAllocated, MarksNotStale, hasSizeData, atomsPerCell, startAtom }>(freeList, NotEmpty, SweepToFreeList, BlockHasDestructors, DontScribble, DoesNotHaveNewlyAllocated, MarksNotStale, destroyFunc);
                    return true;
                case MarksStale:
                    specializedSweep<true, SpecializedSweepData { NotEmpty, SweepToFreeList, BlockHasDestructors, DontScribble, DoesNotHaveNewlyAllocated, MarksStale, hasSizeData, atomsPerCell, startAtom }>(freeList, NotEmpty, SweepToFreeList, BlockHasDestructors, DontScribble, DoesNotHaveNewlyAllocated, MarksStale, destroyFunc);
                    return true;
                }
            case SweepToFreeListAndRecord:
                switch (marksMode) {
                case MarksNotStale:
                    specializedSweep<true, SpecializedSweepData { NotEmpty, SweepToFreeListAndRecord, BlockHasDestructors, DontScribble, DoesNotHaveNewlyAllocated, MarksNotStale, hasSizeData, atomsPerCell, startAtom }>(freeList, NotEmpty, SweepToFreeListAndRecord, BlockHasDestructors, DontScribble, DoesNotHaveNewlyAllocated, MarksNotStale, destroyFunc);
                    return true;
                case MarksStale:
                    specializedSweep<true, SpecializedSweepData { NotEmpty, SweepToFreeListAndRecord, BlockHasDestructors, DontScribble, DoesNotHaveNewlyAllocated, MarksStale, hasSizeData, atomsPerCell, startAtom }>(freeList, NotEmpty, SweepToFreeListAndRecord, BlockHasDestructors, DontScribble, DoesNotHaveNewlyAllocated, MarksStale, destroyFunc);
                    return true;
                }
            }
        }

        return false;
    };

    if (trySpecialized())
        return;

    // The template arguments don't matter because the first one is false.
    specializedSweep<false, SpecializedSweepData { }>(freeList, emptyMode, sweepMode, destructionMode, scribbleMode, newlyAllocatedMode, marksMode, destroyFunc);
}

inline MarkedBlock::Handle::SweepMode MarkedBlock::Handle::sweepMode(FreeList* freeList)
{
    if (!freeList)
        return SweepOnly;
    return freeList == &m_cachedFreeList ? SweepToFreeListAndRecord : SweepToFreeList;
}

inline MarkedBlock::Handle::SweepDestructionMode MarkedBlock::Handle::sweepDestructionMode()
{
    if (m_attributes.destruction != DoesNotNeedDestruction) {
        if (space()->isMarking())
            return BlockHasDestructorsAndCollectorIsRunning;
        return BlockHasDestructors;
    }
    return BlockHasNoDestructors;
}

inline bool MarkedBlock::Handle::isEmpty()
{
    m_directory->assertIsMutatorOrMutatorIsStopped();
    return m_directory->isEmpty(this);
}

inline MarkedBlock::Handle::EmptyMode MarkedBlock::Handle::emptyMode()
{
    // It's not obvious, but this is the only way to know if the block is empty. It's the only
    // bit that captures these caveats:
    // - It's true when the block is freshly allocated.
    // - It's true if the block had been swept in the past, all destructors were called, and that
    //   sweep proved that the block is empty.
    Locker locker(m_directory->bitvectorLock());
    return m_directory->isEmpty(this) ? IsEmpty : NotEmpty;
}

inline MarkedBlock::Handle::ScribbleMode MarkedBlock::Handle::scribbleMode()
{
    return scribbleFreeCells() ? Scribble : DontScribble;
}

inline MarkedBlock::Handle::NewlyAllocatedMode MarkedBlock::Handle::newlyAllocatedMode()
{
    return block().hasAnyNewlyAllocated() ? HasNewlyAllocated : DoesNotHaveNewlyAllocated;
}

inline MarkedBlock::Handle::MarksMode MarkedBlock::Handle::marksMode()
{
    HeapVersion markingVersion = space()->markingVersion();
    bool marksAreUseful = !block().areMarksStale(markingVersion);
    if (!marksAreUseful && space()->isMarking())
        marksAreUseful = block().staleMarksConveyLivenessDuringMarking(markingVersion);
    return marksAreUseful ? MarksNotStale : MarksStale;
}

template <typename Functor>
inline IterationStatus MarkedBlock::Handle::forEachLiveCell(const Functor& functor)
{
    // FIXME: This is not currently efficient to use in the constraint solver because isLive() grabs a
    // lock to protect itself from concurrent calls to aboutToMarkSlow(). But we could get around this by
    // having this function grab the lock before and after the iteration, and check if the marking version
    // changed. If it did, just run again. Inside the loop, we only need to ensure that if a race were to
    // happen, we will just overlook objects. I think that because of how aboutToMarkSlow() does things,
    // a race ought to mean that it just returns false when it should have returned true - but this is
    // something that would have to be verified carefully.
    //
    // NOTE: Some users of forEachLiveCell require that their callback is called exactly once for
    // each live cell. We could optimize this function for those users by using a slow loop if the
    // block is in marks-mean-live mode. That would only affect blocks that had partial survivors
    // during the last collection and no survivors (yet) during this collection.
    //
    // https://bugs.webkit.org/show_bug.cgi?id=180315

    HeapCell::Kind kind = m_attributes.cellKind;
    for (size_t i = m_startAtom; i < endAtom; i += m_atomsPerCell) {
        HeapCell* cell = reinterpret_cast_ptr<HeapCell*>(&m_block->atoms()[i]);
        if (!isLive(cell))
            continue;

        if (functor(i, cell, kind) == IterationStatus::Done)
            return IterationStatus::Done;
    }
    return IterationStatus::Continue;
}

template <typename Functor>
inline IterationStatus MarkedBlock::Handle::forEachDeadCell(const Functor& functor)
{
    HeapCell::Kind kind = m_attributes.cellKind;
    for (size_t i = m_startAtom; i < endAtom; i += m_atomsPerCell) {
        HeapCell* cell = reinterpret_cast_ptr<HeapCell*>(&m_block->atoms()[i]);
        if (isLive(cell))
            continue;

        if (functor(cell, kind) == IterationStatus::Done)
            return IterationStatus::Done;
    }
    return IterationStatus::Continue;
}

template <typename Functor>
inline IterationStatus MarkedBlock::Handle::forEachMarkedCell(const Functor& functor)
{
    HeapCell::Kind kind = m_attributes.cellKind;
    MarkedBlock& block = this->block();
    bool areMarksStale = block.areMarksStale();
    WTF::loadLoadFence();
    if (areMarksStale)
        return IterationStatus::Continue;
    block.header().assertConcurrentReadAccess();
    for (size_t i = m_startAtom; i < endAtom; i += m_atomsPerCell) {
        if (!block.header().m_marks.get(i))
            continue;

        HeapCell* cell = reinterpret_cast_ptr<HeapCell*>(&m_block->atoms()[i]);

        if (functor(i, cell, kind) == IterationStatus::Done)
            return IterationStatus::Done;
    }
    return IterationStatus::Continue;
}

constexpr std::pair<unsigned, unsigned> MarkedBlock::Handle::atomsPerCellAndStartAtom(size_t cellSize)
{
    unsigned atomsPerCell = (cellSize + atomSize - 1) / atomSize;

    // Discount the payload atoms at the front so that startAtom can start on an atom such that
    // atomsPerCell increments from startAtom will get us exactly to endAtom when we have filled
    // up the payload region using bump allocation. This makes simplifies the computation of the
    // termination condition for iteration later.
    size_t numberOfUnallocatableAtoms = numberOfPayloadAtoms % atomsPerCell;
    unsigned startAtom = firstPayloadRegionAtom + numberOfUnallocatableAtoms;
    ASSERT_UNDER_CONSTEXPR_CONTEXT(startAtom < firstPayloadRegionAtom + atomsPerCell);
    return { atomsPerCell, startAtom };
}

// TODO: Fix WTF_IGNORES_THREAD_SAFETY_ANALYSIS
inline void MarkedBlock::Handle::clearCachedFreeList() WTF_IGNORES_THREAD_SAFETY_ANALYSIS
{
    ASSERT(isFreeListed());
    m_freeListStatus = NotFreeListed;
    // TODO: Not sure if this is needed.
    blockHeader().m_recordedFreeListVersion = nullHeapVersion;
    m_cachedFreeList.clear();
}

inline bool MarkedBlock::testAndSetMarked(const void* p, Dependency dependency)
{
    assertMarksNotStale();
    header().assertConcurrentWriteAccess();
    // dataLogLn(RawPointer(this), ": MarkedBlock::testAndSetMarked! (", atomNumber(p), ")");
    return header().m_marks.concurrentTestAndSet(atomNumber(p), dependency);
}

} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
