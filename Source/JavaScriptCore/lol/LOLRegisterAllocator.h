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

template<size_t useCount, size_t defCount, size_t scratchCount = 0>
struct AllocationBindings {
    std::array<JSValueRegs, useCount> uses;
    std::array<JSValueRegs, defCount> defs;
    std::array<JSValueRegs, scratchCount> scratches;
};

template<typename Backend>
class RegisterAllocator {
public:
#ifdef NDEBUG
    static constexpr bool verbose = false;
#else
    static constexpr bool verbose = true;
#endif

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
        m_allocator.initialize(gprs.buildAndValidate(), verbose ? "LOL"_s : ASCIILiteral());
    }

    RegisterSet allocatedRegisters() const { return m_allocator.allocatedRegisters(); }
    Location locationOf(VirtualRegister operand) const { return const_cast<RegisterAllocator<Backend>*>(this)->locationOfImpl(operand); }
    VirtualRegister bindingFor(GPRReg reg) const { return m_allocator.bindingFor(reg); }

    template<size_t scratchCount, size_t useCount, size_t defCount>
    ALWAYS_INLINE AllocationBindings<useCount, defCount, scratchCount> allocateUseDefsImpl(Backend& jit, const auto& instruction, BytecodeIndex index, const std::array<VirtualRegister, useCount>& uses, const std::array<VirtualRegister, defCount>& defs)
    {
        // TODO: Validation.
        UNUSED_PARAM(instruction);
        // Bump the spill count for our uses so we don't spill them when allocating below.
        // TODO: In theory these lock/unlocks are unnecessary because the LRU spill hint should prevent spilling these. It would only help compile times though and it's unclear if it matters yet, so keeping the locking for debugging.
        for (auto operand : uses) {
            if (auto current = locationOf(operand).regs)
                m_allocator.setSpillHint(current.gpr(), index.offset());
        }

        auto doAllocate = [&](VirtualRegister operand, bool isDef) ALWAYS_INLINE_LAMBDA {
            ASSERT_IMPLIES(isDef, operand.isLocal() || operand.isArgument());
            Location& location = locationOfImpl(operand);
            if (location.regs) {
                // Uses might be dirty from a previous instruction, so don't touch them.
                if (isDef)
                    location.isFlushed = false;
                return location.regs;
            }

            // TODO: Consider LRU insertion policy here (aka 0 for hint). Might need locking so these don't spill on the next allocation in the same bytecode.
            location.regs = JSValueRegs(m_allocator.allocate(*this, operand, index.offset()));
            location.isFlushed = !isDef;

            if (!isDef)
                jit.fill(operand, location.regs.gpr());
            return location.regs;
        };

        AllocationBindings<useCount, defCount, scratchCount> result;
        for (size_t i = 0; i < uses.size(); ++i)
            result.uses[i] = doAllocate(uses[i], false);

        for (size_t i = 0; i < defs.size(); ++i)
            result.defs[i] = doAllocate(defs[i], true);

        // TODO: Maybe lock the register here for debugging purposes.
        for (size_t i = 0; i < result.scratches.size(); ++i)
            result.scratches[i] = JSValueRegs(m_allocator.allocate(*this, VirtualRegister(), 0));

        return result;
    }

    template<size_t scratchCount = 0>
    ALWAYS_INLINE auto allocateUnaryOpUseDefs(Backend& jit, const auto& instruction, BytecodeIndex index, VirtualRegister source)
    {
        std::array<VirtualRegister, 1> uses = { source };
        std::array<VirtualRegister, 1> defs = { instruction.m_dst };
        return allocateUseDefsImpl<scratchCount>(jit, instruction, index, uses, defs);
    }

    template<size_t scratchCount = 0>
    ALWAYS_INLINE auto allocateBinaryOpUseDefs(Backend& jit, const auto& instruction, BytecodeIndex index)
    {
        std::array<VirtualRegister, 2> uses = { instruction.m_lhs, instruction.m_rhs };
        std::array<VirtualRegister, 1> defs = { instruction.m_dst };
        return allocateUseDefsImpl<scratchCount>(jit, instruction, index, uses, defs);
    }

    // returns a std::array<JSValueRegs> with the allocated registers + any scratches (if needed)
#define DECLARE_SPECIALIZATION(Op) ALWAYS_INLINE auto allocateUseDefs(Backend& jit, const Op& instruction, BytecodeIndex);
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

    // FIXME: Do I even need this, we could just unbind the scratches immediately after picking them since we can't add more allocations for the same instruction.
    template<size_t useCount, size_t defCount, size_t scratchCount>
    ALWAYS_INLINE void releaseScratches(const AllocationBindings<useCount, defCount, scratchCount>& allocations)
    {
        for (JSValueRegs scratch : allocations.scratches) {
            ASSERT(!bindingFor(scratch.gpr()).isValid());
            m_allocator.unbind(scratch.gpr());
        }
    }

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

// FIXME: Maybe this should be in the unary macro list above.
template<typename Backend>
auto RegisterAllocator<Backend>::allocateUseDefs(Backend& jit, const OpGetFromScope& instruction, BytecodeIndex index)
{
    return allocateUnaryOpUseDefs<1>(jit, instruction, index, instruction.m_scope);
}

} // namespace JSC

#endif
