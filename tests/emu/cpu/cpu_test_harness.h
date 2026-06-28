// license:BSD-3-Clause
// copyright-holders:Mark Mackelprang
/***************************************************************************

    cpu_test_harness.h

    Differential CPU-execution oracle harness for the Catch2 test target.

    This harness boots a minimal headless running_machine that contains a
    single CPU device, applies a SingleStepTests fixture case (initial
    register + RAM state), single-steps exactly one instruction, and reads
    the resulting register + RAM state back out so the caller can assert
    strict equality against the fixture's expected final state and cycle
    count.

    The harness is written to be reusable across CPU cores: a core is
    described by a cpu_core_descriptor (a device type plus a table mapping
    fixture register-field names to device_state_interface state indices).
    The z80, m6502 and m68000 legs are implemented here.

***************************************************************************/
#ifndef MAME_TESTS_EMU_CPU_CPU_TEST_HARNESS_H
#define MAME_TESTS_EMU_CPU_CPU_TEST_HARNESS_H

#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>


// forward references (avoid pulling emu.h into the public header)
class running_machine;
class cpu_device;
class machine_config;
class emu_options;
class osd_interface;
class machine_manager;
struct game_driver;


namespace cpuoracle {

//**************************************************************************
//  CORE DESCRIPTOR
//**************************************************************************

// A single fixture-field -> state-index mapping entry.
struct reg_map_entry
{
	const char *json_field;     // key as it appears in the SingleStepTests JSON
	int         state_index;    // device_state_interface state index
};

// Per-core single-step + cross-instruction-state primitives.
//
// Each oracle CPU device subclass also implements this interface so the
// harness can drive "run exactly one instruction" and reset the
// cross-instruction quirk state generically, without the harness having to
// know the concrete device type.  The harness recovers the interface from the
// live cpu_device via dynamic_cast (every oracle device multiply-inherits it).
class oracle_stepper
{
public:
	virtual ~oracle_stepper() = default;

	// Run exactly one architectural instruction, granting `budget` cycles
	// (the fixture's expected count), and return the number consumed.  The
	// CPU must be parked on an instruction boundary on entry (true after a
	// reset or after the PC is written through the state interface).
	virtual int oracle_step(int budget) = 0;

	// Clear cross-instruction quirk state a SingleStepTests fixture does not
	// carry (e.g. a HALT latch, pending NMI), so each case starts clean.
	// No-op for cores without such state.
	virtual void oracle_prepare_case() { }

	// Seed a core-specific quirk input not exposed through the state
	// interface (the z80 SCF/CCF "Q" byte).  No-op for cores without one.
	virtual void oracle_set_quirk_q(uint8_t) { }

	// Read back the retired program counter in the *corpus's* PC convention,
	// for cores where that differs from what the state interface exports.
	//
	// The m68000 corpus encodes PC from MAME's `m_au` ("next prefetch address",
	// = instruction start + 4), while STATE_GENPC exports `m_pc` (= start + 2);
	// reading the corpus-convention PC therefore means reading `m_au` directly
	// rather than the mapped STATE_GENPC register (ADR 0006 blocker #1 / the
	// close-out's PC read-back adapter).  Returns true and sets `out` when the
	// core overrides PC read-back; the default returns false, meaning the
	// harness should compare PC through the normal register map (z80/m6502,
	// whose state-interface PC already matches their corpus convention).
	virtual bool oracle_retired_pc(uint32_t &out) const { (void)out; return false; }

	// Read a final register from the retirement snapshot (m68000: the state after
	// the instruction but before any deferred trace/exception), keyed by the same
	// state index the register map uses.  Returns true and sets `out` when the core
	// snapshots that index; false when the harness should read the live device
	// instead (the default -- z80/m6502 have no deferred-exception skew).
	virtual bool oracle_snapshot_reg(int state_index, uint64_t &out) const { (void)state_index; (void)out; return false; }

	// Read a final-RAM cell from the retirement snapshot (m68000: the value the
	// address held at instruction retirement, before a single-step over-run could
	// let the next instruction's first write clobber it).  Returns true and sets
	// `out` when the address is in the watch set; false otherwise (live read).
	virtual bool oracle_snapshot_ram(uint32_t address, uint8_t &out) const { (void)address; (void)out; return false; }

	// Register the RAM addresses to snapshot at retirement (the case's final-RAM
	// cells).  No-op for cores that read RAM live.
	virtual void oracle_set_ram_watch(const std::vector<uint32_t> &) { }

	// True iff the last single-step did NOT cleanly retire (the guard loop
	// exhausted without reaching the next instruction) -- the m68000 self-branch
	// signature.  Default false (z80/m6502 always retire cleanly).
	virtual bool oracle_did_not_retire() const { return false; }

	// True iff the live device is actually running in DRC mode (its m_isdrc latch
	// is set).  Overridden by oracle_m68000_device to expose the m68000's live
	// m_isdrc, so Leg B can REQUIRE the DRC really engaged (anti-vacuity guard).
	// Default false (cores without a DRC arm, or DRC off).
	virtual bool oracle_is_drc() const { return false; }

	// True iff the live device's space-topology gate (drc_native_mem_ea_allowed())
	// permits native memory-EA emission.  Overridden by oracle_m68000_device.
	virtual bool oracle_native_mem_ea_allowed() const { return false; }
	// # of gated memory-EA dispatch arms emitted in the last resident-block build
	// (0 => the btst-absolute arm was gated out).  Overridden by oracle_m68000_device.
	virtual uint32_t oracle_native_arm_emit_count() const { return 0; }
};

// Describes one CPU core: how to register its driver and how to translate
// fixture register fields to/from MAME state indices.
struct cpu_core_descriptor
{
	const char            *name;        // short core name, e.g. "z80"
	const game_driver     *driver;      // the registered oracle driver for this core
	const reg_map_entry   *regmap;      // null-terminated (json_field == nullptr) table
};


//**************************************************************************
//  APPLIED / OBSERVED STATE
//**************************************************************************

// One RAM cell as it appears in a fixture (address, byte value).
struct ram_cell
{
	uint32_t address;
	uint8_t  value;
};


//**************************************************************************
//  HARNESS
//**************************************************************************

// Owns the headless machine bootstrap for one CPU core and exposes the
// primitives the oracle test cases need: register set/get, RAM set/get,
// and single-instruction stepping.
class cpu_test_harness
{
public:
	explicit cpu_test_harness(const cpu_core_descriptor &desc);
	~cpu_test_harness();

	cpu_test_harness(const cpu_test_harness &) = delete;
	cpu_test_harness &operator=(const cpu_test_harness &) = delete;

	// Bring up the headless machine and run `body` while it is fully alive.
	//
	// The machine is driven through running_machine::run(), which is the only
	// public entry point and which always tears the machine down (stopping
	// every device and freeing its memory caches) once its run loop exits.
	// We therefore must do all CPU work *before* run() returns: `body` is
	// invoked from the inline driver's machine_reset() hook -- after every
	// device has started and reset, with the CPU live and parked on an
	// instruction boundary -- and the machine is asked to exit immediately
	// afterwards.  Inside `body` the register/RAM/step methods below are
	// valid; they must not be called once run() (and hence run_with_machine)
	// has returned.
	//
	// Returns true if the machine started and `body` ran.
	bool run_with_machine(const std::function<void ()> &body);

	// Request that the machine boot with the DRC enabled (OPTION_DRC) or disabled.
	// Call BEFORE run_with_machine().  Default false preserves Leg A exactly (the
	// interpreter arm).  When enabled, the m68000 oracle device latches m_isdrc
	// from allow_drc(); drc_engaged() reports whether it actually engaged.
	void set_drc(bool enable);

	// True iff the live oracle device is actually running in DRC mode.  Used by
	// Leg B's anti-vacuity guard (REQUIRE the DRC really engaged on the DRC harness,
	// and REQUIRE it did NOT on the interpreter harness).  Valid only while the
	// machine is alive (inside run_with_machine's body).
	bool drc_engaged() const;

	// True iff the live device's space-topology gate permits native memory-EA emission.
	bool native_mem_ea_allowed() const;
	// # of gated memory-EA dispatch arms emitted in the last resident-block build.
	uint32_t native_arm_emit_count() const;

	// Reset the CPU to a clean, between-instructions boundary.
	void reset_cpu();

	// Register access (by fixture field name, mapped via the descriptor's
	// regmap).  Unknown fields are ignored on set and return 0 on get.
	void set_reg(const std::string &field, uint64_t value);
	uint64_t get_reg(const std::string &field) const;
	bool has_reg(const std::string &field) const;

	// Read back the retired PC in the corpus's PC convention for cores that
	// override it (m68000: `m_au`, not the STATE_GENPC `m_pc`).  Returns true
	// and sets `out` when the live core overrides PC read-back; false otherwise
	// (the caller then compares PC through the normal register map).
	bool retired_pc(uint32_t &out) const;

	// Read a final register by fixture field name from the core's retirement
	// snapshot (m68000: post-instruction, pre-deferred-exception state).  Returns
	// true and sets `out` when the core provides a snapshot for that field; false
	// when the caller should use get_reg() (live device) instead.
	bool snapshot_reg(const std::string &field, uint64_t &out) const;

	// Register the RAM addresses to snapshot at retirement (the case's final-RAM
	// cells), and read one back from the retirement snapshot.  snapshot_ram returns
	// false (read live via read_ram) for cores/addresses without a snapshot.
	void set_ram_watch(const std::vector<uint32_t> &addrs);
	bool snapshot_ram(uint32_t address, uint8_t &out) const;

	// True iff the last step_one_instruction did not cleanly retire (m68000
	// self-branch signature; see oracle_did_not_retire).
	bool did_not_retire() const;

	// Reset the cross-instruction quirk state that a SingleStepTests fixture
	// does not carry (e.g. the z80 HALT latch and pending NMI), so each case
	// starts from a clean slate.  Call once per case before applying its
	// initial state.  No-op for cores without such state.
	void prepare_case();

	// Seed the z80 SCF/CCF "Q" quirk byte from a fixture's `q` field.  Q is an
	// input (not exposed through the state interface) that the YX result of
	// SCF/CCF depends on; without it those instructions mismatch.  No-op for
	// cores without such a field.
	void set_quirk_q(uint8_t q);

	// Flat RAM access into the CPU's program space.
	void write_ram(uint32_t address, uint8_t value);
	uint8_t read_ram(uint32_t address) const;

	// Flat RAM access into the CPU's I/O space (for IN/OUT instructions): the
	// fixture's port read values are written here before stepping and port
	// write values read back afterwards.
	void write_io(uint32_t address, uint8_t value);
	uint8_t read_io(uint32_t address) const;

	// Run exactly one architectural instruction, granting `budget` T-states
	// (the fixture's expected cycle count), and return the number consumed.
	// Granting the exact cost makes the core stop cleanly on the next
	// instruction boundary -- including for special cases like HALT whose
	// post-instruction service step would otherwise perturb the architectural
	// state (see the .cpp).  The caller asserts consumed == budget alongside
	// the strict final register/RAM comparison.
	int step_one_instruction(int budget);

	cpu_device &cpu() const { return *m_cpu; }

private:
	const cpu_core_descriptor &m_desc;
	std::map<std::string, int> m_field_to_index;
	oracle_stepper *m_stepper = nullptr;   // the live CPU's stepper interface

	// osd_interface has a protected destructor, so the OSD is owned through
	// a unique_ptr with a custom deleter that knows the concrete test OSD
	// type (defined in the .cpp where that type is complete).
	std::unique_ptr<osd_interface, void (*)(osd_interface *)> m_osd;
	std::unique_ptr<emu_options>      m_options;
	std::unique_ptr<machine_manager>  m_manager;
	std::unique_ptr<machine_config>   m_config;
	std::unique_ptr<running_machine>  m_machine;

	cpu_device *m_cpu = nullptr;
	bool        m_drc = false;          // request DRC (OPTION_DRC) on the next run_with_machine
};

//**************************************************************************
//  Z80 LEG
//**************************************************************************

// Returns the descriptor for the z80 oracle core.
const cpu_core_descriptor &z80_core_descriptor();

//**************************************************************************
//  M6502 LEG
//**************************************************************************

// Returns the descriptor for the m6502 oracle core.
const cpu_core_descriptor &m6502_core_descriptor();

//**************************************************************************
//  M68000 LEG
//**************************************************************************

// Returns the descriptor for the m68000 oracle core.
const cpu_core_descriptor &m68000_core_descriptor();

// Returns descriptors for the three gate-false topology fixtures.
const cpu_core_descriptor &m68000_asopcodes_core_descriptor();
const cpu_core_descriptor &m68000_userspace_core_descriptor();
const cpu_core_descriptor &m68000_mmu_core_descriptor();

} // namespace cpuoracle

#endif // MAME_TESTS_EMU_CPU_CPU_TEST_HARNESS_H
