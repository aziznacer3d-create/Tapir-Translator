# -*- coding: utf-8 -*-
"""
Tapir Translator - translation layer for the tkinter interfaces of Tapir scripts.

This file is written by the Tapir Translator add-on into
%APPDATA%\\Tapir Translator\\layer\\ (it is overwritten every time ArchiCAD starts:
do not edit it there, edit the source file in the repository instead).

How it works, step by step
--------------------------
1. Loading. At ArchiCAD start-up the (preloaded) add-on writes this file into the
   "layer" folder, puts that folder at the head of ArchiCAD's PYTHONPATH and sets
   TAPIR_TRANSLATOR_DIR. Tapir runs every script with "uv run --script <script.py>"
   and keeps this PYTHONPATH, so Python's "site" machinery imports this
   "sitecustomize" module by itself at interpreter start-up, BEFORE the script runs.
   The script is never modified and Tapir only starts one process.
   If another sitecustomize.py exists further down sys.path (the machine's own Python
   environment), it is executed afterwards (see _chain) so it is not shadowed.

2. Activation. The layer does nothing outside a Tapir script (no TAPIR_SCRIPT_ID
   environment variable, unless TAPIR_TRANSLATOR_FORCE=1). Nothing heavy is imported
   at start-up: a meta-path finder (_Hook) waits for the script to import tkinter
   and then patches tkinter.Tk.__init__ / destroy (_hook_up).

3. Tcl interpreter proxy. Every Tk root window gets a proxy (_Proxy) in place
   of its "tk" attribute (the Tcl interpreter). Every Tcl command goes through
   proxy.call(); the proxy translates only the DISPLAYED texts (widget -text, menu
   -label, window titles, notebook tabs, Treeview column headings, canvas texts,
   message boxes, file dialog titles, variables shown by a Label / Button...). It
   remembers the original of each displayed text, so values read back by the script
   (cget, StringVar.get, entrycget, wm title) are returned in the ORIGINAL language:
   the script's logic never sees the translation. Data (view names, lists, Treeview
   rows, Entry, Text, Combobox -values) is never touched.

4. Dictionaries. _Engine loads, from lowest to highest priority, the add-on's common
   dictionary (_common.json), the user's dictionary (_user.json) and the script's own
   dictionary (<tool>.json). Each one may hold multilingual "terms" rows and per-
   language "translations" tables. Entries with {placeholders} become regex templates;
   trailing punctuation, case and multi-line texts are handled as fall-backs. The
   source language of the script is the one chosen in the menu, else the one declared
   by the script's dictionary, else it is detected automatically by "voting".

5. Online translation. Texts unknown to every dictionary are sent in the background
   (worker thread, never in the Tk loop) by _Automatic to the local engine
   TapirTranslatorEngine.exe (HTTP on 127.0.0.1, started by the add-on), which calls
   the online translator chosen in the menu. The original text stays on screen until
   the answer arrives, then the translation appears by itself.

6. Cache. Online results are stored in cache\\<translator>\\<source>-<target>.json,
   shared by every script, so the next time the display is immediate. A dictionary
   always wins over the cache.

7. Corrections. corrections.json fixes wrong ArchiCAD terms produced by online
   translators (e.g. "story" -> "histoire" instead of "etage"). Corrections are applied
   when an online/cached translation is displayed, never to dictionary entries.

8. Live reload. Every _PERIOD_MS milliseconds (Tk "after" timer) the layer checks
   whether settings.json (written by the add-on menu), the dictionaries or
   corrections.json changed, and whether online translations arrived; if so every
   open window re-applies its texts (_Proxy.reapply), so the language
   changes without restarting the tool.

Every internal error is written to log.txt and never reaches the script.
"""

import os
import sys

LAYER_VERSION = "0.9.0"
_PERIOD_MS = 300            # Period (ms) of the live check: settings / dictionaries reload, arrived translations
_LOG_SIZE = 512 * 1024  # Beyond this size (bytes) the log file is deleted and starts again from zero

# Widget / menu options whose value is a displayed text to translate.
_TEXT_OPTIONS = ("-text", "-label")
# Placeholder names in dictionary templates ("{n} elements") that may only match numbers.
_NUMBER_NAMES = ("n", "nb", "nombre", "count", "i", "x", "total", "n1", "n2")
# Menu entry types accepted by "<menu> add <type>" / "<menu> insert <index> <type>".
_MENU_TYPES = ("command", "cascade", "checkbutton", "radiobutton", "separator")
# Tk widget classes (lower case, "ttk::x" normalised to "tx") whose -textvariable is only
# displayed (never typed in by the user), so the variable's value may be translated.
_DISPLAY_CLASSES = ("label", "button", "checkbutton", "radiobutton", "labelframe",
                      "message", "menubutton", "tlabel", "tbutton", "tcheckbutton",
                      "tradiobutton", "tlabelframe", "tmenubutton")
# Tk standard dialog commands (message box, file / directory / colour choosers).
_DIALOGS = ("tk_messageBox", "tk_getOpenFile", "tk_getSaveFile", "tk_chooseDirectory",
              "tk_chooseColor")
# Options of those dialogs whose value is a displayed text to translate.
_DIALOG_OPTIONS = ("-title", "-message", "-detail")


# =====================================================================================
#  Log file (log.txt)
# =====================================================================================

def _folder():
    """Return the Tapir Translator data folder (%APPDATA%\\Tapir Translator).

    The add-on passes it through the TAPIR_TRANSLATOR_DIR environment variable.
    Returns "" when the variable is missing (the layer then stays inactive).
    """
    return os.environ.get("TAPIR_TRANSLATOR_DIR", "")


def _log(message):
    """Append one timestamped line to log.txt in the data folder.

    Input: message (str). Output: none. The line contains the date, the process id
    and the message. This function never raises: logging must never disturb the
    translated script.
    """
    try:
        # Step 1: no data folder -> nowhere to write, do nothing.
        root = _folder()
        if not root:
            return
        path = os.path.join(root, "log.txt")
        # Step 2: keep the log small; delete it once it exceeds _LOG_SIZE.
        try:
            if os.path.getsize(path) > _LOG_SIZE:
                os.remove(path)
        except OSError:
            pass
        # Step 3: append the line (imported lazily to keep start-up light).
        import time
        with open(path, "a", encoding="utf-8") as f:
            f.write("%s [%d] %s\n" % (time.strftime("%Y-%m-%d %H:%M:%S"), os.getpid(), message))
    except Exception:
        pass


# =====================================================================================
#  Translation engine (dictionaries + settings)
# =====================================================================================

class _Engine:
    """Translation engine of one script process.

    It reads the settings (settings.json), the dictionaries and corrections.json,
    builds the lookup tables for the current output language and translates texts
    with them. Texts unknown to the dictionaries are delegated to the online
    translation helper (self.auto, an _Automatic). A single instance exists per
    process (module global _engine), shared by every Tk window.
    """

    def __init__(self, root):
        """Create an empty engine for the data folder 'root'.

        Nothing is read here: reload_if_changed() loads the files.
        """
        self.root = root
        self.tool = None         # Tool (script) name, computed lazily by _tool_name()
        self.language = ""          # Output language code; "" = original language, no translation
        self.collect = False     # True: record texts missing from the dictionaries (missing\<tool>.json)
        self.source_language = ""   # Source language of the script (menu choice or script dictionary), "" = unknown
        self.table = {}           # Original text -> translation (current output language)
        self.table_min = {}       # Same table with case-folded keys (case-insensitive fall-back)
        self.corrections = []     # (pattern on the original or None, pattern to replace, replacement)
        self.patterns = []         # (regex, template) for texts containing {placeholders}
        self.missing = {}       # Language -> {missing text: ""}
        self.missing_changed = False   # True when self.missing has entries not yet written to disk
        self.hash = None     # Dates/sizes of the files read, used to detect changes (live reload)
        self.cache = {}           # Original text -> displayed result, emptied on every reload
        self.input_ = ""          # Input language explicitly chosen in the menu ("" = not chosen)
        self.columns = {}        # Language -> set of texts known by the dictionaries ("terms" columns)
        self.votes = {}           # Language -> number of script texts recognised in that language
        self.seen_set = set()          # Texts already counted in the votes (each text votes once)
        self.detected = ""        # Automatically detected script language, frozen once found
        self.auto = _Automatic(root, self)   # Online translation through the local engine

    # --- files ------------------------------------------------------------------------
    def _tool_name(self):
        """Return the tool name used for the per-script dictionary and missing-text file.

        Priority: TAPIR_TRANSLATOR_TOOL environment variable, else the script file
        name without extension (sys.argv[0]), else "script". Computed once and cached.
        """
        if self.tool is None:
            name = os.environ.get("TAPIR_TRANSLATOR_TOOL", "")
            if not name:
                try:
                    name = os.path.splitext(os.path.basename(sys.argv[0]))[0]
                except Exception:
                    name = ""
            self.tool = name or "script"
        return self.tool

    def _files(self):
        """Return the list of watched files, in a fixed order.

        Order: settings.json, _common.json, _user.json, <tool>.json, corrections.json.
        The dictionaries are listed from the lowest to the highest priority.
        reload_if_changed() unpacks this list positionally.
        """
        tool = self._tool_name()
        dictionary = os.path.join(self.root, "dictionaries")
        # From the lowest to the highest priority
        return [os.path.join(self.root, "settings.json"),
                os.path.join(dictionary, "_common.json"),     # The add-on's dictionary (replaced at every version)
                os.path.join(dictionary, "_user.json"),      # The user's dictionary (never touched by the add-on)
                os.path.join(dictionary, tool + ".json"),    # The dictionary of this particular script
                os.path.join(self.root, "corrections.json")]   # Terms to fix in online translations

    def _compute_hash(self):
        """Return a fingerprint of the watched files.

        Output: tuple with one (modification time in ns, size) pair per file, or None
        for a missing file. Comparing two fingerprints tells whether anything changed.
        """
        hash_ = []
        for path in self._files():
            try:
                s = os.stat(path)
                hash_.append((s.st_mtime_ns, s.st_size))
            except OSError:
                hash_.append(None)
        return tuple(hash_)

    @staticmethod
    def _read_json(path):
        """Read a JSON file and return its content, or None.

        Returns None silently for a missing file; for an unreadable or invalid file
        the error is logged and None is returned. "utf-8-sig" accepts files saved with
        a BOM (e.g. by Notepad).
        """
        import json
        try:
            with open(path, "r", encoding="utf-8-sig") as f:
                return json.load(f)
        except FileNotFoundError:
            return None
        except Exception as e:
            _log("unreadable file %s: %r" % (path, e))
            return None

    def reload_if_changed(self):
        """Reload settings, dictionaries and corrections if any watched file changed.

        Output: True if the language or a translation may have changed (the caller
        must then re-apply the texts of the open windows), False if nothing changed.
        Called once at start-up and then every _PERIOD_MS by the Tk timer.
        """
        # Step 1: compare the file fingerprint with the last one; unchanged -> nothing to do.
        hash_ = self._compute_hash()
        if hash_ == self.hash:
            return False
        self.hash = hash_
        settings_f, common_f, user_f, tool_f, corrections_f = self._files()

        # Step 2: settings written by the add-on menu: output language ("original" means
        # no translation) and the "collect missing texts" switch.
        settings = self._read_json(settings_f) or {}
        language = str(settings.get("output", "") or "")
        if language in ("original", "origine"):	# "origine": versions <= 0.7.5
            language = ""
        self.collect = bool(settings.get("collect", False))

        # Step 3: read the three dictionaries; anything that is not a JSON object is ignored.
        common_ = self._read_json(common_f)
        user = self._read_json(user_f)
        tool = self._read_json(tool_f)
        common_ = common_ if isinstance(common_, dict) else {}
        user = user if isinstance(user, dict) else {}
        tool = tool if isinstance(tool, dict) else {}
        # Step 4: source language of the script. Only the language declared by the script's
        # own dictionary counts: the common dictionary serves scripts written in any language.
        # Priority: input language chosen in the menu; else the one of the script's
        # dictionary; else (empty) automatic detection later on.
        input_ = str(settings.get("input", "") or "")
        source = input_ or str(tool.get("source_language", "") or "")
        self.input_ = input_

        # Step 5: build the lookup table, from the lowest to the highest priority: common
        # (add-on), user (user), script dictionary; in each one, "terms" first, then
        # "translations" (so later entries overwrite earlier ones). Empty translations are skipped.
        table = {}
        if language:
            for d in (common_, user, tool):
                table.update(self._from_terms(d.get("terms"), language, source, bool(input_)))
                trans = d.get("translations", {})
                if isinstance(trans, dict) and isinstance(trans.get(language), dict):
                    for k, v in trans[language].items():
                        if isinstance(k, str) and isinstance(v, str) and v != "":
                            table[k] = v
        self.source_language = source
        # Step 6: requested language == script language -> nothing to translate.
        if language and source and language == source:
            language = ""          # The requested language is the script's own language
        if language != self.language:
            _log("tool=%s language=%s (source=%s) collect=%s" %
                     (self._tool_name(), language or "original", source or "?", self.collect))
        self.language = language
        self.table = table if language else {}
        # Step 7: case-insensitive fall-back table ("CLOSE", "close" -> "CLOSE", "close");
        # templates and 1-character keys are excluded; the first key wins on a collision.
        self.table_min = {}
        for k, v in self.table.items():
            if len(k) >= 2 and "{" not in k:
                self.table_min.setdefault(k.casefold(), v)
        # Step 8: compile the {placeholder} templates and the term corrections, and empty
        # the result cache (every text will be translated again with the new tables).
        self.patterns = self._compile_patterns(self.table)
        self.corrections = self._compile_corrections(self._read_json(corrections_f), language) if language else []
        self.cache = {}

        # Step 9: dictionary columns: the texts known per language, used to recognise the
        # script's language (automatic detection by votes). Templates and "_" keys are skipped.
        self.columns = {}
        for d in (common_, user, tool):
            for t in d.get("terms", []) if isinstance(d.get("terms"), list) else []:
                if isinstance(t, dict):
                    for code, text in t.items():
                        if isinstance(text, str) and not code.startswith("_") and "{" not in text:
                            self.columns.setdefault(code, set()).add(text)
        # Step 10: configure online translation: enabled only if the "engine" setting is on
        # AND a translation is requested. The engine port comes from the environment
        # (set by the add-on) or else from the settings; default translator "mymemory".
        port = os.environ.get("TAPIR_TRANSLATOR_ENGINE_PORT", "") or str(settings.get("port", "") or "")
        self.auto.configure_(bool(settings.get("online", False)) and bool(language), source, language, port,
                             str(settings.get("provider", "") or "mymemory"))
        return True

    # --- term corrections (corrections.json) ----------------------------------------------
    @staticmethod
    def _compile_corrections(data, language):
        """Compile the corrections.json rules of one output language.

        Input: data = parsed corrections.json, {"fr": [rule, ...], "de": [...], ...};
        language = output language code. A rule looks like:
             {"source": "story", "wrong": "histoire", "correct": "étage"}
             {"source": ["story", "stories"], "replace": {"histoire": "étage", "histoires": "étages"}}
             {"wrong": "Toutes les histoires", "correct": "Tous les étages"}
        "source" (optional): the rule only applies if the original text contains one of
        these words (whole words, case-insensitive).
        In fr, es and pt, a single-word "wrong"/"correct" pair also covers the -s plural.
        Output: list of (source regex or None, regex of the wrong term, correct term),
        longest patterns first so that longer expressions are replaced before their parts.
        """
        import re
        rules = []
        if not isinstance(data, dict) or not isinstance(data.get(language), list):
            return rules
        for r in data[language]:
            if not isinstance(r, dict):
                continue
            # Step 1: collect the (wrong, correct) pairs of the rule, either from the
            # "replace" mapping or from "wrong"/"correct" (+ automatic -s plural).
            pairs = []
            if isinstance(r.get("replace"), dict):
                pairs = [(k, v) for k, v in r["replace"].items() if isinstance(k, str) and isinstance(v, str) and k]
            elif isinstance(r.get("wrong"), str) and isinstance(r.get("correct"), str) and r["wrong"]:
                pairs = [(r["wrong"], r["correct"])]
                m, c = r["wrong"], r["correct"]
                if language in ("fr", "es", "pt") and " " not in m and " " not in c and not m.endswith("s"):
                    pairs.append((m + "s", c + "s"))
            if not pairs:
                continue
            # Step 2: optional condition on the original text ("source": str or list of words).
            sources = r.get("source")
            if isinstance(sources, str):
                sources = [sources]
            source_pattern = None
            if isinstance(sources, list) and sources:
                words = [re.escape(s) for s in sources if isinstance(s, str) and s]
                if words:
                    source_pattern = re.compile(r"(?<!\w)(?:%s)(?!\w)" % "|".join(words), re.I)
            # Step 3: one whole-word, case-insensitive regex per pair; the longest expressions
            # first ("toutes les histoires" before "histoires")
            for wrong, correct in sorted(pairs, key=lambda p: -len(p[0])):
                rules.append((source_pattern, re.compile(r"(?<!\w)%s(?!\w)" % re.escape(wrong), re.I), correct))
        # Step 4: global order across all rules: longest pattern first.
        rules.sort(key=lambda r: -len(r[1].pattern))
        return rules

    @staticmethod
    def _same_case(pattern, text):
        """Give 'text' the same case style as 'pattern' (the matched wrong term).

        All upper case (2+ characters) -> upper case; initial capital -> initial
        capital; otherwise 'text' is returned unchanged. Used by apply_corrections().
        """
        if pattern.isupper() and len(pattern) > 1:
            return text.upper()
        if pattern[:1].isupper():
            return text[:1].upper() + text[1:]
        return text

    def apply_corrections(self, original, translated):
        """Apply corrections.json to an online translation (never to dictionary entries).

        Inputs: original = the source text (used by the "source" conditions),
        translated = the online/cached translation. Output: the corrected translation.
        Each replacement keeps the case style of the replaced word.
        """
        if not self.corrections or not isinstance(translated, str) or not translated:
            return translated
        for source_pattern, regex_text, correct in self.corrections:
            # Skip the rule if its "source" words are not in the original text.
            if source_pattern is not None and not source_pattern.search(original):
                continue
            translated = regex_text.sub(lambda m, c=correct: self._same_case(m.group(0), c), translated)
        return translated

    def _vote(self, text):
        """Count in which language the script's texts are written (automatic detection).

        Every dictionary language whose column contains 'text' gets one vote.
        """
        for code, texts in self.columns.items():
            if text in texts:
                self.votes[code] = self.votes.get(code, 0) + 1

    def detected_language(self):
        """Return the automatically detected script language ("" while unknown).

        The result is frozen as soon as it is recognised without ambiguity: otherwise a
        change of mind would send the already translated texts to another language pair.
        """
        if self.detected or not self.votes:
            return self.detected
        ranking = sorted(self.votes.items(), key=lambda kv: -kv[1])
        first = ranking[0]
        second = ranking[1][1] if len(ranking) > 1 else 0
        # At least 2 recognised texts and a clear lead (identical words count for
        # several languages: "Options", "Zone"...)
        if first[1] >= 2 and first[1] > second:
            self.detected = first[0]
            _log("script language detected: %s (%s)" % (self.detected, dict(ranking)))
        return self.detected

    @staticmethod
    def _from_terms(terms, language, source, single=False):
        """Build an {original: translation} table from a multilingual "terms" list.

        Format: "terms": [{"en": "Close", "fr": "Fermer", ...}, ...]; 'language' is the
        output language, 'source' the script language ("" if unknown).
        single=True (input language chosen in the menu): only the source column is read.
        Otherwise every text of every language gives the translation into the target
        language, whatever the script's language; on a duplicate (same text in two
        notions), the script's declared language comes first, then the column order.
        """
        res = {}
        if not isinstance(terms, list):
            return res
        # Step 1: list of source columns to read: the declared source first, then (unless
        # single) every other language code found in the rows ("_" keys are comments).
        languages = []
        if source:
            languages.append(source)
        if not single:
            for t in terms:
                if isinstance(t, dict):
                    for code in t:
                        if code not in languages and not code.startswith("_"):
                            languages.append(code)
        # Step 2: automatic detection: a text already valid in the target language is not
        # translated (a script written in English keeps "Surface", even though "Surface" (fr)
        # means "Area"). With a chosen input language, we know what the text is.
        already = set()
        if not single:
            for t in terms:
                if isinstance(t, dict) and isinstance(t.get(language), str):
                    already.add(t[language])
        # Step 3: map each text of each source column to the row's target text; the first
        # mapping found for a text wins (setdefault).
        for code in languages:
            if code == language:
                continue
            for t in terms:
                if not isinstance(t, dict):
                    continue
                target = t.get(language)
                text = t.get(code)
                if (isinstance(target, str) and target and isinstance(text, str) and text
                        and text != target and text not in already):
                    res.setdefault(text, target)
        return res

    def _case(self, text):
        """Case-insensitive dictionary look-up, adapting the case of the result.

        Output: the translation with the case style of 'text' (all caps, lower-case
        initial, capital initial), or None if the text is unknown.
        """
        t = self.table_min.get(text.casefold())
        if t is None:
            return None
        if text.isupper():
            return t.upper()
        # (German keeps the capital letter of its nouns)
        if text[:1].islower() and t[:1].isupper() and self.language != "de":
            return t[:1].lower() + t[1:]
        if text[:1].isupper() and t[:1].islower():
            return t[:1].upper() + t[1:]
        return t

    @staticmethod
    def _compile_patterns(table):
        """Compile the dictionary keys containing {placeholders} into regex templates.

        Input: the lookup table. Output: list of (compiled regex matching the whole
        text, translation template for str.format), longest patterns first.
        Example: "{n} elements selected" -> "^(?P<n>number) elements selected$"; the
        translation "{n} éléments sélectionnés" is then filled with the captured values.
        """
        import re
        patterns = []
        for key, val in table.items():
            if "{" not in key:
                continue
            # Step 1: placeholder names and the literal pieces between them.
            names = re.findall(r"\{([A-Za-z_][A-Za-z0-9_]*)\}", key)
            if not names:
                continue
            pieces = re.split(r"\{[A-Za-z_][A-Za-z0-9_]*\}", key)
            # Step 2: build the pattern: escaped literal, then a capture group per placeholder.
            regex_text = ""
            for i, m in enumerate(pieces):
                regex_text += re.escape(m)
                if i < len(names):
                    name = names[i]
                    if name in names[:i]:
                        # Same placeholder used twice: back-reference to the first capture.
                        regex_text += "(?P=%s)" % name
                    elif name in _NUMBER_NAMES:
                        # {n} = a number: "{n} elements" must not swallow
                        # "Only the selected elements" ("Only the selected éléments")
                        regex_text += r"(?P<%s>[-+]?\d+(?:[.,  ]\d+)*)" % name
                    else:
                        # {name} = a short text without line break (lazy, at most 80 characters)
                        regex_text += r"(?P<%s>[^\n]{1,80}?)" % name
            # Step 3: anchor the pattern on the whole text; an invalid pattern is skipped.
            try:
                patterns.append((re.compile("^" + regex_text + "$", re.S), val))
            except re.error:
                pass
        # Step 4: the longest (most precise) templates first
        patterns.sort(key=lambda m: -len(m[0].pattern))
        return patterns

    # --- translation ------------------------------------------------------------------
    def translate(self, text):
        """Translate one displayed text into the current output language.

        Input: any value (only non-blank str are translated). Output: the text to
        display; the original text itself when no translation is requested, when the
        text is unknown, or while an online translation is pending.
        Order: result cache -> dictionaries (_translate) -> text already valid in the
        output language -> online translation (self.auto) -> original text.
        """
        # Step 1: no output language, not a string, or blank -> unchanged.
        if not self.language or not isinstance(text, str) or not text.strip():
            return text
        # Step 2: result cache (emptied at every reload and when online translations arrive).
        r = self.cache.get(text)
        if r is not None:
            return r
        # Step 3: each new text votes once for the script's language (automatic detection).
        if text not in self.seen_set:
            self.seen_set.add(text)
            self._vote(text)
        # Step 4: dictionaries (exact, punctuation, templates, case, line by line).
        r = self._translate(text, True)
        if r is None and text.strip() in self.columns.get(self.language, ()):
            r = text    # Already valid in the output language ("Options", "Zone"...)
        if r is None:
            # Step 5: sentence outside the dictionaries: online translation through the local
            # engine (if enabled). While pending, the original text stays displayed and is not
            # cached: it will be translated again when the answer arrives.
            r, pending_ = self.auto.translate(text)
            if pending_:
                return text
            if r is None:
                # No translation at all: keep the original and record it as missing.
                r = text
                self._note_missing(text)
        # Step 6: remember the final result for this text.
        self.cache[text] = r
        return r

    def _translate(self, text, lines):
        """Dictionary-only translation of one text; returns None if unknown.

        Inputs: text = text to translate; lines = True to allow splitting a
        multi-line text and translating each line separately (False for the lines
        themselves, to avoid recursion). Missing lines are recorded via _note_missing.
        """
        # Step 1: exact match.
        t = self.table.get(text)
        if t is not None:
            return t
        # Step 2: surrounding spaces and trailing punctuation: "Nom :" -> "Name:" if "Nom" is known.
        # The core is looked up exactly, then as a template, then case-insensitively.
        core, before, after = self._split(text)
        if core != text and core:
            t = self.table.get(core)
            if t is None:
                t = self._pattern(core)
            if t is None:
                t = self._case(core)
            if t is not None:
                # Adapt the punctuation spacing to the output language.
                if self.language == "fr":
                    # French typography: a space before : ? ! ;
                    if after[:1] in (":", "?", "!", ";"):
                        after = " " + after
                else:
                    # Other languages: no-break spaces become normal spaces and the space
                    # before : ? ! ; is removed.
                    after = after.replace(" ", " ")
                    for p in (" :", " ?", " !", " ;"):
                        if after.startswith(p):
                            after = after[1:]
                            break
                return before + t + after
        # Step 3: whole text as a {placeholder} template.
        t = self._pattern(text)
        if t is not None:
            return t
        # Step 4: whole text, case-insensitive.
        t = self._case(text)
        if t is not None:
            return t
        # Step 5: multi-line text: translate line by line; succeed if at least one line
        # was translated (unknown lines stay as they are and are recorded as missing).
        if lines and "\n" in text:
            parties = text.split("\n")
            res = []
            change = False
            for p in parties:
                tp = self._translate(p, False) if p.strip() else None
                if tp is None:
                    res.append(p)
                    if p.strip():
                        self._note_missing(p)
                else:
                    res.append(tp)
                    change = True
            if change:
                return "\n".join(res)
        return None

    @staticmethod
    def _split(text):
        """Split a text into (core, leading part, trailing part).

        The leading part is the leading whitespace; the trailing part gathers the
        trailing whitespace, the final punctuation (: ? ! ; . and the ellipsis
        character), a final "..." and the spaces before them. Example:
        "  Name :" -> ("Name", "  ", " :"). Used to look up the bare core in the
        dictionaries and put the decoration back around its translation.
        """
        # Step 1: strip the leading and trailing whitespace.
        start = len(text) - len(text.lstrip())
        end_ = len(text.rstrip())
        core = text[start:end_]
        before = text[:start]
        after = text[end_:]
        # Step 2: move the final punctuation characters one by one into the trailing part,
        # stopping at ".." so that a "..." ending is handled as a whole below.
        while core and core[-1] in ":?!;.…" and not core.endswith(".."):
            # (the ".." test keeps the "..." of menu entries that open a dialog together)
            after = core[-1] + after
            core = core[:-1]
        # Step 3: a final "..." goes to the trailing part as a whole.
        if core.endswith("..."):
            after = "..." + after
            core = core[:-3]
        # Step 4: spaces left between the core and the punctuation ("Nom :") also go to
        # the trailing part.
        space = len(core) - len(core.rstrip())
        if space:
            after = core[len(core) - space:] + after
            core = core.rstrip()
        return core, before, after

    def _pattern(self, text):
        """Translate 'text' with the first {placeholder} template that matches it.

        Output: the template filled with the captured values, or None if no template
        matches (or if filling the first matching template fails).
        """
        for regex, template in self.patterns:
            m = regex.match(text)
            if m:
                try:
                    return template.format(**m.groupdict())
                except Exception:
                    return None
        return None

    # --- missing texts ----------------------------------------------------------------
    def _note_missing(self, text):
        """Record a text missing from the dictionaries (only when collection is enabled).

        Texts without any letter or longer than 2000 characters are ignored. The texts
        are kept in memory and written later by write_missing().
        """
        if not self.collect or not self.language:
            return
        if not any(c.isalpha() for c in text) or len(text) > 2000:
            return
        d = self.missing.setdefault(self.language, {})
        if text not in d:
            d[text] = ""
            self.missing_changed = True

    def write_missing(self):
        """Write the recorded missing texts to missing\\<tool>.json.

        The file has the same shape as a script dictionary ("source_language" and
        "translations": {language: {text: ""}}) so the user can fill it in and use it
        as a dictionary. Existing entries are kept (merge). Called by the Tk timer, when
        a window is destroyed and at process exit; does nothing if nothing new.
        """
        if not self.missing_changed:
            return
        import json
        self.missing_changed = False
        try:
            # Step 1: read the existing file (if any) to merge with it.
            folder = os.path.join(self.root, "missing")
            os.makedirs(folder, exist_ok=True)
            path = os.path.join(folder, self._tool_name() + ".json")
            existing = self._read_json(path)
            if not isinstance(existing, dict):
                existing = {}
            # Step 2: add the new texts with an empty translation, never overwriting one.
            existing.setdefault("source_language", self.source_language or "")
            trans = existing.setdefault("translations", {})
            for language, texts in self.missing.items():
                target = trans.setdefault(language, {})
                for t in texts:
                    target.setdefault(t, "")
            # Step 3: atomic write (temporary file, then replace).
            tmp = path + ".tmp"
            with open(tmp, "w", encoding="utf-8") as f:
                json.dump(existing, f, ensure_ascii=False, indent=2, sort_keys=True)
            os.replace(tmp, path)
        except Exception as e:
            _log("cannot write the missing texts: %r" % e)


class _Automatic(object):
    """Online translation of the texts the dictionaries do not know.

    The texts are sent in the background (separate worker thread, never in the Tk
    loop) to the local engine TapirTranslatorEngine.exe (HTTP on 127.0.0.1, started by
    the add-on), which forwards them to the online translator chosen in the menu
    (DeepL, Microsoft, Google, MyMemory, LibreTranslate). The original text stays
    displayed while waiting, then the translation appears by itself (the Tk timer asks
    to_reapply() and re-applies the texts).
    Results are kept in cache\\<translator>\\<source>-<target>.json, shared by every
    script: the next time, the display is immediate. A dictionary always wins over
    the online translation.

    Threading: the Tk thread calls translate() / to_reapply(); the worker thread
    runs _work(). Shared state is protected by self.lock.
    """

    LOT = 16                  # Maximum number of texts per request to the engine
    OUTAGE_PAUSE = 15.0        # Engine/service unavailable: new attempt after this delay (s)
    DETECTION_DELAY = 2.0     # Script language unknown: wait this long before sending without a language (s)

    def __init__(self, root, engine):
        """Create the helper (inactive until configure_() enables it).

        Inputs: root = data folder; engine = the owning _Engine (used for language
        detection and term corrections). The worker thread is started lazily.
        """
        import threading
        self.root = root
        self.engine = engine
        self.active_ = False        # Online translation enabled
        self.source = ""          # Source language ("" = use the detected language)
        self.target = ""           # Target language
        self.port = "8779"        # Port of the local engine
        self.lock = threading.Lock()       # Protects the state shared with the worker thread
        self.event = threading.Event()   # Set when texts wait in self.pending
        self.memory = {}         # (translator, source, target) -> {text: translation, "" = failure}
        self.pending = []         # [(translator, source, target, text)] waiting to be sent
        self.requests = set()     # Requests already in the queue or in progress
        self.modifies = set()     # (translator, source, target) caches to save to disk
        self.new = False      # New translations have arrived (display to recompute)
        self.outage_until = 0.0   # time.time() until which no new request is made (outage)
        self.outage_reported = False   # The current outage has already been logged
        self.provider = "mymemory"  # Online translator chosen in the menu
        self.outage_failures = False   # Texts were refused during the outage (retry when it ends)
        self.detection_start = None    # time.time() of the first text sent while the language was unknown
        self.awaiting_detection = False  # Texts are waiting for the language detection
        self.thread_ = None           # Worker thread (created on first activation)

    def configure_(self, active_, source, target, port, provider="mymemory"):
        """Apply the current settings (called by _Engine.reload_if_changed).

        Inputs: active_ = online translation on/off, source / target = language codes,
        port = engine port ("" keeps the current one), provider = translator name.
        Starts the daemon worker thread the first time the helper is enabled.
        """
        self.active_ = active_
        self.source = source
        self.target = target
        self.provider = provider or "mymemory"
        if port:
            self.port = port
        if active_ and self.thread_ is None:
            import threading
            self.thread_ = threading.Thread(target=self._work, name="TapirTranslatorAuto", daemon=True)
            self.thread_.start()

    @staticmethod
    def translatable(text):
        """Tell whether a text may be sent to the online translator.

        Data displayed in Labels (paths, file names, identifiers, codes) is never sent.
        Output: False for texts shorter than 2 / longer than 600 characters or with
        fewer than 2 letters, for texts containing a path separator, URL, "@" or a known
        file extension, and for single words containing digits, "_" or an inner ".",
        or made of at most 6 capital letters (codes); True otherwise.
        """
        import re
        t = text.strip()
        # Step 1: length and letter count.
        if len(t) < 2 or len(t) > 600 or sum(1 for c in t if c.isalpha()) < 2:
            return False
        # Step 2: paths, URLs, e-mail addresses, file names.
        if re.search(r"[\\/]|://|@|\.(py|json|txt|csv|pln|pla|tpl|ifc|dwg|dxf|xlsx?|gsm|png|jpe?g|pdf)\b", t, re.I):
            return False
        # Step 3: single words that look like identifiers or codes.
        if " " not in t:
            if any(c.isdigit() for c in t) or "_" in t or "." in t.strip("."):
                return False
            if t.isupper() and len(t) <= 6:
                return False
        return True

    def _path(self, provider, source, target):
        """Return the cache file path cache\\<translator>\\<source or auto>-<target>.json."""
        # One cache per translator: switching translator gives back its own translations
        return os.path.join(self.root, "cache", provider, "%s-%s.json" % (source or "auto", target))

    def _load(self, key):
        """Load the cache file of 'key' = (translator, source, target) into self.memory.

        Done once per key; a missing or invalid file gives an empty table.
        Must be called with self.lock held.
        """
        if key in self.memory:
            return
        d = _Engine._read_json(self._path(*key))
        trans = d.get("translations") if isinstance(d, dict) else None
        self.memory[key] = dict(trans) if isinstance(trans, dict) else {}

    def translate(self, text):
        """Look up or request the online translation of one text (Tk thread).

        Output: (translation or None, pending_). pending_ = True means the text
        has been queued (or waits for the language detection): the caller displays
        the original for now and must not cache the result.
        (None, False) means "no online translation" (disabled, not translatable,
        same language, known failure, or engine in outage).
        """
        # Step 1: disabled, or data that must not leave the computer.
        if not self.active_ or not self.translatable(text):
            return None, False
        import time
        # Step 2: source language: from the settings, else the detected one.
        source = self.source or self.engine.detected_language()
        if not source:
            # Script language not recognised yet: wait a little for the other texts
            # (after DETECTION_DELAY the text is sent with an empty source, "auto").
            if self.detection_start is None:
                self.detection_start = time.time()
            if time.time() - self.detection_start < self.DETECTION_DELAY:
                self.awaiting_detection = True
                return None, True
        if source == self.target:
            return None, False
        key = (self.provider, source, self.target)
        with self.lock:
            # Step 3: already in the cache ("" = known failure -> None).
            self._load(key)
            t = self.memory[key].get(text)
            if t is not None:
                # Term corrections are applied at display time: a newly added correction
                # also applies to the translations already in the cache
                return (self.engine.apply_corrections(text, t) if t else None), False
            # Step 4: during an outage, do not queue; remember to retry when it ends.
            if time.time() < self.outage_until:
                self.outage_failures = True
                return None, False
            # Step 5: queue the request once and wake the worker thread.
            queued = (self.provider, source, self.target, text)
            if queued not in self.requests:
                self.requests.add(queued)
                self.pending.append(queued)
                self.event.set()
        return None, True

    def to_reapply(self):
        """Called by the Tk loop: True if the display must be recomputed.

        That is the case when new translations arrived, when an outage has ended
        (texts refused during it can now be requested), or when texts waiting for the
        language detection can now be sent (language found or delay elapsed).
        """
        import time
        r = False
        with self.lock:
            if self.new:
                self.new = False
                r = True
            if self.outage_failures and time.time() >= self.outage_until:
                self.outage_failures = False
                r = True
        if self.awaiting_detection and (self.engine.detected_language() or
                                      time.time() - (self.detection_start or 0) >= self.DETECTION_DELAY):
            self.awaiting_detection = False
            r = True
        return r

    def _work(self):
        """Worker thread loop: send the queued texts to the local engine.

        Protocol: POST http://127.0.0.1:<port>/translate with JSON
        {"provider", "source", "target", "texts": [...]}; the answer is
        {"translations": [...]} in the same order. HTTP 400 (language refused, unknown
        translator) is stored as a permanent failure; any other error (503: quota, key,
        service network; engine unreachable) starts an outage of OUTAGE_PAUSE seconds.
        """
        import json
        import time
        import urllib.error
        import urllib.request
        # No proxy for 127.0.0.1 (the computer's HTTP_PROXY must not be used)
        opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
        while True:
            # Step 1: wait for work, then take at most LOT texts from the queue.
            self.event.wait()
            time.sleep(0.05)   # Let the other texts of the same window arrive
            with self.lock:
                lot = self.pending[:self.LOT]
                del self.pending[:self.LOT]
                if not self.pending:
                    self.event.clear()
            # Step 2: group the texts by (translator, source, target): one request per group.
            groups = {}
            for provider, source, target, text in lot:
                groups.setdefault((provider, source, target), []).append(text)
            for (provider, source, target), texts in groups.items():
                key = (provider, source, target)
                try:
                    # Step 3: send the request and check that one translation came back per text
                    # (blank answers are stored as "" = failure).
                    body = json.dumps({"provider": provider, "source": source, "target": target,
                                        "texts": texts}, ensure_ascii=False).encode("utf-8")
                    request = urllib.request.Request(
                        "http://127.0.0.1:%s/translate" % self.port, data=body,
                        headers={"Content-Type": "application/json; charset=utf-8"})
                    with opener.open(request, timeout=120) as response:
                        res = json.loads(response.read().decode("utf-8"))
                    transs = res.get("translations") if isinstance(res, dict) else None
                    if not isinstance(transs, list) or len(transs) != len(texts):
                        raise ValueError("reponse incomplete")
                    results = [t if isinstance(t, str) and t.strip() else "" for t in transs]
                except Exception as e:
                    # Step 4: failure. The engine's JSON body may carry an "error" message.
                    detail = repr(e)
                    if isinstance(e, urllib.error.HTTPError):
                        try:
                            detail = json.loads(e.read().decode("utf-8")).get("error", detail)
                        except Exception:
                            pass
                        if e.code == 400:
                            # Language refused, unknown translator: failure stored, no new attempt
                            _log("translation refused (%s): %s" % (provider, detail))
                            with self.lock:
                                self._load(key)
                                for t in texts:
                                    self.memory[key][t] = ""
                                    self.requests.discard((provider, source, target, t))
                            continue
                    # 503 (quota, key, service network) or engine unreachable: the original text
                    # is kept, new attempt later; logged only once while the outage lasts
                    with self.lock:
                        for t in texts:
                            self.requests.discard((provider, source, target, t))
                        if not self.outage_reported:
                            self.outage_reported = True
                            _log("online translation unavailable (%s, port %s): %s; new attempt every %d s"
                                     % (provider, self.port, detail, int(self.OUTAGE_PAUSE)))
                        self.outage_until = time.time() + self.OUTAGE_PAUSE
                        self.outage_failures = True
                    continue
                # Step 5: success: end of outage (logged), store the results in memory, mark
                # the cache file to save and tell the Tk loop that new translations arrived.
                with self.lock:
                    if self.outage_reported:
                        self.outage_reported = False
                        _log("online translation restored (%s)" % provider)
                    self._load(key)
                    m = self.memory[key]
                    for t, r in zip(texts, results):
                        m[t] = r
                        self.requests.discard((provider, source, target, t))
                    self.modifies.add(key)
                    self.new = True
            # Step 6: save the modified caches to disk (outside the lock).
            self._save()

    def _save(self):
        """Write the modified in-memory caches to their JSON files (worker thread).

        Step 1 copies the modified tables under the lock; the files are then written
        without holding it. Each file is merged with its current content on disk and
        written atomically through a per-process temporary file.
        """
        import json
        with self.lock:
            pairs = list(self.modifies)
            self.modifies.clear()
            copies = {p: dict(self.memory.get(p, {})) for p in pairs}
        for (provider, source, target), translations in copies.items():
            try:
                path = self._path(provider, source, target)
                os.makedirs(os.path.dirname(path), exist_ok=True)
                # Step 2: another script may have written the same cache: merge (our values win)
                existing = _Engine._read_json(path)
                if isinstance(existing, dict) and isinstance(existing.get("translations"), dict):
                    fusion = dict(existing["translations"])
                    fusion.update(translations)
                    translations = fusion
                # Step 3: atomic write; the pid in the temporary name avoids clashes between processes.
                tmp = path + ".%d.tmp" % os.getpid()
                with open(tmp, "w", encoding="utf-8") as f:
                    json.dump({"provider": provider, "source": source, "target": target,
                               "translations": translations},
                              f, ensure_ascii=False, indent=1, sort_keys=True)
                os.replace(tmp, path)
            except Exception as e:
                _log("translation cache not saved: %r" % e)


_engine = None          # The single _Engine of this process (created by _install)
_proxies = []   # One _Proxy per open Tk root window


# =====================================================================================
#  Proxy in front of the Tcl interpreter of a Tk window
# =====================================================================================

class _Proxy(object):
    """Proxy that replaces root.tk (the Tcl interpreter object of a Tk root window).

    tkinter sends every Tcl command through root.tk.call(...); this proxy inspects
    each command before it runs (_before), translates the displayed texts in its
    arguments and records (original, displayed) pairs in a registry, so that the
    values read back by the script are given back in the original language. Tcl
    variables shown by display widgets (-textvariable) are handled through the
    setvar/getvar methods. Everything that is not intercepted goes unchanged to the
    real interpreter (self._real).
    """

    def __init__(self, real):
        """Wrap the real Tcl interpreter 'real' (the original root.tk)."""
        self._real = real
        # Key -> [original text, displayed text]. The key is
        # (kind, widget path, sub-id, option), kind being "opt", "title", "menu",
        # "tab", "header" or "canvas".
        self._registry = {}
        self._variables_ = {}   # Name of a displayed Tcl variable -> [original, displayed]

    def __getattr__(self, name):
        """Forward every attribute not defined here to the real interpreter."""
        return getattr(self._real, name)

    # --- call ---------------------------------------------------------------------------
    def call(self, *args):
        """Run a Tcl command, translating its displayed texts on the way.

        Step 1: _before() may rewrite the arguments (translations) and may return a
        post-processing function. Step 2: the real command runs. Step 3: the
        post-processing function (if any) converts the result, e.g. gives back the
        original text of a cget. Interception errors are logged and ignored: the
        command then runs with its arguments as they are at that point.
        """
        # tkinter may pass the whole command as a single tuple argument.
        if len(args) == 1 and isinstance(args[0], tuple):
            args = args[0]
        after = None
        try:
            argv = list(args)
            after = self._before(argv)
            args = tuple(argv)
        except Exception as e:
            _log("interception of call: %r" % e)
            after = None
        res = self._real.call(*args)
        if after is not None:
            try:
                res = after(res)
            except Exception as e:
                _log("after call: %r" % e)
        return res

    def _tr(self, value):
        """Translate 'value' if it is a str, otherwise return it unchanged."""
        if isinstance(value, str):
            return _engine.translate(value)
        return value

    def _note(self, key, original, displayed):
        """Record in the registry that 'original' is displayed as 'displayed' at 'key'."""
        if isinstance(original, str):
            self._registry[key] = [original, displayed]

    def _render(self, key, res):
        """Convert a value read from Tcl back to the original text.

        If 'res' (str or Tcl object compared through str()) equals the text displayed
        at 'key', the recorded original is returned; otherwise 'res' unchanged.
        """
        e = self._registry.get(key)
        if e is not None and isinstance(res, str) and res == e[1]:
            return e[0]
        if e is not None and not isinstance(res, str) and str(res) == e[1]:
            return e[0]
        return res

    def _options(self, argv, start):
        """Translate the text options (-text, -label) of argv[start:] in place.

        argv is read as "-option value" pairs from index 'start'. Output: list of
        (option, original, displayed) for the options that were translated.
        """
        done = []
        i = start
        n = len(argv)
        while i + 1 < n:
            opt = argv[i]
            if isinstance(opt, str) and opt in _TEXT_OPTIONS and isinstance(argv[i + 1], str):
                original = argv[i + 1]
                displayed = self._tr(original)
                argv[i + 1] = displayed
                done.append((opt, original, displayed))
            i += 2
        return done

    def _text_variable(self, argv, start, class_name):
        """Handle a -textvariable option found in argv[start:] ("-option value" pairs).

        If the widget class 'class_name' only displays its variable (Label, Button...),
        the variable is bound (_bind_variable) and its value becomes translated.
        Variables of input widgets (Entry...) are never touched.
        """
        i = start
        n = len(argv)
        while i + 1 < n:
            if argv[i] == "-textvariable":
                name = str(argv[i + 1])
                if name and self._is_display(class_name):
                    self._bind_variable(name)
            i += 2

    @staticmethod
    def _is_display(class_name):
        """True if the Tk class 'class_name' is a display-only widget (_DISPLAY_CLASSES).

        Accepts creation commands ("label", "ttk::label") and winfo class names
        ("Label", "TLabel"): "ttk::x" is normalised to "tx", comparison is case-insensitive.
        """
        c = str(class_name).lower()
        if c.startswith("ttk::"):
            c = "t" + c[5:]
        return c in _DISPLAY_CLASSES

    def _bind_variable(self, name):
        """Start translating the Tcl variable 'name' (done once per variable).

        If the variable already holds a string, it is translated and the displayed
        value is written to Tcl directly (bypassing the proxy). Otherwise the entry is
        [None, None] and its value will be picked up by reapply().
        """
        if name in self._variables_:
            return
        try:
            val = self._real.globalgetvar(name)
        except Exception:
            val = None
        if isinstance(val, str):
            shown = self._tr(val)
            self._variables_[name] = [val, shown]
            if shown != val:
                self._real.globalsetvar(name, shown)
        else:
            self._variables_[name] = [None, None]

    def _before(self, argv):
        """Inspect a Tcl command before it runs; rewrite argv in place if needed.

        Input: argv = the command as a mutable list. Output: None, or a function that
        will receive the command's result and return the value given to the script.
        Handled: "wm title", standard dialogs, widget sub-commands (".path cmd ...")
        and widget creation ("class .path -option value ...").
        """
        n = len(argv)
        if n < 2 or not isinstance(argv[0], str):
            return None
        a0 = argv[0]
        a1 = argv[1] if isinstance(argv[1], str) else str(argv[1])

        # Case 1: window title ("wm title .path ?title?")
        if a0 == "wm" and a1 == "title" and n >= 3:
            path = str(argv[2])
            key = ("title", path, None, None)
            # Wm.title() queries with a 4th argument set to None
            if n >= 4 and argv[3] is not None:
                # Setting the title: translate and record it.
                original = argv[3]
                argv[3] = self._tr(original)
                self._note(key, original, argv[3])
                return None
            # Reading the title: give back the original.
            return lambda r: self._render(key, r)

        # Case 2: message boxes and file dialogs (-title, -message, -detail); nothing recorded
        if a0 in _DIALOGS:
            i = 1
            while i + 1 < n:
                if argv[i] in _DIALOG_OPTIONS:
                    argv[i + 1] = self._tr(argv[i + 1])
                i += 2
            return None

        # Case 3: command of an existing widget: ".path sub-command ..."
        if a0.startswith("."):
            return self._sub_command(argv, a0, a1, n)

        # Case 4: creation of a widget: "class .path -option value ..."
        if a1.startswith("."):
            path = a1
            for opt, original, displayed in self._options(argv, 2):
                self._note(("opt", path, None, opt), original, displayed)
            self._text_variable(argv, 2, a0)
        return None

    def _sub_command(self, argv, path, sub, n):
        """Handle ".path sub-command ..." for the sub-commands that carry displayed texts.

        Inputs: argv = command (mutable), path = widget path, sub = sub-command,
        n = len(argv). Output: None or a result post-processing function (see _before).
        Covers configure / cget, menu add / insert / entryconfigure / entrycget,
        ttk.Notebook add / tab, ttk.Treeview heading, canvas create text /
        itemconfigure / itemcget.
        """
        # --- widget options: configure / cget
        if sub == "configure":
            if n >= 4:
                # Setting options: translate them, and handle a -textvariable (the widget
                # class is asked to Tcl to know if it is a display widget).
                for opt, original, displayed in self._options(argv, 2):
                    self._note(("opt", path, None, opt), original, displayed)
                if "-textvariable" in argv[2::2]:
                    try:
                        class_name = self._real.call("winfo", "class", path)
                    except Exception:
                        class_name = ""
                    self._text_variable(argv, 2, class_name)
            elif n == 3 and argv[2] in _TEXT_OPTIONS:
                # "configure -text" (query) returns a 5-item description: give back the original value.
                key = ("opt", path, None, argv[2])

                def render(r, key=key):
                    # r = (option, database name, class, default, value)
                    try:
                        if isinstance(r, tuple) and len(r) == 5:
                            return r[:4] + (self._render(key, r[4]),)
                    except Exception:
                        pass
                    return r
                return render
            return None

        if sub == "cget" and n == 3 and argv[2] in _TEXT_OPTIONS:
            # The script reads its own text back: give back the original.
            key = ("opt", path, None, argv[2])
            return lambda r: self._render(key, r)

        # --- menus: add / insert / entryconfigure / entrycget
        # Menu entries are recorded by their numeric index.
        if sub == "add" and n >= 3 and argv[2] in _MENU_TYPES:
            done = self._options(argv, 3)
            if not done:
                return None

            def note(r, done=done):
                # After the add, the new entry is the last one ("end").
                idx = self._real.call(path, "index", "end")
                for opt, original, displayed in done:
                    self._note(("menu", path, str(idx), opt), original, displayed)
                return r
            return note
        if sub == "insert" and n >= 4 and argv[3] in _MENU_TYPES:
            done = self._options(argv, 4)
            if not done:
                return None
            position = argv[2]

            def note(r, done=done, position=position):
                # After the insert, the new entry sits at the requested position.
                idx = self._real.call(path, "index", position)
                for opt, original, displayed in done:
                    self._note(("menu", path, str(idx), opt), original, displayed)
                return r
            return note
        if sub == "entryconfigure" and n >= 5:
            # The entry may be given as a label or pattern: resolve it to a numeric index.
            try:
                idx = str(self._real.call(path, "index", argv[2]))
            except Exception:
                idx = str(argv[2])
            for opt, original, displayed in self._options(argv, 3):
                self._note(("menu", path, idx, opt), original, displayed)
            return None
        if sub == "entrycget" and n == 4 and argv[3] in _TEXT_OPTIONS:
            try:
                idx = str(self._real.call(path, "index", argv[2]))
            except Exception:
                idx = str(argv[2])
            key = ("menu", path, idx, argv[3])
            return lambda r: self._render(key, r)

        # --- ttk.Notebook tabs: add .child -text ... / tab id -text ...
        # (a tab is designated by its window or by its number: the number is recorded)
        if sub == "add" and n >= 3 and str(argv[2]).startswith("."):
            child = argv[2]
            done = self._options(argv, 3)
            if not done:
                return None

            def note(r, done=done, child=child):
                # After the add, ask the notebook for the tab number of the child window.
                idx = str(self._real.call(path, "index", child))
                for opt, original, displayed in done:
                    self._note(("tab", path, idx, opt), original, displayed)
                return r
            return note
        if sub == "tab" and n >= 4:
            try:
                tab = str(self._real.call(path, "index", argv[2]))
            except Exception:
                tab = str(argv[2])
            # "tab id -text" alone is a query (give back the original); otherwise options are set.
            if n == 4 and argv[3] in _TEXT_OPTIONS:
                key = ("tab", path, tab, argv[3])
                return lambda r: self._render(key, r)
            for opt, original, displayed in self._options(argv, 3):
                self._note(("tab", path, tab, opt), original, displayed)
            return None

        # --- ttk.Treeview column headings (the rows are data: never touched)
        if sub == "heading" and n >= 4:
            column = str(argv[2])
            # "heading col -text" alone is a query; otherwise options are set.
            if n == 4 and argv[3] in _TEXT_OPTIONS:
                key = ("header", path, column, argv[3])
                return lambda r: self._render(key, r)
            for opt, original, displayed in self._options(argv, 3):
                self._note(("header", path, column, opt), original, displayed)
            return None

        # --- canvas texts: create text x y -text ... / itemconfigure / itemcget
        if sub == "create" and n >= 3 and argv[2] == "text":
            # Coordinates may be negative ("-5"): the 1st option starts with "-letter"
            start = 3
            while start < n and not (isinstance(argv[start], str) and len(argv[start]) > 1
                                     and argv[start][0] == "-" and argv[start][1].isalpha()):
                start += 1
            done = self._options(argv, start)
            if not done:
                return None

            def note(r, done=done):
                # The result of "create" is the new item id, used as sub-id.
                for opt, original, displayed in done:
                    self._note(("canvas", path, str(r), opt), original, displayed)
                return r
            return note
        if sub == "itemconfigure" and n >= 5:
            item = str(argv[2])
            for opt, original, displayed in self._options(argv, 3):
                self._note(("canvas", path, item, opt), original, displayed)
            return None
        if sub == "itemcget" and n == 4 and argv[3] in _TEXT_OPTIONS:
            key = ("canvas", path, str(argv[2]), argv[3])
            return lambda r: self._render(key, r)
        return None

    # --- Tcl variables (StringVar...) ----------------------------------------------------
    def _write_variable(self, function, name, value, rest):
        """Write a Tcl variable, translating the value if the variable is displayed.

        Inputs: function = real setvar/globalsetvar, name = variable name, value =
        value set by the script, rest = extra arguments. For a bound variable the
        new original and displayed values are recorded, and the translation is written.
        """
        e = self._variables_.get(str(name))
        if e is not None and isinstance(value, str):
            shown = self._tr(value)
            e[0], e[1] = value, shown
            return function(name, shown, *rest)
        return function(name, value, *rest)

    def _read_variable(self, function, name, rest):
        """Read a Tcl variable; give back the original if the displayed translation is read.

        Inputs: function = real getvar/globalgetvar, name = variable name, rest = extra
        arguments. If the value differs from the recorded displayed text (changed by
        another way), it is returned unchanged.
        """
        r = function(name, *rest)
        e = self._variables_.get(str(name))
        if e is not None and e[1] is not None and isinstance(r, str) and r == e[1]:
            return e[0]
        return r

    def globalsetvar(self, name, value, *rest):
        """Intercepted interpreter method used by tkinter (Variable.set)."""
        return self._write_variable(self._real.globalsetvar, name, value, rest)

    def setvar(self, name, value, *rest):
        """Intercepted interpreter method (Misc.setvar)."""
        return self._write_variable(self._real.setvar, name, value, rest)

    def globalgetvar(self, name, *rest):
        """Intercepted interpreter method used by tkinter (StringVar.get)."""
        return self._read_variable(self._real.globalgetvar, name, rest)

    def getvar(self, name, *rest):
        """Intercepted interpreter method (Misc.getvar)."""
        return self._read_variable(self._real.getvar, name, rest)

    # --- live language change -------------------------------------------------------------
    def reapply(self):
        """Re-translate every recorded text and variable of this window (live update).

        Called by the Tk timer after a settings / dictionary change or when online
        translations arrived. For each registry entry: compute the new translation of
        the original; if it differs from what we displayed, check that the widget still
        shows our text (otherwise the script changed it by another way and the entry is
        dropped) and write the new text. Entries of destroyed widgets are dropped.
        """
        real = self._real
        # Part 1: widget texts recorded in the registry.
        for key in list(self._registry.keys()):
            # Step 1: new translation of the original; unchanged -> nothing to do.
            original, old = self._registry[key]
            new = _engine.translate(original)
            if new == old:
                continue
            kind, path, sub_id, opt = key
            try:
                # Step 2: read the text currently displayed, with the real interpreter
                # (no translation / restoration on the way).
                if kind == "opt":
                    current = real.call(path, "cget", opt)
                elif kind == "title":
                    current = real.call("wm", "title", path)
                elif kind == "menu":
                    current = real.call(path, "entrycget", sub_id, opt)
                elif kind == "tab":
                    current = real.call(path, "tab", sub_id, opt)
                elif kind == "header":
                    current = real.call(path, "heading", sub_id, opt)
                elif kind == "canvas":
                    current = real.call(path, "itemcget", sub_id, opt)
                else:
                    continue
                if str(current) != old:
                    # The text changed without going through us: do not overwrite it
                    del self._registry[key]
                    continue
                # Step 3: write the new translation with the matching set command.
                if kind == "opt":
                    real.call(path, "configure", opt, new)
                elif kind == "title":
                    real.call("wm", "title", path, new)
                elif kind == "menu":
                    real.call(path, "entryconfigure", sub_id, opt, new)
                elif kind == "tab":
                    real.call(path, "tab", sub_id, opt, new)
                elif kind == "header":
                    real.call(path, "heading", sub_id, opt, new)
                elif kind == "canvas":
                    real.call(path, "itemconfigure", sub_id, opt, new)
                # Step 4: remember what is displayed now.
                self._registry[key][1] = new
            except Exception:
                # Widget destroyed, menu entry gone...
                self._registry.pop(key, None)
        # Part 2: displayed Tcl variables.
        for name in list(self._variables_.keys()):
            e = self._variables_[name]
            if e[0] is None:
                # Variable bound before it had a string value: take its current value
                # as the original (it is not translated yet).
                try:
                    val = real.globalgetvar(name)
                except Exception:
                    continue
                if isinstance(val, str):
                    e[0], e[1] = val, val
                else:
                    continue
            new = _engine.translate(e[0])
            if new == e[1]:
                continue
            # Only overwrite the variable if it still holds the text we displayed.
            try:
                if real.globalgetvar(name) == e[1]:
                    real.globalsetvar(name, new)
                    e[1] = new
            except Exception:
                self._variables_.pop(name, None)


# =====================================================================================
#  Hooking into tkinter
# =====================================================================================

def _watch(root):
    """Start the live-update timer on the Tk root window 'root'.

    Every _PERIOD_MS milliseconds, as long as the window lives, the timer reloads
    the settings and dictionaries if they changed, checks whether online translations
    arrived, re-applies the texts of every open window if needed and writes the
    missing texts. The timer re-arms itself with Tk's "after" (runs in the Tk thread).
    """
    def tic():
        """One timer tick (see _watch); errors are logged, the timer goes on."""
        try:
            # Step 1: settings / dictionaries / corrections changed?
            change = _engine.reload_if_changed()
            # Step 2: online translations arrived (or an outage / detection wait ended)?
            if _engine.auto.to_reapply():
                _engine.cache = {}   # Online translations have arrived: forget the cached originals
                change = True
            # Step 3: re-translate every open window.
            if change:
                for inter in list(_proxies):
                    inter.reapply()
            # Step 4: save the missing texts collected so far (no-op if nothing new).
            _engine.write_missing()
        except Exception as e:
            _log("watchdog: %r" % e)
        # Step 5: schedule the next tick (fails silently once the window is destroyed).
        try:
            root.after(_PERIOD_MS, tic)
        except Exception:
            pass
    try:
        root.after(_PERIOD_MS, tic)
    except Exception as e:
        _log("after failed: %r" % e)


def _hook_up(tkinter):
    """Patch the tkinter module so that every Tk root window gets a proxy.

    Input: the imported tkinter module. Done once per module (marker attribute
    _tapir_translator). Tk.__init__ is wrapped to replace self.tk by an
    _Proxy and start the live-update timer; Tk.destroy is wrapped to
    unregister the proxy and write the missing texts.
    """
    # Step 1: patch only once.
    if getattr(tkinter, "_tapir_translator", False):
        return
    tkinter._tapir_translator = True
    init_original = tkinter.Tk.__init__

    # Step 2: wrapped constructor: run the original, then insert the proxy.
    def init(self, *args, **kwargs):
        """Tk.__init__ replacement: original constructor, then proxy + timer."""
        init_original(self, *args, **kwargs)
        try:
            if not isinstance(self.tk, _Proxy):
                inter = _Proxy(self.tk)
                self.tk = inter
                _proxies.append(inter)
                _watch(self)
        except Exception as e:
            _log("branchement Tk : %r" % e)

    init.__wrapped__ = init_original
    tkinter.Tk.__init__ = init

    destroy_original = tkinter.Tk.destroy

    # Step 3: wrapped destroy: forget the proxy, save the missing texts, then destroy.
    def destroy(self):
        """Tk.destroy replacement: unregister the proxy, write missing texts, destroy."""
        inter = self.__dict__.get("tk")
        try:
            if isinstance(inter, _Proxy) and inter in _proxies:
                _proxies.remove(inter)
            _engine.write_missing()
        except Exception:
            pass
        return destroy_original(self)

    tkinter.Tk.destroy = destroy
    _log("tkinter hooked (tool=%s, layer %s)" % (_engine._tool_name(), LAYER_VERSION))


class _Loader(object):
    """Import loader wrapper used for the tkinter module.

    It delegates to the real loader, and once the module has been executed it calls
    _hook_up() on it, so tkinter is patched before the script can use it.
    """

    def __init__(self, loader):
        """Wrap the real loader 'loader' found by the standard PathFinder."""
        self._loader = loader

    def create_module(self, spec):
        """Delegate module creation to the real loader."""
        return self._loader.create_module(spec)

    def exec_module(self, module):
        """Execute the module with the real loader, then patch it (errors are logged)."""
        self._loader.exec_module(module)
        try:
            _hook_up(module)
        except Exception as e:
            _log("branchement : %r" % e)

    def __getattr__(self, name):
        """Forward every other loader attribute to the real loader."""
        return getattr(self._loader, name)


class _Hook(object):
    """Meta-path finder that waits for the import of tkinter to patch it.

    Nothing is imported in advance: start-up stays light and scripts without a
    window are not affected.
    """

    def find_spec(self, name, path=None, target=None):
        """Return the tkinter spec with a wrapping loader; None for any other module.

        The spec is found with the standard PathFinder, then its loader is replaced
        by a _Loader. Returning None lets the normal import machinery proceed.
        """
        if name != "tkinter":
            return None
        try:
            from importlib.machinery import PathFinder
            spec = PathFinder.find_spec(name, path)
            if spec is None or spec.loader is None:
                return None
            spec.loader = _Loader(spec.loader)
            return spec
        except Exception as e:
            _log("crochet : %r" % e)
            return None


def _install():
    """Create the engine, load the files and hook tkinter.

    Step 1: create the global _engine and load settings / dictionaries.
    Step 2: if tkinter is already imported, patch it now; otherwise install the
    import hook at the head of sys.meta_path.
    Step 3: write the missing texts at process exit.
    """
    global _engine
    root = _folder()
    _engine = _Engine(root)
    _engine.reload_if_changed()
    if "tkinter" in sys.modules:
        _hook_up(sys.modules["tkinter"])
    else:
        sys.meta_path.insert(0, _Hook())
    import atexit
    atexit.register(lambda: _engine.write_missing())


def _chain():
    """Run another sitecustomize.py (from the computer's Python environment), if any.

    Python only imports the first "sitecustomize" module found on sys.path, i.e. this
    one: without this chaining, an existing sitecustomize further down sys.path would
    be silently skipped. The first sitecustomize.py found in another sys.path folder
    is executed under the module name "_sitecustomize_next"; errors are logged.
    """
    try:
        # Step 1: our own folder, to skip it.
        here = os.path.normcase(os.path.abspath(os.path.dirname(__file__)))
        for p in list(sys.path):
            try:
                if not p or os.path.normcase(os.path.abspath(p)) == here:
                    continue
            except Exception:
                continue
            # Step 2: first other sitecustomize.py found: execute it and stop.
            candidate_ = os.path.join(p, "sitecustomize.py")
            if os.path.isfile(candidate_):
                import importlib.util
                spec = importlib.util.spec_from_file_location("_sitecustomize_suivant", candidate_)
                if spec and spec.loader:
                    module = importlib.util.module_from_spec(spec)
                    spec.loader.exec_module(module)
                return
    except Exception as e:
        _log("sitecustomize suivant : %r" % e)


def _start():
    """Entry point, run when Python imports this module at start-up.

    The layer is installed only inside a Tapir script (TAPIR_SCRIPT_ID set, or
    TAPIR_TRANSLATOR_FORCE=1 for tests) and when the data folder is known. Any other
    sitecustomize is chained in every case.
    """
    active_ = bool(os.environ.get("TAPIR_SCRIPT_ID")) or os.environ.get("TAPIR_TRANSLATOR_FORCE") == "1"
    if active_ and _folder():
        try:
            _install()
        except Exception as e:
            _log("installation failed: %r" % e)
    _chain()


# Executed at import time (Python's "site" module imports sitecustomize automatically).
_start()
