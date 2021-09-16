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
#include "JSWebAssemblyException.h"

namespace JSC {

const ClassInfo JSWebAssemblyException::s_info = { "WebAssembly.Exception", &ErrorInstance::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(JSWebAssemblyException) };

// TODO: remove global object?
JSWebAssemblyException::JSWebAssemblyException(JSGlobalObject*, VM& vm, Structure* structure, const Wasm::Tag& tag, Vector<uint64_t>&& payload)
    : Base(vm, structure, ErrorType::Error)
    , m_tag(tag)
    , m_payload(WTFMove(payload))
{
}

void JSWebAssemblyException::finishCreation(VM& vm, JSGlobalObject* globalObject)
{
    Base::finishCreation(vm, globalObject, "wasm exception", { });
    ASSERT(inherits(vm, info()));

}

JSValue JSWebAssemblyException::getArg(unsigned) const
{
    // TODO
    return { };
}

} // namespace JSC
