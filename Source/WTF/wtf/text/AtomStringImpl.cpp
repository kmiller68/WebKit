/*
 * Copyright (C) 2004-2022 Apple Inc. All rights reserved.
 * Copyright (C) 2010 Patrick Gansterer <paroga@paroga.com>
 * Copyright (C) 2012 Google Inc. All rights reserved.
 * Copyright (C) 2015 Yusuke Suzuki<utatane.tea@gmail.com>. All rights reserved.
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
#include <wtf/text/AtomStringImpl.h>

#include <wtf/text/ASCIIFastPath.h>
#include <wtf/text/AtomStringTable.h>
#include <wtf/text/WTFString.h>

namespace WTF {

using namespace Unicode;

IGNORE_CLANG_WARNINGS_BEGIN("missing-noreturn")
// Always destroyed via StringImpl::destroy().
AtomStringImpl::~AtomStringImpl()
{
    RELEASE_ASSERT_NOT_REACHED();
}
IGNORE_CLANG_WARNINGS_END

// Interning is read-mostly, so probe under the shared lock first: the common case is a hit, which is
// a pure lookup and only has to take a reference. A miss upgrades in place and inserts at the slot the
// probe already found.
//
// The table holds a reference of its own: for a new entry it is the one the translator leaked into the
// entry, which is why nothing is adopted here. Either way the caller gets a reference of its own, so
// an atom string reached from the table always has a count of at least two.
template<typename T, typename HashTranslator>
static inline Ref<AtomStringImpl> addToStringTable(const T& value)
{
    auto& table = AtomStringTable::singleton();
    {
        Locker readLocker { table.lock().read() };
        auto& atomStringTable = table.tableUnderSharedLock();
        // findSlotForInsert() requires storage, and a table with none has nothing to find.
        if (atomStringTable.capacity()) [[likely]] {
            auto lookup = atomStringTable.findSlotForInsert<HashTranslator>(value);
            // The reference is taken before the locker leaves scope. Entries are only removed under
            // the exclusive lock, so no removal can run while this reader is counted.
            if (lookup.found) [[likely]]
                return *uncheckedDowncast<AtomStringImpl>(lookup.slot->get());

            // A successful upgrade admits no other writer in between, so the slot and hash just
            // probed for are still the right ones and the insert skips a second probe. It fails only
            // to a competing writer, which is exactly the case where the slot would have been
            // invalidated anyway; then nothing is held and the slot is meaningless.
            if (readLocker.tryUpgrade(table.lock())) {
                Locker writeLocker { AdoptLock, table.lock().write() };
                auto addResult = table.tableUnderExclusiveLock().addAtSlot<HashTranslator>(lookup, value);
                return *uncheckedDowncast<AtomStringImpl>(addResult.iterator->get());
            }
        }
    }

    Locker writeLocker { table.lock().write() };
    auto addResult = table.tableUnderExclusiveLock().add<HashTranslator>(value);
    return *uncheckedDowncast<AtomStringImpl>(addResult.iterator->get());
}

// The table's own value type as a translator, so that the slow-case add() paths can reach the
// two-phase API, which has no untranslated form. Equivalent to what an untranslated add() does: the
// table hashes and compares its entries by string contents, not by pointer.
struct StringEntryTranslator {
    using StringEntry = AtomStringTable::StringEntry;

    static unsigned hash(const StringEntry& entry) { return DefaultHash<StringEntry>::hash(entry); }
    static bool equal(const StringEntry& a, const StringEntry& b) { return DefaultHash<StringEntry>::equal(a, b); }
    static void translate(StringEntry& location, const StringEntry& entry, unsigned) { location = entry; }
};

// Take the table's own reference and set the atom flag when the string we offered is the one that
// landed in the table. Both have to happen under the lock that admitted the entry, so that a
// concurrent releaseAndRemoveIfNeeded() cannot find an entry whose count has not been raised yet.
static inline Ref<AtomStringImpl> claimStringEntry(StringImpl& string, auto addResult)
{
    if (addResult.isNewEntry) {
        ASSERT(addResult.iterator->get() == &string);
        string.setIsAtom(true);
        string.ref(); // The table's own reference. Released when the entry is removed.
    }
    return *uncheckedDowncast<AtomStringImpl>(addResult.iterator->get());
}

// Intern a string that is not yet an atom. Shares addToStringTable()'s probe-then-upgrade shape; the
// difference is the bookkeeping a new entry needs, which is why it cannot just call it.
static inline Ref<AtomStringImpl> addStringEntry(StringImpl& string)
{
    auto& table = AtomStringTable::singleton();
    {
        Locker readLocker { table.lock().read() };
        auto& atomStringTable = table.tableUnderSharedLock();
        if (atomStringTable.capacity()) [[likely]] {
            auto lookup = atomStringTable.findSlotForInsert<StringEntryTranslator>(&string);
            if (lookup.found) [[likely]]
                return *uncheckedDowncast<AtomStringImpl>(lookup.slot->get());

            if (readLocker.tryUpgrade(table.lock())) {
                Locker writeLocker { AdoptLock, table.lock().write() };
                return claimStringEntry(string, table.tableUnderExclusiveLock().addAtSlot<StringEntryTranslator>(lookup, &string));
            }
        }
    }

    Locker writeLocker { table.lock().write() };
    return claimStringEntry(string, table.tableUnderExclusiveLock().add(&string));
}

using UTF16Buffer = HashTranslatorCharBuffer<char16_t>;
struct UTF16BufferTranslator {
    static unsigned NODELETE hash(const UTF16Buffer& buf)
    {
        return buf.hash;
    }

    static bool equal(AtomStringTable::StringEntry const& str, const UTF16Buffer& buf)
    {
        return WTF::equal(str.get(), buf.characters);
    }

    static void translate(AtomStringTable::StringEntry& location, const UTF16Buffer& buf, unsigned hash)
    {
        Ref stringImpl = StringImpl::create8BitIfPossible(buf.characters);
        stringImpl->setHash(hash);
        stringImpl->setIsAtom(true);
        location = &stringImpl.leakRef();
    }
};

RefPtr<AtomStringImpl> AtomStringImpl::add(std::span<const char16_t> characters)
{
    if (!characters.data())
        return nullptr;

    if (characters.empty())
        return uncheckedDowncast<AtomStringImpl>(StringImpl::empty());

    UTF16Buffer buffer { characters };
    return addToStringTable<UTF16Buffer, UTF16BufferTranslator>(buffer);
}

RefPtr<AtomStringImpl> AtomStringImpl::add(HashTranslatorCharBuffer<char16_t>& buffer)
{
    if (!buffer.characters.data())
        return nullptr;

    if (buffer.characters.empty())
        return uncheckedDowncast<AtomStringImpl>(StringImpl::empty());

    return addToStringTable<UTF16Buffer, UTF16BufferTranslator>(buffer);
}

struct SubstringLocation {
    SUPPRESS_UNCOUNTED_MEMBER StringImpl* baseString;
    unsigned start;
    unsigned length;
    unsigned hash;

    SubstringLocation(StringImpl* baseString, unsigned start, unsigned length)
        : baseString(baseString)
        , start(start)
        , length(length)
        // Computed once here rather than in the translator's hash(), which the table may call more
        // than once for a single add. A substring has no hash of its own to borrow: the base string's
        // is over all of its characters, not this range.
        , hash(baseString->is8Bit()
            ? StringHasher::computeHashAndMaskTop8Bits(baseString->span8().subspan(start, length))
            : StringHasher::computeHashAndMaskTop8Bits(baseString->span16().subspan(start, length)))
    {
    }
};

struct SubstringTranslator {
    static void translate(AtomStringTable::StringEntry& location, const SubstringLocation& buffer, unsigned hash)
    {
        SUPPRESS_UNCOUNTED_ARG Ref stringImpl = StringImpl::createSubstringSharingImpl(*buffer.baseString, buffer.start, buffer.length);
        stringImpl->setHash(hash);
        stringImpl->setIsAtom(true);
        location = &stringImpl.leakRef();
    }
};

struct SubstringTranslator8 : SubstringTranslator {
    static unsigned hash(const SubstringLocation& buffer)
    {
        return buffer.hash;
    }

    static bool equal(AtomStringTable::StringEntry const& string, const SubstringLocation& buffer)
    {
        return WTF::equal(string.get(), buffer.baseString->span8().subspan(buffer.start, buffer.length));
    }
};

struct SubstringTranslator16 : SubstringTranslator {
    static unsigned hash(const SubstringLocation& buffer)
    {
        return buffer.hash;
    }

    static bool equal(AtomStringTable::StringEntry const& string, const SubstringLocation& buffer)
    {
        return WTF::equal(string.get(), buffer.baseString->span16().subspan(buffer.start, buffer.length));
    }
};

RefPtr<AtomStringImpl> AtomStringImpl::add(StringImpl* baseString, unsigned start, unsigned length)
{
    if (!baseString)
        return nullptr;

    if (!length || start >= baseString->length())
        return uncheckedDowncast<AtomStringImpl>(StringImpl::empty());

    unsigned maxLength = baseString->length() - start;
    if (length >= maxLength) {
        if (!start)
            return add(baseString);
        length = maxLength;
    }

    SubstringLocation buffer = { baseString, start, length };
    if (baseString->is8Bit())
        return addToStringTable<SubstringLocation, SubstringTranslator8>(buffer);
    return addToStringTable<SubstringLocation, SubstringTranslator16>(buffer);
}

using Latin1Buffer = HashTranslatorCharBuffer<Latin1Character>;
struct Latin1BufferTranslator {
    static unsigned NODELETE hash(const Latin1Buffer& buf)
    {
        return buf.hash;
    }

    static bool equal(AtomStringTable::StringEntry const& str, const Latin1Buffer& buf)
    {
        return WTF::equal(str.get(), buf.characters);
    }

    static void translate(AtomStringTable::StringEntry& location, const Latin1Buffer& buf, unsigned hash)
    {
        Ref stringImpl = StringImpl::create(buf.characters);
        stringImpl->setHash(hash);
        stringImpl->setIsAtom(true);
        location = &stringImpl.leakRef();
    }
};

template<typename CharType>
struct BufferFromStaticDataTranslator {
    using Buffer = HashTranslatorCharBuffer<CharType>;
    static unsigned NODELETE hash(const Buffer& buf)
    {
        return buf.hash;
    }

    static bool equal(AtomStringTable::StringEntry const& str, const Buffer& buf)
    {
        return WTF::equal(str.get(), buf.characters);
    }

    static void translate(AtomStringTable::StringEntry& location, const Buffer& buf, unsigned hash)
    {
        Ref stringImpl = StringImpl::createWithoutCopying(buf.characters);
        stringImpl->setHash(hash);
        stringImpl->setIsAtom(true);
        location = &stringImpl.leakRef();
    }
};

template<typename CharType>
struct StaticStringAtomBuffer {
    SUPPRESS_UNCOUNTED_MEMBER const StringImpl& staticImpl;
    std::span<const CharType> characters;
    unsigned hash;
};

// Translator that stores a StaticStringImpl directly in the atom table without
// heap-allocating a copy. The StaticStringImpl must have been constructed with
// StringImpl::StringAtom so that isAtom() returns true. This enables global
// atom strings that share the same StringImpl* across all threads.
template<typename CharType>
struct StaticStringAtomTranslator {
    using Buffer = StaticStringAtomBuffer<CharType>;

    static unsigned NODELETE hash(const Buffer& buf)
    {
        return buf.hash;
    }

    static bool equal(AtomStringTable::StringEntry const& str, const Buffer& buf)
    {
        return WTF::equal(str.get(), buf.characters);
    }

    static void translate(AtomStringTable::StringEntry& location, const Buffer& buf, unsigned)
    {
        // Take a reference like any other entry. This string is immortal so it makes no difference
        // to its lifetime, but it keeps the table's invariant uniform.
        SUPPRESS_UNCOUNTED_ARG Ref stringImpl = const_cast<StringImpl&>(buf.staticImpl);
        location = &stringImpl.leakRef();
    }
};

RefPtr<AtomStringImpl> AtomStringImpl::add(HashTranslatorCharBuffer<Latin1Character>& buffer)
{
    if (!buffer.characters.data())
        return nullptr;

    if (buffer.characters.empty())
        return uncheckedDowncast<AtomStringImpl>(StringImpl::empty());

    return addToStringTable<Latin1Buffer, Latin1BufferTranslator>(buffer);
}

RefPtr<AtomStringImpl> AtomStringImpl::add(std::span<const Latin1Character> characters)
{
    if (!characters.data())
        return nullptr;

    if (characters.empty())
        return uncheckedDowncast<AtomStringImpl>(StringImpl::empty());

    Latin1Buffer buffer { characters };
    return addToStringTable<Latin1Buffer, Latin1BufferTranslator>(buffer);
}

Ref<AtomStringImpl> AtomStringImpl::addLiteral(std::span<const Latin1Character> characters)
{
    ASSERT(characters.data());
    ASSERT(!characters.empty());

    Latin1Buffer buffer { characters };
    return addToStringTable<Latin1Buffer, BufferFromStaticDataTranslator<Latin1Character>>(buffer);
}

static Ref<AtomStringImpl> addSymbol(StringImpl& base)
{
    ASSERT(base.length());
    ASSERT(base.isSymbol());

    SubstringLocation buffer = { &base, 0, base.length() };
    if (base.is8Bit())
        return addToStringTable<SubstringLocation, SubstringTranslator8>(buffer);
    return addToStringTable<SubstringLocation, SubstringTranslator16>(buffer);
}

static Ref<AtomStringImpl> addStatic(const StringImpl& base)
{
    ASSERT(base.length());
    ASSERT(base.isStatic());

    // StaticStringImpl with StringAtom: store the static pointer directly in the
    // atom table with no heap allocation. The isAtom() flag is already set at
    // construction time, enabling uncheckedDowncast<AtomStringImpl> and the
    // dynamicDowncast fast path in add(StringImpl&). All threads that register
    // the same StaticStringImpl share the same StringImpl pointer.
    if (base.isAtom()) {
        if (base.is8Bit()) {
            StaticStringAtomBuffer<Latin1Character> buffer { base, base.span8(), base.hash() };
            return addToStringTable<StaticStringAtomBuffer<Latin1Character>, StaticStringAtomTranslator<Latin1Character>>(buffer);
        }
        StaticStringAtomBuffer<char16_t> buffer { base, base.span16(), base.hash() };
        return addToStringTable<StaticStringAtomBuffer<char16_t>, StaticStringAtomTranslator<char16_t>>(buffer);
    }

    if (base.is8Bit()) {
        Latin1Buffer buffer { base.span8(), base.hash() };
        return addToStringTable<Latin1Buffer, BufferFromStaticDataTranslator<Latin1Character>>(buffer);
    }
    UTF16Buffer buffer { base.span16(), base.hash() };
    return addToStringTable<UTF16Buffer, BufferFromStaticDataTranslator<char16_t>>(buffer);
}

RefPtr<AtomStringImpl> AtomStringImpl::add(const StaticStringImpl& string)
{
    ASSERT(static_cast<const StringImpl&>(string).isStatic());
    SUPPRESS_UNCOUNTED_ARG return addStatic(static_cast<const StringImpl&>(string));
}

Ref<AtomStringImpl> AtomStringImpl::addSlowCase(StringImpl& string)
{
    // This check is necessary for null symbols.
    // Their length is zero, but they are not AtomStringImpl.
    if (!string.length())
        return *uncheckedDowncast<AtomStringImpl>(StringImpl::empty());

    if (string.isStatic())
        return addStatic(string);

    if (string.isSymbol())
        return addSymbol(string);

    ASSERT_WITH_MESSAGE(!string.isAtom(), "AtomStringImpl should not hit the slow case if the string is already an atom.");

    return addStringEntry(string);
}

Ref<AtomStringImpl> AtomStringImpl::addSlowCase(Ref<StringImpl>&& string)
{
    // This check is necessary for null symbols.
    // Their length is zero, but they are not AtomStringImpl.
    if (!string->length())
        return *uncheckedDowncast<AtomStringImpl>(StringImpl::empty());

    if (string->isStatic())
        return addStatic(WTF::move(string));

    if (string->isSymbol())
        return addSymbol(WTF::move(string));

    ASSERT_WITH_MESSAGE(!string->isAtom(), "AtomStringImpl should not hit the slow case if the string is already an atom.");

    // The caller's reference is dropped on return rather than moved into the result. On a new entry
    // that costs one ref/deref pair over moving it, which only the cold path pays, and the counts end
    // up the same either way: the table's reference plus the one being returned.
    return addStringEntry(string);
}

Ref<AtomStringImpl> AtomStringImpl::addSlowCase(AtomStringTable&, StringImpl& string)
{
    return addSlowCase(string);
}

RefPtr<AtomStringImpl> AtomStringImpl::lookUpSlowCase(StringImpl& string)
{
    ASSERT_WITH_MESSAGE(!string.isAtom(), "AtomStringImpl objects should return from the fast case.");

    if (!string.length())
        return uncheckedDowncast<AtomStringImpl>(StringImpl::empty());

    auto& table = AtomStringTable::singleton();
    Locker locker { table.lock().read() };
    auto& atomStringTable = table.tableUnderSharedLock();
    auto iterator = atomStringTable.find(&string);
    // Shared, because this only reads. The reference is taken before the locker goes out of scope,
    // which is what keeps the entry alive: an entry is only removed by
    // AtomStringTable::releaseAndRemoveIfNeeded() under the exclusive lock, so no removal can run
    // while any reader is counted. Neither the iterator nor the entry pointer may outlive this scope.
    if (iterator != atomStringTable.end())
        return uncheckedDowncast<AtomStringImpl>(iterator->get());
    return nullptr;
}

RefPtr<AtomStringImpl> AtomStringImpl::add(std::span<const char8_t> characters)
{
    if (charactersAreAllASCII(characters))
        return add(byteCast<Latin1Character>(characters));
    auto string = String::fromUTF8(characters);
    if (string.isNull())
        return nullptr;
    return add(string.releaseImpl());
}

RefPtr<AtomStringImpl> AtomStringImpl::lookUp(std::span<const Latin1Character> characters)
{
    auto& table = AtomStringTable::singleton();
    Locker locker { table.lock().read() };
    auto& atomStringTable = table.tableUnderSharedLock();

    Latin1Buffer buffer { characters };
    auto iterator = atomStringTable.find<Latin1BufferTranslator>(buffer);
    if (iterator != atomStringTable.end())
        return uncheckedDowncast<AtomStringImpl>(iterator->get());
    return nullptr;
}

RefPtr<AtomStringImpl> AtomStringImpl::lookUp(std::span<const char16_t> characters)
{
    auto& table = AtomStringTable::singleton();
    Locker locker { table.lock().read() };
    auto& atomStringTable = table.tableUnderSharedLock();

    UTF16Buffer buffer { characters };
    auto iterator = atomStringTable.find<UTF16BufferTranslator>(buffer);
    if (iterator != atomStringTable.end())
        return uncheckedDowncast<AtomStringImpl>(iterator->get());
    return nullptr;
}

#if ASSERT_ENABLED
bool AtomStringImpl::isInAtomStringTable(StringImpl* string)
{
    auto& table = AtomStringTable::singleton();
    Locker locker { table.lock().read() };
    return table.tableUnderSharedLock().contains(string);
}
#endif

} // namespace WTF
