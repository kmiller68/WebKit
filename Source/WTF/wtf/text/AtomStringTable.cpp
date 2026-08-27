/*
 * Copyright (C) 2004, 2005, 2006, 2007, 2008, 2013 Apple Inc. All rights reserved.
 * Copyright (C) 2010 Patrick Gansterer <paroga@paroga.com>
 * Copyright (C) 2012 Google Inc. All rights reserved.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public License
 * along with this library; see the file COPYING.LIB.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 *
 */

#include "config.h"
#include <wtf/text/AtomStringTable.h>

#include <wtf/NeverDestroyed.h>
#include <wtf/text/AtomStringImpl.h>

namespace WTF {

struct AtomStringTableRemovalHashTranslator {
    static unsigned hash(const StringImpl* string) { return string->existingHash(); }
    static bool equal(const AtomStringTable::StringEntry& a, const StringImpl* b) { return a == b; }
};

AtomStringTable& AtomStringTable::singleton()
{
    static LazyNeverDestroyed<AtomStringTable> table;
    static std::once_flag flag;
    std::call_once(flag, [&] {
        table.construct();
    });
    return table;
}

void AtomStringTable::reserveInitialCapacityIfEmpty(unsigned keyCount)
{
    Locker locker { m_lock.write() };
    if (m_table.isEmpty())
        m_table.reserveInitialCapacity(keyCount);
}

bool AtomStringTable::releaseAndRemoveIfNeeded(AtomStringImpl* string)
{
    ASSERT(string->isAtom());
    auto& table = singleton();

    // Exclusive for the whole function, with no read-locked fast path: the exchangeSub below is the
    // decision to destroy, not a test of one made elsewhere. Under a shared lock two threads could
    // each observe a count of 2 and both destroy, and a concurrent reader could take a reference to a
    // string already committed to destruction.
    Locker locker { table.m_lock.write() };

    // The caller has not decremented: for a uniqued string the drop to the table's own reference has
    // to happen here, under the lock, so that exactly one thread observes it. add() can have taken a
    // reference between deref()'s load and our acquiring the lock, in which case this decrement just
    // drops the caller's and the string lives on. Compare counts rather than raw values, because the
    // low bits of the reference count hold the string kind.
    //
    // Acquire, because when we are the one destroying we go on to read the string's flags and run
    // its destructor, and this is where we pick up the writes of a thread that took a reference
    // and dropped it again after deref() decided. There is no need for a release half: the only
    // thread that can destroy an atom string is one that reached this function, so it acquires the
    // lock we are about to release, and the atom bit is never cleared once set.
    auto oldRefCount = string->m_refCount.exchangeSub(StringImpl::s_refCountIncrement, std::memory_order_acquire);

    if (oldRefCount / StringImpl::s_refCountIncrement != 2)
        return false;

    // Ours is gone and the table's is the only one left, so drop the entry and the reference it
    // holds. Nothing can find the string to revive it after this, because finding it means holding
    // this lock.
    if (string->length()) {
        auto iterator = table.m_table.find<AtomStringTableRemovalHashTranslator>(string);
        ASSERT(iterator != table.m_table.end());
        table.m_table.remove(iterator);
    }
    string->m_refCount.exchangeSub(StringImpl::s_refCountIncrement, std::memory_order_relaxed);
    ASSERT(!string->refCount());
    return true;
}

} // namespace WTF
