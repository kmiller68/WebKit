/*
 * Copyright (C) 2025 Apple Inc. All rights reserved.
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

#if ENABLE(JIT)

#include "BytecodeStructs.h"
#include "CodeBlock.h"
#include "Opcode.h"
#include "SimpleRegisterAllocator.h"
#include "VirtualRegister.h"

namespace JSC::LOL {

// TODO: Pack this.
struct Location {
    GPRReg gpr() const { return regs.gpr(); }
    void dumpInContext(PrintStream& out, const auto*) const
    {
        if (!isFlushed)
            out.print("!"_s);
    }

    JSValueRegs regs { InvalidGPRReg };
    bool isFlushed { false };
};

template<typename Backend>
class RegisterAllocator {
public:
    static constexpr GPRReg s_scratch = GPRInfo::nonPreservedNonArgumentGPR0;

    struct GPRBank {
        using JITBackend = RegisterAllocator;
        using Register = GPRReg;
        static constexpr Register invalidRegister = InvalidGPRReg;
        // FIXME: Make this more precise
        static constexpr unsigned numberOfRegisters = 32;
        static constexpr Width defaultWidth = widthForBytes(sizeof(CPURegister));
    };
    using SpillHint = uint32_t;
    using RegisterBinding = VirtualRegister;
    template<typename> friend class JSC::SimpleRegisterAllocator;

    RegisterAllocator(Backend& backend, CodeBlock* codeBlock)
        : m_numVars(codeBlock->numVars())
        , m_constantsOffset(codeBlock->numCalleeLocals())
        , m_headersOffset(m_constantsOffset + codeBlock->constantRegisters().size())
        , m_locations(codeBlock->numCalleeLocals() + codeBlock->constantRegisters().size() + CallFrame::headerSizeInRegisters + codeBlock->numParameters())
        , m_backend(backend)
    {
        RegisterSetBuilder gprs = RegisterSetBuilder::allGPRs();
        gprs.exclude(RegisterSetBuilder::specialRegisters());
        gprs.exclude(RegisterSetBuilder::macroClobberedGPRs());
        gprs.exclude(RegisterSetBuilder::vmCalleeSaveRegisters());
        gprs.remove(s_scratch);
        m_allocator.initialize(gprs.buildAndValidate(), "LOL"_s);
    }

    Location locationOf(VirtualRegister operand) const { return const_cast<RegisterAllocator<Backend>*>(this)->locationOf(operand); }

    template<size_t useCounts, size_t defCounts>
    ALWAYS_INLINE std::array<JSValueRegs, useCounts + defCounts> allocateUseDefsImpl(Backend& jit, const auto& instruction, BytecodeIndex index, const std::array<VirtualRegister, useCounts>& uses, const std::array<VirtualRegister, defCounts>& defs)
    {
        // TODO: Validation.
        UNUSED_PARAM(instruction);
        // Bump the spill count for our uses so we don't spill them when allocating below.
        // TODO: In theory these lock/unlocks are unnecessary because the LRU spill hint should prevent spilling these. It would only help compile times though and it's unclear if it matters yet, so keeping the locking for debugging.
        for (auto operand : uses) {
            if (auto current = locationOf(operand).regs)
                m_allocator.setSpillHint(current.gpr(), index.offset());
        }

        bool isDef = true;
        auto doAllocate = [&](VirtualRegister operand) ALWAYS_INLINE_LAMBDA {
            ASSERT_IMPLIES(isDef, operand.isLocal());
            Location& location = locationOfImpl(operand);
            if (location.regs) {
                // Uses might be dirty from a previous instruction, so don't touch them.
                if (isDef)
                    location.isFlushed = false;
                return location.regs;
            }

            // TODO: Consider LRU insertion policy here (aka 0 for hint)
            location.regs = JSValueRegs(m_allocator.allocate(jit, operand, index.offset()));
            location.isFlushed = !isDef;

            if (!isDef)
                jit.fill(operand, location.regs);
            return location.regs;
        };

        std::array<JSValueRegs, useCounts + defCounts> result;
        size_t i = 0;
        for (auto def : defs)
            result[i++] = doAllocate(def);

        isDef = false;
        for (auto use : uses)
            result[i++] = doAllocate(use);

        return result;
    }

    template<size_t useDefCount, size_t scratchCount>
    ALWAYS_INLINE std::array<JSValueRegs, useDefCount + scratchCount> allocateScratches(Backend& jit, const std::array<JSValueRegs, useDefCount>& useDefs)
    {
        std::array<JSValueRegs, useDefCount + scratchCount> result;
        size_t i = 0;
        for (auto useDef : useDefs)
            result[i++] = useDef;

        while (i < result.size())
            result[i++] = m_allocator.allocate(jit, VirtualRegister(), 0);
    }

    template<typename Op>
    auto allocateUnaryOpUseDefs(Backend& jit, const Op& instruction, BytecodeIndex index, VirtualRegister source)
    {
        std::array<VirtualRegister, 1> uses = { source };
        std::array<VirtualRegister, 1> defs = { instruction.m_dst };
        return allocateUseDefsImpl(jit, instruction, index, uses, defs);
    }

    template<typename Op>
    auto allocateBinaryOpUseDefs(Backend& jit, const Op& instruction, BytecodeIndex index)
    {
        std::array<VirtualRegister, 2> uses = { instruction.m_lhs, instruction.m_rhs };
        std::array<VirtualRegister, 1> defs = { instruction.m_dst };
        return allocateUseDefsImpl(jit, instruction, index, uses, defs);
    }

    // returns a std::array<JSValueRegs> with the allocated registers + any scratches (if needed)
#define DECLARE_SPECIALIZATION(Op) inline auto allocateUseDefs(Backend& jit, const Op& instruction, BytecodeIndex);
    FOR_EACH_BYTECODE_STRUCT(DECLARE_SPECIALIZATION)
#undef DECLARE_SPECIALIZATION

    // template<std::derived_from<JSInstruction> Op>
    // ALWAYS_INLINE auto allocateUseDefs(const Op& currentInstruction, BytecodeIndex index)
    // {
    //     // Bump the spill count for our uses so we don't spill them when allocating below.
    //     // TODO: In theory these lock/unlocks are unnecessary because the LRU spill hint should prevent spilling these. It would only help compile times though and it's unclear if it matters yet, so keeping the locking for debugging.
    //     ASSERT(!currentInstruction->hasCheckpoints());
    //     computeUsesForBytecodeIndexImpl(Op::opcodeID, currentInstruction, noCheckpoints, [&](VirtualRegister operand) ALWAYS_INLINE_LAMBDA {
    //         if (auto current = locationOf(operand).regs) {
    //             m_allocator.setSpillHint(current.gpr(), index.offset());
    //         }
    //     });
    //     // isDef = true;
    //     // TODO Figure out checkpoints
    //     // computeDefsForBytecodeIndex(m_profiledCodeBlock, currentInstruction, noCheckpoints, lock);

    //     bool isDef = true;
    //     auto doAllocate = [&](VirtualRegister operand) ALWAYS_INLINE_LAMBDA {
    //         ASSERT_IMPLIES(isDef, operand.isLocal());
    //         Location& location = locationOf(operand);
    //         if (location.regs) {
    //             // Uses might be dirty from a previous instruction, so don't touch them.
    //             if (isDef) {
    //                 location.isFlushed = false;
    //             }
    //             return;
    //         }

    //         // TODO: Consider LRU insertion policy here (aka 0 for hint)
    //         location.regs = JSValueRegs(m_allocator.allocate(m_backend, operand, index.offset()));
    //         location.isFlushed = !isDef;

    //         if (!isDef)
    //             m_backend.fill(operand, location.regs);
    //     };

    //     // Allocate our defs first so that we don't end up flushing any uses them when allocating uses.
    //     computeDefsForBytecodeIndexImpl(m_numVars, Op::opcodeID, currentInstruction, noCheckpoints, doAllocate);
    //     isDef = false;
    //     computeUsesForBytecodeIndexImpl(Op::opcodeID, currentInstruction, noCheckpoints, doAllocate);
    // }

    void flushAllRegisters(Backend&) { m_allocator.flushAllRegisters(*this); }

    void dump(PrintStream& out) const { m_allocator.dumpInContext(out, this); }

private:
    friend class SimpleRegisterAllocator<GPRBank>;
    void flush(GPRReg gpr, VirtualRegister binding)
    {
        Location& location = locationOfImpl(binding);
        ASSERT(location.gpr() == gpr);
        m_backend.flush(location, gpr, binding);
        location = Location();
    }

    Location& locationOfImpl(VirtualRegister operand)
    {
        ASSERT(operand.isValid());
        if (operand.isLocal())
            return m_locations[operand.toLocal()];
        if (operand.isConstant())
            return m_locations[operand.toConstantIndex() + m_constantsOffset];
        ASSERT(operand.isArgument() || operand.isHeader());
        // arguments just naturally follow the headers.
        return m_locations[operand.offset() + m_headersOffset];
    }

    // Only used for debugging.
    const uint32_t m_numVars;
    const uint32_t m_constantsOffset;
    const uint32_t m_headersOffset;
    // This is laid out as [ locals, constants, headers, arguments ]
    FixedVector<Location> m_locations;
    SimpleRegisterAllocator<GPRBank> m_allocator;
    Backend& m_backend;
};

class ReplayBackend {
public:
    ALWAYS_INLINE void flush(const Location&, GPRReg, VirtualRegister) { }
    ALWAYS_INLINE void fill(VirtualRegister, GPRReg) { }
};

using ReplayRegisterAllocator = RegisterAllocator<ReplayBackend>;

#define FOR_EACH_UNARY_OP(macro) \
    macro(OpToNumber, m_operand) \
    macro(OpNegate, m_operand) \
    macro(OpToString, m_operand) \
    macro(OpToObject, m_operand) \
    macro(OpToNumeric, m_operand)

#define ALLOCATE_USE_DEFS_FOR_UNARY_OP(Struct, operand) \
template<typename Backend> \
auto RegisterAllocator<Backend>::allocateUseDefs(Backend& jit, const Struct& instruction, BytecodeIndex index) \
{ \
    return allocateUnaryOpUseDefs(jit, instruction, index, instruction.operand); \
}

FOR_EACH_UNARY_OP(ALLOCATE_USE_DEFS_FOR_UNARY_OP)

#undef ALLOCATE_USE_DEFS_FOR_UNARY_OP
#undef FOR_EACH_UNARY_OP

#define FOR_EACH_BINARY_OP(macro) \
    macro(OpAdd) \
    macro(OpMul) \
    macro(OpSub) \
    macro(OpEq) \
    macro(OpNeq) \
    macro(OpLess) \
    macro(OpLesseq) \
    macro(OpGreater) \
    macro(OpGreatereq) \
    macro(OpLshift) \
    macro(OpRshift) \
    macro(OpUrshift)

#define ALLOCATE_USE_DEFS_FOR_BINARY_OP(Struct) \
template<typename Backend> \
auto RegisterAllocator<Backend>::allocateUseDefs(Backend& jit, const Struct& instruction, BytecodeIndex index) \
{ \
    return allocateBinaryOpUseDefs(jit, instruction, index); \
}

FOR_EACH_BINARY_OP(ALLOCATE_USE_DEFS_FOR_BINARY_OP)

#undef ALLOCATE_USE_DEFS_FOR_BINARY_OP
#undef FOR_EACH_BINARY_OP

template<typename Backend>
auto RegisterAllocator<Backend>::allocateUseDefs(Backend& jit, const OpGetFromScope& instruction, BytecodeIndex index)
{
    auto useDefs = allocateUnaryOpUseDefs(jit, instruction, index, instruction.m_scope);
    return allocateScratches<2, 1>(jit, useDefs);
}

} // namespace JSC

#endif
