// SPDX-License-Identifier: GPL-3.0-or-later
// PS5CEMU-HAR: the languages (lang.h). A table is a gettext .po file, read whole: each entry's
// English (msgid, with its msgctxt when it has one) and its translation, or its plural forms in the
// order the language's rule numbers them. Entries flagged fuzzy are left out; entries flagged menu
// (tools/lang.py flags what the in-game menus say) give the in-game fonts their characters.

#include "lang.h"
#include "paths.h"
#include "../ps5/log.h"

#include <algorithm>
#include <atomic>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <set>
#include <unordered_map>

extern "C" int32_t sceSystemServiceParamGetInt(int32_t paramId, int32_t* value);

namespace ps5lang
{
	namespace
	{
		const std::vector<Language> kLanguages = {
			{"ja", "日本語", 0},
			{"en-US", "English (United States)", 1},
			{"en-GB", "English (United Kingdom)", 18},
			{"fr", "Français (France)", 2},
			{"fr-CA", "Français (Canada)", 22},
			{"es", "Español (España)", 3},
			{"es-419", "Español (Latinoamérica)", 20},
			{"de", "Deutsch", 4},
			{"it", "Italiano", 5},
			{"nl", "Nederlands", 6},
			{"pt-PT", "Português (Portugal)", 7},
			{"pt-BR", "Português (Brasil)", 17},
			{"ru", "Русский", 8},
			{"uk", "Українська", 30},
			{"pl", "Polski", 16},
			{"cs", "Čeština", 23},
			{"hu", "Magyar", 24},
			{"ro", "Română", 26},
			{"el", "Ελληνικά", 25},
			{"tr", "Türkçe", 19},
			{"fi", "Suomi", 12},
			{"sv", "Svenska", 13},
			{"da", "Dansk", 14},
			{"nb", "Norsk", 15},
			{"ar", "العربية", 21},
			{"th", "ไทย", 27},
			{"vi", "Tiếng Việt", 28},
			{"id", "Bahasa Indonesia", 29},
			{"ko", "한국어", 9},
			{"zh-Hans", "简体中文", 11},
			{"zh-Hant", "繁體中文", 10},
		};

		// the English the code is written in (its spelling the UK's: docs/UI-REDESIGN.md, Appendix B)
		constexpr const char* kSource = "en-GB";

		// the table a regional variant builds on: what it does not say differently, it says as that
		const char* Parent(const std::string& code)
		{
			if (code == "fr-CA")
				return "fr";
			if (code == "es-419")
				return "es";
			return nullptr;
		}

		// Which of a language's plural forms a number takes, numbered as its table's msgstr[n] are
		// (the Plural-Forms tools/lang.py writes in each table's header)
		int PluralIndex(const std::string& code, long long count)
		{
			const unsigned long long n = count < 0 ? (unsigned long long)-count : (unsigned long long)count;
			const unsigned long long n10 = n % 10, n100 = n % 100;
			const std::string language = code.substr(0, code.find('-'));
			if (language == "ja" || language == "ko" || language == "zh" || language == "th" || language == "vi" || language == "id")
				return 0;
			if (language == "fr" || code == "pt-BR")
				return n > 1 ? 1 : 0;
			if (language == "ru" || language == "uk")
				return n10 == 1 && n100 != 11 ? 0 : n10 >= 2 && n10 <= 4 && (n100 < 10 || n100 >= 20) ? 1 : 2;
			if (language == "pl")
				return n == 1 ? 0 : n10 >= 2 && n10 <= 4 && (n100 < 10 || n100 >= 20) ? 1 : 2;
			if (language == "cs")
				return n == 1 ? 0 : n >= 2 && n <= 4 ? 1 : 2;
			if (language == "ro")
				return n == 1 ? 0 : n == 0 || (n100 > 0 && n100 < 20) ? 1 : 2;
			if (language == "ar")
				return n == 0 ? 0 : n == 1 ? 1 : n == 2 ? 2 : n100 >= 3 && n100 <= 10 ? 3 : n100 >= 11 ? 4 : 5;
			return n != 1 ? 1 : 0;
		}

		struct Entry
		{
			std::vector<std::string> forms; // the translation, or its plural forms
			bool plural = false;
			bool menu = false;
		};

		struct Table
		{
			std::string code;
			std::unordered_map<std::string, Entry> entries; // by msgctxt "\x04" msgid, or msgid
			const Table* parent = nullptr;

			const Entry* Find(const std::string& key) const
			{
				for (const Table* table = this; table; table = table->parent)
				{
					const auto it = table->entries.find(key);
					if (it != table->entries.end())
						return &it->second;
				}
				return nullptr;
			}
		};

		std::mutex s_mutex; // Load against itself
		std::vector<std::unique_ptr<Table>> s_tables; // every one read, kept (Tr's results stay valid)
		std::atomic<const Table*> s_current{nullptr};
		std::string s_code = kSource;
		std::string s_folder;
		MenuFont s_menuFont;

		std::string Key(const char* context, std::string_view english)
		{
			if (!context)
				return std::string(english);
			std::string key = context;
			key += '\x04';
			key += english;
			return key;
		}

		// A quoted .po string's contents, its escapes undone
		bool Unquote(std::string_view line, std::string& out)
		{
			const size_t open = line.find('"'), close = line.rfind('"');
			if (open == std::string_view::npos || close <= open)
				return false;
			for (size_t i = open + 1; i < close; i++)
			{
				char c = line[i];
				if (c == '\\' && i + 1 < close)
				{
					c = line[++i];
					c = c == 'n' ? '\n' : c == 't' ? '\t' : c == 'r' ? '\r' : c;
				}
				out += c;
			}
			return true;
		}

		bool Parse(const std::string& path, Table& table)
		{
			std::ifstream file(path, std::ios::binary);
			if (!file)
				return false;
			struct Pending
			{
				std::string context, id, plural;
				std::vector<std::string> forms;
				bool hasContext = false, fuzzy = false, menu = false;
			} entry;
			std::string* field = nullptr;
			auto finish = [&] {
				const bool translated = std::any_of(entry.forms.begin(), entry.forms.end(), [](const std::string& form) { return !form.empty(); });
				if (!entry.id.empty() && translated && !entry.fuzzy)
				{
					Entry& out = table.entries[Key(entry.hasContext ? entry.context.c_str() : nullptr, entry.id)];
					out.forms = std::move(entry.forms);
					out.plural = !entry.plural.empty();
					out.menu = entry.menu;
				}
				entry = {};
				field = nullptr;
			};
			bool inStrings = false; // past the comments of the entry being read
			bool afterContext = false; // the last keyword was msgctxt: its msgid belongs to it
			for (std::string line; std::getline(file, line);)
			{
				if (!line.empty() && line.back() == '\r')
					line.pop_back();
				if (line.empty())
					continue;
				if (line[0] == '#')
				{
					// a comment ends the entry before it; the flags belong to the next
					if (inStrings)
					{
						finish();
						inStrings = false;
					}
					if (line.rfind("#,", 0) == 0)
					{
						entry.fuzzy |= line.find("fuzzy") != std::string::npos;
						entry.menu |= line.find("menu") != std::string::npos;
					}
					continue; // obsolete entries (#~) and comments
				}
				if (line[0] == '"')
				{
					if (field)
						Unquote(line, *field);
					continue;
				}
				auto starts = [&](const char* keyword) { return line.rfind(keyword, 0) == 0; };
				const bool opens = starts("msgctxt") || (starts("msgid ") && !afterContext);
				if (opens && inStrings)
					finish(); // an entry with no comments between it and the one before
				afterContext = starts("msgctxt");
				inStrings = true;
				if (starts("msgctxt"))
				{
					entry.hasContext = true;
					field = &entry.context;
				}
				else if (starts("msgid_plural"))
					field = &entry.plural;
				else if (starts("msgid"))
					field = &entry.id;
				else if (starts("msgstr["))
				{
					const size_t index = (size_t)std::atoi(line.c_str() + 7);
					if (index > 8)
					{
						field = nullptr;
						continue;
					}
					if (entry.forms.size() <= index)
						entry.forms.resize(index + 1);
					field = &entry.forms[index];
				}
				else if (starts("msgstr"))
				{
					entry.forms.resize(1);
					field = &entry.forms[0];
				}
				else
				{
					field = nullptr;
					continue;
				}
				Unquote(line, *field);
			}
			if (inStrings)
				finish();
			return true;
		}

		const Table* Read(const std::string& code)
		{
			for (const auto& table : s_tables)
				if (table->code == code)
					return table.get();
			auto table = std::make_unique<Table>();
			table->code = code;
			const std::string folder = s_folder.empty() ? ps5paths::Assets() + "/lang" : s_folder;
			if (!Parse(folder + "/" + code + ".po", *table))
			{
				ps5log::Line("[lang] no table for {} in {}", code, folder);
				return nullptr;
			}
			if (const char* parent = Parent(code))
				table->parent = Read(parent);
			ps5log::Line("[lang] {}: {} strings", code, table->entries.size());
			s_tables.push_back(std::move(table));
			return s_tables.back().get();
		}
	}

	const std::vector<Language>& Languages()
	{
		return kLanguages;
	}

	const char* NameOf(const std::string& code)
	{
		for (const Language& language : kLanguages)
			if (code == language.code)
				return language.name;
		return code.c_str();
	}

	std::string SystemLanguage()
	{
		int32_t value = -1;
		if (sceSystemServiceParamGetInt(1, &value) == 0) // SCE_SYSTEM_SERVICE_PARAM_ID_LANG
			for (const Language& language : kLanguages)
				if (language.system == value)
					return language.code;
		return kSource;
	}

	std::string Resolve(const std::string& setting)
	{
		for (const Language& language : kLanguages)
			if (setting == language.code)
				return setting;
		// the pseudo-language tools/lang.py makes for finding text that does not fit: only where its
		// table is (it is never packaged)
		if (setting == "qps")
		{
			const std::string folder = s_folder.empty() ? ps5paths::Assets() + "/lang" : s_folder;
			if (std::ifstream(folder + "/qps.po"))
				return setting;
		}
		return SystemLanguage();
	}

	bool Load(const std::string& setting)
	{
		std::lock_guard lock(s_mutex);
		const std::string code = Resolve(setting);
		s_code = code;
		if (code == kSource)
		{
			s_current = nullptr;
			ps5log::Line("[lang] {}: the English the app is written in", code);
			return true;
		}
		const Table* table = Read(code);
		s_current = table;
		if (!table)
			s_code = kSource;
		return table != nullptr;
	}

	void SetFolder(std::string folder)
	{
		s_folder = std::move(folder);
	}

	const std::string& Current()
	{
		return s_code;
	}

	bool RightToLeft()
	{
		return s_code == "ar";
	}

	const char* Tr(const char* english)
	{
		const Table* table = s_current;
		if (!table || !english || !*english)
			return english;
		const Entry* entry = table->Find(english);
		return entry && !entry->forms.empty() && !entry->forms[0].empty() ? entry->forms[0].c_str() : english;
	}

	std::string Tr(const std::string& english)
	{
		const Table* table = s_current;
		if (!table || english.empty())
			return english;
		const Entry* entry = table->Find(english);
		return entry && !entry->forms.empty() && !entry->forms[0].empty() ? entry->forms[0] : english;
	}

	const char* TrC(const char* context, const char* english)
	{
		const Table* table = s_current;
		if (!table || !english || !*english)
			return english;
		const Entry* entry = table->Find(Key(context, english));
		return entry && !entry->forms.empty() && !entry->forms[0].empty() ? entry->forms[0].c_str() : english;
	}

	std::string TrC(const char* context, const std::string& english)
	{
		const Table* table = s_current;
		if (!table || english.empty())
			return english;
		const Entry* entry = table->Find(Key(context, english));
		return entry && !entry->forms.empty() && !entry->forms[0].empty() ? entry->forms[0] : english;
	}

	namespace detail
	{
		const char* Plural(long long count, const char* context, const char* one, const char* other)
		{
			const Table* table = s_current;
			if (table)
				if (const Entry* entry = table->Find(Key(context, one)); entry && entry->plural)
				{
					const size_t index = (size_t)PluralIndex(table->code, count);
					if (index < entry->forms.size() && !entry->forms[index].empty())
						return entry->forms[index].c_str();
				}
			return count == 1 ? one : other;
		}
	}

	std::string Percent(long long value)
	{
		// tr: a percentage: where the % goes, and the space before it where the language has one
		return TrFC("percent", "{0}%", value);
	}

	std::string Decimal(double value, int decimals)
	{
		std::string text = fmt::format("{:.{}f}", value, std::max(0, decimals));
		// tr: the decimal separator, as in 0.4 (a comma in many languages)
		const char* separator = TrC("decimal separator", ".");
		if (std::string_view(separator) != ".")
			if (const size_t dot = text.find('.'); dot != std::string::npos)
				text.replace(dot, 1, separator);
		return text;
	}

	std::string Seconds(double value, int decimals)
	{
		// tr: a time in seconds, as in 0.4 s
		return TrFC("seconds", "{0} s", Decimal(value, decimals));
	}

	namespace
	{
		// tr: the months as a full date has them ("March 3, 2017"; in some languages their genitive)
		constexpr const char* kMonths[] = {TrMarkC("month in a date", "January"), TrMarkC("month in a date", "February"),
			TrMarkC("month in a date", "March"), TrMarkC("month in a date", "April"), TrMarkC("month in a date", "May"),
			TrMarkC("month in a date", "June"), TrMarkC("month in a date", "July"), TrMarkC("month in a date", "August"),
			TrMarkC("month in a date", "September"), TrMarkC("month in a date", "October"), TrMarkC("month in a date", "November"),
			TrMarkC("month in a date", "December")};
		// tr: the months alone, with a year after them ("March 2017")
		constexpr const char* kMonthsAlone[] = {TrMarkC("month alone", "January"), TrMarkC("month alone", "February"),
			TrMarkC("month alone", "March"), TrMarkC("month alone", "April"), TrMarkC("month alone", "May"), TrMarkC("month alone", "June"),
			TrMarkC("month alone", "July"), TrMarkC("month alone", "August"), TrMarkC("month alone", "September"),
			TrMarkC("month alone", "October"), TrMarkC("month alone", "November"), TrMarkC("month alone", "December")};
		// tr: the months, short, as a save state's date has them ("Oct 9, 14:05")
		constexpr const char* kShortMonths[] = {TrMarkC("short month", "Jan"), TrMarkC("short month", "Feb"), TrMarkC("short month", "Mar"),
			TrMarkC("short month", "Apr"), TrMarkC("short month", "May"), TrMarkC("short month", "Jun"), TrMarkC("short month", "Jul"),
			TrMarkC("short month", "Aug"), TrMarkC("short month", "Sep"), TrMarkC("short month", "Oct"), TrMarkC("short month", "Nov"),
			TrMarkC("short month", "Dec")};
	}

	std::string Date(int year, int month, int day)
	{
		if (month < 1 || month > 12)
			return std::to_string(year);
		if (day < 1)
			return MonthAndYear(year, month);
		// tr: a full date: {0} the month, {1} the day, {2} the year
		return TrFC("date", "{0} {1}, {2}", TrC("month in a date", kMonths[month - 1]), day, year);
	}

	std::string MonthAndYear(int year, int month)
	{
		if (month < 1 || month > 12)
			return std::to_string(year);
		// tr: a month ({0}) and a year ({1})
		return TrFC("month and year", "{0} {1}", TrC("month alone", kMonthsAlone[month - 1]), year);
	}

	std::string ShortDateTime(int month, int day, int hour, int minute)
	{
		if (month < 1 || month > 12)
			return fmt::format("{:02}:{:02}", hour, minute);
		// tr: when a save state was made: {0} the month (short), {1} the day, {2} the time (14:05)
		return TrFC("short date and time", "{0} {1}, {2}", TrC("short month", kShortMonths[month - 1]), day,
			fmt::format("{:02}:{:02}", hour, minute));
	}

	std::u32string MenuCharacters()
	{
		std::set<char32_t> characters;
		for (const Table* table = s_current; table; table = table->parent)
			for (const auto& [key, entry] : table->entries)
				if (entry.menu)
					for (const std::string& form : entry.forms)
						for (size_t i = 0; i < form.size();)
						{
							// UTF-8 to code points
							const unsigned char lead = (unsigned char)form[i];
							const int length = lead < 0x80 ? 1 : (lead >> 5) == 6 ? 2 : (lead >> 4) == 14 ? 3 : (lead >> 3) == 30 ? 4 : 1;
							char32_t c = length == 1 ? lead : lead & (0x7f >> length);
							for (int k = 1; k < length && i + k < form.size(); k++)
								c = c << 6 | ((unsigned char)form[i + k] & 0x3f);
							characters.insert(c);
							i += length;
						}
		return std::u32string(characters.begin(), characters.end());
	}

	void SetMenuFont(const MenuFont& font)
	{
		s_menuFont = font;
	}

	const MenuFont& GetMenuFont()
	{
		return s_menuFont;
	}
}
