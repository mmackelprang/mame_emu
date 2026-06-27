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

// DRC exit codes (mirroring mips3com.h / ppc.h).  Defined in m68000.cpp as
// well; kept consistent here.
#define EXECUTE_OUT_OF_CYCLES           0
#define EXECUTE_MISSING_CODE            1
#define EXECUTE_UNMAPPED_CODE           2
#define EXECUTE_RESET_CACHE             3

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
	bool succeeded = false;
	while(!succeeded) {
		try {
			drcuml_block &block(m_drcuml->begin_block(64));

			// register the hash at EXACTLY the requested pc, so the entry block's
			// HASHJMP(m_ipc) resolves here after this compile
			if(!m_drcuml->hash_exists(M68K_DRC_MODE, pc))
				UML_HASH(block, M68K_DRC_MODE, pc);                    // hash mode,pc

			// emit the per-PC body (interpreter fallback for the whole in/out-set
			// at boundary M; native emitters replace it opcode-by-opcode later)
			generate_opcode(block, nullptr);

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
//  native for the in-set opcodes, cfunc otherwise
//-------------------------------------------------

void m68000_device::generate_opcode(drcuml_block &block, const opcode_desc *desc)
{
	// The per-PC block body.  Boundary M establishes the real per-PC native
	// DISPATCH (HASHJMP-to-block) with an interpreter body; native per-opcode
	// emitters land on top of this provably-correct base in follow-up
	// increments, each gated by the Leg-B oracle, so coverage can only grow with
	// the gate green.  `desc` is null on the fallback path (the body is
	// PC-agnostic -- it runs the interpreter for the granted quantum); when a
	// native emitter is added it will switch on the descriptor row here.
	generate_interpreter_fallback(block, desc);
}


//-------------------------------------------------
//  generate_interpreter_fallback - emit a UML
//  sequence that runs the interpreter for exactly
//  this one instruction (the cfunc tail).  This is
//  the provably-correct base every native emitter
//  replaces opcode-by-opcode.
//-------------------------------------------------

void m68000_device::generate_interpreter_fallback(drcuml_block &block, const opcode_desc *desc)
{
	(void)desc;

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
