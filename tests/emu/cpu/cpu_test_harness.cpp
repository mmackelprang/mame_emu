// license:BSD-3-Clause
// copyright-holders:Mark Mackelprang
/***************************************************************************

    cpu_test_harness.cpp

    Headless single-CPU machine bootstrap for the differential CPU
    execution oracle (see cpu_test_harness.h).

    Bootstrap mirrors the lifecycle in src/frontend/mame/mame.cpp: an
    emu_options, a no-op osd_interface, a trivial machine_manager, a
    machine_config built from an inline game_driver, and a running_machine
    driven through its only public entry point, run().  Because
    running_machine::start() is private, all oracle work is injected by the
    inline driver's machine_reset() override (which runs after every device
    has been started and reset) and the machine is then asked to exit so
    run() returns.

    Single-stepping
    ---------------
    The modern z80 core's execute_run() is a micro-cycle state machine: it
    decrements its instruction counter once per T-state group and only
    returns to the scheduler when the counter reaches zero.  Between
    instructions the core's m_ref marker is 0xffff00 (the "next opcode fetch
    pending" boundary); mid-instruction it holds a non-zero low byte naming
    the sub-step to resume.  Writing the PC via the state interface also
    resets m_ref to 0xffff00, so after applying a fixture the CPU is
    guaranteed to sit on an instruction boundary.

    To run exactly one instruction we drive the core one T-state group at a
    time: grant a one-cycle budget, call run() (which returns after the next
    decrement takes the counter non-positive), accumulate the cycles that
    decrement consumed, and repeat until m_ref returns to the 0xffff00
    instruction boundary.  This measures the instruction's true T-state count
    independently of the fixture (it never trusts len(cycles)), and it can
    never run past the end of the instruction.

    The mechanism needs to (a) preset the device instruction counter, (b)
    read it back, and (c) observe the m_ref boundary marker.
    device_execute_interface::run() is public, but the counter pointer
    (m_icountptr) and the z80 m_ref marker are protected, so we wrap the
    z80_device in a tiny subclass that exposes a single step_instruction()
    helper.  This avoids touching scheduler attotime/quantum math entirely.

***************************************************************************/

#include "emu.h"

#include "cpu_test_harness.h"

#include "cpu/z80/z80.h"
#include "cpu/m6502/m6502.h"
#include "cpu/m68000/m68000.h"

#include "emuopts.h"
#include "ioport.h"
#include "main.h"
#include "render.h"

#include "drivenum.h"

#include "osdepend.h"

#include "ui/menuitem.h"
#include "ui/uimain.h"

#include <functional>


//**************************************************************************
//  FILE-LOCAL MACHINERY (device, driver, OSD, manager)
//**************************************************************************
//
//  The CPU device subclass and its device type, and the GAME registration,
//  must live in the global namespace (the device-type macros declare the
//  class in the global namespace and supply their own anonymous-namespace
//  traits).  These classes are file-local by virtue of being defined in a
//  single translation unit.  Only the public harness class and the core
//  descriptor live in namespace cpuoracle, further down.

// m_ref value the z80 core parks at between instructions.
constexpr u32 Z80_INSTRUCTION_BOUNDARY = 0xffff00;


// z80_device subclass that exposes a single-instruction step primitive.
// m_icountptr is a protected member of device_execute_interface, run() is a
// public method of the same interface, and m_ref is a protected member of
// z80_device, so all three are reachable from a z80_device subclass without
// friending or touching the scheduler.  It also implements oracle_stepper so
// the harness can drive it without knowing the concrete type.
class oracle_z80_device : public z80_device, public cpuoracle::oracle_stepper
{
public:
	oracle_z80_device(const machine_config &mconfig, const char *tag, device_t *owner, u32 clock);

	// Run exactly one architectural instruction and return the number of
	// T-states it consumed.  `budget` is the fixture's expected T-state count;
	// the step still verifies that count (a wrong budget cannot land cleanly
	// on the next instruction boundary, which the caller checks via the
	// returned count plus the strict final state comparison).  The CPU must be
	// parked on an instruction boundary on entry (true after a reset or after
	// the PC is written through the state interface).
	//
	// How the core works (see the generated z80.hxx): execute_run() is a
	// sub-cycle state machine.  At the start of every M1 opcode fetch it does
	//     if (m_icount <= 0) { m_ref = 0xffff00; return; }   // boundary
	//     if (m_service_attention) { ... if (m_halt) { ...; PC--; } ... }
	// i.e. the 0xffff00 instruction boundary is taken *before* any pending
	// service work (such as the HALT re-execution that decrements PC).  Within
	// an instruction each step charges its cost and returns once the counter
	// goes non-positive; the trailing bookkeeping (PC++/R++/HALT body ...)
	// runs in later zero-cost steps.
	//
	// Technique: grant exactly the instruction's cost and run() once -- the
	// counter reaches zero on the final charged step and the core returns from
	// inside the instruction.  Then re-enter with the counter held at zero to
	// flush the remaining zero-cost steps: the instruction's tail executes and
	// the very next M1 fetch sees m_icount <= 0 and parks on the boundary, both
	// without charging any further cycle and -- crucially -- before any HALT
	// PC-- service step.  Granting the cost exactly is what makes the boundary
	// land at the end of this instruction rather than running into the next.
	int step_instruction(int budget)
	{
		// Charge the granted budget across the instruction's steps.
		*m_icountptr = budget;
		run();
		int consumed = budget - *m_icountptr;

		// Flush the trailing steps and re-park on the boundary.  We hold the
		// counter at exactly zero between flush calls: zero-cost tail steps
		// then run for free, but if the instruction actually costs *more* than
		// the granted budget a remaining cycle-charging step drives the counter
		// negative again -- which we fold into `consumed`, so an under-granted
		// (too-low) fixture cycle count is still caught as consumed > budget.
		// Holding the counter non-positive also guarantees the next M1 fetch
		// takes the 0xffff00 boundary before any HALT PC-- service step.
		while (m_ref != Z80_INSTRUCTION_BOUNDARY)
		{
			if (*m_icountptr > 0)
				*m_icountptr = 0;
			const int before = *m_icountptr;
			run();
			consumed += before - *m_icountptr;
		}

		return consumed;
	}

	// Clear the cross-instruction quirk state that is NOT carried in a
	// SingleStepTests fixture, so each case starts clean: the HALT latch (else
	// a prior case's HALT keeps this case halted and its PC frozen) and any
	// pending NMI service request.  Writing the PC through the state interface
	// already clears the AFTER_EI / AFTER_LDAIR attentions and re-parks m_ref.
	void clear_quirk_state()
	{
		leave_halt();                                    // clears m_halt + SA_HALT
		set_service_attention<SA_NMI_PENDING, 0>();
	}

	// Seed the z80 SCF/CCF "Q" quirk state from a SingleStepTests `q` field.
	//
	// The SCF/CCF undocumented YX (bits 5/3) result is, per Patrik Rak's
	// analysis, ((F ^ q) | A), where q is the F value left by the previous
	// flag-affecting instruction.  MAME computes it as ((F & m_f.q) | A), and
	// for the SingleStepTests corpus the two are identical when m_f.q == ~q
	// (verified bit-exact over the SCF/CCF vectors).  The opcode fetch copies
	// m_f.qtemp into m_f.q (Q = QT) before the instruction body runs, so we
	// seed qtemp, not q, for the value to survive into the SCF/CCF step.
	//
	// m_f is a protected z80_device member, reachable from this subclass.
	void set_q(u8 q) { m_f.qtemp = u8(~q); }

	// oracle_stepper
	virtual int oracle_step(int budget) override { return step_instruction(budget); }
	virtual void oracle_prepare_case() override { clear_quirk_state(); }
	virtual void oracle_set_quirk_q(uint8_t q) override { set_q(q); }
};

DECLARE_DEVICE_TYPE(ORACLE_Z80, oracle_z80_device)
DEFINE_DEVICE_TYPE(ORACLE_Z80, oracle_z80_device, "oracle_z80", "CPU Oracle Z80")

oracle_z80_device::oracle_z80_device(const machine_config &mconfig, const char *tag, device_t *owner, u32 clock) :
	z80_device(mconfig, ORACLE_Z80, tag, owner, clock)
{
}


//**************************************************************************
//  M6502 ORACLE DEVICE
//**************************************************************************
//
//  The m6502 core, like the z80, is a sub-cycle state machine: execute_run()
//  loops while m_icount > 0, and each architectural instruction is charged
//  cycle-by-cycle through the generated do_exec_full()/do_exec_partial()
//  bodies.  Two facts make single-stepping it identical in spirit to z80:
//
//   * Writing PC through the state interface (state_import, M6502_PC) re-parks
//     the core on an instruction boundary: it prefetches the opcode
//     (m_IR = read_sync(m_PC)) and decodes m_inst_state, with m_inst_substate
//     left at 0.
//   * The 6502 prefetches the *next* opcode as the final memory cycle of the
//     current instruction (the generated `prefetch()` expands to a
//     read_sync(m_PC) that IS charged a cycle), exactly as the SingleStepTests
//     corpus counts it.  So len(cycles) == the icount the core consumes.
//
//  The instruction boundary marker is m_inst_substate == 0: a fully retired
//  instruction leaves substate 0 (the generated _partial() sets it to 0 on
//  reaching `break`, and the trailing prefetch tail runs as a zero-cost step).
//  Mid-instruction it is non-zero (the substate to resume from).

class oracle_m6502_device : public m6502_device, public cpuoracle::oracle_stepper
{
public:
	oracle_m6502_device(const machine_config &mconfig, const char *tag, device_t *owner, u32 clock);

	// Run exactly one architectural instruction and return the cycles its
	// body consumed (the icount the core charges), measured independently of
	// the fixture's count.  `budget` is accepted for signature symmetry with
	// the z80 leg but not trusted.
	//
	// Unlike the z80 core, the m6502 fetches the opcode *eagerly* in
	// state_import (writing PC prefetches m_IR and decodes m_inst_state), so
	// when execute_run() is entered the body charges only the post-opcode bus
	// cycles.  The instruction is bounded by m_inst_substate: it is 0 on an
	// instruction boundary and non-zero mid-instruction.  We therefore drive
	// the core one cycle at a time and stop the instant it returns to a
	// boundary (substate 0) having charged at least one cycle -- i.e. just as
	// the body's final bus cycle (the prefetch of the *next* opcode) completes,
	// before execute_run() would start decoding that next instruction.  This
	// can never run past the end of the instruction, and the caller applies the
	// per-core cycle adapter (see cpuoracle.cpp) to reconcile this body count
	// with the corpus's full bus-cycle count.
	int step_instruction(int budget)
	{
		(void)budget;
		int consumed = 0;

		// Phase 1 -- move the core off the instruction boundary.  Granting one
		// cycle lets execute_run() decode the (already-prefetched) opcode and run
		// the body's first charged bus cycle: a MEMORY step does m_icount-- and,
		// on reaching <= 0, stows the resume substate and returns.  In practice
		// this loop runs exactly once for every real opcode -- the first charged
		// cycle takes m_inst_substate from 0 to non-zero, so the `== 0` guard
		// fails immediately afterwards.  The bounded loop is a safety net for a
		// hypothetical core whose first run() does not advance the substate (it
		// must never spin).
		int guard = 0;
		while (m_inst_substate == 0 && guard < 64)
		{
			*m_icountptr = 1;
			run();
			consumed += 1 - *m_icountptr;
			++guard;
		}

		// Phase 2 -- flush the zero-cost tail (prefetch_end's PC++ and the
		// switch fall-through that resets m_inst_substate to 0) while holding
		// the counter non-positive, so execute_run()'s `while (m_icount > 0)`
		// loop can never start decoding the *next* instruction.  An honestly
		// cycle-charging step here (would only happen if phase 1 mis-stopped)
		// drives the counter negative and is folded into `consumed`.
		while (m_inst_substate != 0 && guard < 128)
		{
			if (*m_icountptr > 0)
				*m_icountptr = 0;
			const int before = *m_icountptr;
			run();
			consumed += before - *m_icountptr;
			++guard;
		}

		return consumed;
	}

	// Clear the cross-instruction state a SingleStepTests fixture does not
	// carry: pending/asserted interrupt and set-overflow lines.  The corpus
	// runs each case with no external lines active, so a prior case's pending
	// NMI (or a latched IRQ/SO edge) must not leak into this one.
	//
	// Crucially we also force m_inst_substate back to 0 (an instruction
	// boundary).  Writing PC through the state interface re-decodes
	// m_inst_state but does NOT reset m_inst_substate, so a previous case that
	// left the core mid-instruction -- in particular a JAM/KIL opcode (0x02,
	// 0x12, ...), whose body is an infinite read loop that never retires and
	// which our stepper abandons via its guard cap with substate != 0 -- would
	// otherwise make the next case resume from that stale substate and execute
	// garbage.  Zeroing it here guarantees every case starts on a clean
	// instruction boundary.
	void clear_quirk_state()
	{
		m_nmi_pending = false;
		m_nmi_state = false;
		m_irq_state = false;
		m_apu_irq_state = false;
		m_v_state = false;
		m_irq_taken = false;
		m_inhibit_interrupts = false;
		m_inst_substate = 0;
		// also clear the SYNC (opcode-fetch in progress) latch: a case abandoned
		// mid-instruction (e.g. via the stepper guard cap) could leave it set,
		// and the next case's prefetch_start() would then re-assert SYNC without
		// an intervening clear.  Harmless today (no SYNC callback is wired) but
		// matches device_reset()'s cleanup for defensive completeness.
		m_sync = false;
	}

	// oracle_stepper
	virtual int oracle_step(int budget) override { return step_instruction(budget); }
	virtual void oracle_prepare_case() override { clear_quirk_state(); }
};

DECLARE_DEVICE_TYPE(ORACLE_M6502, oracle_m6502_device)
DEFINE_DEVICE_TYPE(ORACLE_M6502, oracle_m6502_device, "oracle_m6502", "CPU Oracle M6502")

oracle_m6502_device::oracle_m6502_device(const machine_config &mconfig, const char *tag, device_t *owner, u32 clock) :
	m6502_device(mconfig, ORACLE_M6502, tag, owner, clock)
{
}


//**************************************************************************
//  M68000 ORACLE DEVICE  (the new microcode core, NOT Musashi)
//**************************************************************************
//
//  The modern m68000 core (src/devices/cpu/m68000/m68000.cpp) is a microcode
//  state machine.  execute_run() loops while m_icount > 0, advancing the
//  microcode one bus phase at a time; m_inst_state holds the current microcode
//  state and m_inst_substate the sub-phase within it.  An *architectural*
//  instruction boundary is the point where the core is about to fetch the next
//  opcode: m_inst_state >= S_first_instruction with m_inst_substate == 0 (this
//  is exactly where the core's own debugger_instruction_hook fires, line ~165
//  of m68000.cpp).
//
//  Writing PC through the state interface re-parks the core on such a boundary:
//  state_import(STATE_GENPC) recomputes m_ipc/m_pc, re-reads the prefetch queue
//  (m_ir/m_irc) from the program space, and sets m_inst_state =
//  m_decode_table[m_ird], m_inst_substate = 0.  So, like the m6502, the opcode
//  is fetched eagerly when PC is written and RAM must be applied first.
//
//  --- Cycle adapter (the artifact Phase 2 / ADR 0002 consumes) ---
//  The microcode core does not decrement m_icount once per architectural cycle
//  uniformly; instead each handler charges a bus phase's cost, and execute_run()
//  carries any over-run across calls in m_count_before_instruction_step (it
//  subtracts that from m_icount at entry).  To measure one instruction's true
//  cycle cost independently of the corpus we therefore: reset
//  m_count_before_instruction_step to 0, then grant ONE cycle at a time and sum
//  the icount actually consumed (granted - remaining, where remaining can go
//  negative -- a handler that costs more than the single granted cycle drives
//  m_icount below zero, and that overshoot is folded back in).  We stop the
//  instant the core returns to a fresh architectural boundary having run at
//  least one phase of our instruction.  The resulting sum is the SingleStepTests
//  `length` (corpus bus-cycle count) -- this identity IS the documented adapter;
//  no fudge factor is applied.

class oracle_m68000_device : public m68000_device, public cpuoracle::oracle_stepper
{
public:
	oracle_m68000_device(const machine_config &mconfig, const char *tag, device_t *owner, u32 clock);

	// Run exactly one architectural instruction and return the cycles it
	// consumed (the corpus `length`).  The CPU must be parked on an instruction
	// boundary on entry (true after writing PC through the state interface).
	//
	// Boundary detection: the microcode core latches the instruction's address
	// in m_ipc at each instruction start (m68000.cpp line ~162,
	// `m_ipc = m_pc - 2`, run when m_inst_state >= S_first_instruction).  All of
	// a single instruction's microcode states are >= S_first_instruction, so
	// that condition is NOT a per-instruction marker -- but m_ipc only changes
	// when the *next* instruction is decoded.  We therefore record the entry
	// m_ipc and stop the cycle after it changes: that is exactly the retirement
	// of our instruction (its final prefetch loaded the next opcode and advanced
	// m_ipc).  Granting one cycle at a time and summing the consumed icount then
	// yields the instruction's true bus-cycle cost == the corpus `length`.
	int step_instruction(int budget)
	{
		(void)budget;

		// The over-run carry must start clean so the first grant is not
		// silently swallowed by a stale m_count_before_instruction_step from a
		// previous case (we also clear it in clear_quirk_state(), belt and
		// braces).
		m_count_before_instruction_step = 0;

		// Settle the supervisor/user banking from the just-applied SR.  The state
		// interface's M68K_SR / GENPC imports do NOT call update_user_super(), so
		// without this m_sp (the index of the active A7) and the program space keep
		// a stale value from a prior case -- and a user-mode push would land on the
		// supervisor stack (m_da[16]) instead of the USP (m_da[15]), or vice versa.
		// update_user_super() reads m_sr & SR_S and sets m_sp = (S ? 16 : 15) plus
		// the matching program/opcodes space.  This is a harness-side re-sync of
		// derived state to the applied SR -- no change to interpreter behaviour.
		update_user_super();

		const u32 entry_ipc = m_ipc;
		int consumed = 0;

		// Capture the full architectural state (the register file, SR and the m_au
		// prefetch pointer) BEFORE each grant.  When a grant finally advances m_ipc
		// -- the core has dispatched the NEXT thing (next real instruction OR a
		// deferred exception our instruction scheduled) -- the captured pre-grant
		// state is our instruction's retirement state: after its final prefetch but
		// before any deferred exception perturbs SR.S/SR.T, pushes a supervisor
		// frame, or vectors PC.  This is what the 68000 SingleStepTests corpus
		// records: state after the instruction and BEFORE the deferred trace
		// exception a set SR.T schedules (50% of the corpus has SR.T set).  Reading
		// the retired state from this snapshot rather than the live core (which has
		// run through the trace handler by the time m_ipc changes) models the trace
		// away without touching the live interpreter.
		snapshot_retired();
		int frozen_consumed = -1;   // cycle count latched at trace dispatch, if any
		for (int guard = 0; guard < 512; ++guard)
		{
			// Refresh the pre-grant snapshot, and latch the cycle count, only while
			// the core is still executing OUR instruction.  Once the deferred trace
			// has DISPATCHED (m_inst_state == S_TRACE) we stop: the snapshot keeps
			// the post-instruction / pre-trace register state, and frozen_consumed
			// keeps the instruction's own cycle cost (the trace exception's cycles
			// belong to the exception, which the corpus does not run).  The trace
			// handler runs over several grants that do NOT change m_ipc (it bumps
			// m_ipc only when it vectors to the handler's first instruction), so
			// without this guard the snapshot/cycle count would absorb the trace's
			// SR.S|T flip, supervisor-frame push, PC vector and ~34 trap cycles.
			if (m_inst_state == S_TRACE)
			{
				if (frozen_consumed < 0)
					frozen_consumed = consumed;
			}
			else
				snapshot_retired();
			*m_icountptr = 1;
			run();
			// If this step advanced m_ipc, the core has dispatched the next thing,
			// so its cycles belong to that, NOT our instruction -- break WITHOUT
			// accumulating them, keeping the pre-grant snapshot as the retired state.
			// (A self / short-backward branch whose target IS the just-executed
			// instruction leaves m_ipc == entry_ipc; those few cases never retire here
			// and fall into the documented branch-self-loop residual rather than risk
			// a substate-based boundary that mis-retires normal multi-cycle opcodes.)
			if (m_ipc != entry_ipc)
				break;
			consumed += 1 - *m_icountptr;
		}

		return frozen_consumed >= 0 ? frozen_consumed : consumed;
	}

	// Capture the architectural register file + SR + prefetch pointer AND the
	// watched final-RAM cells as the retirement snapshot (see step_instruction).
	// The RAM cells are snapshotted at the SAME pre-grant point as the registers,
	// so a single-step over-run -- where the grant that advances m_ipc also runs
	// the NEXT instruction's first memory write before we break -- cannot corrupt
	// the retired RAM image we compare against.  (The watched set is the case's
	// final-RAM addresses, registered via oracle_set_ram_watch().)
	void snapshot_retired()
	{
		for (int i = 0; i < 17; i++)
			m_snap_da[i] = m_da[i];
		m_snap_sr = m_sr;
		m_retired_au = m_au;
		m_retired_pc = m_pc;
		address_space &prog = space(AS_PROGRAM);
		for (std::size_t i = 0; i < m_ram_watch.size(); i++)
			m_ram_snap[i] = prog.read_byte(m_ram_watch[i]);
	}

	// The prefetch pointer / m_pc as of our instruction's retirement (captured in
	// step_instruction before the next instruction's microcode perturbs them).
	u32 retired_au() const { return m_retired_au; }
	u32 retired_genpc() const { return m_retired_pc; }

	// Reset cross-instruction microcode/IRQ state a fixture does not carry, so
	// each case starts on a clean architectural boundary.  Writing PC re-parks
	// m_inst_state/substate, but the deferred post-instruction state is NOT
	// touched by state_import, so clear it here.
	//
	// The load-bearing reset is m_next_state: at the end of an instruction the
	// microcode latches the *next* dispatch there -- in particular it sets it to
	// S_TRACE when the trace bit (SR_T) is set (m68000gen.py "next_trace").  Our
	// single-step stops at instruction retirement, before that deferred trace
	// exception runs, so a fixture with SR.T set leaves m_next_state == S_TRACE
	// behind.  Without clearing it the *following* case would spuriously take a
	// trace exception (observed as a stray supervisor entry: wrong PC=vector,
	// SR.S set, SSP pushed).  The corpus, by construction, captures the
	// architectural state immediately after the instruction and never runs the
	// deferred trace/interrupt, so clearing these here matches the corpus model.
	void clear_quirk_state()
	{
		m_count_before_instruction_step = 0;
		m_post_run = 0;
		m_post_run_cycles = 0;
		m_inst_substate = 0;
		m_next_state = 0;
		m_t = 0;
		m_isr = 0;          // mid-instruction ALU/flag scratch, not state-interface visible
		m_new_sr = 0;
		m_nmi_pending = 0;
		m_virq_state = 0;
		m_int_level = 0;
		m_int_next_state = 0;
	}

	// oracle_stepper
	virtual int oracle_step(int budget) override { return step_instruction(budget); }
	virtual void oracle_prepare_case() override { clear_quirk_state(); }

	// PC read-back adapter (ADR 0006 blocker #1).  The corpus encodes PC from
	// MAME's m_au -- the "next prefetch address" -- not from STATE_GENPC, which
	// exports m_pc.  We read the m_au value captured at our instruction's
	// retirement (retired_au(), snapshotted in step_instruction before the next
	// instruction's microcode perturbs the live m_au).  m_au is a protected member
	// of m68000_device, reachable here because this oracle device is a subclass --
	// so this is a pure harness-side read-back, no shared-core change.
	virtual bool oracle_retired_pc(u32 &out) const override { out = retired_au(); return true; }

	// Snapshot register read-back (see step_instruction / snapshot_retired).  The
	// harness reads the m68000 final register state from the retirement snapshot
	// rather than the live device, so a deferred trace/exception that runs between
	// our instruction's completion and the m_ipc-change retirement never leaks into
	// the captured result.  Maps the fixture's state index (the same M68K_* index
	// the register map uses) to the snapshot register, mirroring the core's
	// state_add layout (m_da[0..7]=D0-D7, [8..14]=A0-A6, [15]=USP, [16]=SP/SSP).
	// Returns false for indices it doesn't cover (PC -- handled via retired_au --
	// and anything unmapped), so the harness falls back to the live read there.
	virtual bool oracle_snapshot_reg(int state_index, u64 &out) const override
	{
		if (state_index >= M68K_D0 && state_index <= M68K_D7)
			out = m_snap_da[state_index - M68K_D0];
		else if (state_index >= M68K_A0 && state_index <= M68K_A6)
			out = m_snap_da[8 + (state_index - M68K_A0)];
		else if (state_index == M68K_USP)
			out = m_snap_da[15];
		else if (state_index == M68K_SP)
			out = m_snap_da[16];
		else if (state_index == M68K_SR)
			out = m_snap_sr & (SR_SR | SR_CCR);
		else
			return false;
		return true;
	}

	// RAM read-back from the retirement snapshot (see snapshot_retired): the value
	// each watched address held at our instruction's retirement, BEFORE the
	// over-run grant that advances m_ipc could let the next instruction's first
	// memory write clobber it.  Returns false for an address not in the watch set
	// (the caller then falls back to a live read).
	virtual bool oracle_snapshot_ram(u32 address, u8 &out) const override
	{
		for (std::size_t i = 0; i < m_ram_watch.size(); i++)
			if (m_ram_watch[i] == address)
			{
				out = m_ram_snap[i];
				return true;
			}
		return false;
	}

	// Register the set of RAM addresses to snapshot at retirement (the case's
	// final-RAM cells).  Cleared and re-set per case by the harness.
	virtual void oracle_set_ram_watch(const std::vector<u32> &addrs) override
	{
		m_ram_watch = addrs;
		m_ram_snap.assign(addrs.size(), 0);
	}

private:
	u32 m_retired_au = 0;
	u32 m_retired_pc = 0;
	u32 m_snap_da[17] = { 0 };
	u16 m_snap_sr = 0;
	std::vector<u32> m_ram_watch;   // final-RAM addresses to snapshot at retirement
	std::vector<u8>  m_ram_snap;    // their snapshotted values (parallel to m_ram_watch)
};

DECLARE_DEVICE_TYPE(ORACLE_M68000, oracle_m68000_device)
DEFINE_DEVICE_TYPE(ORACLE_M68000, oracle_m68000_device, "oracle_m68000", "CPU Oracle M68000")

oracle_m68000_device::oracle_m68000_device(const machine_config &mconfig, const char *tag, device_t *owner, u32 clock) :
	m68000_device(mconfig, ORACLE_M68000, tag, owner, clock)
{
}


//**************************************************************************
//  ORACLE DRIVER
//**************************************************************************

// Set just before machine.run() so the driver's machine_reset() hook can
// hand the started/reset CPU back to the harness.
namespace {
std::function<void (running_machine &, cpu_device &)> g_reset_hook;
} // anonymous namespace

class oracle_z80_state : public driver_device
{
public:
	oracle_z80_state(const machine_config &mconfig, device_type type, const char *tag) :
		driver_device(mconfig, type, tag),
		m_cpu(*this, "maincpu")
	{
	}

	void z80_machine(machine_config &config) ATTR_COLD;

	// keep the search path empty so we never touch the filesystem
	virtual std::vector<std::string> searchpath() const override { return std::vector<std::string>(); }

protected:
	virtual void machine_reset() override ATTR_COLD;

private:
	void z80_map(address_map &map) ATTR_COLD;
	void z80_io_map(address_map &map) ATTR_COLD;

	required_device<oracle_z80_device> m_cpu;
};


void oracle_z80_state::z80_map(address_map &map)
{
	// 64 KiB of flat RAM covering the whole z80 address space.  The z80
	// core fetches opcodes from AS_OPCODES when present and otherwise falls
	// back to AS_PROGRAM, so a single AS_PROGRAM RAM map serves both fetches
	// and data accesses.
	map(0x0000, 0xffff).ram();
}

void oracle_z80_state::z80_io_map(address_map &map)
{
	// Flat RAM over the full 16-bit I/O space so IN/OUT round-trip through it.
	// The oracle pre-loads the fixture's port READ values here before stepping
	// and reads back port WRITE values afterwards.
	map(0x0000, 0xffff).ram();
}

void oracle_z80_state::z80_machine(machine_config &config)
{
	ORACLE_Z80(config, m_cpu, 4_MHz_XTAL);
	m_cpu->set_addrmap(AS_PROGRAM, &oracle_z80_state::z80_map);
	m_cpu->set_addrmap(AS_IO, &oracle_z80_state::z80_io_map);
}

void oracle_z80_state::machine_reset()
{
	// Every device is started and reset and the scheduler is idle.  Hand the
	// CPU to the harness, then ask the machine to exit so run() returns.
	if (g_reset_hook)
		g_reset_hook(machine(), *m_cpu);
	machine().schedule_exit();
}


//**************************************************************************
//  M6502 ORACLE DRIVER
//**************************************************************************

class oracle_m6502_state : public driver_device
{
public:
	oracle_m6502_state(const machine_config &mconfig, device_type type, const char *tag) :
		driver_device(mconfig, type, tag),
		m_cpu(*this, "maincpu")
	{
	}

	void m6502_machine(machine_config &config) ATTR_COLD;

	// keep the search path empty so we never touch the filesystem
	virtual std::vector<std::string> searchpath() const override { return std::vector<std::string>(); }

protected:
	virtual void machine_reset() override ATTR_COLD;

private:
	void m6502_map(address_map &map) ATTR_COLD;

	required_device<oracle_m6502_device> m_cpu;
};


void oracle_m6502_state::m6502_map(address_map &map)
{
	// 64 KiB of flat RAM covering the whole 16-bit 6502 address space.  The
	// 6502 has no separate I/O space; every access goes through AS_PROGRAM.
	map(0x0000, 0xffff).ram();
}

void oracle_m6502_state::m6502_machine(machine_config &config)
{
	ORACLE_M6502(config, m_cpu, 1_MHz_XTAL);
	m_cpu->set_addrmap(AS_PROGRAM, &oracle_m6502_state::m6502_map);
}

void oracle_m6502_state::machine_reset()
{
	if (g_reset_hook)
		g_reset_hook(machine(), *m_cpu);
	machine().schedule_exit();
}


//**************************************************************************
//  M68000 ORACLE DRIVER
//**************************************************************************

class oracle_m68000_state : public driver_device
{
public:
	oracle_m68000_state(const machine_config &mconfig, device_type type, const char *tag) :
		driver_device(mconfig, type, tag),
		m_cpu(*this, "maincpu")
	{
	}

	void m68000_machine(machine_config &config) ATTR_COLD;

	// keep the search path empty so we never touch the filesystem
	virtual std::vector<std::string> searchpath() const override { return std::vector<std::string>(); }

protected:
	virtual void machine_reset() override ATTR_COLD;

private:
	void m68000_map(address_map &map) ATTR_COLD;

	required_device<oracle_m68000_device> m_cpu;
};


void oracle_m68000_state::m68000_map(address_map &map)
{
	// Flat RAM over the whole 24-bit (16 MiB) 68000 address space.  The corpus
	// model is flat memory with 16-bit big-endian word accesses; a single
	// AS_PROGRAM RAM map serves opcode fetches (AS_OPCODES falls back to
	// AS_PROGRAM) and data accesses alike.
	map(0x000000, 0xffffff).ram();
}

void oracle_m68000_state::m68000_machine(machine_config &config)
{
	ORACLE_M68000(config, m_cpu, 8_MHz_XTAL);
	m_cpu->set_addrmap(AS_PROGRAM, &oracle_m68000_state::m68000_map);
}

void oracle_m68000_state::machine_reset()
{
	if (g_reset_hook)
		g_reset_hook(machine(), *m_cpu);
	machine().schedule_exit();
}


//**************************************************************************
//  MINIMAL OSD
//**************************************************************************

// No-op osd_interface: every overridable is a do-nothing / default-return.
// The oracle never produces video, audio, input, fonts, MIDI or network
// traffic, so none of these is ever meaningfully invoked.
class test_osd : public osd_interface
{
public:
	test_osd() { }

	// general -- allocate a (non-hidden) render target so the render manager
	// has a UI target.  Real OSDs do this when they open their window; without
	// it render_manager::config_save() dereferences a null m_ui_target during
	// the machine's exit-phase config save.
	virtual void init(running_machine &machine) override
	{
		machine.render().target_alloc();
	}
	virtual void update(bool) override { }
	virtual void input_update(bool) override { }
	virtual void check_osd_inputs() override { }
	virtual void set_verbose(bool) override { }

	// debugger
	virtual void init_debugger() override { }
	virtual void wait_for_debugger(device_t &, bool) override { }

	// audio
	virtual bool no_sound() override { return true; }
	virtual bool sound_external_per_channel_volume() override { return false; }
	virtual bool sound_split_streams_per_source() override { return false; }
	virtual uint32_t sound_get_generation() override { return 0; }
	virtual osd::audio_info sound_get_information() override { return osd::audio_info(); }
	virtual uint32_t sound_stream_sink_open(uint32_t, std::string, uint32_t) override { return 0; }
	virtual uint32_t sound_stream_source_open(uint32_t, std::string, uint32_t) override { return 0; }
	virtual void sound_stream_close(uint32_t) override { }
	virtual void sound_stream_sink_update(uint32_t, const int16_t *, int) override { }
	virtual void sound_stream_source_update(uint32_t, int16_t *, int) override { }
	virtual void sound_stream_set_volumes(uint32_t, const std::vector<float> &) override { }
	virtual void sound_begin_update() override { }
	virtual void sound_end_update() override { }

	// input
	virtual void customize_input_type_list(std::vector<input_type_entry> &) override { }

	// video
	virtual void add_audio_to_recording(const int16_t *, int) override { }
	virtual std::vector<ui::menu_item> get_slider_list() override { return std::vector<ui::menu_item>(); }

	// font
	virtual osd_font::ptr font_alloc() override { return nullptr; }
	virtual bool get_font_families(std::string const &, std::vector<std::pair<std::string, std::string> > &) override { return false; }

	// command options
	virtual bool execute_command(const char *) override { return false; }

	// MIDI
	virtual std::unique_ptr<osd::midi_input_port> create_midi_input(std::string_view) override { return nullptr; }
	virtual std::unique_ptr<osd::midi_output_port> create_midi_output(std::string_view) override { return nullptr; }
	virtual std::vector<osd::midi_port_info> list_midi_ports() override { return std::vector<osd::midi_port_info>(); }

	// network
	virtual std::unique_ptr<osd::network_device> open_network_device(int, osd::network_handler &) override { return nullptr; }
	virtual std::vector<osd::network_device_info> list_network_devices() override { return std::vector<osd::network_device_info>(); }
};


// Trivial machine_manager: the base ctor is protected and reachable from a
// subclass.  running_machine::start() dereferences the result of create_ui()
// (m_ui->set_startup_text), so we return a plain ui_manager -- it is a
// concrete class whose every method is a no-op default, which is exactly
// what a headless run needs.
class test_machine_manager : public machine_manager
{
public:
	test_machine_manager(emu_options &options, osd_interface &osd) :
		machine_manager(options, osd)
	{
	}

	virtual ui_manager *create_ui(running_machine &machine) override
	{
		m_ui = std::make_unique<ui_manager>(machine);
		return m_ui.get();
	}

private:
	std::unique_ptr<ui_manager> m_ui;
};


// Deleter for the harness's osd_interface unique_ptr: osd_interface has a
// protected destructor, so deletion must go through the concrete type, which
// is only complete here.
void delete_test_osd(osd_interface *osd)
{
	delete static_cast<test_osd *>(osd);
}


//**************************************************************************
//  INPUT PORTS / ROM
//**************************************************************************

static INPUT_PORTS_START( oraclez80 )
INPUT_PORTS_END

ROM_START( oraclez80 )
ROM_END

static INPUT_PORTS_START( oraclem6502 )
INPUT_PORTS_END

ROM_START( oraclem6502 )
ROM_END

static INPUT_PORTS_START( oraclem68000 )
INPUT_PORTS_END

ROM_START( oraclem68000 )
ROM_END


//**************************************************************************
//  GAME DRIVER REGISTRATION (global scope)
//**************************************************************************

GAME( 2026, oraclez80, 0, z80_machine, oraclez80, oracle_z80_state, empty_init, ROT0, "MAME", "CPU Oracle z80 fixture", MACHINE_NO_SOUND | MACHINE_IS_BIOS_ROOT )
GAME( 2026, oraclem6502, 0, m6502_machine, oraclem6502, oracle_m6502_state, empty_init, ROT0, "MAME", "CPU Oracle m6502 fixture", MACHINE_NO_SOUND | MACHINE_IS_BIOS_ROOT )
GAME( 2026, oraclem68000, 0, m68000_machine, oraclem68000, oracle_m68000_state, empty_init, ROT0, "MAME", "CPU Oracle m68000 fixture", MACHINE_NO_SOUND | MACHINE_IS_BIOS_ROOT )


namespace cpuoracle {

//**************************************************************************
//  Z80 CORE DESCRIPTOR
//**************************************************************************

// Fixture field -> z80 state index.  ei/p/q are internal z80 quirk-state
// fields that MAME does not expose through the state interface, so they are
// deliberately absent here and are not asserted by the oracle (out of scope
// for this leg).
static const reg_map_entry s_z80_regmap[] =
{
	{ "pc",   Z80_PC   },
	{ "sp",   Z80_SP   },
	{ "a",    Z80_A    },
	{ "f",    Z80_F    },
	{ "b",    Z80_B    },
	{ "c",    Z80_C    },
	{ "d",    Z80_D    },
	{ "e",    Z80_E    },
	{ "h",    Z80_H    },
	{ "l",    Z80_L    },
	{ "i",    Z80_I    },
	{ "r",    Z80_R    },
	{ "wz",   Z80_WZ   },
	{ "ix",   Z80_IX   },
	{ "iy",   Z80_IY   },
	{ "af_",  Z80_AF2  },
	{ "bc_",  Z80_BC2  },
	{ "de_",  Z80_DE2  },
	{ "hl_",  Z80_HL2  },
	{ "im",   Z80_IM   },
	{ "iff1", Z80_IFF1 },
	{ "iff2", Z80_IFF2 },
	{ nullptr, 0 }
};

const cpu_core_descriptor &z80_core_descriptor()
{
	static const cpu_core_descriptor desc =
	{
		"z80",
		&GAME_NAME(oraclez80),
		s_z80_regmap
	};
	return desc;
}


//**************************************************************************
//  M6502 CORE DESCRIPTOR
//**************************************************************************

// Fixture field -> m6502 state index.  The SingleStepTests 6502 corpus names
// its registers pc/s/a/x/y/p; MAME exposes them as M6502_PC/S/A/X/Y/P through
// the state interface.  The whole architectural state of a 6502 is these six
// registers plus RAM, so every fixture field is mapped and asserted.
static const reg_map_entry s_m6502_regmap[] =
{
	{ "pc", M6502_PC },
	{ "s",  M6502_S  },
	{ "a",  M6502_A  },
	{ "x",  M6502_X  },
	{ "y",  M6502_Y  },
	{ "p",  M6502_P  },
	{ nullptr, 0 }
};

const cpu_core_descriptor &m6502_core_descriptor()
{
	static const cpu_core_descriptor desc =
	{
		"m6502",
		&GAME_NAME(oraclem6502),
		s_m6502_regmap
	};
	return desc;
}


//**************************************************************************
//  M68000 CORE DESCRIPTOR
//**************************************************************************

// Fixture field -> m68000 state index.  The SingleStepTests m68000 corpus names
// the 8 data + 7 address registers, the user and supervisor stack pointers
// separately (usp/ssp), the status register, and PC.  MAME stores a7 as two
// banked slots -- m_da[15] = USP (M68K_USP) and m_da[16] = SSP (exposed as
// M68K_SP) -- both mapped here directly, so their read-back is bank-independent.
// The test applies SR before PC mainly so the supervisor bit is settled when the
// PC write re-parks the core.  (Note: state_import(M68K_SR) does NOT itself call
// update_user_super(), so it does not switch m_program/m_opcodes; that is benign
// only because the oracle driver maps a single flat RAM, where the supervisor and
// user program spaces resolve to the same memory.)  The corpus `prefetch` queue is
// not a state-interface register; it is reconstructed when PC is written (the
// flat RAM holds the opcode words) and is not asserted directly.
static const reg_map_entry s_m68000_regmap[] =
{
	{ "d0",  M68K_D0  },
	{ "d1",  M68K_D1  },
	{ "d2",  M68K_D2  },
	{ "d3",  M68K_D3  },
	{ "d4",  M68K_D4  },
	{ "d5",  M68K_D5  },
	{ "d6",  M68K_D6  },
	{ "d7",  M68K_D7  },
	{ "a0",  M68K_A0  },
	{ "a1",  M68K_A1  },
	{ "a2",  M68K_A2  },
	{ "a3",  M68K_A3  },
	{ "a4",  M68K_A4  },
	{ "a5",  M68K_A5  },
	{ "a6",  M68K_A6  },
	{ "usp", M68K_USP },
	{ "ssp", M68K_SP  },
	{ "sr",  M68K_SR  },
	{ "pc",  M68K_PC  },
	{ nullptr, 0 }
};

const cpu_core_descriptor &m68000_core_descriptor()
{
	static const cpu_core_descriptor desc =
	{
		"m68000",
		&GAME_NAME(oraclem68000),
		s_m68000_regmap
	};
	return desc;
}


//**************************************************************************
//  HARNESS IMPLEMENTATION
//**************************************************************************

cpu_test_harness::cpu_test_harness(const cpu_core_descriptor &desc) :
	m_desc(desc),
	m_osd(nullptr, &delete_test_osd)
{
	for (const reg_map_entry *e = desc.regmap; e->json_field != nullptr; ++e)
		m_field_to_index.emplace(e->json_field, e->state_index);
}

cpu_test_harness::~cpu_test_harness()
{
	// destroy in reverse order of construction
	m_machine.reset();
	m_config.reset();
	m_manager.reset();
	m_options.reset();
	m_osd.reset();
	g_reset_hook = nullptr;
}

bool cpu_test_harness::run_with_machine(const std::function<void ()> &body)
{
	m_options = std::make_unique<emu_options>();
	m_options->set_value(OPTION_THROTTLE, false, OPTION_PRIORITY_MAXIMUM);
	m_options->set_system_name(m_desc.driver->name);

	m_osd = std::unique_ptr<osd_interface, void (*)(osd_interface *)>(new test_osd, &delete_test_osd);
	m_manager = std::make_unique<test_machine_manager>(*m_options, *m_osd);

	// running_machine::run() unconditionally pokes manager().http(); the HTTP
	// option defaults off so this just creates an inactive (no socket) manager
	// whose clear()/is_active() are safe to call.  Without it run() null-derefs.
	m_manager->start_http_server();

	m_config = std::make_unique<machine_config>(*m_desc.driver, *m_options);
	m_machine = std::make_unique<running_machine>(*m_config, *m_manager);

	// The reset hook runs after every device is started and reset, with the
	// CPU live.  Capture the CPU, run the caller's body against it, then ask
	// the machine to exit so run() returns (and only then tears everything
	// down).  All CPU work must happen here, while the device is alive.
	bool ran = false;
	g_reset_hook =
			[this, &body, &ran] (running_machine &machine, cpu_device &cpu)
			{
				m_cpu = &cpu;
				// Every oracle CPU device also implements oracle_stepper; recover
				// it once here so the single-step/quirk primitives dispatch
				// generically (no per-core downcast in the harness body).
				m_stepper = dynamic_cast<oracle_stepper *>(&cpu);
				body();
				m_stepper = nullptr;
				m_cpu = nullptr;
				ran = true;
				machine.schedule_exit();
			};

	m_machine->run(false);

	g_reset_hook = nullptr;
	return ran;
}

void cpu_test_harness::reset_cpu()
{
	// reset the device subtree directly so we land on an instruction boundary
	m_cpu->reset();
}

void cpu_test_harness::set_reg(const std::string &field, uint64_t value)
{
	auto it = m_field_to_index.find(field);
	if (it != m_field_to_index.end())
		m_cpu->set_state_int(it->second, value);
}

uint64_t cpu_test_harness::get_reg(const std::string &field) const
{
	auto it = m_field_to_index.find(field);
	if (it == m_field_to_index.end())
		return 0;
	return m_cpu->state_int(it->second);
}

bool cpu_test_harness::has_reg(const std::string &field) const
{
	return m_field_to_index.find(field) != m_field_to_index.end();
}

bool cpu_test_harness::retired_pc(uint32_t &out) const
{
	return m_stepper->oracle_retired_pc(out);
}

bool cpu_test_harness::snapshot_reg(const std::string &field, uint64_t &out) const
{
	auto it = m_field_to_index.find(field);
	if (it == m_field_to_index.end())
		return false;
	return m_stepper->oracle_snapshot_reg(it->second, out);
}

void cpu_test_harness::set_ram_watch(const std::vector<uint32_t> &addrs)
{
	m_stepper->oracle_set_ram_watch(addrs);
}

bool cpu_test_harness::snapshot_ram(uint32_t address, uint8_t &out) const
{
	return m_stepper->oracle_snapshot_ram(address, out);
}

void cpu_test_harness::write_ram(uint32_t address, uint8_t value)
{
	m_cpu->space(AS_PROGRAM).write_byte(address, value);
}

uint8_t cpu_test_harness::read_ram(uint32_t address) const
{
	return m_cpu->space(AS_PROGRAM).read_byte(address);
}

void cpu_test_harness::write_io(uint32_t address, uint8_t value)
{
	m_cpu->space(AS_IO).write_byte(address, value);
}

uint8_t cpu_test_harness::read_io(uint32_t address) const
{
	return m_cpu->space(AS_IO).read_byte(address);
}

int cpu_test_harness::step_one_instruction(int budget)
{
	return m_stepper->oracle_step(budget);
}

void cpu_test_harness::prepare_case()
{
	m_stepper->oracle_prepare_case();
}

void cpu_test_harness::set_quirk_q(uint8_t q)
{
	m_stepper->oracle_set_quirk_q(q);
}

} // namespace cpuoracle


//**************************************************************************
//  DRIVER LIST + FRONTEND STUBS
//**************************************************************************
//
//  The emu library expects the frontend/target to provide the driver list
//  and the emulator_info hooks.  mametests does not link the MAME frontend,
//  so we supply minimal definitions here (mirroring the standalone zexall
//  target in src/zexall/main.cpp).  Only ___empty and our oracle driver are
//  registered; that is all the headless bootstrap needs.

GAME_EXTERN(___empty);

// Must be sorted by short name (driver_list uses binary search): '_' (0x5f)
// sorts before lowercase letters, and
// "oraclem6502" < "oraclem68000" < "oraclez80".
const game_driver * const driver_list::s_drivers_sorted[] =
{
	&GAME_NAME(___empty),
	&GAME_NAME(oraclem6502),
	&GAME_NAME(oraclem68000),
	&GAME_NAME(oraclez80),
};

std::size_t const driver_list::s_driver_count = 4;

// emulator_info stubs -- none of these is exercised by the oracle path, but
// the symbols must resolve to link the emu library without the frontend.
const char *emulator_info::get_appname()               { return "cpuoracle"; }
const char *emulator_info::get_appname_lower()          { return "cpuoracle"; }
const char *emulator_info::get_configname()             { return "cpuoracle"; }
const char *emulator_info::get_copyright()              { return ""; }
const char *emulator_info::get_copyright_info()         { return ""; }
const char *emulator_info::get_bare_build_version()     { return ""; }
const char *emulator_info::get_build_version()          { return ""; }
void emulator_info::display_ui_chooser(running_machine &) { }
bool emulator_info::draw_user_interface(running_machine &) { return true; }
void emulator_info::periodic_check()                    { }
bool emulator_info::frame_hook()                        { return false; }
void emulator_info::sound_hook(const std::map<std::string, std::vector<std::pair<const float *, int>>> &) { }
void emulator_info::layout_script_cb(layout_file &, const char *) { }
bool emulator_info::standalone()                        { return true; }

int emulator_info::start_frontend(emu_options &, osd_interface &, std::vector<std::string> &) { return 0; }
int emulator_info::start_frontend(emu_options &, osd_interface &, int, char *[]) { return 0; }
