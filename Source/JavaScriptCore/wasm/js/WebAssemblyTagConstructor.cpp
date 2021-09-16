/*
 * Copyright (C) 2021 Apple Inc. All rights reserved.
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
#include "WebAssemblyTagConstructor.h"

#if ENABLE(WEBASSEMBLY)

#include "Error.h"
#include "JSGlobalObject.h"
#include "JSWebAssemblyTag.h"
#include "WebAssemblyTagPrototype.h"

namespace JSC {

const ClassInfo WebAssemblyTagConstructor::s_info = { "Function", &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(WebAssemblyTagConstructor) };

static JSC_DECLARE_HOST_FUNCTION(callJSWebAssemblyTag);
static JSC_DECLARE_HOST_FUNCTION(constructJSWebAssemblyTag);

JSC_DEFINE_HOST_FUNCTION(constructJSWebAssemblyTag, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    
    UNUSED_PARAM(callFrame);
    //Vector<uint8_t> source = createSourceBufferFromValue(vm, globalObject, callFrame->argument(0));
    //RETURN_IF_EXCEPTION(scope, { });

    //RELEASE_AND_RETURN(scope, JSValue::encode(WebAssemblyTagConstructor::createTag(globalObject, callFrame, WTFMove(source))));
    // TODO
    RELEASE_ASSERT_NOT_REACHED();
    RELEASE_AND_RETURN(scope, { });
}

JSC_DEFINE_HOST_FUNCTION(callJSWebAssemblyTag, (JSGlobalObject* globalObject, CallFrame*))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    return JSValue::encode(throwConstructorCannotBeCalledAsFunctionTypeError(globalObject, scope, "WebAssembly.Tag"));
}

JSWebAssemblyTag* WebAssemblyTagConstructor::createTag(JSGlobalObject* globalObject, CallFrame* callFrame, const Wasm::Tag& tag)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    JSObject* newTarget = asObject(callFrame->newTarget());
    Structure* structure = JSC_GET_DERIVED_STRUCTURE(vm, webAssemblyTagStructure, newTarget, callFrame->jsCallee());
    RETURN_IF_EXCEPTION(scope, nullptr);

    RELEASE_AND_RETURN(scope, JSWebAssemblyTag::create(vm, globalObject, structure, tag));
}

WebAssemblyTagConstructor* WebAssemblyTagConstructor::create(VM& vm, Structure* structure, WebAssemblyTagPrototype* thisPrototype)
{
    auto* constructor = new (NotNull, allocateCell<WebAssemblyTagConstructor>(vm.heap)) WebAssemblyTagConstructor(vm, structure);
    constructor->finishCreation(vm, thisPrototype);
    return constructor;
}

Structure* WebAssemblyTagConstructor::createStructure(VM& vm, JSGlobalObject* globalObject, JSValue prototype)
{
    return Structure::create(vm, globalObject, prototype, TypeInfo(InternalFunctionType, StructureFlags), info());
}

void WebAssemblyTagConstructor::finishCreation(VM& vm, WebAssemblyTagPrototype* prototype)
{
    Base::finishCreation(vm, 1, "Tag"_s, PropertyAdditionMode::WithoutStructureTransition);
    putDirectWithoutTransition(vm, vm.propertyNames->prototype, prototype, PropertyAttribute::DontEnum | PropertyAttribute::DontDelete | PropertyAttribute::ReadOnly);
}

WebAssemblyTagConstructor::WebAssemblyTagConstructor(VM& vm, Structure* structure)
    : Base(vm, structure, callJSWebAssemblyTag, constructJSWebAssemblyTag)
{
}

} // namespace JSC

#endif // ENABLE(WEBASSEMBLY)

