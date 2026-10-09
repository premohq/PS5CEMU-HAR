// SPDX-License-Identifier: GPL-3.0-or-later
// PS5CEMU-HAR: the languages (docs/UI-REDESIGN.md, 7.7). Every word the launcher and the in-game menus
// show is English in the code, and is looked up in the language's string table: assets/lang/<code>.po,
// gettext's format, made from the code and checked by tools/lang.py. The table is read as the app
// starts, in the PS5's own language unless Settings > General > Language names another, and again
// whenever that setting changes. A string a table lacks stays English, so nothing is ever blank.
//
//   Tr("Settings")                        a word or a sentence, in the language
//   TrC("border", "Shell")                when the English means more than one thing: what it is here
//   TrF("Installed: {0}", version)        with something in it; {0}, {1} go where the language puts them
//   TrP(count, "{0} game", "{0} games")   a number, in the language's plural forms ({0} is the number)
//   TrMark("Top above bottom")            a name in a table, for Tr to look up where it is shown
//
// Tables stay loaded once read, so what Tr returned stays valid when the language changes.

#pragma once

#include <fmt/format.h>

#include <cstdint>
#include <exception>
#include <string>
#include <string_view>
#include <vector>

namespace ps5lang
{
	struct Language
	{
		const char* code; // as its table is named: "de", "pt-BR", "zh-Hans"
		const char* name; // in its own words: "Deutsch"
		int system;		  // the PS5's number for it (sceSystemServiceParamGetInt's language)
	};
	// Every language the PS5 offers, in the order Settings lists them
	const std::vector<Language>& Languages();
	// A language's own name ("Deutsch"); the code itself when it is not one of them
	const char* NameOf(const std::string& code);

	// The PS5's language, as a code ("en-GB" when it cannot be read)
	std::string SystemLanguage();
	// The language a setting asks for: its code, or the PS5's when it is empty or unknown
	std::string Resolve(const std::string& setting);
	// Reads the language's table (and the table it builds on: fr-CA's is fr's). False, with English
	// in its place, when there is none.
	bool Load(const std::string& setting);
	// Where the tables are: the app's assets/lang (the preview's port/lang)
	void SetFolder(std::string folder);
	const std::string& Current(); // the code in use
	bool RightToLeft();			  // Arabic's

	const char* Tr(const char* english);
	std::string Tr(const std::string& english);
	const char* TrC(const char* context, const char* english);
	std::string TrC(const char* context, const std::string& english);
	constexpr const char* TrMark(const char* english) { return english; }
	constexpr const char* TrMarkC(const char*, const char* english) { return english; }

	namespace detail
	{
		const char* Plural(long long count, const char* context, const char* one, const char* other);

		// Formatted here, with the fmt of the code that asks (Azahar's tree has its own, whose types
		// cannot cross into lang.cpp's): a table's mistake (a brace too many) costs the line nothing,
		// the English is formatted instead
		template<typename... Args>
		std::string Format(const char* pattern, const char* english, const Args&... args)
		{
			try
			{
				return fmt::vformat(pattern, fmt::make_format_args(args...));
			}
			catch (const std::exception&)
			{
			}
			try
			{
				return fmt::vformat(english, fmt::make_format_args(args...));
			}
			catch (const std::exception&)
			{
				return english;
			}
		}
	}

	template<typename... Args>
	std::string TrF(const char* english, const Args&... args)
	{
		return detail::Format(Tr(english), english, args...);
	}

	template<typename... Args>
	std::string TrFC(const char* context, const char* english, const Args&... args)
	{
		return detail::Format(TrC(context, english), english, args...);
	}

	template<typename... Args>
	std::string TrP(long long count, const char* one, const char* other, const Args&... args)
	{
		return detail::Format(detail::Plural(count, nullptr, one, other), count == 1 ? one : other, count, args...);
	}

	// Numbers as the language writes them: a percentage ("50 %", "%50"), a decimal ("0,4"), seconds
	std::string Percent(long long value);
	std::string Decimal(double value, int decimals);
	std::string Seconds(double value, int decimals);
	// Dates as the language writes them: a game's release (a full date, or a month and year), and
	// when a save state was made (a short date and the time)
	std::string Date(int year, int month, int day);
	std::string MonthAndYear(int year, int month);
	std::string ShortDateTime(int month, int day, int hour, int minute);

	// The characters of what the in-game menus say, in the language in use (their fonts are made
	// with these only: docs/UI-REDESIGN.md, 7.7)
	std::u32string MenuCharacters();

	// The font the in-game menus take what Lexend lacks from (a CJK title's, a Russian menu's): one
	// of the console's, which the launcher chose as it handed over to the game. Empty: none needed.
	struct MenuFont
	{
		std::string path;
		int face = 0; // in a collection (.ttc)
	};
	void SetMenuFont(const MenuFont& font);
	const MenuFont& GetMenuFont();
}
