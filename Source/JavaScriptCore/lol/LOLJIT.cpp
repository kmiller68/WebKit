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

#include "config.h"
#include "LOLJIT.h"

#if ENABLE(JIT)

#include "BaselineJITPlan.h"
#include "BytecodeUseDef.h"
#include "BytecodeGraph.h"
#include "CodeBlock.h"
#include "CodeBlockWithJITType.h"
#include "DFGCapabilities.h"
#include "JITInlines.h"
#include "JITLeftShiftGenerator.h"
#include "JITOperations.h"
#include "JITSizeStatistics.h"
#include "JITThunks.h"
#include "LLIntEntrypoint.h"
#include "LLIntThunks.h"
#include "LinkBuffer.h"
#include "MaxFrameExtentForSlowPathCall.h"
#include "ModuleProgramCodeBlock.h"
#include "PCToCodeOriginMap.h"
#include "ProbeContext.h"
#include "ProfilerDatabase.h"
#include "ProgramCodeBlock.h"
#include "SlowPathCall.h"
#include "StackAlignment.h"
#include "ThunkGenerators.h"
#include "TypeProfilerLog.h"
#include <wtf/BubbleSort.h>
#include <wtf/GraphNodeWorklist.h>
#include <wtf/SequesteredMalloc.h>
#include <wtf/SimpleStats.h>
#include <wtf/text/MakeString.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace LOLJITInternal {
static constexpr bool verbose = true;
}

LOLJIT::LOLJIT(VM& vm, BaselineJITPlan& plan, CodeBlock* codeBlock)
    : JIT(vm, plan, codeBlock)
    , m_constantsOffset(codeBlock->numCalleeLocals())
    , m_headersOffset(m_constantsOffset + codeBlock->constantRegisters().size() )
    , m_locations(codeBlock->numCalleeLocals() + codeBlock->constantRegisters().size() + CallFrame::headerSizeInRegisters + codeBlock->numParameters())
{
    RegisterSetBuilder gprs = RegisterSetBuilder::allGPRs();
    gprs.exclude(RegisterSetBuilder::specialRegisters());
    gprs.exclude(RegisterSetBuilder::macroClobberedGPRs());
    gprs.exclude(RegisterSetBuilder::vmCalleeSaveRegisters());
    gprs.remove(s_scratch);
    m_allocator.initialize(gprs.buildAndValidate(), "LOL"_s);
}

RefPtr<BaselineJITCode> LOLJIT::compileAndLinkWithoutFinalizing(JITCompilationEffort effort)
{
    DFG::CapabilityLevel level = m_profiledCodeBlock->capabilityLevel();
    switch (level) {
    case DFG::CannotCompile:
        m_canBeOptimized = false;
        m_shouldEmitProfiling = false;
        break;
    case DFG::CanCompile:
    case DFG::CanCompileAndInline:
        m_canBeOptimized = true;
        m_shouldEmitProfiling = true;
        break;
    default:
        RELEASE_ASSERT_NOT_REACHED();
        break;
    }

    if (m_unlinkedCodeBlock->numberOfUnlinkedSwitchJumpTables() || m_unlinkedCodeBlock->numberOfUnlinkedStringSwitchJumpTables()) {
        if (m_unlinkedCodeBlock->numberOfUnlinkedSwitchJumpTables())
            m_switchJumpTables = FixedVector<SimpleJumpTable>(m_unlinkedCodeBlock->numberOfUnlinkedSwitchJumpTables());
        if (m_unlinkedCodeBlock->numberOfUnlinkedStringSwitchJumpTables())
            m_stringSwitchJumpTables = FixedVector<StringJumpTable>(m_unlinkedCodeBlock->numberOfUnlinkedStringSwitchJumpTables());
    }

    if (Options::dumpDisassembly() || Options::dumpBaselineDisassembly() || (m_vm->m_perBytecodeProfiler && Options::disassembleBaselineForProfiler())) [[unlikely]] {
        // FIXME: build a disassembler off of UnlinkedCodeBlock.
        m_disassembler = makeUnique<JITDisassembler>(m_profiledCodeBlock);
    }

    if (m_vm->m_perBytecodeProfiler) [[unlikely]] {
        // FIXME: build profiler disassembler off UnlinkedCodeBlock.
        m_compilation = adoptRef(
            new Profiler::Compilation(
                m_vm->m_perBytecodeProfiler->ensureBytecodesFor(m_profiledCodeBlock),
                Profiler::Baseline));
        m_compilation->addProfiledBytecodes(*m_vm->m_perBytecodeProfiler, m_profiledCodeBlock);
    }

    m_pcToCodeOriginMapBuilder.appendItem(label(), CodeOrigin(BytecodeIndex(0)));

    std::optional<JITSizeStatistics::Marker> sizeMarker;
    if (Options::dumpBaselineJITSizeStatistics()) [[unlikely]]
        sizeMarker = m_vm->jitSizeStatistics->markStart("Baseline_prologue"_s, *this);

    Label entryLabel(this);
    if (m_disassembler)
        m_disassembler->setStartOfCode(entryLabel);

    // Just add a little bit of randomness to the codegen
    if (random() & 1)
        nop();

    emitFunctionPrologue();
    jitAssertCodeBlockOnCallFrameWithType(regT2, JITType::BaselineJIT);
    jitAssertCodeBlockMatchesCurrentCalleeCodeBlockOnCallFrame(regT1, regT2, *m_unlinkedCodeBlock);

    int frameTopOffset = stackPointerOffsetFor(m_unlinkedCodeBlock) * sizeof(Register);
    addPtr(TrustedImm32(frameTopOffset), callFrameRegister, regT1);
    JumpList stackOverflow;
#if !CPU(ADDRESS64)
    unsigned maxFrameSize = -frameTopOffset;
    if (maxFrameSize > Options::reservedZoneSize()) [[unlikely]]
        stackOverflow.append(branchPtr(Above, regT1, callFrameRegister));
#endif
    stackOverflow.append(branchPtr(GreaterThan, AbsoluteAddress(m_vm->addressOfSoftStackLimit()), regT1));

    move(regT1, stackPointerRegister);
    checkStackPointerAlignment();

    emitSaveCalleeSaves();
    emitMaterializeTagCheckRegisters();
    emitMaterializeMetadataAndConstantPoolRegisters();

    if (m_unlinkedCodeBlock->codeType() == FunctionCode) {
        ASSERT(!m_bytecodeIndex);
        if (shouldEmitProfiling() && (!m_unlinkedCodeBlock->isConstructor() || m_unlinkedCodeBlock->numParameters() > 1)) {
            emitGetFromCallFrameHeaderPtr(CallFrameSlot::codeBlock, regT2);
            loadPtr(Address(regT2, CodeBlock::offsetOfArgumentValueProfiles() + FixedVector<ArgumentValueProfile>::offsetOfStorage()), regT2);

            for (unsigned argument = 0; argument < m_unlinkedCodeBlock->numParameters(); ++argument) {
                // If this is a constructor, then we want to put in a dummy profiling site (to
                // keep things consistent) but we don't actually want to record the dummy value.
                if (m_unlinkedCodeBlock->isConstructor() && !argument)
                    continue;
                int offset = CallFrame::argumentOffsetIncludingThis(argument) * static_cast<int>(sizeof(Register));
                loadValue(Address(callFrameRegister, offset), jsRegT10);
                storeValue(jsRegT10, Address(regT2, FixedVector<ArgumentValueProfile>::Storage::offsetOfData() + argument * sizeof(ArgumentValueProfile) + ArgumentValueProfile::offsetOfFirstBucket()));
            }
        }
    }

    RELEASE_ASSERT(!JITCode::isJIT(m_profiledCodeBlock->jitType()));

    if (sizeMarker) [[unlikely]]
        m_vm->jitSizeStatistics->markEnd(WTFMove(*sizeMarker), *this, m_plan);

    privateCompileMainPass();
    privateCompileLinkPass();
    privateCompileSlowCases();

    if (m_disassembler)
        m_disassembler->setEndOfSlowPath(label());
    m_pcToCodeOriginMapBuilder.appendItem(label(), PCToCodeOriginMapBuilder::defaultCodeOrigin());

#if ASSERT_ENABLED
    emitConsistencyCheck();
#endif

    // If the number of parameters is 1, we never require arity fixup.
    JumpList stackOverflowWithEntry;
    bool requiresArityFixup = m_unlinkedCodeBlock->numParameters() != 1;
    if (m_unlinkedCodeBlock->codeType() == FunctionCode && requiresArityFixup) {
        m_arityCheck = label();
        RELEASE_ASSERT(m_unlinkedCodeBlock->codeType() == FunctionCode);

        unsigned numberOfParameters = m_unlinkedCodeBlock->numParameters();
        load32(CCallHelpers::calleeFramePayloadSlot(CallFrameSlot::argumentCountIncludingThis).withOffset(sizeof(CallerFrameAndPC) - prologueStackPointerDelta()), GPRInfo::argumentGPR2);
        branch32(AboveOrEqual, GPRInfo::argumentGPR2, TrustedImm32(numberOfParameters)).linkTo(entryLabel, this);
        m_bytecodeIndex = BytecodeIndex(0);
        getArityPadding(*m_vm, numberOfParameters, GPRInfo::argumentGPR2, GPRInfo::argumentGPR0, GPRInfo::argumentGPR1, GPRInfo::argumentGPR3, stackOverflowWithEntry);

#if CPU(X86_64)
        pop(GPRInfo::argumentGPR1);
#else
        tagPtr(NoPtrTag, linkRegister);
        move(linkRegister, GPRInfo::argumentGPR1);
#endif
        nearCallThunk(CodeLocationLabel { LLInt::arityFixup() });
#if CPU(X86_64)
        push(GPRInfo::argumentGPR1);
#else
        move(GPRInfo::argumentGPR1, linkRegister);
        untagPtr(NoPtrTag, linkRegister);
        validateUntaggedPtr(linkRegister, GPRInfo::argumentGPR0);
#endif
#if ASSERT_ENABLED
        m_bytecodeIndex = BytecodeIndex(); // Reset this, in order to guard its use with ASSERTs.
#endif
        jump(entryLabel);
    } else
        m_arityCheck = entryLabel; // Never require arity fixup.

    stackOverflowWithEntry.link(this);
    emitFunctionPrologue();
    m_bytecodeIndex = BytecodeIndex(0);
    stackOverflow.link(this);
    jumpThunk(CodeLocationLabel(vm().getCTIStub(CommonJITThunkID::ThrowStackOverflowAtPrologue).retaggedCode<NoPtrTag>()));

    ASSERT(m_jmpTable.isEmpty());

    if (m_disassembler)
        m_disassembler->setEndOfCode(label());
    m_pcToCodeOriginMapBuilder.appendItem(label(), PCToCodeOriginMapBuilder::defaultCodeOrigin());

    LinkBuffer linkBuffer(*this, m_profiledCodeBlock, LinkBuffer::Profile::Baseline, effort);
    return link(linkBuffer);
}

void LOLJIT::privateCompileMainPass()
{
#define NEXT_OPCODE_IN_MAIN(name) \
    if (previousSlowCasesSize != m_slowCases.size()) { \
        ASSERT(hasSlowCase(m_currentInstruction->opcodeID())); \
        ++m_bytecodeCountHavingSlowCase; \
    } \
    m_bytecodeIndex = BytecodeIndex(m_bytecodeIndex.offset() + m_currentInstruction->size()); \
    break;

#define DEFINE_SLOW_OP(name) \
    case op_##name: { \
        dataLogLnIf(LOLJITInternal::verbose, "Starting " #name); \
        m_allocator.flushAllRegisters(*this); \
        JITSlowPathCall slowPathCall(this, slow_path_##name); \
        slowPathCall.call(); \
        NEXT_OPCODE_IN_MAIN(op_##name); \
    }

#define DEFINE_OP(name) \
    case name: { \
        dataLogLnIf(LOLJITInternal::verbose, "Starting " #name); \
        m_allocator.flushAllRegisters(*this); \
        emit_##name(m_currentInstruction); \
        NEXT_OPCODE_IN_MAIN(name); \
    }

#define DEFINE_LOL_OP(name) \
    case name: { \
        dataLogLnIf(LOLJITInternal::verbose, "Starting " #name); \
        emit_##name(m_currentInstruction); \
        NEXT_OPCODE_IN_MAIN(name); \
    }

    dataLogIf(LOLJITInternal::verbose, "Compiling ", *m_profiledCodeBlock, "\n");

    jitAssertTagsInPlace();
    jitAssertArgumentCountSane();

    auto& instructions = m_unlinkedCodeBlock->instructions();
    unsigned instructionCount = m_unlinkedCodeBlock->instructions().size();

    m_bytecodeCountHavingSlowCase = 0;
    unsigned currentJumpTargetIndex = 0;
    for (m_bytecodeIndex = BytecodeIndex(0); m_bytecodeIndex.offset() < instructionCount; ) {
        unsigned previousSlowCasesSize = m_slowCases.size();
        m_currentInstruction = instructions.at(m_bytecodeIndex).ptr();
        ASSERT(m_currentInstruction->size());

        if (currentJumpTargetIndex < m_unlinkedCodeBlock->numberOfJumpTargets() && m_bytecodeIndex.offset() == m_unlinkedCodeBlock->jumpTarget(currentJumpTargetIndex)) {
            // This instruction is a jump target, we have to flush.
            if (currentJumpTargetIndex + 1 < m_unlinkedCodeBlock->numberOfJumpTargets())
                ASSERT(m_unlinkedCodeBlock->jumpTarget(currentJumpTargetIndex) < m_unlinkedCodeBlock->jumpTarget(currentJumpTargetIndex + 1));
            dataLogLnIf(LOLJITInternal::verbose, "Flushing for jump target: ",m_bytecodeIndex.offset());
            m_allocator.flushAllRegisters(*this);
            currentJumpTargetIndex++;
        }

        if (m_disassembler)
            m_disassembler->setForBytecodeMainPath(m_bytecodeIndex.offset(), label());
        m_pcToCodeOriginMapBuilder.appendItem(label(), CodeOrigin(m_bytecodeIndex));
        m_labels[m_bytecodeIndex.offset()] = label();

        if (LOLJITInternal::verbose)
            dataLogLn("LOL JIT emitting code for ", m_bytecodeIndex, " at offset ", (long)debugOffset(), " allocator: ", inContext(m_allocator, this));

        OpcodeID opcodeID = m_currentInstruction->opcodeID();

        std::optional<JITSizeStatistics::Marker> sizeMarker;
        if (Options::dumpBaselineJITSizeStatistics()) [[unlikely]] {
            String id = makeString("Baseline_fast_"_s, opcodeNames[opcodeID]);
            sizeMarker = m_vm->jitSizeStatistics->markStart(id, *this);
        }

// #if ASSERT_ENABLED
//         if (opcodeID != op_catch) {
//             loadPtr(addressFor(CallFrameSlot::codeBlock), regT0);
//             static_assert(static_cast<ptrdiff_t>(CodeBlock::offsetOfJITData() + sizeof(void*)) == CodeBlock::offsetOfMetadataTable());
//             loadPairPtr(Address(regT0, CodeBlock::offsetOfJITData()), regT2, regT1);
//             m_consistencyCheckCalls.append(nearCall());
//         }
// #endif

        if (m_compilation) [[unlikely]] {
            add64(
                TrustedImm32(1),
                AbsoluteAddress(m_compilation->executionCounterFor(Profiler::OriginStack(Profiler::Origin(
                    m_compilation->bytecodes(), m_bytecodeIndex)))->address()));
        }

        if (Options::eagerlyUpdateTopCallFrame())
            updateTopCallFrame();

        unsigned bytecodeOffset = m_bytecodeIndex.offset();
        if (Options::traceBaselineJITExecution()) [[unlikely]] {
            VM* vm = m_vm;
            probeDebug([=] (Probe::Context& ctx) {
                CallFrame* callFrame = ctx.fp<CallFrame*>();
                if (opcodeID == op_catch) {
                    // The code generated by emit_op_catch() will update the callFrame to
                    // vm->callFrameForCatch later. Since that code doesn't execute until
                    // later, we should get the callFrame from vm->callFrameForCatch to get
                    // the real codeBlock that owns this op_catch bytecode.
                    callFrame = vm->callFrameForCatch;
                }
                CodeBlock* codeBlock = callFrame->codeBlock();
                dataLogLn("JIT [", bytecodeOffset, "] ", opcodeNames[opcodeID], " cfr ", RawPointer(ctx.fp()), " @ ", codeBlock);
            });
        }

        switch (opcodeID) {
        DEFINE_SLOW_OP(is_callable)
        DEFINE_SLOW_OP(is_constructor)
        DEFINE_SLOW_OP(typeof)
        DEFINE_SLOW_OP(typeof_is_object)
        DEFINE_SLOW_OP(strcat)
        DEFINE_SLOW_OP(push_with_scope)
        DEFINE_SLOW_OP(put_by_id_with_this)
        DEFINE_SLOW_OP(put_by_val_with_this)
        DEFINE_SLOW_OP(resolve_scope_for_hoisting_func_decl_in_eval)
        DEFINE_SLOW_OP(define_data_property)
        DEFINE_SLOW_OP(define_accessor_property)
        DEFINE_SLOW_OP(unreachable)
        DEFINE_SLOW_OP(throw_static_error)
        DEFINE_SLOW_OP(new_array_with_spread)
        DEFINE_SLOW_OP(new_array_with_species)
        DEFINE_SLOW_OP(new_array_buffer)
        DEFINE_SLOW_OP(spread)
        DEFINE_SLOW_OP(create_rest)
        DEFINE_SLOW_OP(create_promise)
        DEFINE_SLOW_OP(new_promise)
        DEFINE_SLOW_OP(create_generator)
        DEFINE_SLOW_OP(create_async_generator)
        DEFINE_SLOW_OP(new_generator)

        DEFINE_OP(op_add)
        DEFINE_OP(op_bitnot)
        DEFINE_OP(op_bitand)
        DEFINE_OP(op_bitor)
        DEFINE_OP(op_bitxor)
        DEFINE_OP(op_call)
        DEFINE_OP(op_call_ignore_result)
        DEFINE_OP(op_tail_call)
        DEFINE_OP(op_call_direct_eval)
        DEFINE_OP(op_call_varargs)
        DEFINE_OP(op_tail_call_varargs)
        DEFINE_OP(op_tail_call_forward_arguments)
        DEFINE_OP(op_construct_varargs)
        DEFINE_OP(op_super_construct_varargs)
        DEFINE_OP(op_catch)
        DEFINE_OP(op_construct)
        DEFINE_OP(op_super_construct)
        DEFINE_OP(op_create_this)
        DEFINE_OP(op_to_this)
        DEFINE_OP(op_get_argument)
        DEFINE_OP(op_argument_count)
        DEFINE_OP(op_get_rest_length)
        DEFINE_OP(op_check_tdz)
        DEFINE_OP(op_identity_with_profile)
        DEFINE_OP(op_debug)
        DEFINE_OP(op_del_by_id)
        DEFINE_OP(op_del_by_val)
        DEFINE_OP(op_div)
        DEFINE_OP(op_end)
        DEFINE_OP(op_enter)
        DEFINE_OP(op_get_scope)
        DEFINE_LOL_OP(op_eq)
        DEFINE_OP(op_eq_null)
        DEFINE_OP(op_below)
        DEFINE_OP(op_beloweq)
        DEFINE_OP(op_try_get_by_id)
        DEFINE_OP(op_in_by_id)
        DEFINE_OP(op_in_by_val)
        DEFINE_OP(op_has_private_name)
        DEFINE_OP(op_has_private_brand)
        DEFINE_OP(op_get_by_id)
        DEFINE_OP(op_get_length)
        DEFINE_OP(op_get_by_id_with_this)
        DEFINE_OP(op_get_by_id_direct)
        DEFINE_OP(op_get_by_val)
        DEFINE_OP(op_get_by_val_with_this)
        DEFINE_OP(op_get_property_enumerator)
        DEFINE_OP(op_enumerator_next)
        DEFINE_OP(op_enumerator_get_by_val)
        DEFINE_OP(op_enumerator_in_by_val)
        DEFINE_OP(op_enumerator_put_by_val)
        DEFINE_OP(op_enumerator_has_own_property)
        DEFINE_OP(op_get_private_name)
        DEFINE_OP(op_set_private_brand)
        DEFINE_OP(op_check_private_brand)
        DEFINE_OP(op_get_prototype_of)
        DEFINE_OP(op_overrides_has_instance)
        DEFINE_OP(op_instanceof)
        DEFINE_OP(op_is_empty)
        DEFINE_OP(op_typeof_is_undefined)
        DEFINE_OP(op_typeof_is_function)
        DEFINE_OP(op_is_undefined_or_null)
        DEFINE_OP(op_is_boolean)
        DEFINE_OP(op_is_number)
        DEFINE_OP(op_is_big_int)
        DEFINE_OP(op_is_object)
        DEFINE_OP(op_is_cell_with_type)
        DEFINE_OP(op_has_structure_with_flags)
        DEFINE_OP(op_jeq_null)
        DEFINE_OP(op_jfalse)
        DEFINE_OP(op_jmp)
        DEFINE_OP(op_jneq_null)
        DEFINE_OP(op_jundefined_or_null)
        DEFINE_OP(op_jnundefined_or_null)
        DEFINE_OP(op_jeq_ptr)
        DEFINE_OP(op_jneq_ptr)
        DEFINE_OP(op_less)
        DEFINE_OP(op_lesseq)
        DEFINE_OP(op_greater)
        DEFINE_OP(op_greatereq)
        DEFINE_OP(op_jless)
        DEFINE_OP(op_jlesseq)
        DEFINE_OP(op_jgreater)
        DEFINE_OP(op_jgreatereq)
        DEFINE_OP(op_jnless)
        DEFINE_OP(op_jnlesseq)
        DEFINE_OP(op_jngreater)
        DEFINE_OP(op_jngreatereq)
        DEFINE_OP(op_jeq)
        DEFINE_OP(op_jneq)
        DEFINE_OP(op_jstricteq)
        DEFINE_OP(op_jnstricteq)
        DEFINE_OP(op_jbelow)
        DEFINE_OP(op_jbeloweq)
        DEFINE_OP(op_jtrue)
        DEFINE_OP(op_loop_hint)
        DEFINE_OP(op_check_traps)
        DEFINE_OP(op_nop)
        DEFINE_OP(op_super_sampler_begin)
        DEFINE_OP(op_super_sampler_end)
        DEFINE_LOL_OP(op_lshift)
        DEFINE_OP(op_mod)
        DEFINE_OP(op_pow)
        DEFINE_OP(op_mov)
        DEFINE_OP(op_mul)
        DEFINE_OP(op_negate)
        DEFINE_OP(op_neq)
        DEFINE_OP(op_neq_null)
        DEFINE_OP(op_new_array)
        DEFINE_OP(op_new_array_with_size)
        DEFINE_OP(op_new_func)
        DEFINE_OP(op_new_func_exp)
        DEFINE_OP(op_new_generator_func)
        DEFINE_OP(op_new_generator_func_exp)
        DEFINE_OP(op_new_async_func)
        DEFINE_OP(op_new_async_func_exp)
        DEFINE_OP(op_new_async_generator_func)
        DEFINE_OP(op_new_async_generator_func_exp)
        DEFINE_OP(op_new_object)
        DEFINE_OP(op_new_reg_exp)
        DEFINE_OP(op_not)
        DEFINE_OP(op_nstricteq)
        DEFINE_OP(op_create_lexical_environment)
        DEFINE_OP(op_create_direct_arguments)
        DEFINE_OP(op_create_scoped_arguments)
        DEFINE_OP(op_create_cloned_arguments)
        DEFINE_OP(op_dec)
        DEFINE_OP(op_inc)
        DEFINE_OP(op_profile_type)
        DEFINE_OP(op_profile_control_flow)
        DEFINE_OP(op_get_parent_scope)
        DEFINE_OP(op_put_by_id)
        DEFINE_OP(op_put_by_val_direct)
        DEFINE_OP(op_put_by_val)
        DEFINE_OP(op_put_private_name)
        DEFINE_OP(op_put_getter_by_id)
        DEFINE_OP(op_put_setter_by_id)
        DEFINE_OP(op_put_getter_setter_by_id)
        DEFINE_OP(op_put_getter_by_val)
        DEFINE_OP(op_put_setter_by_val)
        DEFINE_OP(op_to_property_key)
        DEFINE_OP(op_to_property_key_or_number)

        DEFINE_OP(op_get_internal_field)
        DEFINE_OP(op_put_internal_field)

        DEFINE_OP(op_iterator_open)
        DEFINE_OP(op_iterator_next)

        DEFINE_OP(op_ret)
        DEFINE_OP(op_rshift)
        DEFINE_OP(op_unsigned)
        DEFINE_OP(op_urshift)
        DEFINE_OP(op_set_function_name)
        DEFINE_OP(op_stricteq)
        DEFINE_OP(op_sub)
        DEFINE_OP(op_switch_char)
        DEFINE_OP(op_switch_imm)
        DEFINE_OP(op_switch_string)
        DEFINE_OP(op_throw)
        DEFINE_OP(op_to_number)
        DEFINE_OP(op_to_numeric)
        DEFINE_OP(op_to_string)
        DEFINE_OP(op_to_object)
        DEFINE_OP(op_to_primitive)

        DEFINE_OP(op_resolve_scope)
        DEFINE_OP(op_get_from_scope)
        DEFINE_OP(op_put_to_scope)
        DEFINE_OP(op_get_from_arguments)
        DEFINE_OP(op_put_to_arguments)

        DEFINE_OP(op_log_shadow_chicken_prologue)
        DEFINE_OP(op_log_shadow_chicken_tail)

        default:
            RELEASE_ASSERT_NOT_REACHED();
        }
        if (sizeMarker) [[unlikely]]
            m_vm->jitSizeStatistics->markEnd(WTFMove(*sizeMarker), *this, m_plan);

        if (LOLJITInternal::verbose)
            dataLogLn("At ", bytecodeOffset, ": ", m_slowCases.size(), " allocator: ", inContext(m_allocator, this));
    }

#undef NEXT_OPCODE_IN_MAIN
#undef DEFINE_SLOW_OP
#undef DEFINE_OP
}

void LOLJIT::privateCompileSlowCases()
{
#define DEFINE_SLOWCASE_OP(name) \
    case name: { \
        dataLogLnIf(LOLJITInternal::verbose, "Starting " #name); \
        m_allocator.flushAllRegisters(*this); \
        emitSlow_##name(currentInstruction, iter); \
        break; \
    }

#define DEFINE_SLOWCASE_SLOW_OP(name) \
    case op_##name: { \
        dataLogLnIf(LOLJITInternal::verbose, "Starting " #name); \
        m_allocator.flushAllRegisters(*this); \
        emitSlowCaseCall(iter, slow_path_##name); \
        break; \
    }

#define DEFINE_SLOWCASE_LOL_OP(name) \
    case name: { \
        emitSlow_##name(currentInstruction, iter); \
        break; \
    }

#define DEFINE_SLOWCASE_LOL_SLOW_OP(name) \
    case op_##name: { \
        emitCommonSlowPathSlowCaseCall(currentInstruction, iter, slow_path_##name); \
        break; \
    }

    m_getByIdIndex = 0;
    m_getByValIndex = 0;
    m_getByIdWithThisIndex = 0;
    m_getByValWithThisIndex = 0;
    m_putByIdIndex = 0;
    m_putByValIndex = 0;
    m_inByIdIndex = 0;
    m_inByValIndex = 0;
    m_delByIdIndex = 0;
    m_delByValIndex = 0;
    m_instanceOfIndex = 0;
    m_privateBrandAccessIndex = 0;

    unsigned bytecodeCountHavingSlowCase = 0;
    for (Vector<SlowCaseEntry>::iterator iter = m_slowCases.begin(); iter != m_slowCases.end();) {
        m_bytecodeIndex = iter->to;

        m_pcToCodeOriginMapBuilder.appendItem(label(), CodeOrigin(m_bytecodeIndex));

        BytecodeIndex firstTo = m_bytecodeIndex;

        const auto* currentInstruction = m_unlinkedCodeBlock->instructions().at(m_bytecodeIndex).ptr();

        if (LOLJITInternal::verbose)
            dataLogLn("LOL JIT emitting slow code for ", m_bytecodeIndex, " at offset ", (long)debugOffset(), " allocator: ", inContext(m_allocator, this));

        if (m_disassembler)
            m_disassembler->setForBytecodeSlowPath(m_bytecodeIndex.offset(), label());

        OpcodeID opcodeID = currentInstruction->opcodeID();

        std::optional<JITSizeStatistics::Marker> sizeMarker;
        if (Options::dumpBaselineJITSizeStatistics()) [[unlikely]] {
            String id = makeString("Baseline_slow_"_s, opcodeNames[opcodeID]);
            sizeMarker = m_vm->jitSizeStatistics->markStart(id, *this);
        }

        if (Options::traceBaselineJITExecution()) [[unlikely]] {
            unsigned bytecodeOffset = m_bytecodeIndex.offset();
            probeDebug([=] (Probe::Context& ctx) {
                CodeBlock* codeBlock = ctx.fp<CallFrame*>()->codeBlock();
                dataLogLn("JIT [", bytecodeOffset, "] SLOW ", opcodeNames[opcodeID], " cfr ", RawPointer(ctx.fp()), " @ ", codeBlock);
            });
        }

        switch (currentInstruction->opcodeID()) {
        DEFINE_SLOWCASE_OP(op_add)
        DEFINE_SLOWCASE_OP(op_call_direct_eval)
        DEFINE_SLOWCASE_LOL_OP(op_eq)
        DEFINE_SLOWCASE_OP(op_try_get_by_id)
        DEFINE_SLOWCASE_OP(op_in_by_id)
        DEFINE_SLOWCASE_OP(op_in_by_val)
        DEFINE_SLOWCASE_OP(op_has_private_name)
        DEFINE_SLOWCASE_OP(op_has_private_brand)
        DEFINE_SLOWCASE_OP(op_get_by_id)
        DEFINE_SLOWCASE_OP(op_get_length)
        DEFINE_SLOWCASE_OP(op_get_by_id_with_this)
        DEFINE_SLOWCASE_OP(op_get_by_id_direct)
        DEFINE_SLOWCASE_OP(op_get_by_val)
        DEFINE_SLOWCASE_OP(op_get_by_val_with_this)
        DEFINE_SLOWCASE_OP(op_enumerator_get_by_val)
        DEFINE_SLOWCASE_OP(op_enumerator_put_by_val)
        DEFINE_SLOWCASE_OP(op_get_private_name)
        DEFINE_SLOWCASE_OP(op_set_private_brand)
        DEFINE_SLOWCASE_OP(op_check_private_brand)
        DEFINE_SLOWCASE_OP(op_instanceof)
        DEFINE_SLOWCASE_OP(op_less)
        DEFINE_SLOWCASE_OP(op_lesseq)
        DEFINE_SLOWCASE_OP(op_greater)
        DEFINE_SLOWCASE_OP(op_greatereq)
        DEFINE_SLOWCASE_OP(op_jless)
        DEFINE_SLOWCASE_OP(op_jlesseq)
        DEFINE_SLOWCASE_OP(op_jgreater)
        DEFINE_SLOWCASE_OP(op_jgreatereq)
        DEFINE_SLOWCASE_OP(op_jnless)
        DEFINE_SLOWCASE_OP(op_jnlesseq)
        DEFINE_SLOWCASE_OP(op_jngreater)
        DEFINE_SLOWCASE_OP(op_jngreatereq)
        DEFINE_SLOWCASE_OP(op_jeq)
        DEFINE_SLOWCASE_OP(op_jneq)
        DEFINE_SLOWCASE_OP(op_jstricteq)
        DEFINE_SLOWCASE_OP(op_jnstricteq)
        DEFINE_SLOWCASE_OP(op_loop_hint)
        DEFINE_SLOWCASE_OP(op_enter)
        DEFINE_SLOWCASE_OP(op_check_traps)
        DEFINE_SLOWCASE_OP(op_mod)
        DEFINE_SLOWCASE_OP(op_pow)
        DEFINE_SLOWCASE_OP(op_mul)
        DEFINE_SLOWCASE_OP(op_negate)
        DEFINE_SLOWCASE_OP(op_neq)
        DEFINE_SLOWCASE_OP(op_new_object)
        DEFINE_SLOWCASE_OP(op_put_by_id)
        DEFINE_SLOWCASE_OP(op_put_by_val_direct)
        DEFINE_SLOWCASE_OP(op_put_by_val)
        DEFINE_SLOWCASE_OP(op_put_private_name)
        DEFINE_SLOWCASE_OP(op_del_by_val)
        DEFINE_SLOWCASE_OP(op_del_by_id)
        DEFINE_SLOWCASE_OP(op_sub)
        DEFINE_SLOWCASE_OP(op_resolve_scope)
        DEFINE_SLOWCASE_OP(op_get_from_scope)
        DEFINE_SLOWCASE_OP(op_put_to_scope)

        DEFINE_SLOWCASE_OP(op_iterator_open)
        DEFINE_SLOWCASE_OP(op_iterator_next)

        DEFINE_SLOWCASE_SLOW_OP(unsigned)
        DEFINE_SLOWCASE_SLOW_OP(inc)
        DEFINE_SLOWCASE_SLOW_OP(dec)
        DEFINE_SLOWCASE_SLOW_OP(bitnot)
        DEFINE_SLOWCASE_SLOW_OP(bitand)
        DEFINE_SLOWCASE_SLOW_OP(bitor)
        DEFINE_SLOWCASE_SLOW_OP(bitxor)
        DEFINE_SLOWCASE_LOL_SLOW_OP(lshift)
        DEFINE_SLOWCASE_LOL_SLOW_OP(rshift)
        DEFINE_SLOWCASE_LOL_SLOW_OP(urshift)
        DEFINE_SLOWCASE_SLOW_OP(div)
        DEFINE_SLOWCASE_SLOW_OP(create_this)
        DEFINE_SLOWCASE_SLOW_OP(create_promise)
        DEFINE_SLOWCASE_SLOW_OP(create_generator)
        DEFINE_SLOWCASE_SLOW_OP(create_async_generator)
        DEFINE_SLOWCASE_SLOW_OP(to_this)
        DEFINE_SLOWCASE_SLOW_OP(to_primitive)
        DEFINE_SLOWCASE_LOL_SLOW_OP(to_number)
        DEFINE_SLOWCASE_SLOW_OP(to_numeric)
        DEFINE_SLOWCASE_SLOW_OP(to_string)
        DEFINE_SLOWCASE_SLOW_OP(to_object)
        DEFINE_SLOWCASE_SLOW_OP(not)
        DEFINE_SLOWCASE_SLOW_OP(stricteq)
        DEFINE_SLOWCASE_SLOW_OP(nstricteq)
        DEFINE_SLOWCASE_SLOW_OP(get_prototype_of)
        DEFINE_SLOWCASE_SLOW_OP(check_tdz)
        DEFINE_SLOWCASE_SLOW_OP(to_property_key)
        DEFINE_SLOWCASE_SLOW_OP(to_property_key_or_number)
        DEFINE_SLOWCASE_SLOW_OP(typeof_is_function)
        default:
            RELEASE_ASSERT_NOT_REACHED();
        }

        if (LOLJITInternal::verbose)
            dataLog("At ", firstTo, " slow: ", iter - m_slowCases.begin(), "\n");

        RELEASE_ASSERT_WITH_MESSAGE(iter == m_slowCases.end() || firstTo.offset() != iter->to.offset(), "Not enough jumps linked in slow case codegen.");
        RELEASE_ASSERT_WITH_MESSAGE(firstTo.offset() == (iter - 1)->to.offset(), "Too many jumps linked in slow case codegen.");

        jump().linkTo(fastPathResumePoint(), this);
        ++bytecodeCountHavingSlowCase;

        if (sizeMarker) [[unlikely]] {
            m_bytecodeIndex = BytecodeIndex(m_bytecodeIndex.offset() + currentInstruction->size());
            m_vm->jitSizeStatistics->markEnd(WTFMove(*sizeMarker), *this, m_plan);
        }
    }

    RELEASE_ASSERT(bytecodeCountHavingSlowCase == m_bytecodeCountHavingSlowCase);
    RELEASE_ASSERT(m_getByIdIndex == m_getByIds.size());
    RELEASE_ASSERT(m_getByIdWithThisIndex == m_getByIdsWithThis.size());
    RELEASE_ASSERT(m_getByValWithThisIndex == m_getByValsWithThis.size());
    RELEASE_ASSERT(m_putByIdIndex == m_putByIds.size());
    RELEASE_ASSERT(m_putByValIndex == m_putByVals.size());
    RELEASE_ASSERT(m_inByIdIndex == m_inByIds.size());
    RELEASE_ASSERT(m_instanceOfIndex == m_instanceOfs.size());
    RELEASE_ASSERT(m_privateBrandAccessIndex == m_privateBrandAccesses.size());

#ifndef NDEBUG
    // Reset this, in order to guard its use with ASSERTs.
    m_bytecodeIndex = BytecodeIndex();
#endif
}

// Comparison bytecodes

void LOLJIT::emitCommonSlowPathSlowCaseCall(const JSInstruction* currentInstruction, Vector<SlowCaseEntry>::iterator& iter, SlowPathFunction stub)
{
    allocateUseDefs(currentInstruction);
    VirtualRegister def;
    computeDefsForBytecodeIndex(m_profiledCodeBlock, currentInstruction, noCheckpoints, [&](VirtualRegister reg) ALWAYS_INLINE_LAMBDA {
        ASSERT(!def.isValid());
        def = reg;
    });
    ASSERT(def.isValid());
    JSValueRegs defRegs = locationOf(def).regs;

    linkAllSlowCases(iter);

    silentSpill(defRegs.gpr());
    JITSlowPathCall slowPathCall(this, stub);
    slowPathCall.call();
    // The slow path will write the result to the stack, so we have silentFill fill it.
    silentFill();

    releaseUseDefsAndNotifySlowPaths(currentInstruction);
}

void LOLJIT::emit_op_eq(const JSInstruction* currentInstruction)
{
    auto bytecode = currentInstruction->as<OpEq>();
    allocateUseDefs(currentInstruction);
    auto [ leftRegs, rightRegs, destRegs ] = regsFor(bytecode.m_lhs, bytecode.m_rhs, bytecode.m_dst);

    emitJumpSlowCaseIfNotInt(leftRegs.gpr(), rightRegs.gpr(), destRegs.gpr());
    compare32(Equal, leftRegs.gpr(), rightRegs.gpr(), destRegs.gpr());
    boxBoolean(destRegs.gpr(), destRegs);
    releaseUseDefsAndNotifySlowPaths(currentInstruction);
}

void LOLJIT::emitSlow_op_eq(const JSInstruction* currentInstruction, Vector<SlowCaseEntry>::iterator& iter)
{
    auto bytecode = currentInstruction->as<OpEq>();
    allocateUseDefs(currentInstruction);
    auto [ leftRegs, rightRegs, destRegs ] = regsFor(bytecode.m_lhs, bytecode.m_rhs, bytecode.m_dst);

    linkAllSlowCases(iter);

    loadGlobalObject(s_scratch);
    callOperation(operationCompareEq, s_scratch, leftRegs, rightRegs);
    boxBoolean(returnValueGPR, destRegs);
    releaseUseDefsAndNotifySlowPaths(currentInstruction);
}

void LOLJIT::emit_op_to_number(const JSInstruction* currentInstruction)
{
    auto bytecode = currentInstruction->as<OpToNumber>();
    allocateUseDefs(currentInstruction);
    auto [ operand, dst ] = regsFor(bytecode.m_operand, bytecode.m_dst);

    UnaryArithProfile* arithProfile = &m_unlinkedCodeBlock->unaryArithProfile(bytecode.m_profileIndex);

    auto isInt32 = branchIfInt32(operand);
    addSlowCase(branchIfNotNumber(operand, InvalidGPRReg));
    if (arithProfile && shouldEmitProfiling())
        arithProfile->emitUnconditionalSet(*this, UnaryArithProfile::observedNumberBits());
    isInt32.link(this);
    moveValueRegs(operand, dst);
    releaseUseDefsAndNotifySlowPaths(currentInstruction);
}

// void LOLJIT::emitSlow_op_to_number(const JSInstruction* currentInstruction, Vector<SlowCaseEntry>::iterator& iter)
// {
//     auto bytecode = currentInstruction->as<OpToNumber>();
//     allocateUseDefs(currentInstruction);
//     auto [ operand, dst ] = regsFor(bytecode.m_operand, bytecode.m_dst);

//     linkAllSlowCases(iter);

//     loadGlobalObject(s_scratch);
//     callOperationWithSilentSpill(operationToNumber, dst, s_scratch, operand);
// }

template<typename Op>
void LOLJIT::emitRightShiftFastPath(const JSInstruction* currentInstruction, JITRightShiftGenerator::ShiftType snippetShiftType)
{
    // FIXME: This allocates registers for constants but don't even use them if it's a constant.
    auto bytecode = currentInstruction->as<Op>();
    allocateUseDefs(currentInstruction);
    auto [ leftRegs, rightRegs, resultRegs ] = regsFor(bytecode.m_lhs, bytecode.m_rhs, bytecode.m_dst);

    VirtualRegister op1 = bytecode.m_lhs;
    VirtualRegister op2 = bytecode.m_rhs;

    SnippetOperand leftOperand;
    SnippetOperand rightOperand;

    if (isOperandConstantInt(op1))
        leftOperand.setConstInt32(getOperandConstantInt(op1));
    else if (isOperandConstantInt(op2))
        rightOperand.setConstInt32(getOperandConstantInt(op2));

    RELEASE_ASSERT(!leftOperand.isConst() || !rightOperand.isConst());

    JITRightShiftGenerator gen(leftOperand, rightOperand, resultRegs, leftRegs, rightRegs, fpRegT0, s_scratch, snippetShiftType);

    gen.generateFastPath(*this);

    ASSERT(gen.didEmitFastPath());
    gen.endJumpList().link(this);

    addSlowCase(gen.slowPathJumpList());
    releaseUseDefsAndNotifySlowPaths(currentInstruction);
}

void LOLJIT::emit_op_rshift(const JSInstruction* currentInstruction)
{
    emitRightShiftFastPath<OpRshift>(currentInstruction, JITRightShiftGenerator::SignedShift);
}

void LOLJIT::emit_op_urshift(const JSInstruction* currentInstruction)
{
    emitRightShiftFastPath<OpUrshift>(currentInstruction, JITRightShiftGenerator::UnsignedShift);
}

void LOLJIT::emit_op_lshift(const JSInstruction* currentInstruction)
{
    auto bytecode = currentInstruction->as<OpLshift>();
    allocateUseDefs(currentInstruction);
    auto [ leftRegs, rightRegs, resultRegs ] = regsFor(bytecode.m_lhs, bytecode.m_rhs, bytecode.m_dst);

    VirtualRegister op1 = bytecode.m_lhs;
    VirtualRegister op2 = bytecode.m_rhs;

    SnippetOperand leftOperand;
    SnippetOperand rightOperand;

    if (isOperandConstantInt(op1))
        leftOperand.setConstInt32(getOperandConstantInt(op1));
    else if (isOperandConstantInt(op2))
        rightOperand.setConstInt32(getOperandConstantInt(op2));

    RELEASE_ASSERT(!leftOperand.isConst() || !rightOperand.isConst());

    JITLeftShiftGenerator gen(leftOperand, rightOperand, resultRegs, leftRegs, rightRegs, s_scratch);

    gen.generateFastPath(*this);

    ASSERT(gen.didEmitFastPath());
    gen.endJumpList().link(this);

    addSlowCase(gen.slowPathJumpList());
    releaseUseDefsAndNotifySlowPaths(currentInstruction);
}

} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(JIT)

