// license:BSD-3-Clause
// copyright-holders:Mark Mackelprang
/***************************************************************************

    cpuoracle.cpp

    Differential CPU-execution oracle test cases.

    These tests replay the SingleStepTests corpus (one JSON file per opcode,
    each an array of up to 1000 cases) against MAME's CPU cores.  For each
    case the harness applies the initial register + RAM state, single-steps
    exactly one instruction, and the test asserts strict equality of every
    architectural register, every fixture RAM cell, and the instruction's
    T-state (cycle) count.

    The corpus is fetched into the gitignored cache build/cpuoracle/<core>/
    by tests/cpuoracle/fetch_vectors.py.  When the cache is absent the test
    skips cleanly (SUCCEED + return) so a developer without the corpus still
    gets a green mametests run.

    Only the z80 leg is implemented in this PR.

***************************************************************************/

#include "catch.hpp"

#include "cpu_test_harness.h"

#include "rapidjson/document.h"
#include "rapidjson/error/en.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>


namespace {

namespace fs = std::filesystem;

// Where the fetched corpus lives, relative to the repo root (mametests is
// launched from MAME_DIR).
const char *const ORACLE_Z80_DIR = "build/cpuoracle/z80";


// Optional cap on the number of fixture files exercised, read from the
// environment.  The full z80 corpus is ~1604 files / ~1.6M cases; this lets a
// developer or CI run a fast representative subset (e.g. CPUORACLE_MAX_FILES=64)
// while still defaulting to the complete corpus.  0 / unset means "all".
std::size_t fixture_file_cap()
{
	const char *const env = std::getenv("CPUORACLE_MAX_FILES");
	if (env == nullptr || env[0] == '\0')
		return 0;
	const long v = std::strtol(env, nullptr, 10);
	return (v > 0) ? std::size_t(v) : 0;
}


// Read an entire file into a string.  Returns false if it can't be opened.
bool read_file(const fs::path &path, std::string &out)
{
	FILE *fp = std::fopen(path.string().c_str(), "rb");
	if (!fp)
		return false;
	std::fseek(fp, 0, SEEK_END);
	long const size = std::ftell(fp);
	std::fseek(fp, 0, SEEK_SET);
	if (size < 0)
	{
		std::fclose(fp);
		return false;
	}
	out.resize(std::size_t(size));
	std::size_t const got = size ? std::fread(&out[0], 1, std::size_t(size), fp) : 0;
	std::fclose(fp);
	out.resize(got);
	return true;
}


// Collect the corpus JSON files (sorted) from the cache directory, excluding
// the checksums sidecar.  Returns an empty vector when the directory is
// missing or holds no fixtures.
std::vector<fs::path> collect_fixtures(const fs::path &dir)
{
	std::vector<fs::path> files;
	std::error_code ec;
	if (!fs::is_directory(dir, ec))
		return files;
	for (const auto &entry : fs::directory_iterator(dir, ec))
	{
		if (!entry.is_regular_file())
			continue;
		const fs::path &p = entry.path();
		const std::string name = p.filename().string();
		if (p.extension() != ".json")
			continue;
		// skip non-fixture sidecars: the sha256 manifest and the fetcher's
		// .fetch_stamp.json metadata (a dotfile, and a JSON object not array)
		if (name.empty() || name[0] == '.')
			continue;
		if (name.find("checksums") != std::string::npos)
			continue;
		files.push_back(p);
	}
	std::sort(files.begin(), files.end());
	return files;
}


// Apply one register state object (initial or final) onto/against the CPU.
// On set==true the values are written into the device; otherwise the value
// returned is the live device value for the named field (caller compares).

// Write every register named in a fixture state object into the harness.
void apply_registers(cpuoracle::cpu_test_harness &harness, const rapidjson::Value &state)
{
	for (auto it = state.MemberBegin(); it != state.MemberEnd(); ++it)
	{
		const char *name = it->name.GetString();
		if (!it->value.IsInt() && !it->value.IsUint() && !it->value.IsInt64() && !it->value.IsUint64())
			continue; // skip "ram" (array) and any non-integer quirk fields
		if (harness.has_reg(name))
			harness.set_reg(name, std::uint64_t(it->value.GetInt64()));
	}
}

// Write the RAM cells from a fixture state object into the harness.
void apply_ram(cpuoracle::cpu_test_harness &harness, const rapidjson::Value &state)
{
	if (!state.HasMember("ram") || !state["ram"].IsArray())
		return;
	for (const auto &cell : state["ram"].GetArray())
	{
		std::uint32_t const addr = std::uint32_t(cell[0].GetInt64());
		std::uint8_t const val = std::uint8_t(cell[1].GetInt64());
		harness.write_ram(addr, val);
	}
}

// Pre-load the I/O space with the values an IN instruction is expected to read
// from each "r" (read) port named in the fixture's `ports` list.
void apply_read_ports(cpuoracle::cpu_test_harness &harness, const rapidjson::Value &test)
{
	if (!test.HasMember("ports") || !test["ports"].IsArray())
		return;
	for (const auto &port : test["ports"].GetArray())
	{
		if (std::string(port[2].GetString()) != "r")
			continue;
		const std::uint32_t addr = std::uint32_t(port[0].GetInt64());
		const std::uint8_t val = std::uint8_t(port[1].GetInt64());
		harness.write_io(addr, val);
	}
}

} // anonymous namespace


//**************************************************************************
//  SMOKE TEST (Task 2)
//**************************************************************************

TEST_CASE("CPU oracle harness boots and RAM round-trips", "[cpu][harness]")
{
	cpuoracle::cpu_test_harness harness(cpuoracle::z80_core_descriptor());

	bool checked = false;
	const bool ran = harness.run_with_machine(
			[&harness, &checked] ()
			{
				// a write into the flat program RAM must read back unchanged
				harness.write_ram(0x1234, 0xa5);
				REQUIRE(int(harness.read_ram(0x1234)) == 0xa5);

				harness.write_ram(0x0000, 0x00);
				REQUIRE(int(harness.read_ram(0x0000)) == 0x00);

				harness.write_ram(0xffff, 0xff);
				REQUIRE(int(harness.read_ram(0xffff)) == 0xff);

				checked = true;
			});

	REQUIRE(ran);
	REQUIRE(checked);
}


//**************************************************************************
//  Z80 ORACLE (Task 3)
//**************************************************************************

TEST_CASE("CPU oracle z80 SingleStepTests", "[cpu][z80]")
{
	std::vector<fs::path> fixtures = collect_fixtures(ORACLE_Z80_DIR);
	if (fixtures.empty())
	{
		SUCCEED("z80 fixtures not present -- run tests/cpuoracle/fetch_vectors.py --cores z80; skipping");
		return;
	}

	// honour an optional CPUORACLE_MAX_FILES cap (fast subset runs)
	const std::size_t cap = fixture_file_cap();
	if (cap != 0 && fixtures.size() > cap)
		fixtures.resize(cap);

	cpuoracle::cpu_test_harness harness(cpuoracle::z80_core_descriptor());

	std::size_t total_cases = 0;

	// All fixture replay happens inside the live machine (see run_with_machine):
	// the CPU's memory caches are only valid before running_machine::run()
	// tears the machine down.
	const bool ran = harness.run_with_machine(
			[&harness, &fixtures, &total_cases] ()
			{
				for (const fs::path &path : fixtures)
				{
					std::string text;
					REQUIRE(read_file(path, text));

					rapidjson::Document doc;
					doc.Parse(text.c_str());
					INFO("fixture: " << path.filename().string());
					REQUIRE_FALSE(doc.HasParseError());
					REQUIRE(doc.IsArray());

					for (const auto &test : doc.GetArray())
					{
						const std::string case_name = test.HasMember("name") ? test["name"].GetString() : "<unnamed>";
						INFO("fixture: " << path.filename().string() << "  case: " << case_name);

						const rapidjson::Value &initial = test["initial"];
						const rapidjson::Value &final = test["final"];

						// the fixture's cycle list length is the exact T-state count
						REQUIRE(test.HasMember("cycles"));
						REQUIRE(test["cycles"].IsArray());
						const int expected_cycles = int(test["cycles"].GetArray().Size());

						// 1. apply initial register + RAM state.  First clear the
						//    cross-instruction quirk state a fixture doesn't carry
						//    (HALT latch, pending NMI) so a prior case can't leak
						//    into this one.  Writing PC also re-parks the core on
						//    an instruction boundary, so register write order does
						//    not matter.
						harness.prepare_case();
						apply_registers(harness, initial);
						apply_ram(harness, initial);

						// pre-load the I/O space so IN instructions read the
						// fixture's expected port values
						apply_read_ports(harness, test);

						// seed the SCF/CCF Q quirk byte (an input not exposed
						// through the state interface) so those instructions'
						// undocumented YX flags come out right
						if (initial.HasMember("q") && initial["q"].IsInt())
							harness.set_quirk_q(std::uint8_t(initial["q"].GetInt()));

						// 2. run exactly one instruction (granting its expected
						//    cycle count); measure what was consumed
						const int consumed = harness.step_one_instruction(expected_cycles);

						// 3a. strict register equality for every mapped field
						//     present in the fixture's final state
						for (auto it = final.MemberBegin(); it != final.MemberEnd(); ++it)
						{
							const char *name = it->name.GetString();
							if (!it->value.IsInt() && !it->value.IsUint() && !it->value.IsInt64() && !it->value.IsUint64())
								continue; // "ram" array / non-int quirk fields (ei/p/q)
							if (!harness.has_reg(name))
								continue; // ei/p/q are deliberately not mapped (see harness)
							const std::uint64_t expected = std::uint64_t(it->value.GetInt64());
							const std::uint64_t actual = harness.get_reg(name);
							INFO("register " << name << " expected=" << expected << " actual=" << actual);
							REQUIRE(actual == expected);
						}

						// 3b. strict RAM equality for every fixture final cell
						if (final.HasMember("ram") && final["ram"].IsArray())
						{
							for (const auto &cell : final["ram"].GetArray())
							{
								const std::uint32_t addr = std::uint32_t(cell[0].GetInt64());
								const std::uint8_t expected = std::uint8_t(cell[1].GetInt64());
								const std::uint8_t actual = harness.read_ram(addr);
								INFO("ram[" << addr << "] expected=" << int(expected) << " actual=" << int(actual));
								REQUIRE(int(actual) == int(expected));
							}
						}

						// 3b'. strict I/O equality for every "w" (write) port:
						//      an OUT must have written the fixture's value into
						//      the I/O space at the named port address
						if (test.HasMember("ports") && test["ports"].IsArray())
						{
							for (const auto &port : test["ports"].GetArray())
							{
								if (std::string(port[2].GetString()) != "w")
									continue;
								const std::uint32_t addr = std::uint32_t(port[0].GetInt64());
								const std::uint8_t expected = std::uint8_t(port[1].GetInt64());
								const std::uint8_t actual = harness.read_io(addr);
								INFO("port[" << addr << "] expected=" << int(expected) << " actual=" << int(actual));
								REQUIRE(int(actual) == int(expected));
							}
						}

						// 3c. cycle (T-state) equality: the core must consume
						//     exactly the granted budget (len(cycles)) and stop
						//     cleanly on the instruction boundary
						INFO("cycles expected=" << expected_cycles << " actual=" << consumed);
						REQUIRE(consumed == expected_cycles);

						++total_cases;
					}
				}
			});

	REQUIRE(ran);
	WARN("z80 oracle: " << fixtures.size() << " fixtures, " << total_cases << " cases checked");
}
