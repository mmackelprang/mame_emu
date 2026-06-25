// license:BSD-3-Clause
// copyright-holders:MAMEdev Team
/***************************************************************************

    clihelp.h

    Command-line interface help/discoverability helpers for MAME.

    These are small, self-contained formatting and fuzzy-matching helpers
    that surface already-present information to the user (slot/media usage
    syntax, unknown-option "did you mean" suggestions, usage examples).
    They depend only on the portable utility and OSD-core layers (no
    emulation core), so they can be unit-tested in isolation by the
    golden-CLI test fixture (tests/frontend/clitext.cpp).

    All emitted text is wrapped in the _() translation macro; English
    source strings are added here.

***************************************************************************/
#ifndef MAME_FRONTEND_MAME_CLIHELP_H
#define MAME_FRONTEND_MAME_CLIHELP_H

#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>


namespace cli_help {

// -------------------------------------------------------------------------
//  A1 - slot/media usage-syntax hints
// -------------------------------------------------------------------------

// Emit (via osd_printf_info) a one-line "to use" hint for -listslots showing
// the literal invocation a user must type to select a slot option, e.g.
//   To select a slot option, add to the command line: -<slot> <opt>
void print_listslots_usage_hint();

// Emit (via osd_printf_info) a one-line "to use" hint for -listmedia showing
// the literal invocation a user must type to mount media, e.g.
//   To mount media, add to the command line: -<media> <image> (e.g. -flop1 game.img)
void print_listmedia_usage_hint();


// -------------------------------------------------------------------------
//  A2 - unknown-option "did you mean" suggestions
// -------------------------------------------------------------------------

// Extract the offending option name (without the leading dash) from a
// core_options "unknown option" error message of the form
//   "Error: unknown option: -<name>".
// Returns an empty string if the message is not of that form.  This keeps the
// caller decoupled from the exact prose while still recovering the token to
// match against.
std::string extract_unknown_option_name(std::string_view message);

// Given an unknown option name (without leading dashes) and the set of known
// option names, return up to max_results known names that are close matches,
// best match first.  Matching reuses util::edit_distance (the same
// Jaro-Winkler-derived similarity used for system-name suggestions).  Only
// reasonably-close candidates (above a similarity threshold) are returned, so
// a wildly different token yields an empty list rather than noise.
std::vector<std::string> suggest_option_matches(
		std::string_view unknown,
		std::vector<std::string> const &known,
		std::size_t max_results = 5);

// Emit (via osd_printf_info) a "did you mean:" block for the supplied
// suggestions.  No output is produced when suggestions is empty.
void print_option_suggestions(std::vector<std::string> const &suggestions);


// -------------------------------------------------------------------------
//  A3 - -showusage examples block
// -------------------------------------------------------------------------

// Emit (via osd_printf_info) a short "Examples:" block showing concrete
// invocations (run a game, mount a floppy, list a system's slots).  exename
// is the base name of the running executable (e.g. "mame").
void print_usage_examples(std::string_view exename);

} // namespace cli_help

#endif // MAME_FRONTEND_MAME_CLIHELP_H
