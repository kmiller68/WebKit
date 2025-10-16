/*
 *  Copyright (C) 1999-2001 Harri Porten (porten@kde.org)
 *  Copyright (C) 2003-2017 Apple Inc. All rights reserved.
 *  Copyright (C) 2007 Samuel Weinig <sam@webkit.org>
 *  Copyright (C) 2009 Google, Inc. All rights reserved.
 *  Copyright (C) 2012 Ericsson AB. All rights reserved.
 *  Copyright (C) 2013 Michael Pruett <michael@68k.org>
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#pragma once

#include <JavaScriptCore/ExceptionScope.h>
#include <WebCore/ExceptionDetails.h>
#include <WebCore/ExceptionOr.h>

namespace JSC {
class ExceptionScope;
}

namespace WebCore {

class CachedScript;
class DeferredPromise;
class JSDOMGlobalObject;

void throwAttributeTypeError(JSC::JSGlobalObject&, JSC::ExceptionScope&, ASCIILiteral interfaceName, ASCIILiteral attributeName, ASCIILiteral expectedType);

void throwDataCloneError(JSC::JSGlobalObject&, JSC::ExceptionScope&);
void throwDOMSyntaxError(JSC::JSGlobalObject&, JSC::ExceptionScope&, ASCIILiteral); // Not the same as a JavaScript syntax error.
void throwInvalidStateError(JSC::JSGlobalObject&, JSC::ExceptionScope&, ASCIILiteral);
WEBCORE_EXPORT void throwNonFiniteTypeError(JSC::JSGlobalObject&, JSC::ExceptionScope&);
void throwNotSupportedError(JSC::JSGlobalObject&, JSC::ExceptionScope&, ASCIILiteral);
void throwSecurityError(JSC::JSGlobalObject&, JSC::ExceptionScope&, const String& message);
WEBCORE_EXPORT void throwSequenceTypeError(JSC::JSGlobalObject&, JSC::ExceptionScope&);

WEBCORE_EXPORT JSC::EncodedJSValue throwArgumentMustBeEnumError(JSC::JSGlobalObject&, JSC::ExceptionScope&, unsigned argumentIndex, ASCIILiteral argumentName, ASCIILiteral functionInterfaceName, ASCIILiteral functionName, ASCIILiteral expectedValues);
WEBCORE_EXPORT JSC::EncodedJSValue throwArgumentMustBeFunctionError(JSC::JSGlobalObject&, JSC::ExceptionScope&, unsigned argumentIndex, ASCIILiteral argumentName, ASCIILiteral functionInterfaceName, ASCIILiteral functionName);
WEBCORE_EXPORT JSC::EncodedJSValue throwArgumentMustBeObjectError(JSC::JSGlobalObject&, JSC::ExceptionScope&, unsigned argumentIndex, ASCIILiteral argumentName, ASCIILiteral functionInterfaceName, ASCIILiteral functionName);
WEBCORE_EXPORT JSC::EncodedJSValue throwArgumentTypeError(JSC::JSGlobalObject&, JSC::ExceptionScope&, unsigned argumentIndex, ASCIILiteral argumentName, ASCIILiteral functionInterfaceName, ASCIILiteral functionName, ASCIILiteral expectedType);
WEBCORE_EXPORT JSC::EncodedJSValue throwRequiredMemberTypeError(JSC::JSGlobalObject&, JSC::ExceptionScope&, ASCIILiteral memberName, ASCIILiteral dictionaryName, ASCIILiteral expectedType);
JSC::EncodedJSValue throwConstructorScriptExecutionContextUnavailableError(JSC::JSGlobalObject&, JSC::ExceptionScope&, ASCIILiteral interfaceName);

String makeThisTypeErrorMessage(const char* interfaceName, const char* attributeName);
String makeUnsupportedIndexedSetterErrorMessage(ASCIILiteral interfaceName);

WEBCORE_EXPORT JSC::EncodedJSValue throwThisTypeError(JSC::JSGlobalObject&, JSC::ExceptionScope&, const char* interfaceName, const char* functionName);

WEBCORE_EXPORT JSC::EncodedJSValue rejectPromiseWithGetterTypeError(JSC::JSGlobalObject&, const JSC::ClassInfo*, JSC::PropertyName attributeName);
WEBCORE_EXPORT JSC::EncodedJSValue rejectPromiseWithThisTypeError(DeferredPromise&, const char* interfaceName, const char* operationName);
WEBCORE_EXPORT JSC::EncodedJSValue rejectPromiseWithThisTypeError(JSC::JSGlobalObject&, const char* interfaceName, const char* operationName);

String retrieveErrorMessageWithoutName(JSC::JSGlobalObject&, JSC::VM&, JSC::JSValue exception, JSC::ExceptionScope&);
String retrieveErrorMessage(JSC::JSGlobalObject&, JSC::VM&, JSC::JSValue exception, JSC::ExceptionScope&);
WEBCORE_EXPORT void reportException(JSC::JSGlobalObject*, JSC::JSValue exception, CachedScript* = nullptr, bool = false);
WEBCORE_EXPORT void reportException(JSC::JSGlobalObject*, JSC::Exception*, CachedScript* = nullptr, bool = false, ExceptionDetails* = nullptr);
WEBCORE_EXPORT void reportExceptionIfJSDOMWindow(JSC::JSGlobalObject*, JSC::JSValue exception);
void reportCurrentException(JSC::JSGlobalObject*);

JSC::JSValue createDOMException(JSC::JSGlobalObject&, Exception&&);
JSC::JSValue createDOMException(JSC::JSGlobalObject*, ExceptionCode, const String& = emptyString());

// Convert a DOM implementation exception into a JavaScript exception in the execution lexicalGlobalObject.
WEBCORE_EXPORT void propagateExceptionSlowPath(JSC::JSGlobalObject&, JSC::ExceptionScope&, Exception&&);

ALWAYS_INLINE void propagateException(JSC::JSGlobalObject& lexicalGlobalObject, JSC::ExceptionScope& throwScope, Exception&& exception)
{
    if (throwScope.exception())
        return;
    propagateExceptionSlowPath(lexicalGlobalObject, throwScope, WTFMove(exception));
}

inline void propagateException(JSC::JSGlobalObject& lexicalGlobalObject, JSC::ExceptionScope& throwScope, ExceptionOr<void>&& value)
{
    if (value.hasException()) [[unlikely]]
        propagateException(lexicalGlobalObject, throwScope, value.releaseException());
}

template<typename Functor> void invokeFunctorPropagatingExceptionIfNecessary(JSC::JSGlobalObject& lexicalGlobalObject, JSC::ExceptionScope& throwScope, NOESCAPE Functor&& functor)
{
    using ReturnType = std::invoke_result_t<Functor>;

    if constexpr (IsExceptionOr<ReturnType>) {
        auto result = functor();
        if (result.hasException()) [[unlikely]]
            propagateException(lexicalGlobalObject, throwScope, result.releaseException());
    } else
        functor();
}

} // namespace WebCore
