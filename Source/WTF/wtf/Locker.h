/*
 * Copyright (C) 2008-2020 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1.  Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 * 2.  Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 * 3.  Neither the name of Apple Inc. ("Apple") nor the names of
 *     its contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE AND ITS CONTRIBUTORS "AS IS" AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL APPLE OR ITS CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#include <wtf/Compiler.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

#include <mutex>
#include <wtf/Assertions.h>
#include <wtf/Atomics.h>
#include <wtf/Compiler.h>
#include <wtf/ForbidHeapAllocation.h>
#include <wtf/Noncopyable.h>
#include <wtf/ThreadSafetyAnalysis.h>
#include <wtf/ThreadSanitizerSupport.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

namespace WTF {

enum NoLockingNecessaryTag { NoLockingNecessary };

class WTF_CAPABILITY_LOCK WordLock;
class WTF_CAPABILITY_LOCK UnfairLock;
class WTF_CAPABILITY_LOCK ReadWriteLock;
// Views of a ReadWriteLock that select a mode. The write side behaves like any other exclusive lock
// and joins the specialization below; the read side needs its own, because acquiring shared cannot be
// expressed by a Locker that always acquires exclusively.
class WTF_CAPABILITY_LOCK ReadLockView;
class WTF_CAPABILITY_LOCK WriteLockView;

class AbstractLocker {
    WTF_MAKE_NONCOPYABLE(AbstractLocker);
public:
    AbstractLocker(NoLockingNecessaryTag)
    {
    }
    
protected:
    AbstractLocker()
    {
    }
};

template<typename T> class Locker;
template<typename T> class DropLockForScope;

using AdoptLockTag = std::adopt_lock_t;
constexpr AdoptLockTag AdoptLock;

// Locker specialization to use with Lock, WordLock, and UnfairLock that integrates with thread safety analysis.
// Non-movable simple scoped lock holder.
// Example: Locker locker { m_lock };
template<typename T>
#if ENABLE(UNFAIR_LOCK)
    requires (std::same_as<T, Lock> || std::same_as<T, WordLock> || std::same_as<T, UnfairLock> || std::same_as<T, WriteLockView>)
#else
    requires (std::same_as<T, Lock> || std::same_as<T, WordLock> || std::same_as<T, WriteLockView>)
#endif
class WTF_CAPABILITY_SCOPED_LOCK Locker<T> : public AbstractLocker {
public:
    explicit Locker(T& lock) WTF_ACQUIRES_LOCK(lock)
        : m_lock(lock)
        , m_isLocked(true)
    {
        m_lock.lock();
        TSAN_ANNOTATE_HAPPENS_AFTER(&m_lock);
    }
    Locker(AdoptLockTag, T& lock) WTF_REQUIRES_LOCK(lock)
        : m_lock(lock)
        , m_isLocked(true)
    {
    }
    ~Locker() WTF_RELEASES_LOCK()
    {
        if (m_isLocked) {
            TSAN_ANNOTATE_HAPPENS_BEFORE(&m_lock);
            m_lock.unlock();
        }
    }
    void unlockEarly() WTF_RELEASES_LOCK()
    {
        ASSERT(m_isLocked);
        m_isLocked = false;
        TSAN_ANNOTATE_HAPPENS_BEFORE(&m_lock);
        m_lock.unlock();
    }
    Locker(const Locker<T>&) = delete;
    Locker& operator=(const Locker<T>&) = delete;

    void assertIsHolding(T& lock) WTF_ASSERTS_ACQUIRED_LOCK(lock)
    {
        ASSERT(m_isLocked);
        ASSERT(&lock == &m_lock);
        lock.assertIsOwner();
    }

private:
    // Support DropLockForScope even though it doesn't support thread safety analysis.
    template<typename>
    friend class DropLockForScope;

    // Support Condition class to access private lock/unlock methods
    friend class Condition;

    void lock() WTF_ACQUIRES_LOCK(m_lock)
    {
        m_lock.lock();
        TSAN_ANNOTATE_HAPPENS_AFTER(&m_lock);
        compilerFence();
    }

    void unlock() WTF_RELEASES_LOCK(m_lock)
    {
        compilerFence();
        TSAN_ANNOTATE_HAPPENS_BEFORE(&m_lock);
        m_lock.unlock();
    }

    T& m_lock;
    bool m_isLocked { false };
};

// Locker specialization for the read side of a ReadWriteLock, which acquires shared rather than
// exclusive. Without this the analysis either has to be skipped or has to misreport the mode, and
// misreporting it means a write to guarded data under a read lock goes unnoticed.
// Example: Locker locker { m_rwLock.read() };
template<typename T>
    requires (std::same_as<T, ReadLockView>)
class WTF_CAPABILITY_SCOPED_LOCK Locker<T> : public AbstractLocker {
public:
    explicit Locker(T& lock) WTF_ACQUIRES_SHARED_LOCK(lock)
        : m_lock(lock)
    {
        m_lock.lock();
        TSAN_ANNOTATE_HAPPENS_AFTER(&m_lock);
    }

    // Generic rather than shared, because the scoped-lockable convention has the destructor release
    // the capability without restating the mode it was taken in.
    ~Locker() WTF_RELEASES_GENERIC_LOCK()
    {
        if (m_isLocked) {
            TSAN_ANNOTATE_HAPPENS_BEFORE(&m_lock);
            m_lock.unlock();
        }
    }

    // Turn the read lock into a write lock. Either way this locker's read lock is gone afterwards, so
    // it stops releasing anything of its own; the annotation says the same thing to the analysis that
    // m_isLocked says at runtime. See ReadWriteLock::tryUpgrade() for why failure gives up the read
    // lock too.
    //
    // This is the only way to upgrade, and it exists because a read Locker has to be told its read
    // lock is gone. Take ownership of the write lock by adopting it:
    //     Locker readLocker { m_lock.read() };
    //     if (readLocker.tryUpgrade(m_lock)) {
    //         Locker writeLocker { AdoptLock, m_lock.write() };
    //         ...
    //     }
    // The lock is passed even though this locker holds it, so that the capability the analysis sees
    // acquired is the caller's own expression for it. Without that the adopt below cannot be checked,
    // because a capability named through this locker's member does not unify with the one the caller's
    // data is guarded by.
    bool tryUpgrade(ReadWriteLock& lock) WTF_RELEASES_GENERIC_LOCK() WTF_ACQUIRES_LOCK_IF(true, lock);

    Locker(const Locker<T>&) = delete;
    Locker& operator=(const Locker<T>&) = delete;

private:
    T& m_lock;
    bool m_isLocked { true };
};

// Unspecialized Locker that skips thread safety analysis.
template<typename T>
class [[nodiscard]] Locker : public AbstractLocker { // NOLINT
public:
    explicit Locker(T& lockable) : m_lockable(&lockable) { lock(); }
    explicit Locker(T* lockable) : m_lockable(lockable) { lock(); }
    explicit Locker(AdoptLockTag, T& lockable) : m_lockable(&lockable) { }

    // You should be wary of using this constructor. It's only applicable
    // in places where there is a locking protocol for a particular object
    // but it's not necessary to engage in that protocol yet. For example,
    // this often happens when an object is newly allocated and it can not
    // be accessed concurrently.
    Locker(NoLockingNecessaryTag) : m_lockable(nullptr) { }
    
    Locker(std::underlying_type_t<NoLockingNecessaryTag>) = delete;

    ~Locker()
    {
        unlock();
    }
    
    static Locker tryLock(T& lockable)
    {
        Locker result(NoLockingNecessary);
        if (lockable.tryLock()) {
            result.m_lockable = &lockable;
            return result;
        }
        return result;
    }

    T* lockable() { return m_lockable; }
    
    explicit operator bool() const { return !!m_lockable; }
    
    void unlockEarly()
    {
        unlock();
        m_lockable = nullptr;
    }
    
    // It's great to be able to pass lockers around. It enables custom locking adaptors like
    // JSC::LockDuringMarking.
    Locker(Locker&& other)
        : m_lockable(other.m_lockable)
    {
        ASSERT(&other != this);
        other.m_lockable = nullptr;
    }
    
    Locker& operator=(Locker&& other)
    {
        ASSERT(&other != this);
        m_lockable = other.m_lockable;
        other.m_lockable = nullptr;
        return *this;
    }
    
private:
    template<typename>
    friend class DropLockForScope;

    void unlock()
    {
        compilerFence();
        if (m_lockable) {
            TSAN_ANNOTATE_HAPPENS_BEFORE(m_lockable);
            m_lockable->unlock();
        }
    }

    void lock()
    {
        if (m_lockable) {
            m_lockable->lock();
            TSAN_ANNOTATE_HAPPENS_AFTER(m_lockable);
        }
        compilerFence();
    }
    
    T* m_lockable;
};

template<typename LockType>
class DropLockForScope : private AbstractLocker {
    WTF_FORBID_HEAP_ALLOCATION(DropLockForScope);
public:
    DropLockForScope(Locker<LockType>& lock)
        : m_lock(lock)
    {
        m_lock.unlock();
    }

    ~DropLockForScope()
    {
        m_lock.lock();
    }

private:
    Locker<LockType>& m_lock;
};

// This is a close replica of Locker, but for generic lock/unlock functions.
template<typename T, void (lockFunction)(T*), void (*unlockFunction)(T*)>
class ExternalLocker: public WTF::AbstractLocker {
public:
    explicit ExternalLocker(T* lockable)
        : m_lockable(lockable)
    {
        ASSERT(lockable);
        lock();
    }

    ~ExternalLocker()
    {
        unlock();
    }

    T* lockable() { return m_lockable; }

    explicit operator bool() const { return !!m_lockable; }

    void unlockEarly()
    {
        unlock();
        m_lockable = nullptr;
    }

    ExternalLocker(ExternalLocker&& other)
        : m_lockable(other.m_lockable)
    {
        ASSERT(&other != this);
        other.m_lockable = nullptr;
    }

    ExternalLocker& operator=(ExternalLocker&& other)
    {
        ASSERT(&other != this);
        m_lockable = other.m_lockable;
        other.m_lockable = nullptr;
        return *this;
    }

private:
    template<typename>
    friend class DropLockForScope;

    void unlock()
    {
        if (m_lockable)
            unlockFunction(m_lockable);
    }

    void lock()
    {
        if (m_lockable)
            lockFunction(m_lockable);
    }

    T* m_lockable;
};

Locker(Lock&) -> Locker<Lock>;
Locker(AdoptLockTag, Lock&) -> Locker<Lock>;

Locker(WordLock&) -> Locker<WordLock>;
Locker(AdoptLockTag, WordLock&) -> Locker<WordLock>;

#if ENABLE(UNFAIR_LOCK)
Locker(UnfairLock&) -> Locker<UnfairLock>;
Locker(AdoptLockTag, UnfairLock&) -> Locker<UnfairLock>;
#endif // ENABLE(UNFAIR_LOCK)

}

using WTF::AbstractLocker;
using WTF::AdoptLock;
using WTF::Locker;
using WTF::NoLockingNecessaryTag;
using WTF::NoLockingNecessary;
using WTF::DropLockForScope;
using WTF::ExternalLocker;
