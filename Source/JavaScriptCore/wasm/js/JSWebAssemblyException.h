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

#pragma once

#if ENABLE(WEBASSEMBLY)

#include "WasmTag.h"

namespace JSC {

class JSWebAssemblyException : public ErrorInstance {
public:
    using Base = ErrorInstance;
    static constexpr bool needsDestruction = true;

    static void destroy(JSCell* cell)
    {
        static_cast<JSWebAssemblyException*>(cell)->JSWebAssemblyException::~JSWebAssemblyException();
    }

    template<typename CellType, SubspaceAccess mode>
    static IsoSubspace* subspaceFor(VM& vm)
    {
        return vm.webAssemblyExceptionSpace<mode>();
    }

    DECLARE_EXPORT_INFO;

    static Structure* createStructure(VM& vm, JSGlobalObject* globalObject, JSValue prototype)
    {
        return Structure::create(vm, globalObject, prototype, TypeInfo(ErrorInstanceType, StructureFlags), info());
    }

    static JSWebAssemblyException* create(JSGlobalObject* globalObject, VM& vm, Structure* structure, const Wasm::Tag& tag, Vector<uint64_t>&& payload)
    {
        JSWebAssemblyException* exception = new (NotNull, allocateCell<JSWebAssemblyException>(vm.heap)) JSWebAssemblyException(globalObject, vm, structure, tag, WTFMove(payload));
        exception->finishCreation(vm, globalObject);
        return exception;
    }

    const Wasm::Tag& tag() const { return m_tag; }
    const Vector<uint64_t>& payload() const { return m_payload; }
    JSValue getArg(unsigned) const;

protected:
    JSWebAssemblyException(JSGlobalObject*, VM&, Structure*, const Wasm::Tag&, Vector<uint64_t>&&);

    void finishCreation(VM&, JSGlobalObject*);

    const Wasm::Tag& m_tag;
    Vector<uint64_t> m_payload;
};

JSObject* createJSWebAssemblyException(JSGlobalObject*, VM&, const Wasm::Tag&, Vector<uint64_t>&&);

} // namespace JSC

#endif // ENABLE(WEBASSEMBLY)
