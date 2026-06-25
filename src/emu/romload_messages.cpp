// license:BSD-3-Clause
// copyright-holders:MAMEdev Team
/***************************************************************************

    romload_messages.cpp

    User-facing message formatting for ROM loading.

***************************************************************************/

#include "romload_messages.h"

#include "util/language.h"

#include "strformat.h"


namespace romload {

//-------------------------------------------------
//  make_missing_files_message - B1
//-------------------------------------------------

std::string make_missing_files_message(
		std::string_view sysdescription,
		std::string_view sysshortname,
		std::string_view appname)
{
	return util::string_format(
			_("Required ROM/disk images for the system \"%1$s\" (%2$s) are missing or incorrect. "
				"The machine cannot be run. "
				"Run \"%3$s -verifyroms %2$s\" or open the \"Audit Media\" menu to see exactly which files are missing or wrong."),
			sysdescription,
			sysshortname,
			appname);
}

} // namespace romload
