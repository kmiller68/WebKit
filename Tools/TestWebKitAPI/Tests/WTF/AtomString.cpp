/*
 * Copyright (C) 2012-2017 Apple Inc. All rights reserved.
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

#include "Helpers/Test.h"
#include <numbers>
#include <wtf/Threading.h>
#include <wtf/Vector.h>
#include <wtf/text/AtomString.h>
#include <wtf/text/AtomStringImpl.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringView.h>

namespace TestWebKitAPI {

TEST(WTF, AtomStringCreationFromLiteral)
{
    AtomString stringWithTemplate("Template Literal"_s);
    ASSERT_EQ(strlen("Template Literal"), stringWithTemplate.length());
    ASSERT_TRUE(stringWithTemplate == "Template Literal"_s);
    ASSERT_TRUE(stringWithTemplate.string().is8Bit());

    ASCIILiteral literal("Source literal");
    AtomString stringFromLiteral(literal);
    ASSERT_EQ(strlen("Source literal"), stringFromLiteral.length());
    ASSERT_TRUE(stringFromLiteral == "Source literal"_s);
    ASSERT_TRUE(stringFromLiteral.string().is8Bit());
    ASSERT_TRUE(std::bit_cast<uintptr_t>(stringFromLiteral.impl()->span8().data()) == std::bit_cast<uintptr_t>(literal.span().data()));
}

TEST(WTF, AtomStringCreationFromLiteralUniqueness)
{
    AtomString string1("Template Literal"_s);
    AtomString string2("Template Literal"_s);
    ASSERT_EQ(string1.impl(), string2.impl());

    AtomString string3("Template Literal"_s);
    ASSERT_EQ(string1.impl(), string3.impl());
}

TEST(WTF, AtomStringExistingHash)
{
    AtomString string1("Template Literal"_s);
    ASSERT_EQ(string1.existingHash(), string1.impl()->existingHash());
    AtomString string2;
    ASSERT_EQ(string2.existingHash(), 0u);
}

static inline const char* testAtomStringNumber(double number)
{
    static char testBuffer[100] = { };
    std::strncpy(testBuffer, AtomString::number(number).string().utf8().data(), 99);
    return testBuffer;
}

TEST(WTF, AtomStringCreationFromNullASCIILiteral)
{
    AtomString stringFromNull { ASCIILiteral() };
    ASSERT_TRUE(stringFromNull.isNull());
    ASSERT_TRUE(stringFromNull.isEmpty());

    AtomString stringFromEmpty(""_s);
    ASSERT_FALSE(stringFromEmpty.isNull());
    ASSERT_TRUE(stringFromEmpty.isEmpty());
}

TEST(WTF, AtomStringNumberDouble)
{
    using Limits = std::numeric_limits<double>;

    EXPECT_STREQ("Infinity", testAtomStringNumber(Limits::infinity()));
    EXPECT_STREQ("-Infinity", testAtomStringNumber(-Limits::infinity()));

    EXPECT_STREQ("NaN", testAtomStringNumber(-Limits::quiet_NaN()));

    EXPECT_STREQ("0", testAtomStringNumber(0));
    EXPECT_STREQ("0", testAtomStringNumber(-0));

    EXPECT_STREQ("2.2250738585072014e-308", testAtomStringNumber(Limits::min()));
    EXPECT_STREQ("-1.7976931348623157e+308", testAtomStringNumber(Limits::lowest()));
    EXPECT_STREQ("1.7976931348623157e+308", testAtomStringNumber(Limits::max()));

    EXPECT_STREQ("3.141592653589793", testAtomStringNumber(std::numbers::pi));
    EXPECT_STREQ("3.1415927410125732", testAtomStringNumber(std::numbers::pi_v<float>));
    EXPECT_STREQ("1.5707963267948966", testAtomStringNumber(piOverTwoDouble));
    EXPECT_STREQ("1.5707963705062866", testAtomStringNumber(piOverTwoFloat));
    EXPECT_STREQ("0.7853981633974483", testAtomStringNumber(piOverFourDouble));
    EXPECT_STREQ("0.7853981852531433", testAtomStringNumber(piOverFourFloat));

    EXPECT_STREQ("2.718281828459045", testAtomStringNumber(2.71828182845904523536028747135266249775724709369995));

    EXPECT_STREQ("299792458", testAtomStringNumber(299792458));

    EXPECT_STREQ("1.618033988749895", testAtomStringNumber(1.6180339887498948482));

    EXPECT_STREQ("1000", testAtomStringNumber(1e3));
    EXPECT_STREQ("10000000000", testAtomStringNumber(1e10));
    EXPECT_STREQ("100000000000000000000", testAtomStringNumber(1e20));
    EXPECT_STREQ("1e+21", testAtomStringNumber(1e21));
    EXPECT_STREQ("1e+30", testAtomStringNumber(1e30));

    EXPECT_STREQ("1100", testAtomStringNumber(1.1e3));
    EXPECT_STREQ("11000000000", testAtomStringNumber(1.1e10));
    EXPECT_STREQ("110000000000000000000", testAtomStringNumber(1.1e20));
    EXPECT_STREQ("1.1e+21", testAtomStringNumber(1.1e21));
    EXPECT_STREQ("1.1e+30", testAtomStringNumber(1.1e30));
}

namespace {

// Interned once up front and held for the whole test, so nothing can drop these entries while the
// threads run and every thread must observe the same AtomStringImpl for a given key.
struct PinnedAtoms {
    Vector<String> keys;
    Vector<AtomString> atoms;

    explicit PinnedAtoms(unsigned count)
    {
        keys.reserveInitialCapacity(count);
        atoms.reserveInitialCapacity(count);
        for (unsigned i = 0; i < count; ++i)
            keys.append(makeString("concurrent-atomization-shared-"_s, i));
        for (auto& key : keys)
            atoms.append(AtomString { key });
    }
};

}

TEST(WTF_AtomString, ConcurrentAtomizationIsUnique)
{
    constexpr unsigned threadCount = 8;
    constexpr unsigned keyCount = 256;
    constexpr unsigned rounds = 20;

    PinnedAtoms pinned { keyCount };

    // Every thread's view of every key, filled without any synchronisation of its own: the table's
    // lock is the only thing making this safe.
    Vector<Vector<AtomStringImpl*>> observed;
    observed.reserveInitialCapacity(threadCount);
    for (unsigned i = 0; i < threadCount; ++i)
        observed.append(Vector<AtomStringImpl*>(FillWith { }, keyCount, nullptr));

    Vector<Ref<Thread>> threads;
    for (unsigned threadIndex = 0; threadIndex < threadCount; ++threadIndex) {
        threads.append(Thread::create("AtomString concurrency test"_s, [&, threadIndex] {
            for (unsigned round = 0; round < rounds; ++round) {
                // Hits: keys already in the table. Started at a different offset per thread so the
                // threads are not walking the buckets in lockstep.
                for (unsigned i = 0; i < keyCount; ++i) {
                    unsigned key = (i + threadIndex * 32) % keyCount;
                    AtomString atom { pinned.keys[key] };
                    AtomStringImpl* impl = atom.impl();
                    AtomStringImpl* previous = observed[threadIndex][key];
                    EXPECT_TRUE(!previous || previous == impl);
                    observed[threadIndex][key] = impl;
                }

                // Misses and drops: strings unique to this thread and round, interned and then
                // released, so insertions and removals run concurrently with the hits above.
                for (unsigned i = 0; i < 32; ++i) {
                    AtomString churn { makeString("concurrent-atomization-churn-"_s, threadIndex, '-', round, '-', i) };
                    EXPECT_FALSE(churn.isEmpty());
                    EXPECT_TRUE(churn.impl()->isAtom());
                }

                // Substrings, which take the translator whose hash is computed from characters rather
                // than borrowed from the base string.
                for (unsigned i = 0; i < 32; ++i) {
                    AtomString substring = StringView { pinned.keys[i] }.substring(1, 12).toAtomString();
                    EXPECT_EQ(12u, substring.length());
                }
            }
        }));
    }
    for (auto& thread : threads)
        thread->waitForCompletion();

    // Same characters always yield the same AtomStringImpl, both across threads and against the
    // references the main thread is still holding.
    for (unsigned key = 0; key < keyCount; ++key) {
        AtomStringImpl* expected = pinned.atoms[key].impl();
        EXPECT_TRUE(expected);
        for (unsigned threadIndex = 0; threadIndex < threadCount; ++threadIndex)
            EXPECT_EQ(expected, observed[threadIndex][key]);
    }

    // Every churned string was dropped by its thread, so nothing may be left in the table. A stale
    // entry here would mean a lost removal or a reference the table never released.
    for (unsigned threadIndex = 0; threadIndex < threadCount; ++threadIndex) {
        for (unsigned i = 0; i < 32; ++i) {
            auto key = makeString("concurrent-atomization-churn-"_s, threadIndex, '-', rounds - 1, '-', i);
            EXPECT_NULL(AtomStringImpl::lookUp(key.span8()));
        }
    }
}

} // namespace TestWebKitAPI
