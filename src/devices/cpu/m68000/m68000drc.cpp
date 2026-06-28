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
		drcuml_block &block(m_drcuml->begin_block(64));
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
	return (opword & 0xf100) == 0x7000;
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
//  read step: UML_READ + the interpreter's exact
//  per-bus-cycle checkpoint (charge, two-way
//  suspend, address-error).  The CALLER has already
//  emitted the step's address / m_base_ssw setup.
//  On suspend or fault this stores the descriptor's
//  substate (or S_ADDRESS_ERROR) and JMPs to
//  lbl_delegate (yield -> interpreter resumes, OQ-1).
//  On the clean path it falls through with the read
//  byte/word committed.  Clobbers I0-I6; preserves I7.
//-------------------------------------------------

void m68000_device::generate_bus_step(drcuml_block &block, const struct drc_bus_step &step, uml::code_label lbl_delegate)
{
	uml::code_label const lbl_not_suspended = m_drc_labelnum++;
	uml::code_label const lbl_completed     = m_drc_labelnum++;
	uml::code_label const lbl_no_fault      = m_drc_labelnum++;

	// address = m_aob & ~1  (the interpreter reads the word at the even address)
	UML_LOAD(block, I1, &m_aob, 0, SIZE_DWORD, SCALE_x1);            // i1 = m_aob
	UML_AND(block, I2, I1, ~u32(1));                                 // i2 = m_aob & ~1

	// THE READ.  Both prefetch and data reads go through SPACE_PROGRAM (the
	// 68000 has one program space; the SSW_PROGRAM/SSW_DATA difference is an
	// architectural field the caller wrote, not a UML space selection).  Read
	// the word; the data read then selects a byte lane.
	UML_READ(block, I0, I2, SIZE_WORD, SPACE_PROGRAM);              // i0 = read word at (m_aob & ~1)

	if(step.byte_lane)
	{
		// m_edb = read; if(!(m_aob & 1)) m_edb >>= 8; then keep the low byte.
		// (m_aob&1 ? low byte : high byte) -- matches the interpreter's lane mask
		// 0x00ff/0xff00 + the ">>8 when even" select.
		uml::code_label const lbl_odd = m_drc_labelnum++;
		UML_TEST(block, I1, 1);                                     // m_aob & 1 ?
		UML_JMPc(block, COND_NZ, lbl_odd);                          // odd -> low byte already in place
			UML_SHR(block, I0, I0, 8);                              // even -> high byte to low
		UML_LABEL(block, lbl_odd);
		UML_AND(block, I0, I0, 0xff);                               // keep the selected byte
	}

	// commit the read into m_edb (the interpreter stores read result in m_edb).
	// m_edb is u16 -> SIZE_WORD (a DWORD store would clobber the adjacent m_irc).
	UML_STORE(block, &m_edb, 0, I0, SIZE_WORD, SCALE_x1);           // m_edb = read

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
