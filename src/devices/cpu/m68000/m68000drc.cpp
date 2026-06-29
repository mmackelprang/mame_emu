// license:BSD-3-Clause
// copyright-holders:Mark Mackelprang
/***************************************************************************

    m68000drc.cpp

    UML translator + dispatcher for the m68000 DRC recompiler
    (PR boundary M -- the first native UML emission).

    --- Dispatch model: one resident block, in-block opcode dispatch ---

    Boundary L wired the dual-path execute_run() and a 100%-cfunc dispatcher
    whose single resident entry block did nothing but UML_CALLC the interpreter
    for the granted quantum.  Boundary M keeps that SINGLE RESIDENT BLOCK and
    turns it into a real translator by adding an IN-BLOCK opcode dispatch: the
    entry block decodes the current opword (m_ird) and runs a NATIVE UML
    fast-path for the in-set opcodes, falling through to the interpreter cfunc
    for everything else.

    It deliberately does NOT compile a block per PC / HASHJMP on m_ipc.  A
    per-PC scheme overruns the 8 MiB code cache on a corpus that scatters
    ~310 k distinct PCs across the 24-bit space, forcing repeated cache flushes
    -- which (a) is fragile across UML backends (a flush-time fatal escapes and
    is swallowed by running_machine::run(), and the per-block code is larger on
    the SysV ABI so Linux tips over where Win64 has headroom), and (b) makes
    compiled blocks go stale when the harness rewrites program RAM between
    cases (a per-PC native block hardcodes its opcode; the next case's different
    opcode at the same PC would run the stale block).  A single resident block
    that decodes m_ird at RUNTIME has neither problem: nothing is cached per PC,
    so there is nothing to flush and nothing to go stale.

    --- The cycle-exactness contract (ADR 0006 Leg B) ---

    The oracle harness drives the core one BUS CYCLE at a time (it grants
    *m_icountptr = 1 then run()s, summing 1 - icount per grant) and detects
    instruction retirement when m_ipc advances.  The interpreter's microcode
    core is bus-cycle-stepped and SUSPENDS mid-instruction when the granted
    icount is exhausted (m_inst_substate 1/2), via an interruptible prefetch
    that is a C++-level mechanism with no UML opcode.

    To stay register/flag/RAM/CYCLE-exact under that stepping model without
    re-implementing the whole stepped loop, the native moveq fast-path emits the
    interpreter microcode's CASE 0 (the architectural register/flag write + the
    prefetch-pipe pointer advance) natively, then sets m_inst_substate=1 and
    hands CASE 1+2 (the interruptible prefetch, the m_icount-=4 cycle charge, the
    suspend/payback bookkeeping and the decode-table dispatch) to the UNCHANGED
    interpreter via the quantum cfunc.  The interpreter resumes at substate 1
    WITHOUT re-running case 0, so the work happens once and the cycle charge is
    exactly the interpreter's own -4 -- never re-estimated.  Anything the native
    path cannot mirror exactly stays on the interpreter via cfunc.

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


//**************************************************************************
//  STATIC HANDLER GENERATION
//**************************************************************************

//-------------------------------------------------
//  code_flush_cache - (re)generate the single
//  resident entry block
//
//  There are no per-PC blocks, so this is only ever called once (the initial
//  generation); it never runs as a mid-execution flush, because nothing is
//  compiled per PC to overflow the cache.  It is kept as the standard
//  cache-dirty entry point.
//-------------------------------------------------

void m68000_device::code_flush_cache()
{
	// empty the transient cache contents
	m_drcuml->reset();

	try {
		// labels are scoped per block; restart the counter (the entry block
		// allocates a couple)
		m_drc_labelnum = 1;

		// Generate the single resident entry block into a TRANSIENT block
		// (begin_block, not begin_invariant_block).  reset() clears the
		// transient area, so regenerating here is free; an invariant block would
		// allocate from the small never-freed permanent area.
		//
		// maxinst must cover EVERY native opcode's UML emitted into this one
		// resident block (begin_block allocates maxinst*9/4 instruction slots and
		// drcuml_block_append throws emu_fatalerror "Overran maxinst" past that).
		// boundary M's 64 sufficed for moveq alone (~50 ins); O-mem-1's native
		// btst-absolute (.W 4-read + .L 5-read sequences) brought the block to
		// ~530 instructions; O-mem-2's 24 bit-op forms (each a full ext/EA/read/
		// modify/refill/write/retire sequence) add ~4k more -- size it with
		// headroom so adding an opcode does not silently overrun the block.  At
		// 8192 the transient descriptor allocation (~18k slots) is a small
		// fraction of the 8 MiB DRC cache.
		drcuml_block &block(m_drcuml->begin_block(8192));
		static_generate_entry_point(block);
		block.end();
	}
	catch(drcuml_block::abort_compilation &) {
		fatalerror("Unrecoverable error generating m68000 static DRC code\n");
	}
}


//-------------------------------------------------
//  static_generate_entry_point - the single
//  resident block: dispatch the current opcode to
//  its native fast-path, else to the interpreter
//-------------------------------------------------

void m68000_device::static_generate_entry_point(drcuml_block &block)
{
	alloc_handle(m_drcuml.get(), &m_entry, "entry");

	uml::code_label const lbl_delegate = m_drc_labelnum++;

	UML_HANDLE(block, *m_entry);                                       // handle  entry

	// A native fast-path may run ONLY at a genuine instruction-fetch boundary --
	// the exact point the interpreter's loop is about to execute the FIRST
	// microcode state of the instruction whose opword is in m_ird.  THREE
	// conditions must all hold; otherwise delegate the whole quantum to the
	// interpreter (which owns the microcode sequencing):
	//
	//  (1) m_inst_substate == 0 -- not suspended mid-state on an exhausted
	//      quantum.
	//  (2) m_inst_state == m_decode_table[m_ird] -- the core is positioned at the
	//      START state of decoding m_ird, not partway through a multi-STATE
	//      opcode (which visits several microcode states, all with substate 0).
	//      m_decode_table is resized once in init_decode_table() (before DRC
	//      setup) and never moves, so its data pointer is a stable constant.
	//  (3) m_ipc == m_pc - 2 -- m_ipc reflects the CURRENT opword (m_ird).  This
	//      is the load-bearing guard against the retirement grant: when a prior
	//      multi-state instruction (e.g. ABCD) completes, its final prefetch loads
	//      the NEXT opword into m_ird and advances m_pc, but m_ipc still points at
	//      the prior instruction (the interpreter only re-sets m_ipc = m_pc - 2 at
	//      the top of its loop when it actually begins the next instruction).  On
	//      that grant the next opword may match a native pattern, yet running a
	//      native fast-path would do the next instruction's work one grant early
	//      (before m_ipc advances), diverging from the interpreter.  At a TRUE
	//      first-grant of the instruction being stepped, m_ipc == m_pc - 2 holds;
	//      on a prior instruction's retirement grant it does not.
	UML_LOAD(block, I0, &m_inst_substate, 0, SIZE_WORD, SCALE_x1);     // i0 = m_inst_substate
	UML_CMP(block, I0, 0);
	UML_JMPc(block, COND_NE, lbl_delegate);                           // mid-state suspend -> interpreter

	// m_ipc == m_pc - 2  (both u32).
	// NB: device-state fields are accessed via UML_LOAD/UML_STORE (a pointer
	// base), NOT plain mem(&field) operands.  drcbe_x64 lowers a mem() operand
	// RBP-relative and THROWS offset_from_rbp if the field is >2 GiB from the
	// cache (the heap m68000_device vs the high-mmap'd executable drc_cache on
	// Linux/SysV -- which silently fataled the oracle there while passing on
	// Windows, where both allocations stay low).  LOAD/STORE route the base
	// through get_base_register_and_offset, which falls back to an absolute
	// pointer load and never throws -- correct on every OS.
	UML_LOAD(block, I0, &m_pc, 0, SIZE_DWORD, SCALE_x1);               // i0 = m_pc
	UML_SUB(block, I0, I0, 2);                                        // i0 = m_pc - 2
	UML_LOAD(block, I1, &m_ipc, 0, SIZE_DWORD, SCALE_x1);              // i1 = m_ipc
	UML_CMP(block, I0, I1);
	UML_JMPc(block, COND_NE, lbl_delegate);                           // m_ipc not at the current opword -> interpreter

	UML_LOAD(block, I7, &m_ird, 0, SIZE_WORD, SCALE_x1);               // i7 = m_ird (opword); kept across the fast-paths

	// I1 = m_decode_table[m_ird]  (u16 entries); compare to m_inst_state (u16)
	UML_LOAD(block, I1, m_decode_table.data(), I7, SIZE_WORD, SCALE_x2); // i1 = decode_table[m_ird]
	UML_LOAD(block, I0, &m_inst_state, 0, SIZE_WORD, SCALE_x1);        // i0 = m_inst_state
	UML_CMP(block, I0, I1);
	UML_JMPc(block, COND_NE, lbl_delegate);                           // mid multi-state instruction -> interpreter

	generate_native_dispatch(block, lbl_delegate);

	// --- interpreter delegate (the granted quantum) ---
	UML_LABEL(block, lbl_delegate);
	UML_CALLC(block, &m68000_device::cfunc_interpret_quantum, this);   // callc  cfunc_interpret_quantum,this
	UML_EXIT(block, EXECUTE_OUT_OF_CYCLES);                           // exit   EXECUTE_OUT_OF_CYCLES
}


//**************************************************************************
//  NATIVE OPCODE DISPATCH + EMITTERS
//**************************************************************************

//-------------------------------------------------
//  is_native_opcode - the predicate identifying
//  which opcodes have a native fast-path
//-------------------------------------------------

bool m68000_device::is_native_opcode(u16 opword)
{
	// moveq #imm,Dn : 0111 rrr0 dddddddd  (bit 8 must be 0)
	if((opword & 0xf100) == 0x7000)
		return true;
	// btst #n,(xxx).W : 0x0838  /  btst #n,(xxx).L : 0x0839  (mask 0xfffe matches both)
	if((opword & 0xfffe) == 0x0838)
		return true;
	// O-mem-2: btst/bchg/bclr/bset #n,(An)/(An)+/-(An)  (mask 0xfff8)
	switch(opword & 0xfff8)
	{
	case 0x0810: case 0x0818: case 0x0820:   // btst #n
	case 0x0850: case 0x0858: case 0x0860:   // bchg #n
	case 0x0890: case 0x0898: case 0x08a0:   // bclr #n
	case 0x08d0: case 0x08d8: case 0x08e0:   // bset #n
		return true;
	}
	// O-mem-2: btst/bchg/bclr/bset Dn,(An)/(An)+/-(An)  (mask 0xf1f8)
	switch(opword & 0xf1f8)
	{
	case 0x0110: case 0x0118: case 0x0120:   // btst Dn
	case 0x0150: case 0x0158: case 0x0160:   // bchg Dn
	case 0x0190: case 0x0198: case 0x01a0:   // bclr Dn
	case 0x01d0: case 0x01d8: case 0x01e0:   // bset Dn
		return true;
	}
	return false;
}


//-------------------------------------------------
//  generate_native_dispatch - emit the in-block
//  opcode dispatch.  I7 holds the opword (m_ird).
//  A matched native opcode runs its fast-path and
//  ends by jumping to lbl_delegate (the interpreter
//  handles the timing tail / the granted quantum).
//  An unmatched opcode falls through to
//  lbl_delegate untouched.
//-------------------------------------------------

void m68000_device::generate_native_dispatch(drcuml_block &block, uml::code_label lbl_delegate)
{
	m_drc_native_mem_ea_arms = 0; // emission probe: counts gated memory-EA arms actually emitted (Task 7)

	// moveq: (m_ird & 0xf100) == 0x7000
	{
		uml::code_label const lbl_not_moveq = m_drc_labelnum++;
		UML_AND(block, I0, I7, 0xf100);                               // i0 = opword & 0xf100
		UML_CMP(block, I0, 0x7000);
		UML_JMPc(block, COND_NE, lbl_not_moveq);                      // not moveq -> next test / delegate
		generate_moveq(block);                                        // native moveq CASE 0
		UML_JMP(block, lbl_delegate);                                 // hand the timing tail to the interpreter
		UML_LABEL(block, lbl_not_moveq);
	}

	// btst #n,(xxx).W/.L: (m_ird & 0xfffe) == 0x0838 -- native ONLY behind the
	// space-topology gate (ADR 0007 Addendum 2026-06-28).  When the bound bus
	// configures AS_OPCODES / user spaces / an MMU, the arm is NOT emitted and
	// dispatch falls through to lbl_delegate (cfunc_), exactly as before O-mem-1
	// -- SPACE_PROGRAM would otherwise read the wrong space.
	if (drc_native_mem_ea_allowed())
	{
		uml::code_label const lbl_not_btst_abs = m_drc_labelnum++;
		UML_AND(block, I0, I7, 0xfffe);                              // i0 = opword & 0xfffe
		UML_CMP(block, I0, 0x0838);
		UML_JMPc(block, COND_NE, lbl_not_btst_abs);                 // not btst-absolute -> next test / delegate
		generate_btst_imm8_absolute(block, lbl_delegate);          // emit the native opcode (suspend/fault paths JMP lbl_delegate from within)
		UML_JMP(block, lbl_delegate);                               // fully-granted path: retired -> hand the timing tail to the interpreter
		UML_LABEL(block, lbl_not_btst_abs);
		m_drc_native_mem_ea_arms++;                                 // emission probe (Task 7)
	}

	// O-mem-2: btst/bchg/bclr/bset (An)/(An)+/-(An), #imm8 and Dn source -- native
	// ONLY behind the space-topology gate (ADR 0007 Addendum; W6).  Each arm calls
	// the generic emitter with its form constants; the bus-step run (substates /
	// charges / pre_charge) is single-sourced from the generator.  The #imm8 forms
	// (mask 0xfff8) and Dn forms (mask 0xf1f8) are disjoint on the EA-mode field and
	// never collide with the btst-absolute arm above (0x0838/0x0839).
	if (drc_native_mem_ea_allowed())
	{
		static const bitop_form k_forms[] = {
			// #imm8 source (mask 0xfff8)
			{ 0x0810, 0xfff8, BITOP_BTST, BITEA_AIS,  BITSRC_IMM8 },
			{ 0x0818, 0xfff8, BITOP_BTST, BITEA_AIPS, BITSRC_IMM8 },
			{ 0x0820, 0xfff8, BITOP_BTST, BITEA_PAIS, BITSRC_IMM8 },
			{ 0x0850, 0xfff8, BITOP_BCHG, BITEA_AIS,  BITSRC_IMM8 },
			{ 0x0858, 0xfff8, BITOP_BCHG, BITEA_AIPS, BITSRC_IMM8 },
			{ 0x0860, 0xfff8, BITOP_BCHG, BITEA_PAIS, BITSRC_IMM8 },
			{ 0x0890, 0xfff8, BITOP_BCLR, BITEA_AIS,  BITSRC_IMM8 },
			{ 0x0898, 0xfff8, BITOP_BCLR, BITEA_AIPS, BITSRC_IMM8 },
			{ 0x08a0, 0xfff8, BITOP_BCLR, BITEA_PAIS, BITSRC_IMM8 },
			{ 0x08d0, 0xfff8, BITOP_BSET, BITEA_AIS,  BITSRC_IMM8 },
			{ 0x08d8, 0xfff8, BITOP_BSET, BITEA_AIPS, BITSRC_IMM8 },
			{ 0x08e0, 0xfff8, BITOP_BSET, BITEA_PAIS, BITSRC_IMM8 },
			// Dn source (mask 0xf1f8)
			{ 0x0110, 0xf1f8, BITOP_BTST, BITEA_AIS,  BITSRC_DN },
			{ 0x0118, 0xf1f8, BITOP_BTST, BITEA_AIPS, BITSRC_DN },
			{ 0x0120, 0xf1f8, BITOP_BTST, BITEA_PAIS, BITSRC_DN },
			{ 0x0150, 0xf1f8, BITOP_BCHG, BITEA_AIS,  BITSRC_DN },
			{ 0x0158, 0xf1f8, BITOP_BCHG, BITEA_AIPS, BITSRC_DN },
			{ 0x0160, 0xf1f8, BITOP_BCHG, BITEA_PAIS, BITSRC_DN },
			{ 0x0190, 0xf1f8, BITOP_BCLR, BITEA_AIS,  BITSRC_DN },
			{ 0x0198, 0xf1f8, BITOP_BCLR, BITEA_AIPS, BITSRC_DN },
			{ 0x01a0, 0xf1f8, BITOP_BCLR, BITEA_PAIS, BITSRC_DN },
			{ 0x01d0, 0xf1f8, BITOP_BSET, BITEA_AIS,  BITSRC_DN },
			{ 0x01d8, 0xf1f8, BITOP_BSET, BITEA_AIPS, BITSRC_DN },
			{ 0x01e0, 0xf1f8, BITOP_BSET, BITEA_PAIS, BITSRC_DN },
		};
		for(const bitop_form &f : k_forms)
		{
			uml::code_label const lbl_next = m_drc_labelnum++;
			UML_AND(block, I0, I7, f.mask);
			UML_CMP(block, I0, f.value);
			UML_JMPc(block, COND_NE, lbl_next);                     // not this form -> next test
			generate_bitop_mem(block, f, lbl_delegate);            // emit the form (suspend yields JMP lbl_delegate from within)
			UML_JMP(block, lbl_delegate);                          // fully-granted: retired -> hand the tail to the interpreter
			UML_LABEL(block, lbl_next);
			m_drc_native_mem_ea_arms++;                            // emission probe (gate test)
		}
	}

	// (more native opcodes are added here in boundary O, each ending in
	//  UML_JMP(lbl_delegate); the final fall-through reaches lbl_delegate.)
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
//  m_ftu at moveq entry == s8(opword), so ext32(m_ftu) == s32(s16(s8(op))) ==
//  sign-extend(imm8) into 32 bits, and sr_nzvc after alu_and(m_ftu,0xffff)
//  gives N=bit15(m_ftu)=sign of imm8, Z=(imm8==0), V=C=0; X/I/S/T preserved.
//
//  This emits CASE 0 natively (decoding rx/imm from m_ird at RUNTIME -- I7 holds
//  the opword), sets m_inst_substate=1, and returns; the caller jumps to the
//  interpreter delegate, which resumes at substate 1 (CASE 1+2: the prefetch,
//  the m_icount-=4 cycle charge, the suspend/dispatch) WITHOUT re-running CASE 0.
//  So the architectural effect is native and the cycle cost is exactly the
//  interpreter's own -4 (never re-estimated).  Clobbers I0-I6; preserves I7.
//-------------------------------------------------

void m68000_device::generate_moveq(drcuml_block &block)
{
	// result = ext32(s8(opword)) = sign-extend the low byte of the opword to 32 bits
	UML_SEXT(block, I2, I7, SIZE_BYTE);                               // i2 = s32(s8(opword)) = moveq result

	// rx = (opword >> 9) & 7
	UML_SHR(block, I3, I7, 9);
	UML_AND(block, I3, I3, 7);                                        // i3 = rx (0..7)

	// m_da[rx] = result   (m_da is u32[]; index by rx with scale 4)
	UML_STORE(block, &m_da[0], I3, I2, SIZE_DWORD, SCALE_x4);         // m_da[rx] = result

	// flags: sr_nzvc -> N = (result < 0), Z = (result == 0), V = C = 0; X/I/S/T kept.
	//   set_nz = (Z ? SR_Z : 0) | (N ? SR_N : 0)
	UML_CMP(block, I2, 0);
	UML_SETc(block, COND_Z, I4);                                      // i4 = (result == 0)
	UML_SETc(block, COND_S, I5);                                      // i5 = (result <  0)  (sign bit)
	UML_SHL(block, I4, I4, 2);                                        // i4 = Z ? SR_Z(0x04) : 0
	UML_SHL(block, I5, I5, 3);                                        // i5 = N ? SR_N(0x08) : 0
	UML_OR(block, I4, I4, I5);                                        // i4 = set_nz
	UML_LOAD(block, I0, &m_sr, 0, SIZE_WORD, SCALE_x1);               // i0 = m_sr
	UML_AND(block, I0, I0, u32(u16(~(SR_N | SR_Z | SR_V | SR_C))));   // clear N,Z,V,C
	UML_OR(block, I0, I0, I4);                                        // set N,Z
	UML_STORE(block, &m_sr, 0, I0, SIZE_WORD, SCALE_x1);             // m_sr = ...

	// prefetch-pipe pointer advance (CASE 0):
	//   m_aob = m_au; m_ir = m_irc; m_pc = m_au; m_au += 2; m_ird = m_ir;
	// (all device-state access via UML_LOAD/UML_STORE -- see the note in
	// static_generate_entry_point on why plain mem(&field) is avoided.)
	UML_LOAD(block, I2, &m_au, 0, SIZE_DWORD, SCALE_x1);              // i2 = m_au
	UML_STORE(block, &m_aob, 0, I2, SIZE_DWORD, SCALE_x1);            // m_aob = m_au
	UML_STORE(block, &m_pc, 0, I2, SIZE_DWORD, SCALE_x1);             // m_pc = m_au
	UML_ADD(block, I2, I2, 2);                                       // i2 = m_au + 2
	UML_STORE(block, &m_au, 0, I2, SIZE_DWORD, SCALE_x1);             // m_au += 2
	UML_LOAD(block, I0, &m_irc, 0, SIZE_WORD, SCALE_x1);              // i0 = m_irc
	UML_STORE(block, &m_ir, 0, I0, SIZE_WORD, SCALE_x1);            // m_ir = m_irc
	UML_STORE(block, &m_ird, 0, I0, SIZE_WORD, SCALE_x1);          // m_ird = m_ir (== old m_irc, in i0)

	// if(m_next_state != S_TRACE) m_next_state = m_int_next_state;
	// (register-dest conditional move -- mem-dest MOVc is not uniformly supported)
	UML_LOAD(block, I0, &m_next_state, 0, SIZE_DWORD, SCALE_x1);     // i0 = m_next_state (kept if == S_TRACE)
	UML_LOAD(block, I1, &m_int_next_state, 0, SIZE_DWORD, SCALE_x1); // i1 = m_int_next_state
	UML_CMP(block, I0, u32(S_TRACE));
	UML_MOVc(block, COND_NE, I0, I1);                                // i0 = (next_state != S_TRACE) ? int_next_state : next_state
	UML_STORE(block, &m_next_state, 0, I0, SIZE_DWORD, SCALE_x1);    // m_next_state = i0

	// m_base_ssw = SSW_PROGRAM | SSW_R
	UML_MOV(block, I0, u32(u16(SSW_PROGRAM | SSW_R)));
	UML_STORE(block, &m_base_ssw, 0, I0, SIZE_WORD, SCALE_x1);       // m_base_ssw = ...

	// advance the substate so the interpreter resumes at CASE 1 (prefetch +
	// cycle charge + suspend + dispatch) and does NOT re-run CASE 0
	UML_MOV(block, I0, 1);
	UML_STORE(block, &m_inst_substate, 0, I0, SIZE_WORD, SCALE_x1);  // m_inst_substate = 1
}

//-------------------------------------------------
//  generate_bus_step - emit ONE native 68000 bus
//  access step (read OR write, by step.kind) +
//  the interpreter's exact per-bus-cycle checkpoint
//  (charge, two-way suspend, address-error).  A
//  read does UML_READ (+ byte-lane select, m_edb
//  commit); a DATA_WRITE does UML_WRITEM (masked
//  word write of m_dbout, no m_edb commit, no
//  address-error -- ADR 0007 W1).  An optional
//  step.pre_charge (the -(An) predecrement -2) is
//  charged first with NO checkpoint.  The CALLER has
//  already emitted the step's address / m_dbout /
//  m_base_ssw setup.  On suspend or fault this stores
//  the descriptor's substate (or S_ADDRESS_ERROR) and
//  JMPs to lbl_delegate (yield -> interpreter resumes,
//  OQ-1).  On the clean path it falls through.
//  Clobbers I0-I6; preserves I7.
//-------------------------------------------------

void m68000_device::generate_bus_step(drcuml_block &block, const struct drc_bus_step &step, uml::code_label lbl_delegate)
{
	uml::code_label const lbl_not_suspended = m_drc_labelnum++;
	uml::code_label const lbl_completed     = m_drc_labelnum++;
	uml::code_label const lbl_no_fault      = m_drc_labelnum++;

	// predecrement internal micro-charge (-(An)): charged with NO suspend
	// checkpoint, exactly as the interpreter's pdcw1 'm_icount -= 2;' (W2/W5).
	// The descriptor folds it onto the following data-read/write step; 0 otherwise.
	if(step.pre_charge)
	{
		UML_LOAD(block, I3, &m_icount, 0, SIZE_DWORD, SCALE_x1);
		UML_SUB(block, I3, I3, step.pre_charge);
		UML_STORE(block, &m_icount, 0, I3, SIZE_DWORD, SCALE_x1);
	}

	// address = m_aob & ~1  (the interpreter accesses the word at the even address)
	UML_LOAD(block, I1, &m_aob, 0, SIZE_DWORD, SCALE_x1);            // i1 = m_aob
	UML_AND(block, I2, I1, ~u32(1));                                 // i2 = m_aob & ~1

	// THE ACCESS.  Both prefetch and data reads, and the data write, go through
	// SPACE_PROGRAM (the 68000 has one program space under the gate; the
	// SSW_PROGRAM/SSW_DATA difference is an architectural field the caller wrote,
	// not a UML space selection).  Branch on the descriptor's kind (ADR 0007 W1):
	// the charge + suspend checkpoint below are shared; only the access op and the
	// fault branch differ.
	if(step.kind == DRC_BUS_DATA_WRITE)
	{
		// WRITE: m_program.write_interruptible(m_aob & ~1, m_dbout, (m_aob&1)?0x00ff:0xff00)
		// The CALLER has already done set_8xl(m_dbout, modified) and m_base_ssw =
		// SSW_DATA.  UML_WRITEM is a masked word write at the even address mirroring
		// the interpreter (a SIZE_BYTE write at the byte address would present a
		// different bus shape -- W1: do NOT decompose).  No m_edb commit, no
		// address-error branch (has_addr_error == 0 for writes).
		uml::code_label const lbl_mask_done = m_drc_labelnum++;
		UML_TEST(block, I1, 1);                                     // m_aob & 1 ?
		UML_MOV(block, I4, u32(0xff00));                           // even -> high lane
		UML_JMPc(block, COND_Z, lbl_mask_done);
		UML_MOV(block, I4, u32(0x00ff));                           // odd -> low lane
		UML_LABEL(block, lbl_mask_done);
		UML_LOAD(block, I0, &m_dbout, 0, SIZE_WORD, SCALE_x1);     // i0 = m_dbout (replicated byte)
		UML_WRITEM(block, I2, I0, I4, SIZE_WORD, SPACE_PROGRAM);   // masked word write, byte lane
	}
	else
	{
		// READ (unchanged from O-mem-1): read the word, then a data read selects a
		// byte lane and commits m_edb.
		UML_READ(block, I0, I2, SIZE_WORD, SPACE_PROGRAM);         // i0 = read word at (m_aob & ~1)

		if(step.byte_lane)
		{
			// m_edb = read; if(!(m_aob & 1)) m_edb >>= 8; then keep the low byte.
			// (m_aob&1 ? low byte : high byte) -- matches the interpreter's lane mask
			// 0x00ff/0xff00 + the ">>8 when even" select.
			uml::code_label const lbl_odd = m_drc_labelnum++;
			UML_TEST(block, I1, 1);                                 // m_aob & 1 ?
			UML_JMPc(block, COND_NZ, lbl_odd);                      // odd -> low byte already in place
				UML_SHR(block, I0, I0, 8);                          // even -> high byte to low
			UML_LABEL(block, lbl_odd);
			UML_AND(block, I0, I0, 0xff);                           // keep the selected byte
		}

		// commit the read into m_edb (the interpreter stores read result in m_edb).
		// m_edb is u16 -> SIZE_WORD (a DWORD store would clobber the adjacent m_irc).
		UML_STORE(block, &m_edb, 0, I0, SIZE_WORD, SCALE_x1);       // m_edb = read
	}

	// m_icount -= charge
	UML_LOAD(block, I3, &m_icount, 0, SIZE_DWORD, SCALE_x1);        // i3 = m_icount
	UML_SUB(block, I3, I3, step.charge);                            // i3 -= N
	UML_STORE(block, &m_icount, 0, I3, SIZE_DWORD, SCALE_x1);       // m_icount = i3

	// --- suspend checkpoint: if(m_icount <= 0) ---
	UML_CMP(block, I3, 0);
	UML_JMPc(block, COND_G, lbl_not_suspended);                    // m_icount > 0 -> no suspend
		// out of budget this bus cycle: read-and-clear the redo flag (cold path)
		UML_CALLC(block, &m68000_device::cfunc_take_access_to_be_redone, this);
		UML_LOAD(block, I4, &m_drc_redo_scratch, 0, SIZE_BYTE, SCALE_x1); // i4 = redo?
		UML_CMP(block, I4, 0);
		UML_JMPc(block, COND_E, lbl_completed);                    // !redo -> read completed
			// redo: refund the charge and replay THIS read on resume
			UML_LOAD(block, I3, &m_icount, 0, SIZE_DWORD, SCALE_x1);
			UML_ADD(block, I3, I3, step.charge);                   // m_icount += N (refund)
			UML_STORE(block, &m_icount, 0, I3, SIZE_DWORD, SCALE_x1);
			UML_MOV(block, I5, step.redo_substate);
			UML_STORE(block, &m_inst_substate, 0, I5, SIZE_WORD, SCALE_x1);
			UML_JMP(block, lbl_delegate);                          // yield -> interpreter resumes at redo substate
		UML_LABEL(block, lbl_completed);
			UML_MOV(block, I5, step.completed_substate);
			UML_STORE(block, &m_inst_substate, 0, I5, SIZE_WORD, SCALE_x1);
			UML_JMP(block, lbl_delegate);                          // yield -> read DID happen, resume after it
	UML_LABEL(block, lbl_not_suspended);

	// --- address-error branch (PROGRAM reads only): if(m_aob & 1) ---
	if(step.has_addr_error)
	{
		UML_TEST(block, I1, 1);                                     // m_aob & 1 ?
		UML_JMPc(block, COND_Z, lbl_no_fault);                     // even -> no fault
			// the interpreter's extra -4 on fault, then transition to S_ADDRESS_ERROR
			UML_LOAD(block, I3, &m_icount, 0, SIZE_DWORD, SCALE_x1);
			UML_SUB(block, I3, I3, 4);
			UML_STORE(block, &m_icount, 0, I3, SIZE_DWORD, SCALE_x1);
			UML_MOV(block, I5, u32(S_ADDRESS_ERROR));
			UML_STORE(block, &m_inst_state, 0, I5, SIZE_WORD, SCALE_x1);
			UML_JMP(block, lbl_delegate);                          // route the group-0 frame to the interpreter
		UML_LABEL(block, lbl_no_fault);
	}

	// clean path: m_edb is committed; the caller continues to the next step
	// (m_irc/m_dbin commit and any ALU are emitted by the opcode emitter).
}


//-------------------------------------------------
//  generate_btst_imm8_absolute - native UML for
//  btst #n,(xxx).W and btst #n,(xxx).L
//
//  Mirrors m68000_device::btst_imm8_adr16_df (.W, 4
//  reads, substates 1..8) and btst_imm8_adr32_df
//  (.L, 5 reads, substates 1..10) in m68000-sdf.cpp.
//  Each read is a generate_bus_step() (the charge +
//  two-way suspend + address-error).  Between reads
//  this emits the opcode-specific architectural setup
//  (m_aob/m_pc/m_au/m_at/m_dcr/m_base_ssw/...) and the
//  m_irc/m_dbin commits, and after the data read the
//  Z = !(data_byte & (1 << (m_dcr & 7))) computation.
//  On a fully-granted instruction it runs every read
//  then retires (set_ftu_const via cfunc, m_inst_state
//  = next) and returns to the caller (which JMPs the
//  delegate); on a suspend/fault generate_bus_step
//  yields to lbl_delegate and the PARTIAL interpreter
//  handler (m68000-sdp.cpp) resumes at the substate.
//  I7 holds m_ird (the opword), preserved across reads.
//
//  Field widths (m68000.h): m_aob/m_au/m_pc/m_at/m_dt
//  are u32 (SIZE_DWORD); m_irc/m_ir/m_ird/m_dbin/m_edb/
//  m_sr/m_base_ssw are u16 (SIZE_WORD); m_dcr is u8
//  (SIZE_BYTE) -- the SIZE_ on every LOAD/STORE matches
//  the target field's width.
//
//  The interpreter's intermediate alu_eor8(m_dt,m_dbin)
//  and alu_and8(m_dbin,0xffff) are intentionally NOT
//  emitted: both take their operands BY VALUE (they do
//  not modify m_dt or m_dbin), they write only m_aluo/
//  m_isr -- which are not in the oracle's compared
//  architectural state and are recomputed by the partial
//  handler on resume -- and their results are overwritten
//  before any sr_* commit.  Only the final
//  alu_and8(m_dbin,1<<(m_dcr&7))+sr_z is architecturally
//  live (it sets SR.Z); that is the compute_z below.
//-------------------------------------------------

void m68000_device::generate_btst_imm8_absolute(drcuml_block &block, uml::code_label lbl_delegate)
{
	// locate an opcode's generated bus-step run (single-sourced from m68000gen.py)
	auto find_run = [](u16 value) -> const drc_bus_run & {
		for(const drc_bus_run &r : s_drc_bus_run_table)
			if(r.value == value)
				return r;
		return s_drc_bus_run_table[0]; // unreachable for the wired opcodes
	};

	// m_base_ssw = SSW_PROGRAM | SSW_R
	auto ssw_program = [&]() {
		UML_MOV(block, I0, u32(u16(SSW_PROGRAM | SSW_R)));
		UML_STORE(block, &m_base_ssw, 0, I0, SIZE_WORD, SCALE_x1);
	};
	// m_irc = m_edb; m_dbin = m_edb;
	auto commit_irc_dbin = [&]() {
		UML_LOAD(block, I0, &m_edb, 0, SIZE_WORD, SCALE_x1);
		UML_STORE(block, &m_irc, 0, I0, SIZE_WORD, SCALE_x1);
		UML_STORE(block, &m_dbin, 0, I0, SIZE_WORD, SCALE_x1);
	};
	// m_dbin = m_edb;
	auto commit_dbin = [&]() {
		UML_LOAD(block, I0, &m_edb, 0, SIZE_WORD, SCALE_x1);
		UML_STORE(block, &m_dbin, 0, I0, SIZE_WORD, SCALE_x1);
	};
	// read-1 setup (.W and .L identical): m_aob=m_au; m_pc=m_au;
	//   set_16l(m_dt,m_dbin); m_au+=2; m_base_ssw=SSW_PROGRAM|SSW_R
	auto setup_read1 = [&]() {
		UML_LOAD(block, I0, &m_au, 0, SIZE_DWORD, SCALE_x1);          // i0 = m_au
		UML_STORE(block, &m_aob, 0, I0, SIZE_DWORD, SCALE_x1);        // m_aob = m_au
		UML_STORE(block, &m_pc, 0, I0, SIZE_DWORD, SCALE_x1);         // m_pc = m_au
		UML_ADD(block, I1, I0, 2);                                    // i1 = m_au + 2
		UML_STORE(block, &m_au, 0, I1, SIZE_DWORD, SCALE_x1);         // m_au += 2
		// set_16l(m_dt,m_dbin): m_dt = (m_dt & 0xffff0000) | (m_dbin & 0xffff)
		UML_LOAD(block, I2, &m_dt, 0, SIZE_DWORD, SCALE_x1);
		UML_AND(block, I2, I2, u32(0xffff0000));
		UML_LOAD(block, I3, &m_dbin, 0, SIZE_WORD, SCALE_x1);         // i3 = m_dbin (zero-extended)
		UML_OR(block, I2, I2, I3);
		UML_STORE(block, &m_dt, 0, I2, SIZE_DWORD, SCALE_x1);         // m_dt = set_16l(m_dt, m_dbin)
		ssw_program();
	};
	// data-read setup (.W and .L identical): m_aob=m_at; m_au=m_pc+2;
	//   m_base_ssw=SSW_DATA|SSW_R
	auto setup_dataread = [&]() {
		UML_LOAD(block, I0, &m_at, 0, SIZE_DWORD, SCALE_x1);
		UML_STORE(block, &m_aob, 0, I0, SIZE_DWORD, SCALE_x1);        // m_aob = m_at
		UML_LOAD(block, I1, &m_pc, 0, SIZE_DWORD, SCALE_x1);
		UML_ADD(block, I1, I1, 2);
		UML_STORE(block, &m_au, 0, I1, SIZE_DWORD, SCALE_x1);         // m_au = m_pc + 2
		UML_MOV(block, I0, u32(u16(SSW_DATA | SSW_R)));
		UML_STORE(block, &m_base_ssw, 0, I0, SIZE_WORD, SCALE_x1);    // m_base_ssw = SSW_DATA | SSW_R
	};
	// Z computation: alu_and8(m_dbin, 1<<(m_dcr&7)); sr_z();
	//   m_sr = (m_sr & ~SR_Z) | ((m_dbin & (1<<(m_dcr&7))) ? 0 : SR_Z)
	auto compute_z = [&]() {
		UML_LOAD(block, I0, &m_dcr, 0, SIZE_BYTE, SCALE_x1);          // i0 = m_dcr
		UML_AND(block, I0, I0, 7);                                    // i0 = m_dcr & 7
		UML_MOV(block, I1, 1);
		UML_SHL(block, I1, I1, I0);                                   // i1 = 1 << (m_dcr & 7)
		UML_LOAD(block, I2, &m_dbin, 0, SIZE_WORD, SCALE_x1);         // i2 = m_dbin (data byte)
		UML_AND(block, I2, I2, I1);                                   // i2 = tested bit
		UML_LOAD(block, I3, &m_sr, 0, SIZE_WORD, SCALE_x1);
		UML_AND(block, I3, I3, u32(u16(~SR_Z)));                      // clear Z
		UML_CMP(block, I2, 0);
		UML_SETc(block, COND_E, I4);                                  // i4 = (tested bit == 0) ? 1 : 0
		UML_SHL(block, I4, I4, 2);                                    // i4 = Z ? SR_Z(0x04) : 0
		UML_OR(block, I3, I3, I4);
		UML_STORE(block, &m_sr, 0, I3, SIZE_WORD, SCALE_x1);          // m_sr = (m_sr & ~SR_Z) | Z
	};
	// final-prefetch (btsm1) setup: m_aob=m_au; m_ir=m_irc; m_pc=m_au; m_au+=2;
	//   m_ird=m_ir; if(m_next_state!=S_TRACE) m_next_state=m_int_next_state; SSW_PROGRAM
	auto setup_final_prefetch = [&]() {
		UML_LOAD(block, I0, &m_au, 0, SIZE_DWORD, SCALE_x1);
		UML_STORE(block, &m_aob, 0, I0, SIZE_DWORD, SCALE_x1);        // m_aob = m_au
		UML_STORE(block, &m_pc, 0, I0, SIZE_DWORD, SCALE_x1);         // m_pc = m_au
		UML_ADD(block, I1, I0, 2);
		UML_STORE(block, &m_au, 0, I1, SIZE_DWORD, SCALE_x1);         // m_au += 2
		UML_LOAD(block, I2, &m_irc, 0, SIZE_WORD, SCALE_x1);          // i2 = m_irc
		UML_STORE(block, &m_ir, 0, I2, SIZE_WORD, SCALE_x1);          // m_ir = m_irc
		UML_STORE(block, &m_ird, 0, I2, SIZE_WORD, SCALE_x1);         // m_ird = m_ir
		UML_LOAD(block, I3, &m_next_state, 0, SIZE_DWORD, SCALE_x1);
		UML_LOAD(block, I4, &m_int_next_state, 0, SIZE_DWORD, SCALE_x1);
		UML_CMP(block, I3, u32(S_TRACE));
		UML_MOVc(block, COND_NE, I3, I4);                            // (next_state != S_TRACE) ? int_next_state : kept
		UML_STORE(block, &m_next_state, 0, I3, SIZE_DWORD, SCALE_x1);
		ssw_program();
	};
	// retire: m_irc=m_edb; m_dbin=m_edb; set_ftu_const();
	//   m_inst_state = m_next_state ? m_next_state : m_decode_table[m_ird];
	//   if(m_sr & SR_T) m_next_state = S_TRACE;
	auto retire = [&]() {
		commit_irc_dbin();
		UML_CALLC(block, &m68000_device::cfunc_set_ftu_const, this);  // set_ftu_const() -- single-sourced, not hand-transcribed
		uml::code_label const lbl_have_next = m_drc_labelnum++;
		UML_LOAD(block, I0, &m_next_state, 0, SIZE_DWORD, SCALE_x1);  // i0 = m_next_state
		UML_CMP(block, I0, 0);
		UML_JMPc(block, COND_NE, lbl_have_next);                      // next_state != 0 -> use it
			UML_LOAD(block, I1, &m_ird, 0, SIZE_WORD, SCALE_x1);      // i1 = m_ird (new opword)
			UML_LOAD(block, I0, m_decode_table.data(), I1, SIZE_WORD, SCALE_x2); // i0 = m_decode_table[m_ird]
		UML_LABEL(block, lbl_have_next);
		UML_STORE(block, &m_inst_state, 0, I0, SIZE_WORD, SCALE_x1);  // m_inst_state = next_state ?: decode_table[m_ird]
		uml::code_label const lbl_no_trace = m_drc_labelnum++;
		UML_LOAD(block, I2, &m_sr, 0, SIZE_WORD, SCALE_x1);
		UML_TEST(block, I2, u32(u16(SR_T)));
		UML_JMPc(block, COND_Z, lbl_no_trace);                       // !(SR & T) -> done
			UML_MOV(block, I3, u32(S_TRACE));
			UML_STORE(block, &m_next_state, 0, I3, SIZE_DWORD, SCALE_x1); // m_next_state = S_TRACE
		UML_LABEL(block, lbl_no_trace);
	};

	uml::code_label const lbl_absl = m_drc_labelnum++;
	uml::code_label const lbl_done = m_drc_labelnum++;

	UML_AND(block, I0, I7, 0x0001);                                  // i0 = opword & 1  (0=.W absw, 1=.L absl)
	UML_CMP(block, I0, 0);
	UML_JMPc(block, COND_NE, lbl_absl);

	// ---- .W arm: btst #n,(xxx).W  (value 0x0838, 4 reads) ----
	{
		const drc_bus_run &run = find_run(0x0838);
		setup_read1();
		generate_bus_step(block, s_drc_bus_step_table[run.first + 0], lbl_delegate);  // read 1 (ext word)
		commit_irc_dbin();
		// read-2 setup (.W): m_aob=m_au; m_pc=m_au; m_dcr=m_dt; m_at=ext32(m_dbin); m_au=ext32(m_dbin); SSW_PROGRAM
		UML_LOAD(block, I0, &m_au, 0, SIZE_DWORD, SCALE_x1);
		UML_STORE(block, &m_aob, 0, I0, SIZE_DWORD, SCALE_x1);
		UML_STORE(block, &m_pc, 0, I0, SIZE_DWORD, SCALE_x1);
		UML_LOAD(block, I1, &m_dt, 0, SIZE_DWORD, SCALE_x1);
		UML_STORE(block, &m_dcr, 0, I1, SIZE_BYTE, SCALE_x1);         // m_dcr = m_dt (u8)
		UML_LOAD(block, I2, &m_dbin, 0, SIZE_WORD, SCALE_x1);
		UML_SEXT(block, I2, I2, SIZE_WORD);                           // i2 = ext32(m_dbin) = s32(s16(m_dbin))
		UML_STORE(block, &m_at, 0, I2, SIZE_DWORD, SCALE_x1);         // m_at = ext32(m_dbin)
		UML_STORE(block, &m_au, 0, I2, SIZE_DWORD, SCALE_x1);         // m_au = ext32(m_dbin)
		ssw_program();
		generate_bus_step(block, s_drc_bus_step_table[run.first + 1], lbl_delegate);  // read 2 (refill)
		commit_irc_dbin();
		setup_dataread();
		generate_bus_step(block, s_drc_bus_step_table[run.first + 2], lbl_delegate);  // data read (byte-lane, no addr-error)
		commit_dbin();
		compute_z();
		setup_final_prefetch();
		generate_bus_step(block, s_drc_bus_step_table[run.first + 3], lbl_delegate);  // read 4 (final prefetch)
		retire();
		UML_JMP(block, lbl_done);
	}

	UML_LABEL(block, lbl_absl);
	// ---- .L arm: btst #n,(xxx).L  (value 0x0839, 5 reads) ----
	{
		const drc_bus_run &run = find_run(0x0839);
		setup_read1();
		generate_bus_step(block, s_drc_bus_step_table[run.first + 0], lbl_delegate);  // read 1 (abs-addr hi)
		commit_irc_dbin();
		// read-2 setup (.L): m_aob=m_au; set_16h(m_at,m_dbin); m_au+=2; SSW_PROGRAM
		UML_LOAD(block, I0, &m_au, 0, SIZE_DWORD, SCALE_x1);
		UML_STORE(block, &m_aob, 0, I0, SIZE_DWORD, SCALE_x1);
		UML_ADD(block, I1, I0, 2);
		UML_STORE(block, &m_au, 0, I1, SIZE_DWORD, SCALE_x1);
		// set_16h(m_at,m_dbin): m_at = (m_at & 0x0000ffff) | (m_dbin << 16)
		UML_LOAD(block, I2, &m_at, 0, SIZE_DWORD, SCALE_x1);
		UML_AND(block, I2, I2, u32(0x0000ffff));
		UML_LOAD(block, I3, &m_dbin, 0, SIZE_WORD, SCALE_x1);
		UML_SHL(block, I3, I3, 16);
		UML_OR(block, I2, I2, I3);
		UML_STORE(block, &m_at, 0, I2, SIZE_DWORD, SCALE_x1);         // m_at = set_16h(m_at, m_dbin)
		ssw_program();
		generate_bus_step(block, s_drc_bus_step_table[run.first + 1], lbl_delegate);  // read 2 (abs-addr lo)
		commit_dbin();                                               // .L read 2 commits m_dbin only
		// read-3 setup (.L): m_aob=m_au; m_pc=m_au; m_dcr=m_dt; set_16l(m_at,m_dbin);
		//   m_au=merge_16_32(high16(m_at),m_dbin) == m_at; SSW_PROGRAM
		UML_LOAD(block, I0, &m_au, 0, SIZE_DWORD, SCALE_x1);
		UML_STORE(block, &m_aob, 0, I0, SIZE_DWORD, SCALE_x1);
		UML_STORE(block, &m_pc, 0, I0, SIZE_DWORD, SCALE_x1);
		UML_LOAD(block, I1, &m_dt, 0, SIZE_DWORD, SCALE_x1);
		UML_STORE(block, &m_dcr, 0, I1, SIZE_BYTE, SCALE_x1);         // m_dcr = m_dt (u8)
		UML_LOAD(block, I2, &m_at, 0, SIZE_DWORD, SCALE_x1);
		UML_AND(block, I2, I2, u32(0xffff0000));                      // set_16l keeps high 16 of m_at
		UML_LOAD(block, I3, &m_dbin, 0, SIZE_WORD, SCALE_x1);
		UML_OR(block, I2, I2, I3);                                    // i2 = (m_at & 0xffff0000) | m_dbin
		UML_STORE(block, &m_at, 0, I2, SIZE_DWORD, SCALE_x1);         // m_at = set_16l(m_at, m_dbin)
		UML_STORE(block, &m_au, 0, I2, SIZE_DWORD, SCALE_x1);         // m_au = merge_16_32(high16(m_at), m_dbin)
		ssw_program();
		generate_bus_step(block, s_drc_bus_step_table[run.first + 2], lbl_delegate);  // read 3 (refill)
		commit_irc_dbin();
		setup_dataread();
		generate_bus_step(block, s_drc_bus_step_table[run.first + 3], lbl_delegate);  // data read (byte-lane, no addr-error)
		commit_dbin();
		compute_z();
		setup_final_prefetch();
		generate_bus_step(block, s_drc_bus_step_table[run.first + 4], lbl_delegate);  // read 5 (final prefetch)
		retire();
		UML_JMP(block, lbl_done);
	}

	UML_LABEL(block, lbl_done);
}


//-------------------------------------------------
//  generate_bitop_mem - native UML for
//  btst/bchg/bclr/bset #n|Dn,(An)/(An)+/-(An)  (O-mem-2, 24 forms)
//
//  Mirrors the *_df handlers in m68000-sdf.cpp (e.g. bchg_imm8_ais_df:20703,
//  bchg_dd_ais_df:6770, btst_imm8_ais_df:19582, bchg_imm8_pais_df:20910).
//  One parameterized emitter; family/EA/source are compile-time constants.
//    RMW shape:  [#imm8] ext-fetch -> EA setup + m_dcr -> data read ->
//                modify+refill prefetch -> data WRITE -> retire
//    btst shape: [#imm8] ext-fetch -> EA setup + m_dcr -> data read ->
//                Z(from data) -> final prefetch -> retire   (read-only)
//  The bit number m_dcr is m_dt (#imm8) or m_da[rx] (Dn, rx=(m_ird>>9)&7).
//  Z is taken from the ORIGINAL byte: at the write step from m_alub for RMW
//  (matching the interpreter's bcsm2 ordering -- memory gets the post-op byte,
//  Z reflects the bit as tested), or from m_dbin at the final prefetch for btst.
//  EA arithmetic uses the A7-byte-by-2 rule; the -(An) predecrement internal -2
//  is folded into the data-read step's pre_charge by the generator (Task 1), so
//  it is NOT charged here.  Substates/charges come from the generated run --
//  never hard-coded.  I7 holds m_ird (preserved across generate_bus_step).
//
//  ry = map_sp((m_ird & 7) | 8) is parked in I6 across the EA setup only; the
//  only post-setup m_da[ry] writes (the (An)+/-(An) writeback) happen BEFORE the
//  first generate_bus_step, so ry is never needed across a clobbering bus step.
//-------------------------------------------------

void m68000_device::generate_bitop_mem(drcuml_block &block, const struct bitop_form &form, uml::code_label lbl_delegate)
{
	// locate this form's generated bus-step run by {value, mask}.  value alone is
	// ambiguous (the Dn forms share low bits), so match BOTH.
	auto find_run = [](u16 value, u16 mask) -> const drc_bus_run & {
		for(const drc_bus_run &r : s_drc_bus_run_table)
			if(r.value == value && r.mask == mask)
				return r;
		return s_drc_bus_run_table[0]; // unreachable for the wired opcodes
	};
	const drc_bus_run &run = find_run(form.value, form.mask);
	u16 si = run.first;   // running index into s_drc_bus_step_table for THIS run

	const bool is_rmw = (form.family != BITOP_BTST);

	// m_base_ssw = SSW_PROGRAM | SSW_R
	auto ssw_program = [&]() {
		UML_MOV(block, I0, u32(u16(SSW_PROGRAM | SSW_R)));
		UML_STORE(block, &m_base_ssw, 0, I0, SIZE_WORD, SCALE_x1);
	};
	// m_irc = m_edb; m_dbin = m_edb;
	auto commit_irc_dbin = [&]() {
		UML_LOAD(block, I0, &m_edb, 0, SIZE_WORD, SCALE_x1);
		UML_STORE(block, &m_irc, 0, I0, SIZE_WORD, SCALE_x1);
		UML_STORE(block, &m_dbin, 0, I0, SIZE_WORD, SCALE_x1);
	};
	// m_dbin = m_edb;
	auto commit_dbin = [&]() {
		UML_LOAD(block, I0, &m_edb, 0, SIZE_WORD, SCALE_x1);
		UML_STORE(block, &m_dbin, 0, I0, SIZE_WORD, SCALE_x1);
	};
	// ry = map_sp((m_ird & 7) | 8) -> I6 : if (m_ird & 7) == 7 use m_sp, else |8.
	auto load_ry = [&]() {
		uml::code_label const lbl_a7 = m_drc_labelnum++;
		uml::code_label const lbl_ry_done = m_drc_labelnum++;
		UML_AND(block, I6, I7, 7);                                    // i6 = m_ird & 7
		UML_CMP(block, I6, 7);
		UML_JMPc(block, COND_E, lbl_a7);
			UML_OR(block, I6, I6, 8);                                 // A0..A6 -> (m_ird & 7) | 8
			UML_JMP(block, lbl_ry_done);
		UML_LABEL(block, lbl_a7);
			UML_LOAD(block, I6, &m_sp, 0, SIZE_DWORD, SCALE_x1);      // A7 -> m_sp (15 or 16)
		UML_LABEL(block, lbl_ry_done);
	};
	// delta = (ry < 15 ? 1 : 2) -> Id : byte access adjusts (A7)+/-(A7) by 2.
	auto load_delta = [&](uml::parameter Id) {
		uml::code_label const lbl_two = m_drc_labelnum++;
		uml::code_label const lbl_d_done = m_drc_labelnum++;
		UML_CMP(block, I6, 15);
		UML_JMPc(block, COND_GE, lbl_two);                           // ry >= 15 (A7 bank) -> 2
			UML_MOV(block, Id, 1);
			UML_JMP(block, lbl_d_done);
		UML_LABEL(block, lbl_two);
			UML_MOV(block, Id, 2);
		UML_LABEL(block, lbl_d_done);
	};
	// final-prefetch (btsm1) setup : m_aob=m_au; m_ir=m_irc; m_pc=m_au; m_au+=2;
	//   m_ird=m_ir; if(next_state!=S_TRACE) next_state=int_next_state; SSW_PROGRAM
	auto setup_final_prefetch = [&]() {
		UML_LOAD(block, I0, &m_au, 0, SIZE_DWORD, SCALE_x1);
		UML_STORE(block, &m_aob, 0, I0, SIZE_DWORD, SCALE_x1);        // m_aob = m_au
		UML_STORE(block, &m_pc, 0, I0, SIZE_DWORD, SCALE_x1);         // m_pc = m_au
		UML_ADD(block, I1, I0, 2);
		UML_STORE(block, &m_au, 0, I1, SIZE_DWORD, SCALE_x1);         // m_au += 2
		UML_LOAD(block, I2, &m_irc, 0, SIZE_WORD, SCALE_x1);
		UML_STORE(block, &m_ir, 0, I2, SIZE_WORD, SCALE_x1);          // m_ir = m_irc
		UML_STORE(block, &m_ird, 0, I2, SIZE_WORD, SCALE_x1);         // m_ird = m_ir
		UML_LOAD(block, I3, &m_next_state, 0, SIZE_DWORD, SCALE_x1);
		UML_LOAD(block, I4, &m_int_next_state, 0, SIZE_DWORD, SCALE_x1);
		UML_CMP(block, I3, u32(S_TRACE));
		UML_MOVc(block, COND_NE, I3, I4);
		UML_STORE(block, &m_next_state, 0, I3, SIZE_DWORD, SCALE_x1);
		ssw_program();
	};
	// retire : set_ftu_const(); m_inst_state = next_state ?: decode_table[m_ird];
	//   if(m_sr & SR_T) m_next_state = S_TRACE.  (No m_irc/m_dbin commit -- btst
	//   commits the final-prefetch word separately; RMW already committed at refill.)
	auto retire = [&]() {
		UML_CALLC(block, &m68000_device::cfunc_set_ftu_const, this);  // single-sourced
		uml::code_label const lbl_have_next = m_drc_labelnum++;
		UML_LOAD(block, I0, &m_next_state, 0, SIZE_DWORD, SCALE_x1);
		UML_CMP(block, I0, 0);
		UML_JMPc(block, COND_NE, lbl_have_next);
			UML_LOAD(block, I1, &m_ird, 0, SIZE_WORD, SCALE_x1);
			UML_LOAD(block, I0, m_decode_table.data(), I1, SIZE_WORD, SCALE_x2);
		UML_LABEL(block, lbl_have_next);
		UML_STORE(block, &m_inst_state, 0, I0, SIZE_WORD, SCALE_x1);
		uml::code_label const lbl_no_trace = m_drc_labelnum++;
		UML_LOAD(block, I2, &m_sr, 0, SIZE_WORD, SCALE_x1);
		UML_TEST(block, I2, u32(u16(SR_T)));
		UML_JMPc(block, COND_Z, lbl_no_trace);
			UML_MOV(block, I3, u32(S_TRACE));
			UML_STORE(block, &m_next_state, 0, I3, SIZE_DWORD, SCALE_x1);
		UML_LABEL(block, lbl_no_trace);
	};
	// Z = !(byte & (1 << (m_dcr & 7))); set into SR.Z.  byte = m_dbin (btst) or
	// m_alub (RMW original).  N/V/C/X untouched.
	auto compute_z = [&](u16 *src_byte) {
		UML_LOAD(block, I0, &m_dcr, 0, SIZE_BYTE, SCALE_x1);
		UML_AND(block, I0, I0, 7);
		UML_MOV(block, I1, 1);
		UML_SHL(block, I1, I1, I0);                                   // i1 = 1 << (m_dcr & 7)
		UML_LOAD(block, I2, src_byte, 0, SIZE_WORD, SCALE_x1);        // i2 = tested byte
		UML_AND(block, I2, I2, I1);                                   // tested bit
		UML_LOAD(block, I3, &m_sr, 0, SIZE_WORD, SCALE_x1);
		UML_AND(block, I3, I3, u32(u16(~SR_Z)));                      // clear Z
		UML_CMP(block, I2, 0);
		UML_SETc(block, COND_E, I4);                                  // Z = (bit == 0)
		UML_SHL(block, I4, I4, 2);                                    // SR_Z = 0x04
		UML_OR(block, I3, I3, I4);
		UML_STORE(block, &m_sr, 0, I3, SIZE_WORD, SCALE_x1);          // m_sr = (m_sr & ~SR_Z) | Z
	};

	// ===== ext-fetch (o#w1), #imm8 ONLY =====
	// m_aob=m_au; m_pc=m_au; set_16l(m_dt,m_dbin); m_au+=2; SSW_PROGRAM; read; commit.
	if(form.src == BITSRC_IMM8)
	{
		UML_LOAD(block, I0, &m_au, 0, SIZE_DWORD, SCALE_x1);
		UML_STORE(block, &m_aob, 0, I0, SIZE_DWORD, SCALE_x1);
		UML_STORE(block, &m_pc, 0, I0, SIZE_DWORD, SCALE_x1);
		UML_ADD(block, I1, I0, 2);
		UML_STORE(block, &m_au, 0, I1, SIZE_DWORD, SCALE_x1);
		// set_16l(m_dt, m_dbin): m_dt = (m_dt & 0xffff0000) | (m_dbin & 0xffff)
		UML_LOAD(block, I2, &m_dt, 0, SIZE_DWORD, SCALE_x1);
		UML_AND(block, I2, I2, u32(0xffff0000));
		UML_LOAD(block, I3, &m_dbin, 0, SIZE_WORD, SCALE_x1);
		UML_OR(block, I2, I2, I3);
		UML_STORE(block, &m_dt, 0, I2, SIZE_DWORD, SCALE_x1);
		ssw_program();
		generate_bus_step(block, s_drc_bus_step_table[si++], lbl_delegate);   // ext read
		commit_irc_dbin();
	}

	// ===== EA setup (m_aob/m_at, reg writeback) + bit number m_dcr =====
	load_ry();
	switch(form.ea)
	{
	case BITEA_AIS:   // (An): m_aob = m_at = m_da[ry]
		UML_LOAD(block, I0, &m_da[0], I6, SIZE_DWORD, SCALE_x4);
		UML_STORE(block, &m_aob, 0, I0, SIZE_DWORD, SCALE_x1);
		UML_STORE(block, &m_at, 0, I0, SIZE_DWORD, SCALE_x1);
		break;
	case BITEA_AIPS:  // (An)+: m_aob = m_at = m_da[ry] (old); m_da[ry] = old + delta
		UML_LOAD(block, I0, &m_da[0], I6, SIZE_DWORD, SCALE_x4);
		UML_STORE(block, &m_aob, 0, I0, SIZE_DWORD, SCALE_x1);
		UML_STORE(block, &m_at, 0, I0, SIZE_DWORD, SCALE_x1);
		load_delta(I1);
		UML_ADD(block, I2, I0, I1);
		UML_STORE(block, &m_da[0], I6, I2, SIZE_DWORD, SCALE_x4);     // post-increment
		break;
	case BITEA_PAIS:  // -(An): m_aob = m_at = m_da[ry] = m_da[ry] - delta
		UML_LOAD(block, I0, &m_da[0], I6, SIZE_DWORD, SCALE_x4);
		load_delta(I1);
		UML_SUB(block, I2, I0, I1);
		UML_STORE(block, &m_aob, 0, I2, SIZE_DWORD, SCALE_x1);
		UML_STORE(block, &m_at, 0, I2, SIZE_DWORD, SCALE_x1);
		UML_STORE(block, &m_da[0], I6, I2, SIZE_DWORD, SCALE_x4);     // pre-decrement
		break;
	}
	// bit number -> m_dcr: #imm8 = m_dt; Dn = m_da[rx], rx = (m_ird >> 9) & 7
	if(form.src == BITSRC_IMM8)
	{
		UML_LOAD(block, I0, &m_dt, 0, SIZE_DWORD, SCALE_x1);
		UML_STORE(block, &m_dcr, 0, I0, SIZE_BYTE, SCALE_x1);
	}
	else
	{
		UML_SHR(block, I0, I7, 9);
		UML_AND(block, I0, I0, 7);                                    // i0 = rx
		UML_LOAD(block, I1, &m_da[0], I0, SIZE_DWORD, SCALE_x4);      // i1 = m_da[rx]
		UML_STORE(block, &m_dcr, 0, I1, SIZE_BYTE, SCALE_x1);
	}
	// data-read SSW: m_base_ssw = SSW_DATA | SSW_R
	UML_MOV(block, I0, u32(u16(SSW_DATA | SSW_R)));
	UML_STORE(block, &m_base_ssw, 0, I0, SIZE_WORD, SCALE_x1);

	// ===== data read (byte-lane; descriptor carries the -(An) predecrement pre_charge) =====
	generate_bus_step(block, s_drc_bus_step_table[si++], lbl_delegate);
	commit_dbin();   // m_dbin = ORIGINAL byte

	if(!is_rmw)
	{
		// ===== btst: Z from the data byte, final prefetch, retire (no write) =====
		compute_z(&m_dbin);
		setup_final_prefetch();
		generate_bus_step(block, s_drc_bus_step_table[si++], lbl_delegate);   // final prefetch
		commit_irc_dbin();                                                    // commit the prefetch word
		retire();
		return;
	}

	// ===== RMW: modify + refill prefetch (bcsm1) =====
	// m_alub = original byte (Z source); compute the modified byte; set_8xl(m_dbout).
	UML_LOAD(block, I0, &m_dbin, 0, SIZE_WORD, SCALE_x1);             // i0 = original byte
	UML_STORE(block, &m_alub, 0, I0, SIZE_WORD, SCALE_x1);            // m_alub = original
	UML_LOAD(block, I1, &m_dcr, 0, SIZE_BYTE, SCALE_x1);
	UML_AND(block, I1, I1, 7);
	UML_MOV(block, I2, 1);
	UML_SHL(block, I2, I2, I1);                                       // i2 = 1 << (m_dcr & 7)
	switch(form.family)
	{
	case BITOP_BCHG: UML_XOR(block, I0, I0, I2); break;                                       // original ^ bit
	case BITOP_BCLR: UML_XOR(block, I2, I2, u32(0xff)); UML_AND(block, I0, I0, I2); break;    // original & ~bit
	case BITOP_BSET: UML_OR (block, I0, I0, I2); break;                                       // original | bit
	default: break; // unreachable (btst handled above)
	}
	UML_AND(block, I0, I0, 0xff);                                     // modified byte (8 bits)
	// set_8xl(m_dbout, modified) = (modified & 0x00ff) | (modified << 8)
	UML_SHL(block, I1, I0, 8);
	UML_OR(block, I0, I0, I1);
	UML_STORE(block, &m_dbout, 0, I0, SIZE_WORD, SCALE_x1);           // m_dbout = replicated modified byte
	// bcsm1 prefetch setup: m_aob=m_au; m_ir=m_irc; m_pc=m_au; m_au+=2; SSW_PROGRAM
	UML_LOAD(block, I3, &m_au, 0, SIZE_DWORD, SCALE_x1);
	UML_STORE(block, &m_aob, 0, I3, SIZE_DWORD, SCALE_x1);
	UML_STORE(block, &m_pc, 0, I3, SIZE_DWORD, SCALE_x1);
	UML_LOAD(block, I4, &m_irc, 0, SIZE_WORD, SCALE_x1);
	UML_STORE(block, &m_ir, 0, I4, SIZE_WORD, SCALE_x1);
	UML_ADD(block, I3, I3, 2);
	UML_STORE(block, &m_au, 0, I3, SIZE_DWORD, SCALE_x1);
	ssw_program();
	generate_bus_step(block, s_drc_bus_step_table[si++], lbl_delegate);   // refill prefetch
	commit_irc_dbin();   // m_dbin <- prefetch word (original safe in m_alub/m_dbout)

	// ===== data write (bcsm2) =====
	// m_aob=m_at; m_ird=m_ir; next_state; (m_dbout already set); Z from m_alub; SSW_DATA; WRITE.
	UML_LOAD(block, I0, &m_at, 0, SIZE_DWORD, SCALE_x1);
	UML_STORE(block, &m_aob, 0, I0, SIZE_DWORD, SCALE_x1);            // m_aob = m_at (read-validated EA)
	UML_LOAD(block, I1, &m_ir, 0, SIZE_WORD, SCALE_x1);
	UML_STORE(block, &m_ird, 0, I1, SIZE_WORD, SCALE_x1);            // m_ird = m_ir
	UML_LOAD(block, I2, &m_next_state, 0, SIZE_DWORD, SCALE_x1);
	UML_LOAD(block, I3, &m_int_next_state, 0, SIZE_DWORD, SCALE_x1);
	UML_CMP(block, I2, u32(S_TRACE));
	UML_MOVc(block, COND_NE, I2, I3);
	UML_STORE(block, &m_next_state, 0, I2, SIZE_DWORD, SCALE_x1);
	compute_z(&m_alub);                                              // Z from the ORIGINAL byte
	UML_MOV(block, I0, u32(u16(SSW_DATA)));                          // SSW_DATA only (write: R clear)
	UML_STORE(block, &m_base_ssw, 0, I0, SIZE_WORD, SCALE_x1);
	generate_bus_step(block, s_drc_bus_step_table[si++], lbl_delegate);   // DATA WRITE

	// ===== retire =====
	retire();
}
