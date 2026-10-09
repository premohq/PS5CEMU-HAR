#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The string tables (port/app/lang.h): the English in the code, and each language's translation.

    lang.py update            port/lang/ps5cemu-har.pot from the code, then every port/lang/<code>.po
                              merged with it: new strings added untranslated, gone ones dropped
    lang.py check [CODE...]   every table (or those named): each string translated, the same {0}, {1}
                              in it as in the English, as many lines, its plural forms all there;
                              exits 1 on a problem
    lang.py stats             how much of each table is translated
    lang.py numbered          the template's strings, numbered, one a line (for import)
    lang.py import CODE FILE  translations by number ("12 text", "12|1 a plural form", \\n for a new
                              line; "# ..." lines are comments) into port/lang/CODE.po
    lang.py pseudo            port/lang/qps.po: every string longer by about 40 %, accented, in
                              brackets, to find text that does not fit (PREVIEW_LANGUAGE=qps); never
                              packaged

What is looked for in port/: Tr("..."), TrC("context", "..."), TrF("...", ...), TrFC("context",
"...", ...), TrP(count, "one", "other", ...), TrMark("...") and TrMarkC("context", "..."), each
string a C++ literal or adjacent literals. A comment "// tr: ..." on the line before (or at the end
of the line) is kept for translators. Strings from the in-game menus' files are flagged "menu": their
characters make the in-game fonts (lang.cpp, MenuCharacters).
"""

import pathlib
import re
import sys
import unicodedata

ROOT = pathlib.Path(__file__).resolve().parent.parent
PORT = ROOT / "port"
LANG = PORT / "lang"
TEMPLATE = LANG / "ps5cemu-har.pot"

CALLS = {"Tr": (None, 0), "TrC": (0, 1), "TrF": (None, 0), "TrFC": (0, 1), "TrP": (None, 1), "TrMark": (None, 0), "TrMarkC": (0, 1)}
# the files whose strings the in-game menus show
MENU_FILES = {
    "port/app/ingame.cpp",
    "port/app/ingame3ds.cpp",
    "port/app/side_menu.h",
    "port/app/menu_canvas.h",
    "port/azahar/core.cpp",
    "port/app/usb_devices.cpp",
    "port/ps5/display.h",
    "port/app/lang.cpp",
}

# code: (nplurals, plural expression), as lang.cpp's PluralIndex numbers the forms
PLURALS = {
    "one": (1, "0"),
    "two": (2, "(n != 1)"),
    "fr": (2, "(n > 1)"),
    "slavic": (3, "(n%10==1 && n%100!=11 ? 0 : n%10>=2 && n%10<=4 && (n%100<10 || n%100>=20) ? 1 : 2)"),
    "pl": (3, "(n==1 ? 0 : n%10>=2 && n%10<=4 && (n%100<10 || n%100>=20) ? 1 : 2)"),
    "cs": (3, "(n==1) ? 0 : (n>=2 && n<=4) ? 1 : 2"),
    "ro": (3, "(n==1 ? 0 : (n==0 || (n%100 > 0 && n%100 < 20)) ? 1 : 2)"),
    "ar": (6, "(n==0 ? 0 : n==1 ? 1 : n==2 ? 2 : n%100>=3 && n%100<=10 ? 3 : n%100>=11 ? 4 : 5)"),
}
LANGUAGES = {
    "ja": "one", "en-US": "two", "fr": "fr", "fr-CA": "fr", "es": "two", "es-419": "two", "de": "two", "it": "two",
    "nl": "two", "pt-PT": "two", "pt-BR": "fr", "ru": "slavic", "uk": "slavic", "pl": "pl", "cs": "cs", "hu": "two",
    "ro": "ro", "el": "two", "tr": "two", "fi": "two", "sv": "two", "da": "two", "nb": "two", "ar": "ar", "th": "one",
    "vi": "one", "id": "one", "ko": "one", "zh-Hans": "one", "zh-Hant": "one", "qps": "two",
}
# regional variants: a string they leave out is their parent's (lang.cpp, Parent)
PARENTS = {"fr-CA": "fr", "es-419": "es"}
# the variants that need only what they say differently: an untranslated string there is not a problem
PARTIAL = {"en-US", "fr-CA", "es-419"}


class Entry:
    def __init__(self, context, msgid, plural=None):
        self.context, self.msgid, self.plural = context, msgid, plural
        self.refs, self.comments, self.menu = [], [], False
        self.forms = []  # the translation's msgstr / msgstr[n]
        self.fuzzy = False
        self.translator = []  # "# " comments, kept

    def key(self):
        return (self.context, self.msgid)


# -- reading the code ----------------------------------------------------------------------------

TOKEN = re.compile(
    r"""(?P<comment>//[^\n]*|/\*.*?\*/)|(?P<string>(?:u8|u|U|L)?"(?:\\.|[^"\\\n])*")|(?P<char>'(?:\\.|[^'\\\n])+')|"""
    r"""(?P<ident>[A-Za-z_][A-Za-z0-9_]*)|(?P<number>[0-9][0-9A-Za-z_.']*)|(?P<space>\s+)|(?P<punct>::|.)""",
    re.S,
)


def unescape(body):
    out, i = [], 0
    while i < len(body):
        c = body[i]
        if c != "\\":
            out.append(c)
            i += 1
            continue
        n = body[i + 1]
        i += 2
        simple = {"n": "\n", "t": "\t", "r": "\r", "\\": "\\", '"': '"', "'": "'", "?": "?", "a": "\a", "0": "\0"}
        if n == "u":
            out.append(chr(int(body[i:i + 4], 16)))
            i += 4
        elif n == "U":
            out.append(chr(int(body[i:i + 8], 16)))
            i += 8
        elif n == "x":
            m = re.match(r"[0-9A-Fa-f]+", body[i:])
            out.append(chr(int(m.group(0), 16)))
            i += len(m.group(0))
        elif n in simple:
            out.append(simple[n])
        else:
            raise ValueError(f"escape \\{n}")
    return "".join(out)


def tokens(text):
    line = 1
    for m in TOKEN.finditer(text):
        kind = m.lastgroup
        value = m.group(0)
        yield kind, value, line
        line += value.count("\n")


def scan(path):
    rel = path.relative_to(ROOT).as_posix()
    text = path.read_text(encoding="utf-8")
    toks = [t for t in tokens(text)]
    # translators' comments by line
    notes = {}
    for kind, value, line in toks:
        if kind == "comment" and value.startswith("//"):
            m = re.match(r"//\s*tr:\s*(.*)", value)
            if m:
                notes[line] = m.group(1).strip()
    code = [t for t in toks if t[0] not in ("space", "comment")]
    found = []
    for i, (kind, value, line) in enumerate(code):
        if kind != "ident" or value not in CALLS:
            continue
        if i + 1 >= len(code) or code[i + 1][1] != "(":
            continue
        if i > 0 and code[i - 1][1] in (".", "->"):
            continue
        if i > 0 and code[i - 1][1] == "::" and not (i > 1 and code[i - 2][1] == "ps5lang"):
            continue
        # its arguments, split at the top level's commas
        args, depth, current = [], 0, []
        j = i + 2
        while j < len(code):
            k, v, _ = code[j]
            if v in "([{":
                depth += 1
            elif v in ")]}":
                if depth == 0:
                    break
                depth -= 1
            if v == "," and depth == 0:
                args.append(current)
                current = []
            else:
                current.append((k, v))
            j += 1
        args.append(current)

        def literal(arg):
            if not arg or any(k != "string" for k, _ in arg):
                return None
            return "".join(unescape(v[v.index('"') + 1:-1]) for _, v in arg)

        context_index, english_index = CALLS[value]
        context = literal(args[context_index]) if context_index is not None and len(args) > context_index else None
        if context_index is not None and context is None:
            continue
        if value == "TrP":
            if len(args) < 3:
                continue
            one, other = literal(args[1]), literal(args[2])
            if one is None or other is None:
                continue  # its definition (lang.h), or forms looked up at run time
            entry = Entry(None, one, other)
        else:
            if len(args) <= english_index:
                continue
            english = literal(args[english_index])
            if english is None:
                continue  # looked up at run time: its literal is marked elsewhere
            entry = Entry(context, english)
        entry.refs.append(f"{rel}:{line}")
        for at in (line, line - 1):
            if at in notes:
                entry.comments.append(notes[at])
        entry.menu = rel in MENU_FILES
        found.append(entry)
    return found


def compatibility_notes():
    """The compatibility list's notes (docs/COMPATIBILITY.md), which the hub shows: one a game."""
    notes = []
    path = ROOT / "docs/COMPATIBILITY.md"
    columns = None
    for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        cells = [c.strip() for c in line.strip().strip("|").split("|")] if line.startswith("|") else None
        if not cells:
            columns = None
            continue
        if "Notes" in cells:
            columns = cells.index("Notes")
            continue
        if columns is None or set("".join(cells)) <= set("-: ") or len(cells) <= columns or not cells[columns]:
            continue
        # as port/app/compatibility.cpp's Plain makes it: no bold, links as their words
        note = re.sub(r"\[([^\]]*)\]\([^)]*\)", r"\1", cells[columns].replace("**", "")).strip()
        entry = Entry("compatibility note", note)
        entry.refs.append(f"docs/COMPATIBILITY.md:{number}")
        entry.comments.append("a note from the compatibility list, about how the game runs")
        notes.append(entry)
    return notes


def extract():
    entries = {}
    order = []
    files = sorted(p for p in PORT.rglob("*") if p.suffix in (".cpp", ".h"))
    for path in files:
        for entry in scan(path):
            if not entry.msgid:
                continue
            key = entry.key()
            if key in entries:
                known = entries[key]
                if known.plural != entry.plural:
                    raise SystemExit(f"{entry.refs[0]}: '{entry.msgid}' is a plural here and not at {known.refs[0]} (or the other way)")
                known.refs += entry.refs
                known.comments += [c for c in entry.comments if c not in known.comments]
                known.menu |= entry.menu
            else:
                entries[key] = entry
                order.append(key)
    for entry in compatibility_notes():
        if entry.key() not in entries:
            entries[entry.key()] = entry
            order.append(entry.key())
    return [entries[k] for k in order]


# -- .po files --------------------------------------------------------------------------------------

def quote(text):
    escaped = text.replace("\\", "\\\\").replace('"', '\\"').replace("\t", "\\t").replace("\n", "\\n")
    if "\\n" in escaped[:-2]:
        parts = escaped.split("\\n")
        lines = [p + "\\n" for p in parts[:-1]] + ([parts[-1]] if parts[-1] else [])
        return '""\n' + "\n".join(f'"{p}"' for p in lines)
    return f'"{escaped}"'


def header(code):
    rule = PLURALS[LANGUAGES[code]] if code in LANGUAGES else PLURALS["two"]
    return (
        "Project-Id-Version: PS5CEMU-HAR\n"
        f"Language: {code}\n"
        "MIME-Version: 1.0\n"
        "Content-Type: text/plain; charset=UTF-8\n"
        "Content-Transfer-Encoding: 8bit\n"
        f"Plural-Forms: nplurals={rule[0]}; plural={rule[1]};\n"
    )


def write(path, entries, code=None):
    out = ['msgid ""', "msgstr " + quote(header(code) if code else header("en-GB").replace("Language: en-GB", "Language: "))]
    for e in entries:
        out.append("")
        for t in e.translator:
            out.append(f"# {t}" if t else "#")
        for c in e.comments:
            out.append(f"#. {c}")
        refs = " ".join(e.refs)
        if refs:
            out.append(f"#: {refs}")
        flags = (["fuzzy"] if e.fuzzy else []) + (["menu"] if e.menu else [])
        if flags:
            out.append("#, " + ", ".join(flags))
        if e.context is not None:
            out.append("msgctxt " + quote(e.context))
        out.append("msgid " + quote(e.msgid))
        if e.plural is not None:
            out.append("msgid_plural " + quote(e.plural))
            n = PLURALS[LANGUAGES.get(code, "two")][0] if code else 2
            forms = (e.forms + [""] * n)[:n]
            for i, f in enumerate(forms):
                out.append(f"msgstr[{i}] " + quote(f))
        else:
            out.append("msgstr " + quote(e.forms[0] if e.forms else ""))
    path.write_text("\n".join(out) + "\n", encoding="utf-8")


def unquote(line):
    body = line[line.index('"') + 1:line.rindex('"')]
    return body.replace("\\n", "\n").replace("\\t", "\t").replace('\\"', '"').replace("\\\\", "\\")


def read(path):
    """A .po file's entries (its header left out)."""
    entries = []
    current = None  # the entry being read
    pending = Entry(None, "")  # the next one's comments and flags, until its first keyword
    field = None  # what a continuation line adds to
    after_context = False

    def flush():
        nonlocal current
        if current is not None and current.msgid:
            entries.append(current)
        current = None

    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.rstrip("\r")
        if not line.strip():
            continue
        if line.startswith("#"):
            flush()
            field = None
            if line.startswith("#,"):
                pending.fuzzy |= "fuzzy" in [f.strip() for f in line[2:].split(",")]
            elif line.startswith("# ") or line == "#":
                pending.translator.append(line[2:])
            continue  # references, extracted comments and obsolete entries (#~) are the template's
        if line.startswith('"'):
            if field is not None:
                field.add(unquote(line))
            continue
        keyword = line.split(" ", 1)[0]
        if keyword == "msgctxt" or (keyword == "msgid" and not after_context):
            flush()
        if current is None:
            current, pending = pending, Entry(None, "")
        after_context = keyword == "msgctxt"
        e, value = current, unquote(line)
        if keyword == "msgctxt":
            field = _Field(lambda v, e=e: setattr(e, "context", v), value)
        elif keyword == "msgid":
            field = _Field(lambda v, e=e: setattr(e, "msgid", v), value)
        elif keyword == "msgid_plural":
            field = _Field(lambda v, e=e: setattr(e, "plural", v), value)
        elif keyword.startswith("msgstr["):
            index = int(keyword[7:-1])
            while len(e.forms) <= index:
                e.forms.append("")
            field = _Field(lambda v, e=e, i=index: e.forms.__setitem__(i, v), value)
        elif keyword == "msgstr":
            e.forms = [""]
            field = _Field(lambda v, e=e: e.forms.__setitem__(0, v), value)
        else:
            raise SystemExit(f"{path}: cannot read: {line}")
    flush()
    return entries


class _Field:
    """A .po string being read: its first line, then each continuation line added."""

    def __init__(self, setter, value):
        self.setter, self.value = setter, ""
        self.add(value)

    def add(self, value):
        self.value += value
        self.setter(self.value)


def tables():
    return sorted(p for p in LANG.glob("*.po"))


# -- commands ---------------------------------------------------------------------------------------

def update():
    LANG.mkdir(parents=True, exist_ok=True)
    template = extract()
    write(TEMPLATE, template)
    codes = sorted(set(LANGUAGES) - {"qps"})
    for code in codes:
        path = LANG / f"{code}.po"
        old = {e.key(): e for e in read(path)} if path.exists() else {}
        merged = []
        for t in template:
            e = Entry(t.context, t.msgid, t.plural)
            e.refs, e.comments, e.menu = t.refs, t.comments, t.menu
            if t.key() in old:
                o = old[t.key()]
                e.forms, e.fuzzy, e.translator = o.forms, o.fuzzy, o.translator
                if (o.plural is None) != (t.plural is None):
                    e.forms, e.fuzzy = [], False
            if code in PARTIAL and not any(e.forms):
                continue  # a variant says only what it says differently
            merged.append(e)
        write(path, merged, code)
    print(f"{TEMPLATE.relative_to(ROOT)}: {len(template)} strings; {len(codes)} tables merged")


PLACEHOLDER = re.compile(r"\{[0-9]*(?::[^{}]*)?\}")


def placeholders(text):
    return sorted(PLACEHOLDER.findall(text))


def check(codes):
    template = {e.key(): e for e in read(TEMPLATE)}
    problems = 0
    paths = [LANG / f"{c}.po" for c in codes] if codes else [p for p in tables() if p.stem != "qps"]
    for path in paths:
        code = path.stem
        if not path.exists():
            print(f"{code}: no table")
            problems += 1
            continue
        entries = read(path)
        n = PLURALS[LANGUAGES[code]][0]
        missing = 0
        for e in entries:
            where = f"{code}: '{e.msgid[:60]}'"
            if e.key() not in template:
                print(f"{where}: not in the template (run lang.py update)")
                problems += 1
                continue
            if not any(e.forms):
                if code not in PARTIAL:
                    missing += 1
                continue
            if e.fuzzy:
                print(f"{where}: fuzzy")
                problems += 1
            english = [e.msgid] if e.plural is None else [e.msgid, e.plural]
            want = placeholders(e.plural if e.plural is not None else e.msgid)
            forms = e.forms if e.plural is not None else e.forms[:1]
            if e.plural is not None and (len(forms) != n or not all(forms)):
                print(f"{where}: {n} plural forms wanted, {sum(1 for f in forms if f)} given")
                problems += 1
            for form in forms:
                if not form:
                    continue
                got = placeholders(form)
                # a singular form may say "one" in words where the language's one form is only ever 1
                if got != want and not (e.plural is not None and set(got) <= set(want) and n > 1 and form is forms[0] and code not in ("ru", "uk", "pl", "cs", "ro", "ar")):
                    print(f"{where}: {{}} differ: {got} for {want}: {form[:70]}")
                    problems += 1
                if form.count("\n") != english[-1].count("\n"):
                    print(f"{where}: {form.count(chr(10))} new lines for {english[-1].count(chr(10))}")
                    problems += 1
                if unicodedata.normalize("NFC", form) != form:
                    print(f"{where}: not in NFC")
                    problems += 1
                if form != form.strip() and english[-1] == english[-1].strip():
                    print(f"{where}: spaces at an end")
                    problems += 1
        known = {e.key() for e in entries}
        if code not in PARTIAL:
            missing += sum(1 for k in template if k not in known)
        if missing:
            print(f"{code}: {missing} of {len(template)} not translated")
            problems += 1
    sys.exit(1 if problems else 0)


def stats():
    template = read(TEMPLATE)
    words = sum(len(e.msgid.split()) + len((e.plural or "").split()) for e in template)
    print(f"template: {len(template)} strings, {words} words")
    for path in tables():
        entries = read(path)
        done = sum(1 for e in entries if any(e.forms) and not e.fuzzy)
        print(f"{path.stem:8} {done:5} of {len(template)}")


def numbered():
    for i, e in enumerate(read(TEMPLATE), 1):
        def esc(t):
            return t.replace("\\", "\\\\").replace("\n", "\\n")
        ctx = f"[{e.context}] " if e.context is not None else ""
        notes = f"   # {'; '.join(e.comments)}" if e.comments else ""
        if e.plural is None:
            print(f"{i} {ctx}{esc(e.msgid)}{notes}")
        else:
            print(f"{i}|0 {esc(e.msgid)}{notes}")
            print(f"{i}|1 {esc(e.plural)}")


def import_(code, source):
    template = read(TEMPLATE)
    path = LANG / f"{code}.po"
    entries = {e.key(): e for e in read(path)} if path.exists() else {}
    n = PLURALS[LANGUAGES[code]][0]
    count = 0
    for raw in pathlib.Path(source).read_text(encoding="utf-8").splitlines():
        line = raw.rstrip()
        if not line or line.startswith("#"):
            continue
        m = re.match(r"(\d+)(?:\|(\d+))?\s(.*)$", line)
        if not m:
            raise SystemExit(f"{source}: cannot read: {line}")
        number, form, text = int(m.group(1)), m.group(2), m.group(3)
        text = unicodedata.normalize("NFC", text.replace("\\n", "\n").replace("\\\\", "\\"))
        if not 1 <= number <= len(template):
            raise SystemExit(f"{source}: no string {number}")
        t = template[number - 1]
        e = entries.get(t.key())
        if e is None:
            e = Entry(t.context, t.msgid, t.plural)
            entries[t.key()] = e
        if t.plural is None:
            if form is not None:
                raise SystemExit(f"{source}: {number} is not a plural")
            e.forms = [text]
        else:
            index = int(form or 0)
            if index >= n:
                raise SystemExit(f"{source}: {number}|{index}: {code} has {n} forms")
            while len(e.forms) < n:
                e.forms.append("")
            e.forms[index] = text
        e.fuzzy = False
        count += 1
    merged = []
    for t in template:
        e = entries.get(t.key()) or Entry(t.context, t.msgid, t.plural)
        e.refs, e.comments, e.menu = t.refs, t.comments, t.menu
        if code in PARTIAL and not any(e.forms):
            continue
        merged.append(e)
    write(path, merged, code)
    print(f"{code}: {count} translations in")


def pseudo():
    accents = str.maketrans("aceinouyAEIOUCN", "àçéîñöûÿÅÉÎÖÜÇÑ")

    def stretch(text):
        parts = re.split(r"(\{[^{}]*\}|\n)", text)
        out = []
        for p in parts:
            if PLACEHOLDER.fullmatch(p) or p == "\n":
                out.append(p)
            else:
                grown = p.translate(accents)
                extra = int(len(p) * 0.4)
                out.append(grown + ("~" * extra if extra and p.strip() else ""))
        return "⟦" + "".join(out) + "⟧"

    entries = read(TEMPLATE)
    for e in entries:
        e.forms = [stretch(e.msgid)] if e.plural is None else [stretch(e.msgid), stretch(e.plural)]
    write(LANG / "qps.po", entries, "qps")
    print(f"port/lang/qps.po: {len(entries)} strings")


def main():
    args = sys.argv[1:]
    if not args or args[0] in ("-h", "--help"):
        print(__doc__)
        return
    command = args[0]
    if command == "update":
        update()
    elif command == "check":
        check(args[1:])
    elif command == "stats":
        stats()
    elif command == "numbered":
        numbered()
    elif command == "import" and len(args) == 3:
        import_(args[1], args[2])
    elif command == "pseudo":
        pseudo()
    else:
        raise SystemExit(__doc__)


if __name__ == "__main__":
    main()
