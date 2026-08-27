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

#pragma once

#include <wtf/CompactPtr.h>
#include <wtf/HashSet.h>
#include <wtf/Packed.h>
#include <wtf/ReadWriteLock.h>
#include <wtf/text/StringHash.h>
#include <wtf/text/StringImpl.h>

namespace WTF {

class AtomStringImpl;

// Atomization is read-mostly: almost every add() finds an existing entry and only takes a reference
// to it, which is a pure lookup and has no reason to exclude other lookups. Hence a reader-writer
// lock rather than an exclusive one.
//
// Nothing reached with this lock held may reenter the table, because ReadWriteLock is recursive in
// neither mode, and the read side fails worse than the write side: a second read lock deadlocks if a
// writer announces itself in between, which makes it an intermittent hang rather than a reliable one,
// and upgrading one of two read locks held by the same thread deadlocks outright.
class AtomStringTable {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(AtomStringTable);
public:
    // If CompactPtr is 32bit, it is more efficient than PackedPtr (6 bytes).
    // We select underlying implementation based on CompactPtr's efficacy.
    using StringEntry = std::conditional_t<CompactPtrTraits<StringImpl>::is32Bit, CompactPtr<StringImpl>, PackedPtr<StringImpl>>;
    using StringTableImpl = UncheckedKeyHashSet<StringEntry>;

    WTF_EXPORT_PRIVATE static AtomStringTable& singleton();

    ReadWriteLock& lock() LIFETIME_BOUND WTF_RETURNS_LOCK(m_lock) { return m_lock; }

    // Two accessors rather than one, catching two different mistakes. The annotations catch touching
    // the table with no lock held, or with a shared lock where an exclusive one is required. The
    // const on the shared accessor is what catches mutating the table under the shared lock, which
    // the annotations cannot see: constness applies to the pointer and not the pointee, so a probe
    // still hands back a writable slot, but add() and remove() do not compile.
    const StringTableImpl& tableUnderSharedLock() const LIFETIME_BOUND WTF_REQUIRES_SHARED_LOCK(m_lock) { return m_table; }
    StringTableImpl& tableUnderExclusiveLock() LIFETIME_BOUND WTF_REQUIRES_LOCK(m_lock) { return m_table; }

    WTF_EXPORT_PRIVATE static bool releaseAndRemoveIfNeeded(AtomStringImpl*);
    WTF_EXPORT_PRIVATE void reserveInitialCapacityIfEmpty(unsigned);

private:
    ReadWriteLock m_lock;
    StringTableImpl m_table WTF_GUARDED_BY_LOCK(m_lock);
};

}
using WTF::AtomStringTable;
