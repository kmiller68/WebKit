/*
 * Copyright (C) 2007-2026 Apple Inc. All rights reserved.
 * Copyright (C) 2007 Justin Haygood (jhaygood@reaktix.com)
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1.  Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 * 2.  Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
 * ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#include <wtf/Atomics.h>
#include <wtf/FastMalloc.h>
#include <wtf/MainThread.h>
#include <wtf/Noncopyable.h>
#include <wtf/RefCounted.h>
#include <wtf/SwiftBridging.h>

namespace WTF {

class WTF_EMPTY_BASE_CLASS ThreadSafeRefCountedBase {
    WTF_MAKE_NONCOPYABLE(ThreadSafeRefCountedBase);
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(ThreadSafeRefCountedBase);
public:
    void ref(std::memory_order order) const
    {
        m_refCountDebugger.willRef(m_refCount.loadRelaxed(), RefCountIsThreadSafe::Yes);
        m_refCount.exchangeAdd(1, order);
    }

    bool hasOneRef() const { return m_refCount.loadRelaxed() == 1; }
    uint32_t refCount() const { return m_refCount.loadRelaxed(); }

    // Debug APIs
    void adopted() { m_refCountDebugger.adopted(); }
    void relaxAdoptionRequirement() { m_refCountDebugger.relaxAdoptionRequirement(); }
    void disableThreadingChecks() { m_refCountDebugger.disableThreadingChecks(); }
    RefCountDebugger& refCountDebugger() { return m_refCountDebugger; }

protected:
    ThreadSafeRefCountedBase()
    {
        // FIXME: Lots of subclasses violate our adoption requirements. Migrate
        // this call into only those subclasses that need it.
        m_refCountDebugger.relaxAdoptionRequirement();
    }

    ~ThreadSafeRefCountedBase()
    {
        m_refCountDebugger.willDestroy(m_refCount.loadRelaxed());
        // FIXME: Test performance, then change this to RELEASE_ASSERT.
        ASSERT(m_refCount.loadRelaxed() == 1);
    }

    // Returns true if the pointer should be freed.
    bool derefBase() const
    {
        m_refCountDebugger.willDeref(m_refCount.loadRelaxed(), RefCountIsThreadSafe::Yes);

        // Use memory_order_release to synchronize-with the acquire fence below when the ref count
        // reaches zero. This ensures the destroying thread sees all modifications made to the object
        // before the last deref. The release also prevents loads from sinking to after the decrement
        // on non-destroying threads, which could otherwise access deallocated memory.
        //
        // Note: this release ordering does *NOT* provide synchronization for accesses to the object
        // while it's still alive - callers must use external synchronization if they access the
        // object's data from multiple threads. In particular, when first publishing a shared object
        // to other threads some store barrier must be issued before publication e.g. a Lock or a
        // store fence. In theory a deref is sufficient but such code is brittle to refactoring so
        // it is preferred to use explicit synchronization.
        if ((m_refCount.exchangeSub(1, std::memory_order_release)) == 1) [[unlikely]] {

            m_refCount.storeRelaxed(1, std::memory_order_acquire);
            m_refCountDebugger.willDelete();
            return true;
        }

        return false;
    }

private:
    mutable Atomic<uint32_t> m_refCount { 1 };
    NO_UNIQUE_ADDRESS RefCountDebugger m_refCountDebugger;
};


template<class T, DestructionThread destructionThread = DestructionThread::Any, std::memory_order order = std::memory_order_relaxed>
class ThreadSafeRefCounted : public ThreadSafeRefCountedBase {
public:
    void ref() const
    {
        ThreadSafeRefCountedBase::ref(order);
    }

    void deref() const
    {
        if (!derefBase())
            return;

        if constexpr (destructionThread == DestructionThread::Any) {
            delete static_cast<const T*>(this);
        } else if constexpr (destructionThread == DestructionThread::Main) {
            SUPPRESS_UNCOUNTED_LAMBDA_CAPTURE ensureOnMainThread([this] {
                delete static_cast<const T*>(this);
            });
        } else if constexpr (destructionThread == DestructionThread::MainRunLoop) {
            SUPPRESS_UNCOUNTED_LAMBDA_CAPTURE ensureOnMainRunLoop([this] {
                delete static_cast<const T*>(this);
            });
        } else
            STATIC_ASSERT_NOT_REACHED_FOR_VALUE(destructionThread, "Unexpected destructionThread enumerator value");
    }

protected:
    ThreadSafeRefCounted() = default;
    ~ThreadSafeRefCounted() = default;
} SWIFT_RETURNED_AS_UNRETAINED_BY_DEFAULT;

template<typename T, DestructionThread destructionThread = DestructionThread::Any>
using DeprecatedThreadSafeRefCountedSeqCst = ThreadSafeRefCounted<T, destructionThread, std::memory_order_seq_cst>;

inline void adopted(ThreadSafeRefCountedBase* object)
{
    if (!object)
        return;
    object->adopted();
}

} // namespace WTF

using WTF::ThreadSafeRefCounted;
using WTF::DeprecatedThreadSafeRefCountedSeqCst;
