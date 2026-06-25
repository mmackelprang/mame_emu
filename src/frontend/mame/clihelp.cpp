// license:BSD-3-Clause
// copyright-holders:MAMEdev Team
/***************************************************************************

    clihelp.cpp

    Command-line interface help/discoverability helpers for MAME.

***************************************************************************/

#include "clihelp.h"

#include "util/language.h"

#include "corestr.h"
#include "strformat.h"
#include "unicode.h"

#include "osdcore.h"

#include <algorithm>
#include <cctype>
#include <utility>


namespace cli_help {

namespace {

// Candidates this dissimilar (or worse) from the unknown token are not worth
// suggesting.  util::edit_distance is a prefix-weighted penalty where 0.0 is
// identical and 1.0 is "no characters in common", so a smaller value is a
// better match.  The threshold is deliberately conservative so that a wildly
// different token produces no noise.
constexpr double SUGGESTION_THRESHOLD = 0.4;

// Normalise to a case-folded UTF-32 string the same way the system-name
// approximate matcher does, so option suggestions behave consistently.
std::u32string normalize_for_match(std::string_view s)
{
	return ustr_from_utf8(normalize_unicode(s, unicode_normalization_form::D, true));
}

} // anonymous namespace


//-------------------------------------------------
//  print_listslots_usage_hint - A1
//-------------------------------------------------

void print_listslots_usage_hint()
{
	osd_printf_info("\n%s\n", _("To select a slot option, add it to the command line, e.g.: -<slot> <opt>"));
}


//-------------------------------------------------
//  print_listmedia_usage_hint - A1
//-------------------------------------------------

void print_listmedia_usage_hint()
{
	osd_printf_info("\n%s\n", _("To mount media, add it to the command line, e.g.: -<media> <image> (such as -flop1 game.img)"));
}


//-------------------------------------------------
//  extract_unknown_option_name - A2
//-------------------------------------------------

std::string extract_unknown_option_name(std::string_view message)
{
	// matches core_options::parse_command_line():
	//   throw options_error_exception("Error: unknown option: -%s\n", optionname);
	constexpr std::string_view marker("unknown option: -");
	std::size_t const pos(message.find(marker));
	if (pos == std::string_view::npos)
		return std::string();

	std::size_t start(pos + marker.size());
	std::size_t end(start);
	while ((end < message.size()) && (message[end] != '\n') && (message[end] != '\r'))
		++end;

	// trim trailing whitespace the format string's "\n" or padding may leave
	while ((end > start) && (std::isspace(static_cast<unsigned char>(message[end - 1]))))
		--end;

	return std::string(message.substr(start, end - start));
}


//-------------------------------------------------
//  suggest_option_matches - A2
//-------------------------------------------------

std::vector<std::string> suggest_option_matches(
		std::string_view unknown,
		std::vector<std::string> const &known,
		std::size_t max_results)
{
	std::vector<std::string> result;
	if (unknown.empty() || known.empty() || (max_results == 0))
		return result;

	std::u32string const search(normalize_for_match(unknown));

	// score every known name; keep those close enough to be worth showing
	std::vector<std::pair<double, std::string const *> > scored;
	scored.reserve(known.size());
	for (std::string const &candidate : known)
	{
		if (candidate.empty())
			continue;
		double const penalty(util::edit_distance(search, normalize_for_match(candidate)));
		if (penalty < SUGGESTION_THRESHOLD)
			scored.emplace_back(penalty, &candidate);
	}

	// best (smallest penalty) first; stable so ties keep input order
	std::stable_sort(
			scored.begin(),
			scored.end(),
			[] (auto const &a, auto const &b) { return a.first < b.first; });

	std::size_t const count((std::min)(max_results, scored.size()));
	result.reserve(count);
	for (std::size_t i = 0; i < count; ++i)
		result.push_back(*scored[i].second);
	return result;
}


//-------------------------------------------------
//  print_option_suggestions - A2
//-------------------------------------------------

void print_option_suggestions(std::vector<std::string> const &suggestions)
{
	if (suggestions.empty())
		return;

	std::string joined;
	for (std::string const &s : suggestions)
	{
		if (!joined.empty())
			joined.append(", ");
		joined.push_back('-');
		joined.append(s);
	}

	osd_printf_info("%s\n", util::string_format(_("Did you mean: %s"), joined));
}


//-------------------------------------------------
//  print_usage_examples - A3
//-------------------------------------------------

void print_usage_examples(std::string_view exename)
{
	std::string const exe(exename);

	osd_printf_info("\n%s\n", _("Examples:"));
	osd_printf_info("  %-28s %s\n", util::string_format("%s asteroid", exe), _("run a game"));
	osd_printf_info("  %-28s %s\n", util::string_format("%s apple2e -flop1 game.dsk", exe), _("run a system with a floppy mounted"));
	osd_printf_info("  %-28s %s\n", util::string_format("%s -listslots apple2e", exe), _("list a system's slot options"));
	osd_printf_info("  %-28s %s\n", util::string_format("%s -listmedia apple2e", exe), _("list a system's media (and file extensions)"));
}

} // namespace cli_help
