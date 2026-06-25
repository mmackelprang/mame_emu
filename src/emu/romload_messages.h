// license:BSD-3-Clause
// copyright-holders:MAMEdev Team
/***************************************************************************

    romload_messages.h

    User-facing message formatting for ROM loading.

    These are small, self-contained formatting helpers that build the
    actionable error text shown when a system cannot be started because
    required ROM/disk images are missing or incorrect.  They depend only on
    the portable utility layer (no emulation core), so they can be unit
    tested in isolation by the golden-CLI test fixture
    (tests/frontend/clitext.cpp), matching the pattern used by
    src/frontend/mame/clihelp.cpp.

    All emitted text is wrapped in the _() translation macro; English source
    strings are added here.

***************************************************************************/
#ifndef MAME_EMU_ROMLOAD_MESSAGES_H
#define MAME_EMU_ROMLOAD_MESSAGES_H

#pragma once

#include <string>
#include <string_view>


namespace romload {

// Build the actionable "required files are missing" message thrown when a
// system cannot be started.  The message names the failing system (both its
// human-readable description and its short name) and points the user at the
// tools that show the full per-file audit detail (-verifyroms and the in-UI
// "Audit Media" menu).  Keeping this pure and string-only lets it be golden
// tested without standing up an emulation core.
//
//   sysdescription - the human-readable system name (game_driver::type.fullname())
//   sysshortname   - the short system name (game_driver::name), e.g. "asteroid"
//   appname        - the lowercase application name (e.g. "mame")
std::string make_missing_files_message(
		std::string_view sysdescription,
		std::string_view sysshortname,
		std::string_view appname);

} // namespace romload

#endif // MAME_EMU_ROMLOAD_MESSAGES_H
