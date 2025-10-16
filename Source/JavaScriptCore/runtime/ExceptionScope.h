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

#include <JavaScriptCore/DeferTermination.h>

#include <wtf/Compiler.h>
#include <wtf/SetForScope.h>
#include <wtf/StackPointer.h>


namespace JSC {

class Exception;

#if ENABLE(EXCEPTION_SCOPE_VERIFICATION)

class ExceptionScope {
    WTF_MAKE_NONCOPYABLE(ExceptionScope);
public:
    JS_EXPORT_PRIVATE ExceptionScope(VM&, ExceptionEventLocation);
    ExceptionScope(ExceptionScope&&) = default;
    JS_EXPORT_PRIVATE ~ExceptionScope();

    VM& vm() const { return m_vm; }
    unsigned recursionDepth() const { return m_recursionDepth; }
    ALWAYS_INLINE Exception* exception() const { return m_vm.exception(); }

    ALWAYS_INLINE void assertNoException() { RELEASE_ASSERT_WITH_MESSAGE(!exception(), "%s", unexpectedExceptionMessage().data()); }
    ALWAYS_INLINE void releaseAssertNoException() { RELEASE_ASSERT_WITH_MESSAGE(!exception(), "%s", unexpectedExceptionMessage().data()); }
    ALWAYS_INLINE void assertNoExceptionExceptTermination() { RELEASE_ASSERT_WITH_MESSAGE(!exception() || m_vm.hasPendingTerminationException(), "%s", unexpectedExceptionMessage().data()); }
    ALWAYS_INLINE void releaseAssertNoExceptionExceptTermination() { RELEASE_ASSERT_WITH_MESSAGE(!exception() || m_vm.hasPendingTerminationException(), "%s", unexpectedExceptionMessage().data()); }

#if ASAN_ENABLED || ENABLE(C_LOOP)
    const void* stackPosition() const {  return m_location.stackPosition; }
#else
    const void* stackPosition() const {  return this; }
#endif

    JS_EXPORT_PRIVATE Exception* throwException(JSGlobalObject*, Exception*);
    JS_EXPORT_PRIVATE Exception* throwException(JSGlobalObject*, JSValue);

    void release() { m_state = Released; }

    // Not really intended for calling directly, generally try to use TRY_CLEAR_EXCEPTION below.
    // Does not clear Termination exceptions so they propagate out to to the runloop.
#if COMPILER(CLANG)
    ALWAYS_INLINE WARN_UNUSED_RETURN bool tryClearException();
#else
    // GCC does not let code silence WARN_UNUSED_RETURN by doing the traditional `(void)expression()` so we just disable the warning.
    ALWAYS_INLINE bool tryClearException();
#endif

    // Don't call `clearExceptionIncludingTermination` or `noRethrow` unless you know what you're doing.
    // Use `TRY_CLEAR_EXCEPTION()` or `tryClearException` unless you have a good reason and understand the
    // intricacies of termination exceptions.

    // Calling this is probably wrong unless you're at the top of the runloop.
    ALWAYS_INLINE void clearExceptionIncludingTermination();
    // Calling this means that your function won't propagate exceptions. In order for that to be true
    // it must be impossible to have a pending termination exception. i.e. have a DeferTermination in effect.
    // Or you should be at the very top of the runloop.
    void noRethrow() { m_state = NoRethrow; }

private:
    enum class State {
        Normal,
        Released,
        NoRethrow,
    };
    using enum State;
    void simulateThrow();


    JS_EXPORT_PRIVATE CString unexpectedExceptionMessage();

    VM& m_vm;
    ExceptionScope* m_previousScope;
    ExceptionEventLocation m_location;
    unsigned m_recursionDepth;
    State m_state { Normal };
};

#define EXCEPTION_ASSERT(assertion) RELEASE_ASSERT(assertion)
#define EXCEPTION_ASSERT_UNUSED(variable, assertion) RELEASE_ASSERT(assertion)
#define EXCEPTION_ASSERT_WITH_MESSAGE(assertion, message) RELEASE_ASSERT_WITH_MESSAGE(assertion, message)

#if ENABLE(C_LOOP)
#define EXCEPTION_SCOPE_POSITION_FOR_ASAN(vm__) (vm__).currentCLoopStackPointer()
#elif ASAN_ENABLED
#define EXCEPTION_SCOPE_POSITION_FOR_ASAN(vm__) currentStackPointer()
#else
#define EXCEPTION_SCOPE_POSITION_FOR_ASAN(vm__) nullptr
#endif

#define DECLARE_EXCEPTION_SCOPE(vm__) JSC::ExceptionScope((vm__), JSC::ExceptionEventLocation(EXCEPTION_SCOPE_POSITION_FOR_ASAN(vm__), __FUNCTION__, __FILE__, __LINE__))

#else // not ENABLE(EXCEPTION_SCOPE_VERIFICATION)
    
class ExceptionScope {
    WTF_MAKE_NONCOPYABLE(ExceptionScope);
public:
    ALWAYS_INLINE ExceptionScope(VM& vm)
        : m_vm(vm)
    { }
    ExceptionScope(ExceptionScope&&) = default;

    ALWAYS_INLINE VM& vm() const { return m_vm; }
    ALWAYS_INLINE Exception* exception() const { return m_vm.exception(); }

    ALWAYS_INLINE void assertNoException() { ASSERT(!exception()); }
    ALWAYS_INLINE void releaseAssertNoException() { RELEASE_ASSERT(!exception()); }
    ALWAYS_INLINE void assertNoExceptionExceptTermination() { ASSERT(!exception() || m_vm.hasPendingTerminationException()); }
    ALWAYS_INLINE void releaseAssertNoExceptionExceptTermination() { RELEASE_ASSERT(!exception() || m_vm.hasPendingTerminationException()); }

    ALWAYS_INLINE Exception* throwException(JSGlobalObject* globalObject, Exception* exception) { return m_vm.throwException(globalObject, exception); }
    ALWAYS_INLINE Exception* throwException(JSGlobalObject* globalObject, JSValue value) { return m_vm.throwException(globalObject, value); }

    ALWAYS_INLINE void release() { }

    // Not really intended for calling directly, use TRY_CLEAR_EXCEPTION below.
    // Does not clear Termination exceptions so they propagate out to to the runloop.
#if COMPILER(CLANG)
    ALWAYS_INLINE WARN_UNUSED_RETURN bool tryClearException();
#else
    // GCC does not let code silence WARN_UNUSED_RETURN by doing the traditional `(void)expression()` so we just disable the warning.
    ALWAYS_INLINE bool tryClearException();
#endif

    // Don't call these unless you know what you're doing. It's probably wrong unless you're at the top of the runloop.
    ALWAYS_INLINE void clearExceptionIncludingTermination();
    ALWAYS_INLINE void noRethrow() { }

private:
    ALWAYS_INLINE CString unexpectedExceptionMessage() { return { }; }

    VM& m_vm;
};

#define EXCEPTION_ASSERT(x) ASSERT(x)
#define EXCEPTION_ASSERT_UNUSED(variable, assertion) ASSERT_UNUSED(variable, assertion)
#define EXCEPTION_ASSERT_WITH_MESSAGE(assertion, message) ASSERT_WITH_MESSAGE(assertion, message)

#define DECLARE_EXCEPTION_SCOPE(vm__) JSC::ExceptionScope((vm__))

#endif // ENABLE(EXCEPTION_SCOPE_VERIFICATION)

class ForbidExceptionScope : public DeferTerminationForAWhile, public SetForScope<bool> {
public:
    ForbidExceptionScope(VM& vm)
        : DeferTerminationForAWhile(vm)
        , SetForScope(vm.m_forbidExceptionThrowing, true)
    { }
};

ALWAYS_INLINE Exception* throwException(JSGlobalObject* globalObject, ExceptionScope& scope, Exception* exception)
{
    return scope.throwException(globalObject, exception);
}

ALWAYS_INLINE Exception* throwException(JSGlobalObject* globalObject, ExceptionScope& scope, JSValue value)
{
    return scope.throwException(globalObject, value);
}

ALWAYS_INLINE EncodedJSValue throwVMException(JSGlobalObject* globalObject, ExceptionScope& scope, Exception* exception)
{
    throwException(globalObject, scope, exception);
    return encodedJSValue();
}

ALWAYS_INLINE EncodedJSValue throwVMException(JSGlobalObject* globalObject, ExceptionScope& scope, JSValue value)
{
    throwException(globalObject, scope, value);
    return encodedJSValue();
}

ALWAYS_INLINE bool ExceptionScope::tryClearException()
{
    // Exception is a GC'd type and this function is very hot so we want to make sure it's inlined but Exception.h exists far below this.
    SUPPRESS_FORWARD_DECL_ARG auto* exception = m_vm.exception();
    SUPPRESS_FORWARD_DECL_ARG if (exception && m_vm.isTerminationException(exception)) [[unlikely]]
        return false;

    m_vm.clearException();
    return true;
}

ALWAYS_INLINE void ExceptionScope::clearExceptionIncludingTermination()
{
    m_vm.clearException();
}

#define CLEAR_AND_RETURN_IF_EXCEPTION(scope__, value__) do { \
        if ((scope__).exception()) [[unlikely]] { \
            (scope__).tryClearException(); \
            return value__; \
        } \
    } while (false)

#define TRY_CLEAR_EXCEPTION(scope__, value__) do { \
    if (!(scope__).tryClearException()) [[unlikely]] \
        return value__; \
} while (false)

#define RETURN_IF_EXCEPTION(scope__, value__) do { \
        SUPPRESS_UNCOUNTED_LOCAL JSC::VM& vm = (scope__).vm(); \
        EXCEPTION_ASSERT(!!(scope__).exception() == vm.traps().needHandling(JSC::VMTraps::NeedExceptionHandling)); \
        if (vm.traps().maybeNeedHandling()) [[unlikely]] { \
            if (vm.hasExceptionsAfterHandlingTraps()) \
                return value__; \
        } \
    } while (false)

#define RETURN_IF_EXCEPTION_WITH_TRAPS_DEFERRED(scope__, value__) do { \
        if ((scope__).exception()) [[unlikely]] \
            return value__; \
    } while (false)

#define RELEASE_AND_RETURN(scope__, expression__) do { \
        scope__.release(); \
        return expression__; \
    } while (false)

} // namespace JSC
