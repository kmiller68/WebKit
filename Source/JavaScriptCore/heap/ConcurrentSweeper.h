/*
 * Copyright (C) 2023 Apple Inc. All rights reserved.
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

#include <wtf/AutomaticThread.h>
#include <wtf/LocklessBag.h>

namespace JSC {

class Heap;
class BlockDirectory;

class ConcurrentSweeper final : public AutomaticThread {
    WTF_MAKE_NONCOPYABLE(ConcurrentSweeper);
    ConcurrentSweeper(const AbstractLocker&, VM&, Box<Lock>, Ref<AutomaticThreadCondition>&&);
public:
    ~ConcurrentSweeper();

    static Ref<ConcurrentSweeper> create(VM&);

    void suspendSweeping() WTF_IGNORES_THREAD_SAFETY_ANALYSIS
    {
        ASSERT(!m_isSuspended);
        m_rightToSweep.lock();
        m_isSuspended = true;
    }

    void resumeSweeping() WTF_IGNORES_THREAD_SAFETY_ANALYSIS
    {
        ASSERT(m_isSuspended);
        m_isSuspended = false;
        m_rightToSweep.unlock();
    }

    bool isSuspended() const { return m_isSuspended; }
    void maybeNotify();
    void shouldStop();

    void pushDirectoryToSweep(BlockDirectory& directory)
    {
        m_directoriesToSweep.add(directory);
    }


    void pushStringImplToMainThreadDeref(Ref<StringImpl>&& impl)
    {
        ASSERT(impl->isUniqued());
        m_stringImplsBuffer->data.append(WTFMove(impl));
    }

    void clearStringImplsToMainThreadDeref()
    {
        if (UNLIKELY(m_stringsToMainThreadDeref.head()))
            clearStringImplsToMainThreadDerefSlow();
    }

    ASCIILiteral name() const override { return "Concurrent Sweeper"_s; }
private:
    PollResult poll(const AbstractLocker&) override;
    WorkResult work() override;
    void threadIsStopping(const AbstractLocker&) override;

    void clearStringImplsToMainThreadDerefSlow();
    void flushStringImplsToMainThreadDeref();

    // Use an UnfairLock since it can boost the priority of the ConcurrentSweeper thread if needed.
    UnfairLock m_rightToSweep;

    using StringImplBag = LocklessBag<Vector<Ref<StringImpl>, 5>>;
    static_assert(!is64Bit() || hasOneBitSet(sizeof(StringImplBag::Node)));
    StringImplBag m_stringsToMainThreadDeref;

    std::unique_ptr<StringImplBag::Node> m_stringImplsBuffer;

    // If we ever decide this is too inefficient then we could do LocklessBag<std::array<BlockDirectory*, X>>
    using DirectoryBag = LocklessBag<BlockDirectory&>;
    DirectoryBag m_directoriesToSweep;

    const DirectoryBag::Node* m_currentDirectory { nullptr };
    unsigned m_currentMarkedBlockIndex { 0 };

    bool m_hasNewWork { true };
    bool m_isSuspended { false };
    bool m_shouldStop { false };
};

} // namespace JSC
