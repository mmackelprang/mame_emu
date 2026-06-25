// license:BSD-3-Clause
// copyright-holders:MAMEdev Team
/***************************************************************************

    clitext.cpp

    Golden-output tests for the command-line interface discoverability
    helpers (src/frontend/mame/clihelp.cpp).

    These tests exercise the small, emulation-core-free helpers that feed
    the -listslots / -listmedia usage hints, the unknown-option "did you
    mean" suggestions, and the -showusage Examples block.

    The locale is pinned to C/English (no translation catalog loaded) so the
    English source strings are emitted verbatim; assertions check for stable
    *structural substrings* rather than full prose, to stay robust against
    future wording or translation changes.

***************************************************************************/

#include "catch.hpp"

#include "clihelp.h"
#include "romload_messages.h"

#include "util/language.h"
#include "strformat.h"

#include "osdcore.h"

#include <string>
#include <string_view>
#include <vector>


namespace {

// -------------------------------------------------------------------------
//  cli_text_fixture - pin locale to C/English and capture CLI output
// -------------------------------------------------------------------------

class cli_text_fixture : public osd_output
{
public:
	cli_text_fixture()
	{
		// pin the locale: drop any translation catalog so _() returns the
		// English source strings verbatim, keeping golden output stable
		util::unload_translation();
		osd_output::push(this);
	}

	virtual ~cli_text_fixture()
	{
		osd_output::pop(this);
	}

	std::string const &info() const { return m_info; }
	std::string const &error() const { return m_error; }

protected:
	virtual void output_callback(osd_output_channel channel, util::format_argument_pack<char> const &args) override
	{
		switch (channel)
		{
		case OSD_OUTPUT_CHANNEL_INFO:
			m_info.append(util::string_format(args));
			break;
		case OSD_OUTPUT_CHANNEL_ERROR:
		case OSD_OUTPUT_CHANNEL_WARNING:
			m_error.append(util::string_format(args));
			break;
		default:
			break;
		}
	}

private:
	std::string m_info;
	std::string m_error;
};

} // anonymous namespace


// -------------------------------------------------------------------------
//  Task 10 - the fixture itself: locale pinned, capture works
// -------------------------------------------------------------------------

TEST_CASE("CLI fixture captures osd_printf_info output", "[cli]")
{
	cli_text_fixture fixture;

	osd_printf_info("hello %s\n", "world");

	REQUIRE(fixture.info().find("hello world") != std::string::npos);
	REQUIRE(fixture.error().empty());
}

TEST_CASE("CLI fixture locale is pinned to C/English", "[cli]")
{
	cli_text_fixture fixture;

	// with no catalog loaded, _() must return the English source string
	REQUIRE(std::string_view(_("Examples:")) == "Examples:");
}


// -------------------------------------------------------------------------
//  Task 11 - A1: -listslots / -listmedia usage-syntax hint
// -------------------------------------------------------------------------

TEST_CASE("listslots prints a slot usage-syntax hint", "[cli]")
{
	cli_text_fixture fixture;

	cli_help::print_listslots_usage_hint();

	// structural substrings: the literal "-<slot> <opt>" invocation
	REQUIRE(fixture.info().find("-<slot> <opt>") != std::string::npos);
}

TEST_CASE("listmedia prints a media usage-syntax hint", "[cli]")
{
	cli_text_fixture fixture;

	cli_help::print_listmedia_usage_hint();

	// structural substrings: the literal "-<media> <image>" invocation and a concrete example
	REQUIRE(fixture.info().find("-<media> <image>") != std::string::npos);
	REQUIRE(fixture.info().find("-flop1 game.img") != std::string::npos);
}


// -------------------------------------------------------------------------
//  Task 12 - A2: unknown-option "did you mean"
// -------------------------------------------------------------------------

TEST_CASE("unknown-option name is extracted from the error message", "[cli]")
{
	// mirrors core_options::parse_command_line() message format
	REQUIRE(cli_help::extract_unknown_option_name("Error: unknown option: -window\n") == "window");
	REQUIRE(cli_help::extract_unknown_option_name("Error: unknown option: -nofilter\n") == "nofilter");
	// a message that isn't an unknown-option error yields nothing
	REQUIRE(cli_help::extract_unknown_option_name("Error: option -foo expected a parameter\n").empty());
}

TEST_CASE("close option names are suggested, distant ones are not", "[cli]")
{
	std::vector<std::string> const known{ "window", "fullscreen", "joystick", "sound", "rompath" };

	// a near-miss of an existing option should be suggested with it on top
	auto const close(cli_help::suggest_option_matches("windo", known));
	REQUIRE_FALSE(close.empty());
	REQUIRE(close.front() == "window");

	// a token with nothing close should suggest nothing
	auto const distant(cli_help::suggest_option_matches("zzzzzzzz", known));
	REQUIRE(distant.empty());
}

TEST_CASE("did-you-mean block is printed for suggestions", "[cli]")
{
	cli_text_fixture fixture;

	cli_help::print_option_suggestions({ "window", "windowed" });

	REQUIRE(fixture.info().find("Did you mean") != std::string::npos);
	// suggestions are presented with the leading dash a user must type
	REQUIRE(fixture.info().find("-window") != std::string::npos);
}

TEST_CASE("did-you-mean block is silent with no suggestions", "[cli]")
{
	cli_text_fixture fixture;

	cli_help::print_option_suggestions({});

	REQUIRE(fixture.info().empty());
}


// -------------------------------------------------------------------------
//  Task 12 - A3: -showusage Examples block
// -------------------------------------------------------------------------

TEST_CASE("showusage prints an Examples section", "[cli]")
{
	cli_text_fixture fixture;

	cli_help::print_usage_examples("mame");

	REQUIRE(fixture.info().find("Examples:") != std::string::npos);
	// concrete, copy-pasteable invocations using the running exe name
	REQUIRE(fixture.info().find("mame asteroid") != std::string::npos);
	REQUIRE(fixture.info().find("-flop1 game.dsk") != std::string::npos);
	REQUIRE(fixture.info().find("-listslots") != std::string::npos);
}


// -------------------------------------------------------------------------
//  Task 13 - B1: actionable launch-time missing-ROM error message
// -------------------------------------------------------------------------

TEST_CASE("missing-files error names the system and points at the audit tools", "[cli]")
{
	// pin the locale so the English source string is produced verbatim
	util::unload_translation();

	std::string const message(
			romload::make_missing_files_message("Asteroids (rev 4)", "asteroid", "mame"));

	// structural substrings: the human description, the short name, and the
	// next-step pointers a user can act on - not the exact prose
	REQUIRE(message.find("Asteroids (rev 4)") != std::string::npos);
	REQUIRE(message.find("asteroid") != std::string::npos);
	REQUIRE(message.find("-verifyroms asteroid") != std::string::npos);
	REQUIRE(message.find("Audit Media") != std::string::npos);
}
