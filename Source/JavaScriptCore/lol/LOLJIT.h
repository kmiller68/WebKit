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

#include "BaselineJITCode.h"
#include "BytecodeUseDef.h"
#include "CodeBlock.h"
#include "CommonSlowPaths.h"
#include "JITDisassembler.h"
#include "JITInlineCacheGenerator.h"
#include "JITMathIC.h"
#include "JITRightShiftGenerator.h"
#include "JSInterfaceJIT.h"
#include "LLIntData.h"
#include "LOLRegisterAllocator.h"
#include "PCToCodeOriginMap.h"
#include "SimpleRegisterAllocator.h"
#include <wtf/SequesteredMalloc.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/UniqueRef.h>

#include "JIT.h"

namespace JSC::LOL {

#define FOR_EACH_IMPLEMENTED_OP(macro) \
    macro(op_add) \
    macro(op_mul) \
    macro(op_sub) \
    macro(op_negate) \
    macro(op_eq) \
    macro(op_neq) \
    macro(op_less) \
    macro(op_lesseq) \
    macro(op_greater) \
    macro(op_greatereq) \
    macro(op_get_from_scope) \
    macro(op_lshift) \
    macro(op_to_number) \
    macro(op_to_string) \
    macro(op_to_object) \
    macro(op_to_numeric) \
    macro(op_rshift) \
    macro(op_urshift) \


#define FOR_EACH_OP_WITH_SLOW_CASE(macro) \
    macro(op_add) \
    macro(op_call_direct_eval) \
    macro(op_eq) \
    macro(op_try_get_by_id) \
    macro(op_in_by_id) \
    macro(op_in_by_val) \
    macro(op_has_private_name) \
    macro(op_has_private_brand) \
    macro(op_get_by_id) \
    macro(op_get_length) \
    macro(op_get_by_id_with_this) \
    macro(op_get_by_id_direct) \
    macro(op_get_by_val) \
    macro(op_get_by_val_with_this) \
    macro(op_enumerator_get_by_val) \
    macro(op_enumerator_put_by_val) \
    macro(op_get_private_name) \
    macro(op_set_private_brand) \
    macro(op_check_private_brand) \
    macro(op_instanceof) \
    macro(op_less) \
    macro(op_lesseq) \
    macro(op_greater) \
    macro(op_greatereq) \
    macro(op_jless) \
    macro(op_jlesseq) \
    macro(op_jgreater) \
    macro(op_jgreatereq) \
    macro(op_jnless) \
    macro(op_jnlesseq) \
    macro(op_jngreater) \
    macro(op_jngreatereq) \
    macro(op_jeq) \
    macro(op_jneq) \
    macro(op_jstricteq) \
    macro(op_jnstricteq) \
    macro(op_loop_hint) \
    macro(op_enter) \
    macro(op_check_traps) \
    macro(op_mod) \
    macro(op_pow) \
    macro(op_mul) \
    macro(op_negate) \
    macro(op_neq) \
    macro(op_new_object) \
    macro(op_put_by_id) \
    macro(op_put_by_val_direct) \
    macro(op_put_by_val) \
    macro(op_put_private_name) \
    macro(op_del_by_val) \
    macro(op_del_by_id) \
    macro(op_sub) \
    macro(op_resolve_scope) \
    macro(op_get_from_scope) \
    macro(op_put_to_scope) \
    macro(op_iterator_open) \
    macro(op_iterator_next) \

#define FOR_EACH_OP_WITH_OPERATION_SLOW_CASE(macro) \
    macro(op_unsigned) \
    macro(op_inc) \
    macro(op_dec) \
    macro(op_bitnot) \
    macro(op_bitand) \
    macro(op_bitor) \
    macro(op_bitxor) \
    macro(op_lshift) \
    macro(op_rshift) \
    macro(op_urshift) \
    macro(op_div) \
    macro(op_create_this) \
    macro(op_create_promise) \
    macro(op_create_generator) \
    macro(op_create_async_generator) \
    macro(op_to_this) \
    macro(op_to_primitive) \
    macro(op_to_number) \
    macro(op_to_numeric) \
    macro(op_to_string) \
    macro(op_to_object) \
    macro(op_not) \
    macro(op_stricteq) \
    macro(op_nstricteq) \
    macro(op_get_prototype_of) \
    macro(op_check_tdz) \
    macro(op_to_property_key) \
    macro(op_to_property_key_or_number) \
    macro(op_typeof_is_function) \

class LOLJIT : public JIT, public ReplayBackend {
    WTF_MAKE_TZONE_NON_HEAP_ALLOCATABLE(LOLJIT);
public:

    LOLJIT(VM&, BaselineJITPlan&, CodeBlock*);

    RefPtr<BaselineJITCode> compileAndLinkWithoutFinalizing(JITCompilationEffort effort);

private:
    // struct GPRBank {
    //     using JITBackend = LOLJIT;
    //     using Register = GPRReg;
    //     static constexpr Register invalidRegister = InvalidGPRReg;
    //     // FIXME: Make this more precise
    //     static constexpr unsigned numberOfRegisters = 32;
    //     static constexpr Width defaultWidth = widthForBytes(sizeof(CPURegister));
    // };
    // using SpillHint = uint16_t;
    // using RegisterBinding = VirtualRegister;

    // // TODO: Pack this.
    // struct Location {
    //     GPRReg gpr() const { return regs.gpr(); }
    //     void dumpInContext(PrintStream& out, const SimpleRegisterAllocator<GPRBank>*) const
    //     {
    //         if (!isFlushed)
    //             out.print("!"_s);
    //     }

    //     JSValueRegs regs { InvalidGPRReg };
    //     bool isFlushed { false };
    // };

    template<typename> friend class RegisterAllocator;
    void fill(VirtualRegister binding, GPRReg gpr)
    {
        JIT_COMMENT(*this, "Filling ", binding);
        emitGetVirtualRegister(binding, gpr);
    }

    void flush(const Location& location, GPRReg gpr, VirtualRegister binding)
    {
        JIT_COMMENT(*this, "Flushing ", binding);
        if (!location.isFlushed)
            emitPutVirtualRegister(binding, gpr);
#if ASSERT_ENABLED
        else {
            JIT_COMMENT(*this, " already flushed, validating");
            emitGetVirtualRegister(binding, scratchRegister());
            Jump ok = branch64(Equal, scratchRegister(), gpr);
            breakpoint();
            ok.link(this);
        }
#endif
    }

    ALWAYS_INLINE constexpr static bool hasSlowCase(OpcodeID op)
    {
        switch (op) {
#define HAS_SLOW_CASE(name) case name: return true;
        FOR_EACH_OP_WITH_OPERATION_SLOW_CASE(HAS_SLOW_CASE)
        FOR_EACH_OP_WITH_SLOW_CASE(HAS_SLOW_CASE)
        default: break;
#undef HAS_SLOW_CASE
        }
        return false;
    }

    ALWAYS_INLINE constexpr static bool isImplemented(OpcodeID op)
    {
        switch (op) {
#define HAS_SLOW_CASE(name) case name: return true;
        FOR_EACH_IMPLEMENTED_OP(HAS_SLOW_CASE)
        default: break;
#undef HAS_SLOW_CASE
        }
        return false;
    }

    // enum class AllocationMode { FillUses, Replay };
    // ALWAYS_INLINE void allocateUseDefs(const JSInstruction* currentInstruction, AllocationMode mode = AllocationMode::FillUses)
    // {
    //     // Bump the spill count for our uses so we don't spill them when allocating below.
    //     // TODO: In theory these lock/unlocks are unnecessary because the LRU spill hint should prevent spilling these. It would only help compile times though and it's unclear if it matters yet, so keeping the locking for debugging.
    //     ASSERT(!currentInstruction->hasCheckpoints());
    //     computeUsesForBytecodeIndexImpl(currentInstruction, noCheckpoints, [&](VirtualRegister operand) ALWAYS_INLINE_LAMBDA {
    //         if (auto current = locationOf(operand).regs) {
    //             m_allocator.setSpillHint(current.gpr(), m_bytecodeIndex.offset());
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
    //         location.regs = JSValueRegs(m_allocator.allocate(*this, operand, m_bytecodeIndex.offset()));
    //         location.isFlushed = !isDef;

    //         if (!isDef && mode == AllocationMode::FillUses)
    //             emitGetVirtualRegister(operand, location.regs);
    //     };

    //     // Allocate our defs first so that we don't end up flushing any uses them when allocating uses.
    //     computeDefsForBytecodeIndex(m_profiledCodeBlock, currentInstruction, noCheckpoints, doAllocate);
    //     isDef = false;
    //     computeUsesForBytecodeIndexImpl(currentInstruction, noCheckpoints, doAllocate);
    // }

    // TODO: This could just take all the registers directly rather than looking them up again.
    ALWAYS_INLINE void releaseUseDefsAndNotifySlowPaths(const JSInstruction*)
    {

        // TODO: Add debug lock validation.
        // auto unlock = [&](VirtualRegister operand) ALWAYS_INLINE_LAMBDA {
        //     m_allocator.unlock(locationOf(operand).gpr());
        // };
        // computeUsesForBytecodeIndexImpl(currentInstruction, noCheckpoints, unlock);
        // computeDefsForBytecodeIndex(m_profiledCodeBlock, currentInstruction, noCheckpoints, unlock);

        // About 41% of bytecodes have a slow path in JS3 (which I'm assuming is close enough for SP3 too if not a higher %)
        // Also 21% of non-slow bytecodes are movs... crazy
        // if (hasSlowCase(currentInstruction->opcodeID())) {
        //     m_liveTempsForSlowPaths.append(m_allocator.allocatedRegisters());
        //     auto push = [&](VirtualRegister operand) ALWAYS_INLINE_LAMBDA {
        //         m_slowPathOperandRegs.append(locationOf(operand).gpr());
        //     };
        //     computeUsesForBytecodeIndexImpl(currentInstruction, noCheckpoints, push);
        //     computeDefsForBytecodeIndex(m_profiledCodeBlock, currentInstruction, noCheckpoints, push);
        // }
    }

    // TODO: try replaying the register allocator to get the state instead of pushing it to the vector.
    void fillLocationsForSlowPath(const JSInstruction*)
    {
// #if ASSERT_ENABLED
//         m_locations.fill(Location());
// #endif
//         auto fill = [&](VirtualRegister operand) ALWAYS_INLINE_LAMBDA {
//             locationOf(operand).regs = m_slowPathOperandRegs[m_currentSlowPathOperandIndex++];
//         };
//         computeUsesForBytecodeIndexImpl(currentInstruction, noCheckpoints, fill);
//         computeDefsForBytecodeIndex(m_profiledCodeBlock, currentInstruction, noCheckpoints, fill);
    }

    // void silentFill(RegisterSet registers)
    // {
    //     bool handleOdd = registers.numberOfSetRegisters() % 2 == 1;
    //     for (Reg reg : registers | std::views::reverse) {
    //         if (handleOdd)
    //             popPair(reg.gpr(), scratchRegister());
    //     }
    // }

private:
    void privateCompileMainPass();
    void privateCompileSlowCases();

#define DECLARE_EMIT_METHODS(name) \
    void emit_##name(const JSInstruction*); \
    void emitSlow_##name(const JSInstruction*, Vector<SlowCaseEntry>::iterator&);
    FOR_EACH_IMPLEMENTED_OP(DECLARE_EMIT_METHODS)
#undef DECLARE_EMIT_METHODS

    void nextBytecodeIndexWithFlushForJumpTargetsIfNeeded(auto& allocator, bool shouldSetFastPathResumePoint)
    {
        auto next = BytecodeIndex(m_bytecodeIndex.offset() + m_currentInstruction->size());
        if (m_currentJumpTargetIndex < m_unlinkedCodeBlock->numberOfJumpTargets() && next.offset() == m_unlinkedCodeBlock->jumpTarget(m_currentJumpTargetIndex)) {
            if (shouldSetFastPathResumePoint) {
                // We need to set a resume point for slow paths to jump back to prior to flushing since the next instruction wouldn't have the flushes and we don't want to re-emit them in the slow path.
                // It's ok if a resume point is already set before here, it should still be correct w.r.t. flushing.
                m_fastPathResumeLabels.add(m_bytecodeIndex, label());
            }

            JIT_COMMENT(*this, "Flush for jump target at bytecode ", m_bytecodeIndex);
            allocator.flushAllRegisters(*this);
            m_currentJumpTargetIndex++;
        }

        m_bytecodeIndex = next;
    }

    // helpers
    template<typename Op>
    void emitRightShiftFastPath(const JSInstruction* currentInstruction, JITRightShiftGenerator::ShiftType snippetShiftType);

    void silentSpill(auto& allocator, auto... excludeGPRs)
    {
        JIT_COMMENT(*this, "Silent spilling");
        for (Reg reg : allocator.allocatedRegisters()) {
            GPRReg gpr = reg.gpr();
            if (((gpr == excludeGPRs) || ... || false))
                continue;
            VirtualRegister binding = allocator.bindingFor(gpr);
            // This is scratch
            if (!binding.isValid())
                continue;
            Location location = allocator.locationOf(binding);
            ASSERT(location.gpr() == gpr);
            if (!location.isFlushed)
                emitPutVirtualRegister(binding, JSValueRegs(gpr));
        }
    }

    void silentFill(auto& allocator, auto... excludeGPRs)
    {
        JIT_COMMENT(*this, "Silent filling");
        for (Reg reg : allocator.allocatedRegisters()) {
            GPRReg gpr = reg.gpr();
            if (((gpr == excludeGPRs) || ... || false))
                continue;
            VirtualRegister binding = allocator.bindingFor(gpr);
            // This is scratch
            if (!binding.isValid())
                continue;
            ASSERT(allocator.locationOf(binding).gpr() == gpr);
            emitGetVirtualRegister(binding, JSValueRegs(gpr));
        }
    }

    // template<typename OperationType>
    // requires (OperationHasResult<OperationType>)
    // void callOperationWithSilentSpill(const OperationType& operation, GPRReg result, auto... args)
    // {
    //     silentSpill(result);
    //     setupArguments<OperationType>(args...);
    //     appendCallWithExceptionCheck<OperationType>(operation);
    //     move(returnValueGPR, result);
    //     silentFill(result);
    // }

    // template<typename OperationType>
    // requires (OperationHasResult<OperationType>)
    // void callOperationWithSilentSpill(const OperationType& operation, JSValueRegs result, auto... args) { callOperationWithSilentSpill(operation, result.gpr(), args...); }

    // template<typename OperationType>
    // requires (!OperationHasResult<OperationType>)
    // void callOperationWithSilentSpill(const OperationType& operation, auto... args)
    // {
    //     silentSpill();
    //     callOperation(operation, args...);
    //     silentFill();
    // }

    template<typename Op>
        requires (LOLJIT::isImplemented(Op::opcodeID))
    void emitCommonSlowPathSlowCaseCall(const JSInstruction*, Vector<SlowCaseEntry>::iterator&, SlowPathFunction);

    template<typename Op>
        requires (!LOLJIT::isImplemented(Op::opcodeID))
    void emitCommonSlowPathSlowCaseCall(const JSInstruction*, Vector<SlowCaseEntry>::iterator&, SlowPathFunction);

    template<typename Op, typename SnippetGenerator>
    void emitBitBinaryOpFastPath(const JSInstruction* currentInstruction);

    template <typename Op, typename Generator, typename ProfiledFunction, typename NonProfiledFunction>
    void emitMathICFast(JITUnaryMathIC<Generator>*, const JSInstruction*, ProfiledFunction, NonProfiledFunction);
    template <typename Op, typename Generator, typename ProfiledFunction, typename NonProfiledFunction>
    void emitMathICFast(JITBinaryMathIC<Generator>*, const JSInstruction*, ProfiledFunction, NonProfiledFunction);

    template <typename Op, typename Generator, typename ProfiledRepatchFunction, typename ProfiledFunction, typename RepatchFunction>
    void emitMathICSlow(JITBinaryMathIC<Generator>*, const JSInstruction*, ProfiledRepatchFunction, ProfiledFunction, RepatchFunction, Vector<SlowCaseEntry>::iterator&);
    template <typename Op, typename Generator, typename ProfiledRepatchFunction, typename ProfiledFunction, typename RepatchFunction>
    void emitMathICSlow(JITUnaryMathIC<Generator>*, const JSInstruction*, ProfiledRepatchFunction, ProfiledFunction, RepatchFunction, Vector<SlowCaseEntry>::iterator&);

    template<typename Op>
    void emitCompare(const JSInstruction*, RelationalCondition);
    template <typename EmitCompareFunctor>
    void emitCompareImpl(VirtualRegister op1, JSValueRegs op1Regs, VirtualRegister op2, JSValueRegs op2Regs, RelationalCondition, const EmitCompareFunctor&);

    template<typename Op, typename SlowOperation>
    void emitCompareSlow(const JSInstruction*, DoubleCondition, SlowOperation, Vector<SlowCaseEntry>::iterator&);
    template<typename SlowOperation, typename EmitDoubleCompareFunctor>
    void emitCompareSlowImpl(VirtualRegister op1, JSValueRegs op1Regs, VirtualRegister op2, JSValueRegs op2Regs, JSValueRegs dstRegs, size_t instructionSize, SlowOperation, Vector<SlowCaseEntry>::iterator&, const EmitDoubleCompareFunctor&);

    template<typename Op>
    void emitNewFuncCommon(const JSInstruction*);

    // template<unsigned numGPRs>
    // class ScratchScope {
    //     WTF_MAKE_NONCOPYABLE(ScratchScope);
    //     WTF_FORBID_HEAP_ALLOCATION;
    // public:
    //     template<typename... Args>
    //     ScratchScope(LOLJIT& generator, Args... locationsToPreserve)
    //         : m_generator(generator)
    //     {
    //         initializedPreservedSet(locationsToPreserve...);
    //         for (JSC::Reg reg : m_preserved)
    //             preserve(reg.gpr());

    //         for (unsigned i = 0; i < numGPRs; i ++) {
    //             m_tempGPRs[i] = m_generator.m_allocator.allocate(m_generator, VirtualRegister(), std::nullopt);
    //             m_generator.m_allocator.lock(m_tempGPRs[i]);
    //         }
    //     }

    //     ~ScratchScope()
    //     {
    //         unbindEarly();
    //     }

    //     void unbindEarly()
    //     {
    //         unbindScratches();
    //         unbindPreserved();
    //     }

    //     void unbindScratches()
    //     {
    //         if (m_unboundScratches)
    //             return;

    //         m_unboundScratches = true;
    //         for (unsigned i = 0; i < numGPRs; i ++)
    //             unbind(m_tempGPRs[i]);
    //     }

    //     void unbindPreserved()
    //     {
    //         if (m_unboundPreserved)
    //             return;

    //         m_unboundPreserved = true;
    //         for (JSC::Reg reg : m_preserved)
    //             unbind(reg.gpr());
    //     }

    //     inline GPRReg gpr(unsigned i) const
    //     {
    //         ASSERT(i < numGPRs);
    //         ASSERT(!m_unboundScratches);
    //         return m_tempGPRs[i];
    //     }

    // private:
    //     // TODO: This could be simplified.
    //     GPRReg preserve(GPRReg reg)
    //     {
    //         if (!m_generator.m_allocator.validRegisters().contains(reg, IgnoreVectors))
    //             return reg;
    //         const VirtualRegister& binding = m_generator.m_allocator.bindingFor(reg);
    //         m_generator.m_allocator.lock(reg);
    //         if (m_preserved.contains(reg, IgnoreVectors) && binding.isValid()) {
    //             if (Options::verboseBBQJITAllocation()) [[unlikely]]
    //                 dataLogLn("LOL\tPreserving GPR ", MacroAssembler::gprName(reg), " currently bound to ", binding);
    //             return reg; // If the register is already bound, we don't need to preserve it ourselves.
    //         }
    //         ASSERT(m_generator.m_allocator.freeRegisters().contains(reg, IgnoreVectors));
    //         if (Options::verboseBBQJITAllocation()) [[unlikely]]
    //             dataLogLn("LOL\tPreserving scratch GPR ", MacroAssembler::gprName(reg), " currently free");
    //         return reg;
    //     }

    //     void unbind(GPRReg reg)
    //     {
    //         if (!m_generator.m_allocator.validRegisters().contains(reg, IgnoreVectors))
    //             return;
    //         const VirtualRegister& binding = m_generator.m_allocator.bindingFor(reg);
    //         m_generator.m_allocator.unlock(reg);
    //         if (Options::verboseBBQJITAllocation()) [[unlikely]]
    //             dataLogLn("LOL\tReleasing GPR ", MacroAssembler::gprName(reg), " preserved? ", m_preserved.contains(reg, IgnoreVectors), " binding: ", binding);
    //         if (m_preserved.contains(reg, IgnoreVectors) && binding.isValid())
    //             return; // It's okay if the register isn't bound to a scratch if we meant to preserve it - maybe it was just already bound to something.
    //         ASSERT(m_generator.m_allocator.freeRegisters().contains(reg, IgnoreVectors));
    //     }

    //     template<typename... Args>
    //     void initializedPreservedSet(Location location, Args... args)
    //     {
    //         if (location.gpr() != InvalidGPRReg)
    //             m_preserved.add(location.gpr(), IgnoreVectors);
    //         initializedPreservedSet(args...);
    //     }

    //     template<typename... Args>
    //     void initializedPreservedSet(RegisterSet registers, Args... args)
    //     {
    //         for (JSC::Reg reg : registers)
    //             initializedPreservedSet(reg);
    //         initializedPreservedSet(args...);
    //     }

    //     template<typename... Args>
    //     void initializedPreservedSet(JSC::Reg reg, Args... args)
    //     {
    //         m_preserved.add(reg.gpr(), IgnoreVectors);
    //         initializedPreservedSet(args...);
    //     }

    //     inline void initializedPreservedSet() { }

    //     LOLJIT& m_generator;
    //     std::array<GPRReg, numGPRs> m_tempGPRs;
    //     RegisterSet m_preserved;
    //     bool m_unboundScratches { false };
    //     bool m_unboundPreserved { false };
    // };
    // template<unsigned> friend class ScratchScope;

    // Location& locationOf(VirtualRegister operand)
    // {
    //     ASSERT(operand.isValid());
    //     if (operand.isLocal())
    //         return m_locations[operand.toLocal()];
    //     if (operand.isConstant())
    //         return m_locations[operand.toConstantIndex() + m_constantsOffset];
    //     ASSERT(operand.isArgument() || operand.isHeader());
    //     // arguments just naturally follow the headers.
    //     return m_locations[operand.offset() + m_headersOffset];
    // }

    // template<typename... Operands>
    // std::array<JSValueRegs, sizeof...(Operands)> regsFor(Operands... operands) { return { locationOf(operands).regs... }; }

    Vector<RegisterSet> m_liveTempsForSlowPaths;
    Vector<JSValueRegs> m_slowPathOperandRegs;
    unsigned m_currentSlowPathOperandIndex;
    unsigned m_currentJumpTargetIndex;

    // This is laid out as [ locals, constants, headers, arguments ]
    // FixedVector<Location> m_locations;
    RegisterAllocator<LOLJIT> m_fastAllocator;
    static constexpr GPRReg s_scratch = RegisterAllocator<LOLJIT>::s_scratch;
    static constexpr JSValueRegs s_scratchRegs = JSValueRegs { s_scratch };
    ReplayRegisterAllocator m_replayAllocator;
    // SimpleRegisterAllocator<GPRBank> m_allocator;
    const JSInstruction* m_currentInstruction;
};

} // namespace JSC

 #endif // ENABLE(JIT)

