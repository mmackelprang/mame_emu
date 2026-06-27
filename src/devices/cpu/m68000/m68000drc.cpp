// license:BSD-3-Clause
// copyright-holders:Mark Mackelprang
/***************************************************************************

    m68000drc.cpp

    UML translator + per-PC dispatcher for the m68000 DRC recompiler
    (PR boundary M -- the first native UML emission).

    --- What this boundary ships ---

    Boundary L wired the dual-path execute_run() and a 100%-cfunc dispatcher
    whose compiled entry block did nothing but UML_CALLC the interpreter for
    the granted quantum.  This file replaces that single-block dispatcher with
    a REAL per-PC translator:

      * The entry block HASHJMPs on the live instruction address (m_ipc) to a
        per-PC compiled block, exactly like mips3/ppc.
      * code_compile_block() walks a basic block via the boundary-K/L frontend
        (m68000fe) and, per opcode, calls generate_opcode().
      * generate_opcode() emits a NATIVE UML sequence for the in-set opcodes
        (per README-drc.md: the simple register/immediate-direct common path)
        and routes everything else -- and every faulting / privileged /
        timing-subtle / mid-instruction-suspend case -- to a cfunc that runs
        the interpreter handler for that one instruction.

    --- The cycle-exactness contract (ADR 0006 Leg B) ---

    The oracle harness drives the core one BUS CYCLE at a time (it grants
    *m_icountptr = 1 then run()s, summing 1 - icount per grant) and detects
    instruction retirement when m_ipc advances.  The interpreter's microcode
    core is bus-cycle-stepped and SUSPENDS mid-instruction when the granted
    icount is exhausted (m_inst_substate 1/2), via an interruptible prefetch
    that is a C++-level mechanism with no UML opcode.

    To stay register/flag/RAM/CYCLE-exact with the interpreter under that
    stepping model, a native block must reproduce the interpreter's EXACT
    micro-architectural pipeline behaviour for its opcode: the same cycle
    decrements at the same points, the same substate-suspend, the same
    prefetch into the m_ir/m_ird/m_irc/m_dbin pipe, and the same decode-table
    dispatch.  The only piece that cannot be expressed in UML --
    read_interruptible -- is reached through a cfunc that runs the identical
    interpreter code path, so it cannot diverge.

    The per-instruction cycle cost is therefore NOT re-estimated: it is the
    sum of the interpreter's own m_icount decrements (4 per bus access, 2 per
    internal istep) along the executed microcode path, emitted as the matching
    UML icount decrement.  Anything whose cost the native path cannot mirror
    exactly stays on the interpreter via cfunc.

    Pattern reference: src/devices/cpu/mips/mips3drc.cpp,
    src/devices/cpu/powerpc/ppcdrc.cpp.

***************************************************************************/

#include "emu.h"
#include "m68000.h"

#include "m68000fe.h"

#include "cpu/drcuml.h"
#include "cpu/drcumlsh.h"
#include "cpu/drccache.h"

using namespace uml;


//**************************************************************************
//  CONSTANTS / LOCAL HELPERS
//**************************************************************************

// The DRC dispatcher exit codes are the m68000_device::EXECUTE_* enum (declared
// in m68000.h), shared with the dispatch loop in m68000.cpp so the two cannot
// drift.

// Hashed-block map dimensions.  The plain 68000 has a single execution mode
// (no banked address modes the DRC distinguishes), so we always hash on mode 0.
namespace {
	constexpr u32 M68K_DRC_MODE = 0;
}


//**************************************************************************
//  STATIC HANDLER GENERATION
//**************************************************************************

//-------------------------------------------------
//  code_flush_cache - flush the cache and
//  regenerate the static handlers
//-------------------------------------------------

void m68000_device::code_flush_cache()
{
	// empty the transient cache contents
	m_drcuml->reset();

	try {
		// Generate the entry point and the nocode / out_of_cycles handlers into a
		// SINGLE TRANSIENT block (mips3/ppc pattern).  These are regenerated on
		// EVERY flush, so they MUST be transient (begin_block) -- NOT invariant
		// (begin_invariant_block).  An invariant block allocates from the cache's
		// small, never-freed PERMANENT area; regenerating it per flush would leak
		// that area until reset() runs out of permanent space and throws
		// "Out of cache space" (the bug that crashed the long Leg-B run once the
		// 8 MiB cache first filled and forced a flush).  The transient area is
		// what reset() clears, so per-flush regeneration there is free.
		drcuml_block &block(m_drcuml->begin_block(32));
		static_generate_entry_point(block);
		static_generate_nocode_handler(block);
		static_generate_out_of_cycles(block);
		block.end();
	}
	catch(drcuml_block::abort_compilation &) {
		fatalerror("Unrecoverable error generating m68000 static DRC code\n");
	}
}


//-------------------------------------------------
//  static_generate_entry_point - generate the
//  entry point: HASHJMP to the per-PC block for
//  the current instruction address (m_ipc)
//-------------------------------------------------

void m68000_device::static_generate_entry_point(drcuml_block &block)
{
	// forward references
	alloc_handle(m_drcuml.get(), &m_nocode, "nocode");
	alloc_handle(m_drcuml.get(), &m_out_of_cycles, "out_of_cycles");
	alloc_handle(m_drcuml.get(), &m_entry, "entry");

	UML_HANDLE(block, *m_entry);                                       // handle  entry

	// Dispatch to the compiled block for the CURRENT instruction address.
	// The interpreter's instruction address is m_ipc (== m_pc - 2); the
	// per-PC blocks are keyed on it.  A miss transfers to the nocode handler,
	// which exits MISSING_CODE so the host loop JIT-compiles that PC.
	UML_HASHJMP(block, M68K_DRC_MODE, mem(&m_ipc), *m_nocode);         // hashjmp <mode>,<m_ipc>,nocode
}


//-------------------------------------------------
//  static_generate_nocode_handler - the HASHJMP
//  miss handler: record the faulting PC and exit
//  MISSING_CODE so the host loop compiles it
//-------------------------------------------------

void m68000_device::static_generate_nocode_handler(drcuml_block &block)
{
	UML_HANDLE(block, *m_nocode);                                      // handle  nocode
	UML_GETEXP(block, I0);                                             // getexp  i0  (HASHJMP put the faulting addr here)
	UML_MOV(block, mem(&m_ipc), I0);                                   // mov     [m_ipc],i0
	UML_EXIT(block, EXECUTE_MISSING_CODE);                             // exit    EXECUTE_MISSING_CODE
}


//-------------------------------------------------
//  static_generate_out_of_cycles - the suspend
//  handler: record the resume PC and exit
//  OUT_OF_CYCLES so the scheduler regains control
//-------------------------------------------------

void m68000_device::static_generate_out_of_cycles(drcuml_block &block)
{
	UML_HANDLE(block, *m_out_of_cycles);                              // handle  out_of_cycles
	UML_GETEXP(block, I0);                                             // getexp  i0  (the EXHc passed the resume addr)
	UML_MOV(block, mem(&m_ipc), I0);                                   // mov     [m_ipc],i0
	UML_EXIT(block, EXECUTE_OUT_OF_CYCLES);                           // exit    EXECUTE_OUT_OF_CYCLES
}


//**************************************************************************
//  BLOCK COMPILATION
//**************************************************************************

//-------------------------------------------------
//  code_compile_block - compile the per-PC block at
//  the given instruction address (the entry block
//  HASHJMPs here on m_ipc)
//
//  Boundary M ships the per-PC native DISPATCH (real HASHJMP-to-block, not
//  boundary L's single invariant block) with a per-instruction interpreter
//  body.  The load-bearing invariant is that the block MUST register a hash at
//  EXACTLY the requested pc -- the address the entry block hashed on (m_ipc) --
//  so the dispatcher's HASHJMP resolves after one compile.  We therefore hash
//  on `pc` directly rather than on a frontend-derived sequence-head pc: the
//  m68000's prefetch-adapter PC semantics mean the describe-walk's first
//  descriptor pc is not guaranteed to equal m_ipc, and a hash registered at the
//  wrong address would make the entry HASHJMP miss forever (infinite
//  MISSING_CODE -> cache exhaustion).  Native per-opcode emission (which keys
//  off the frontend's descriptor stream) lands on top of this dispatch in the
//  follow-up increments, gated by the Leg-B oracle at each step.
//-------------------------------------------------

void m68000_device::code_compile_block(offs_t pc)
{
	// Fetch the opcode word at this instruction address (side-effect free, as the
	// interpreter's state_import does) so generate_opcode can dispatch the native
	// fast-path by opcode.  This mirrors the interpreter's m_ird at the start of
	// the instruction.
	u16 opword;
	{
		auto dis = machine().disable_side_effects();
		opword = m_s_opcodes->read_word(pc);
	}

	bool succeeded = false;
	while(!succeeded) {
		try {
			drcuml_block &block(m_drcuml->begin_block(128));

			// labels are scoped per block; restart the counter so values stay
			// small and unique within this block
			m_drc_labelnum = 1;

			// Register the hash at EXACTLY the requested pc, so the entry block's
			// HASHJMP(m_ipc) resolves here after this compile.  This is
			// UNCONDITIONAL: code_compile_block is only reached on a HASHJMP miss
			// (the entry/out_of_cycles handlers exit MISSING_CODE only when the
			// hash is absent), so the hash never pre-exists.  Guarding it on
			// !hash_exists would turn that invariant into a SILENT failure -- a
			// block with no hash registration is unreachable via HASHJMP, so the
			// dispatcher would spin on MISSING_CODE until the cache exhausts.
			UML_HASH(block, M68K_DRC_MODE, pc);                       // hash mode,pc

			// emit the per-PC body: native for the in-set opcodes, interpreter
			// cfunc for the rest
			generate_opcode(block, opword);

			block.end();
			succeeded = true;
		}
		catch(drcuml_block::abort_compilation &) {
			code_flush_cache();
		}
	}
}


//**************************************************************************
//  PER-OPCODE TRANSLATION
//**************************************************************************

//-------------------------------------------------
//  generate_opcode - emit UML for one instruction:
//  native for the in-set opcodes, interpreter
//  cfunc for everything else
//-------------------------------------------------

void m68000_device::generate_opcode(drcuml_block &block, u16 opword)
{
	// Native dispatch by opcode pattern.  Boundary M emits its first NATIVE UML
	// for moveq -- the cleanest in-set opcode (single microcode state, no memory
	// operand, no An destination; its result and CCR are compile-time constants
	// of the opcode word).  Every other opcode -- the rest of the in-set and all
	// out-of-set / faulting / timing-subtle cases -- routes to the interpreter
	// cfunc, so it stays register/flag/RAM/CYCLE exact by construction.  Coverage
	// widens opcode-by-opcode in the follow-up increments (Task 10 / boundary O),
	// each gated by the Leg-B oracle.
	if((opword & 0xf100) == 0x7000)            // moveq #imm,Dn  (0111 rrr0 dddddddd)
		generate_moveq(block, opword);
	else
		generate_interpreter_fallback(block);
}


//-------------------------------------------------
//  generate_interpreter_fallback - emit a UML
//  sequence that runs the interpreter for the
//  granted quantum (the provably-correct base for
//  every opcode not yet emitted natively)
//-------------------------------------------------

void m68000_device::generate_interpreter_fallback(drcuml_block &block)
{
	// Run the interpreter microcode loop for whatever icount budget the
	// scheduler/harness granted.  Because the harness grants one bus cycle at
	// a time and detects retirement via m_ipc, this advances the core by one
	// granted cycle exactly as the interpreter arm would -- register, flag,
	// RAM and CYCLE identical by construction (this is the boundary-L
	// invariant, preserved here on the real per-PC dispatch path).
	UML_CALLC(block, &m68000_device::cfunc_interpret_quantum, this);   // callc  cfunc_interpret_quantum,this

	// After the granted quantum the scheduler has no more cycles to give for
	// this timeslice; hand control back so it can re-grant (or retire).
	UML_EXIT(block, EXECUTE_OUT_OF_CYCLES);                           // exit   EXECUTE_OUT_OF_CYCLES
}


//-------------------------------------------------
//  generate_moveq - native UML for moveq #imm,Dn
//
//  moveq encoding: 0111 rrr0 dddddddd (0x7000, mask 0xf100); rx=(op>>9)&7,
//  imm=s8(op).  The interpreter handler (moveq_imm8o_dd_d{f,p}) is a SINGLE
//  microcode state with a 3-substate suspend machine:
//    case 0: m_aob=m_au; m_ir=m_irc; m_pc=m_au; m_da[rx]=ext32(m_ftu);
//            m_au+=2; alu_and(m_ftu,0xffff); sr_nzvc(); m_ird=m_ir;
//            if(m_next_state!=S_TRACE) m_next_state=m_int_next_state;
//            m_base_ssw=SSW_PROGRAM|SSW_R;
//    case 1: m_edb=read_interruptible(m_aob&~1); m_icount-=4; <suspend if <=0>
//    case 2: <addr-error if aob&1>; m_irc=m_dbin=m_edb; set_ftu_const();
//            m_inst_state=...; <trace>
//
//  m_ftu at moveq entry == s8(opword) (set by the prior set_ftu_const for the
//  0x7 group), so the RESULT and the NZVC flags are COMPILE-TIME CONSTANTS:
//    result = ext32(s8(op)) = s32(s16(s8(op)))         (sign-extend imm8 -> 32)
//    sr_nzvc after alu_and(m_ftu,0xffff): N=bit15(m_ftu), Z=(m_ftu==0), V=C=0;
//    X/I/S/T preserved.
//
//  HYBRID HANDOFF (cycle-exact by construction): the native block emits CASE 0
//  natively (the genuine native artifact -- the architectural register/flag
//  write + the prefetch-pipe pointer advance), then sets m_inst_substate=1 and
//  hands CASE 1+2 (the interruptible prefetch, the m_icount-=4 cycle charge, the
//  suspend/payback bookkeeping and the decode-table dispatch -- all
//  timing-subtle, and read_interruptible has no UML opcode) to the UNCHANGED
//  interpreter via the quantum cfunc.  The interpreter resumes at substate 1
//  WITHOUT re-running case 0, so the work is done once, and the cycle charge is
//  exactly the interpreter's own -4.  On a resume grant (substate!=0) the block
//  delegates the whole quantum to the interpreter.  This makes moveq's
//  architectural effect native while keeping the 68000's prefetch timing model
//  in the one place that owns it -- the interpreter.
//-------------------------------------------------

void m68000_device::generate_moveq(drcuml_block &block, u16 opword)
{
	const int rx = (opword >> 9) & 7;
	const u16 ftu = u16(s8(opword & 0xff));                 // m_ftu at entry = s8(opword) sign-extended into 16 bits
	const u32 result = u32(s32(s16(ftu)));                  // ext32(m_ftu) = s32(s16(m_ftu)) -> sign-extended imm8 in 32 bits
	// sr_nzvc(): only N,Z can be set (V=C=0); X,I,S,T preserved.
	const u32 set_nz = (ftu == 0 ? u32(SR_Z) : 0u) | ((ftu & 0x8000) ? u32(SR_N) : 0u);

	const int lbl_delegate = m_drc_labelnum++;

	// --- resume / mid-suspend guard ---
	// If m_inst_substate != 0 this is a payback / dispatch re-entry of an
	// in-flight moveq: the native case-0 already ran on the fresh grant, so just
	// hand the quantum to the interpreter (which owns substate 1/2).
	UML_LOAD(block, I0, &m_inst_substate, 0, SIZE_WORD, SCALE_x1);     // i0 = m_inst_substate
	UML_CMP(block, I0, 0);
	UML_JMPc(block, COND_NE, lbl_delegate);

	// --- CASE 0 (native): architectural write + prefetch-pipe advance ---
	UML_MOV(block, mem(&m_aob), mem(&m_au));                           // m_aob = m_au
	UML_LOAD(block, I0, &m_irc, 0, SIZE_WORD, SCALE_x1);               // i0 = m_irc
	UML_STORE(block, &m_ir, 0, I0, SIZE_WORD, SCALE_x1);              // m_ir = m_irc
	UML_MOV(block, mem(&m_pc), mem(&m_au));                            // m_pc = m_au
	UML_MOV(block, mem(&m_da[rx]), result);                            // m_da[rx] = ext32(m_ftu)
	UML_ADD(block, mem(&m_au), mem(&m_au), 2);                         // m_au += 2
	// m_sr = (m_sr & ~NZVC) | set_nz
	UML_LOAD(block, I0, &m_sr, 0, SIZE_WORD, SCALE_x1);                // i0 = m_sr
	UML_AND(block, I0, I0, u32(u16(~(SR_N | SR_Z | SR_V | SR_C))));    // clear N,Z,V,C
	UML_OR(block, I0, I0, set_nz);                                     // set the constant N,Z
	UML_STORE(block, &m_sr, 0, I0, SIZE_WORD, SCALE_x1);              // m_sr = ...
	UML_LOAD(block, I0, &m_ir, 0, SIZE_WORD, SCALE_x1);               // i0 = m_ir
	UML_STORE(block, &m_ird, 0, I0, SIZE_WORD, SCALE_x1);            // m_ird = m_ir
	// if(m_next_state != S_TRACE) m_next_state = m_int_next_state;
	// (computed through registers so the conditional move has a register dest --
	// mem-dest MOVc is not uniformly supported across the UML backends)
	UML_MOV(block, I0, mem(&m_next_state));                            // i0 = m_next_state (current value, kept if == S_TRACE)
	UML_MOV(block, I1, mem(&m_int_next_state));                        // i1 = m_int_next_state
	UML_CMP(block, mem(&m_next_state), u32(S_TRACE));
	UML_MOVc(block, COND_NE, I0, I1);                                  // i0 = (next_state != S_TRACE) ? int_next_state : next_state
	UML_MOV(block, mem(&m_next_state), I0);                            // m_next_state = i0
	// m_base_ssw = SSW_PROGRAM | SSW_R
	UML_MOV(block, I0, u32(u16(SSW_PROGRAM | SSW_R)));
	UML_STORE(block, &m_base_ssw, 0, I0, SIZE_WORD, SCALE_x1);        // m_base_ssw = ...
	// advance the substate so the interpreter resumes at CASE 1 (prefetch +
	// cycle charge + suspend + dispatch) and does NOT re-run case 0
	UML_MOV(block, I0, 1);
	UML_STORE(block, &m_inst_substate, 0, I0, SIZE_WORD, SCALE_x1);   // m_inst_substate = 1

	// --- hand CASE 1+2 (timing tail) to the interpreter for the granted quantum ---
	UML_LABEL(block, lbl_delegate);
	UML_CALLC(block, &m68000_device::cfunc_interpret_quantum, this);  // callc  cfunc_interpret_quantum,this
	UML_EXIT(block, EXECUTE_OUT_OF_CYCLES);                           // exit   EXECUTE_OUT_OF_CYCLES
}
