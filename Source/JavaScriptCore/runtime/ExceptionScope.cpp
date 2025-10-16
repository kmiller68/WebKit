/*
 * Copyright (C) 2016-2017 Apple Inc. All rights reserved.
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

#include "config.h"
#include "ExceptionScope.h"

#include "ErrorInstance.h"
#include "Exception.h"
#include <wtf/StackTrace.h>
#include <wtf/StringPrintStream.h>
#include <wtf/Threading.h>

namespace JSC {
    
#if ENABLE(EXCEPTION_SCOPE_VERIFICATION)
    
ExceptionScope::ExceptionScope(VM& vm, ExceptionEventLocation location)
    : m_vm(vm)
    , m_previousScope(vm.m_topExceptionScope)
    , m_location(location)
    , m_recursionDepth(m_previousScope ? m_previousScope->m_recursionDepth + 1 : 0)
{
    m_vm.m_topExceptionScope = this;
    m_vm.verifyExceptionCheckNeedIsSatisfied(m_recursionDepth, m_location);
}

ExceptionScope::~ExceptionScope()
{
    RELEASE_ASSERT(m_vm.m_topExceptionScope);

    if (m_state == Normal)
        m_vm.verifyExceptionCheckNeedIsSatisfied(m_recursionDepth, m_location);
    else {
        // If we released the scope, that means we're letting our callers do the
        // exception check. However, because our caller may be a LLInt or JIT
        // function (which always checks for exceptions but won't clear the
        // m_needExceptionCheck bit), we should clear m_needExceptionCheck here
        // and let code below decide if we need to simulate a re-throw.
        m_vm.m_needExceptionCheck = false;
    }

    bool willBeHandleByLLIntOrJIT = false;
    const void* previousScopeStackPosition = m_previousScope ? m_previousScope->stackPosition() : nullptr;
    void* topEntryFrame = m_vm.topEntryFrame;

    // If the topEntryFrame was pushed on the stack after the previousScope was instantiated,
    // then this ExceptionScope will be returning to LLInt or JIT code that always do an exception
    // check. In that case, skip the simulated throw because the LLInt and JIT will be
    // checking for the exception their own way instead of calling ExceptionScope::exception().
    if (topEntryFrame && previousScopeStackPosition > topEntryFrame)
        willBeHandleByLLIntOrJIT = true;

    if (!willBeHandleByLLIntOrJIT && m_state != NoRethrow)
        simulateThrow();

    m_vm.m_topExceptionScope = m_previousScope;
}

CString ExceptionScope::unexpectedExceptionMessage()
{
    StringPrintStream out;

    out.println("Unexpected exception observed on thread ", Thread::currentSingleton(), " at:");
    auto currentStack = StackTrace::captureStackTrace(Options::unexpectedExceptionStackTraceLimit(), 1);
    out.print(StackTracePrinter { *currentStack, "    " });

    if (!m_vm.nativeStackTraceOfLastThrow())
        return CString();
    
    out.println("The exception was thrown from thread ", *m_vm.throwingThread(), " at:");
    out.print(StackTracePrinter { *m_vm.nativeStackTraceOfLastThrow(), "    " });

    if (auto* error = jsDynamicCast<ErrorInstance*>(exception()->value()))
        out.println("Error Exception: ", error->tryGetMessageForDebugging());
    else
        out.println("non-Error Exception: ", exception()->value());

    return out.toCString();
}

Exception* ExceptionScope::throwException(JSGlobalObject* globalObject, Exception* exception)
{
    if (m_vm.exception() && m_vm.exception() != exception)
        m_vm.verifyExceptionCheckNeedIsSatisfied(m_recursionDepth, m_location);

    return m_vm.throwException(globalObject, exception);
}

Exception* ExceptionScope::throwException(JSGlobalObject* globalObject, JSValue error)
{
    if (!error.isCell() || error.isObject() || !jsDynamicCast<Exception*>(error.asCell()))
        m_vm.verifyExceptionCheckNeedIsSatisfied(m_recursionDepth, m_location);

    return m_vm.throwException(globalObject, error);
}

void ExceptionScope::simulateThrow()
{
    RELEASE_ASSERT(m_vm.m_topExceptionScope);
    if (m_vm.m_forbidExceptionThrowing)
        return;

    m_vm.m_simulatedThrowPointLocation = m_location;
    m_vm.m_simulatedThrowPointRecursionDepth = m_recursionDepth;
    m_vm.m_needExceptionCheck = true;
    if (Options::dumpSimulatedThrows()) [[unlikely]]
        m_vm.m_nativeStackTraceOfLastSimulatedThrow = StackTrace::captureStackTrace(Options::unexpectedExceptionStackTraceLimit());
}

#endif // ENABLE(EXCEPTION_SCOPE_VERIFICATION)
    
} // namespace JSC
