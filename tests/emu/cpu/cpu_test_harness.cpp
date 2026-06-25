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
// friending or touching the scheduler.
class oracle_z80_device : public z80_device
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
};

DECLARE_DEVICE_TYPE(ORACLE_Z80, oracle_z80_device)
DEFINE_DEVICE_TYPE(ORACLE_Z80, oracle_z80_device, "oracle_z80", "CPU Oracle Z80")

oracle_z80_device::oracle_z80_device(const machine_config &mconfig, const char *tag, device_t *owner, u32 clock) :
	z80_device(mconfig, ORACLE_Z80, tag, owner, clock)
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


//**************************************************************************
//  GAME DRIVER REGISTRATION (global scope)
//**************************************************************************

GAME( 2026, oraclez80, 0, z80_machine, oraclez80, oracle_z80_state, empty_init, ROT0, "MAME", "CPU Oracle z80 fixture", MACHINE_NO_SOUND | MACHINE_IS_BIOS_ROOT )


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
				body();
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
	return downcast<oracle_z80_device &>(*m_cpu).step_instruction(budget);
}

void cpu_test_harness::prepare_case()
{
	downcast<oracle_z80_device &>(*m_cpu).clear_quirk_state();
}

void cpu_test_harness::set_quirk_q(uint8_t q)
{
	downcast<oracle_z80_device &>(*m_cpu).set_q(q);
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

const game_driver * const driver_list::s_drivers_sorted[] =
{
	&GAME_NAME(___empty),
	&GAME_NAME(oraclez80),
};

std::size_t const driver_list::s_driver_count = 2;

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
