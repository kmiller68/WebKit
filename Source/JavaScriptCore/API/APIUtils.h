/*
 * Copyright (C) 2016 Apple Inc. All rights reserved.
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

#ifndef APIUtils_h
#define APIUtils_h

#include "Exception.h"
#include "ExceptionScope.h"
#include "JSCJSValue.h"
#include "JSGlobalObjectInspectorController.h"
#include "JSValueRef.h"

enum class ExceptionStatus {
    DidThrow,
    DidNotThrow
};

inline ExceptionStatus handleExceptionIfNeeded(JSC::ExceptionScope& scope, JSContextRef ctx, JSValueRef* returnedExceptionRef)
{
    // Don't simulate an exception here. We're returning to API user code and we have no idea what they're going to do.
    scope.noRethrow();
    JSC::JSGlobalObject* globalObject = toJS(ctx);
    if (scope.exception()) [[unlikely]] {
        JSC::Exception* exception = scope.exception();
        if (returnedExceptionRef)
            *returnedExceptionRef = toRef(globalObject, exception->value());
        scope.clearExceptionIncludingTermination();
#if ENABLE(REMOTE_INSPECTOR)
        globalObject->inspectorController().reportAPIException(globalObject, exception);
#endif
        return ExceptionStatus::DidThrow;
    }
    return ExceptionStatus::DidNotThrow;
}

inline void setException(JSContextRef ctx, JSValueRef* returnedExceptionRef, JSC::JSValue exception)
{
    JSC::JSGlobalObject* globalObject = toJS(ctx);
    if (returnedExceptionRef)
        *returnedExceptionRef = toRef(globalObject, exception);
#if ENABLE(REMOTE_INSPECTOR)
    JSC::VM& vm = getVM(globalObject);
    globalObject->inspectorController().reportAPIException(globalObject, JSC::Exception::create(vm, exception));
#endif
}

#endif /* APIUtils_h */
