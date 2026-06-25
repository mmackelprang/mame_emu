-- license:BSD-3-Clause
-- copyright-holders:MAMEdev Team

---------------------------------------------------------------------------
--
--   tests.lua
--
--   Rules for building tests
--
---------------------------------------------------------------------------

project("mametests")
	uuid ("66d4c639-196b-4065-a411-7ee9266564f5")
	kind "ConsoleApp"

	flags {
		"Symbols", -- always include minimum symbols for executables
	}

	if _OPTIONS["SEPARATE_BIN"]~="1" then
		targetdir(MAME_DIR)
	end

	configuration { "Release" }
		targetsuffix ""
		if _OPTIONS["PROFILE"] then
			targetsuffix "p"
		end

	configuration { "Debug" }
		targetsuffix "d"
		if _OPTIONS["PROFILE"] then
			targetsuffix "dp"
		end

	configuration { "mingw*" or "vs*" }
		targetextension ".exe"

	configuration { }

	-- Libraries needed to instantiate a CPU device inside a running_machine
	-- for the cpuoracle harness.  Mirrors the machine-running link set in
	-- scripts/src/main.lua minus the frontend/lua/standalone bits.  Static
	-- link order matters: emu before optional's dependencies, utils late.
	links {
		"optional",
		"emu",
		"dasm",
		"formats",
		"softfloat3",
		"wdlfft",
		"ymfm",
		ext_lib("jpeg"),
		"7z",
	}
	if CPU_INCLUDE_DRC_NATIVE then
		links {
			"asmjit",
		}
	end
	links {
		"bgfx",
		"bimg",
		"bx",
		"utils",
		ext_lib("expat"),
		ext_lib("zlib"),
		ext_lib("zstd"),
		ext_lib("flac"),
		ext_lib("utf8proc"),
		"ocore_" .. _OPTIONS["osd"],
	}

	includedirs {
		MAME_DIR .. "3rdparty/catch/single_include",
		MAME_DIR .. "src/osd",
		MAME_DIR .. "src/osd/interface",
		MAME_DIR .. "src/emu",
		MAME_DIR .. "src/devices",
		MAME_DIR .. "src/lib",
		MAME_DIR .. "src/lib/util",
		MAME_DIR .. "src/frontend/mame",
		MAME_DIR .. "tests/emu/cpu",
		ext_includedir("expat"),
		ext_includedir("zlib"),
		ext_includedir("rapidjson"),
	}

	-- NOTE: src/emu/video/rgbsse.cpp / rgbvmx.cpp (and their headers) are
	-- referenced by upstream's tests.lua but do not exist in this tree
	-- snapshot, which left mametests unbuildable.  rgbutil.h dispatches to
	-- the SIMD implementation via headers, so the rgbutil test does not need
	-- these standalone TUs; the dead references are dropped here.

	-- The cpuoracle harness boots a running_machine, which drags in a few
	-- OSD *interface* data classes (osd::input_seq, osd::network_handler)
	-- referenced by libemu.  These live in osd_<osd> alongside the real OSD
	-- backend; linking that whole library would pull in DirectInput, WASAPI,
	-- portaudio, COM dialogs, etc.  Instead we compile just the two
	-- self-contained interface TUs (they only include their own headers) so
	-- the no-op test OSD remains the only OSD behaviour in the binary.
	files {
		MAME_DIR .. "src/osd/interface/inputseq.cpp",
		MAME_DIR .. "src/osd/interface/nethandler.cpp",
	}

	files {
		MAME_DIR .. "tests/main.cpp",
		MAME_DIR .. "tests/lib/util/corestr.cpp",
		MAME_DIR .. "tests/lib/util/options.cpp",
		MAME_DIR .. "tests/emu/attotime.cpp",
		MAME_DIR .. "tests/emu/video/rgbutil.cpp",
		MAME_DIR .. "tests/emu/cpu/cpu_test_harness.cpp",
		MAME_DIR .. "tests/emu/cpu/cpu_test_harness.h",
		MAME_DIR .. "tests/emu/cpu/cpuoracle.cpp",
	}

