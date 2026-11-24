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
   the script's logic never sees the translation. Since 0.10.0 the rows of ttk.Treeview
   (item text and values), the items of a Listbox and the -values of a ttk.Combobox are
   translated too (scripts use them for result labels such as "16 found"); what the
   script reads back (item, set, get, cget -values, the combobox variable) is again the
   original. Texts typed by the user (Entry, Text, free text of a combobox) are never
   touched, and data such as paths, codes and numbers never leaves the computer.

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

9. Script UI pages. Tapir's ShowScriptUI command shows an HTML page written by the
   script in an ArchiCAD palette. The layer patches http.client / urllib3, rewrites the
   request that carries the page and puts a translation script at its head; a local
   server of the script's process answers the page (see "Script UI pages" below).

Every internal error is written to log.txt and never reaches the script.
"""

import os
import sys

LAYER_VERSION = "0.11.0"
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
        self.quiet = False        # True while looking up page script strings: nothing recorded as missing
        self.auto = _Automatic(root, self)   # Online translation through the local engine
        import threading
        # Tk thread and Script UI page server threads share the engine (see _poll_changes).
        self.lock = threading.RLock()

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
        Called once at start-up and then every _PERIOD_MS (see _poll_changes).
        """
        with self.lock:
            return self._reload_if_changed()

    def _reload_if_changed(self):
        """Body of reload_if_changed (self.lock held)."""
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
    def translate(self, text, low=False, request=True):
        """Translate one displayed text into the current output language.

        Input: any value (only non-blank str are translated); low = True for texts of
        secondary importance (detail rows of a Treeview), sent online after the others.
        Output: the text to display; the original text itself when no translation is
        requested, when the text is unknown, or while an online translation is pending.
        Order: result cache -> dictionaries (_translate) -> text already valid in the
        output language -> online translation (self.auto) -> original text.
        request=False: dictionaries and online translations already received only. No
        online request, no language vote, nothing recorded as missing and an unknown
        text is not cached. Used for the strings of a Script UI page's JavaScript, which
        may be identifiers or CSS classes rather than displayed texts.
        Thread-safe (self.lock): Script UI pages are translated from other threads.
        """
        if isinstance(text, str) and text.strip() in _project_data:
            return text     # A name from the ArchiCAD project: never translated
        with self.lock:
            if request:
                return self._translate_text(text, low)
            self.quiet = True
            try:
                return self._lookup_text(text)
            finally:
                self.quiet = False

    def _lookup_text(self, text):
        """translate(text, request=False) (self.lock held, self.quiet set)."""
        if not self.language or not isinstance(text, str) or not text.strip():
            return text
        r = self.cache.get(text)
        if r is not None:
            return r
        r = self._translate(text, True)
        if r is None:
            r = self.auto.cached(text)
        if r is None:
            return text
        self.cache[text] = r
        return r

    def _translate_text(self, text, low=False):
        """translate(text, low) with an online request when needed (self.lock held)."""
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
        # Step 4: dictionaries (exact, punctuation, templates, case, line by line). With
        # online translation, a multi-line or very long text (beyond what the online
        # translators accept) goes piece by piece instead: lines, then sentences, each one
        # from the dictionaries or online (_pieces).
        if self.auto.active_ and ("\n" in text or len(text) > _Automatic.MAX_TEXT):
            r = self._translate(text, False)
            if r is None:
                return self._pieces(text)
        else:
            r = self._translate(text, True)
        if r is None and text.strip() in self.columns.get(self.language, ()):
            r = text    # Already valid in the output language ("Options", "Zone"...)
        if r is None and self.auto.active_:
            # Step 4b: a sentence holding names of the ArchiCAD project: only the pieces
            # around the names go online, the names stay as they are.
            parts = _project_split(text)
            if parts is not None and len(parts) > 1:
                pending, out = False, []
                for piece, is_name in parts:
                    if is_name or not any(c.isalpha() for c in piece):
                        out.append(piece)
                        continue
                    t = self._translate_text(piece, low)
                    if t == piece and piece not in self.cache:
                        pending = True
                    out.append(t)
                result = "".join(out)
                if not pending:
                    self.cache[text] = result
                return result
        core = text.strip()
        if r is None and core != text and self.auto.active_:
            # Step 5a: online, only the core is sent (the translators drop the surrounding
            # spaces, e.g. the indentation of a line of a read-only Text panel).
            r = self._translate_text(core, low)
            if r == core and core not in self.cache:
                return text    # Waiting for the online translator: not cached
            r = text[:len(text) - len(text.lstrip())] + r + text[len(text.rstrip()):]
        if r is None:
            # Step 5: sentence outside the dictionaries: online translation through the local
            # engine (if enabled). While pending, the original text stays displayed and is not
            # cached: it will be translated again when the answer arrives.
            # A decorated text ("✔  Elements...", "Name:") is sent without its decoration,
            # which is put back around the translation, unless it was already translated as
            # a whole (cache of older versions).
            core, before, after = self._split(text)
            r = self.auto.lookup(text) if (before or after) and core else None
            if r is not None:
                pending_ = False
            elif (before or after) and core:
                r, pending_ = self.auto.translate(core, low)
                if r is not None:
                    r = self._attach(before, r, after)
            else:
                r, pending_ = self.auto.translate(text, low)
            if pending_:
                return text
            if r is None:
                # No translation at all: keep the original and record it as missing.
                r = text
                self._note_missing(text)
        # Step 6: remember the final result for this text.
        self.cache[text] = r
        return r

    def _pieces(self, text):
        """Translate a multi-line or very long text piece by piece (self.lock held).

        The lines, and the sentences of a line longer than _Automatic.MAX_TEXT, are
        translated one by one (_translate_text: dictionaries, then online); separators
        are kept. The result is cached only when no piece is still waiting for the online
        translator (it is computed again when the translations arrive).
        """
        import re
        limit = _Automatic.MAX_TEXT
        pending = False
        lines = []
        for line in text.split("\n"):
            chunks = re.split(r"((?<=[.;!?:])\s+)", line) if len(line) > limit else [line]
            out = []
            for k, chunk in enumerate(chunks):
                # Odd items are the separators kept by re.split; a sentence still too long stays.
                if k % 2 == 1 or not chunk.strip() or len(chunk) > limit:
                    out.append(chunk)
                    continue
                r = self._translate_text(chunk)
                if r == chunk and chunk not in self.cache:
                    pending = True   # Not cached: waiting for the online translator
                out.append(r)
            lines.append("".join(out))
        result = "\n".join(lines)
        if not pending:
            self.cache[text] = result
        return result

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
                return self._attach(before, t, after)
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

    def _attach(self, before, t, after):
        """Put the decoration split off by _split back around the translation 't'.

        The punctuation spacing follows the output language: French gets a space
        before : ? ! ; and the other languages lose it (no-break spaces become normal).
        """
        if self.language == "fr":
            if after[:1] in (":", "?", "!", ";"):
                after = " " + after
        else:
            after = after.replace(" ", " ")
            for p in (" :", " ?", " !", " ;"):
                if after.startswith(p):
                    after = after[1:]
                    break
        return before + t + after

    @staticmethod
    def _split(text):
        """Split a text into (core, leading part, trailing part).

        The leading part is the leading whitespace; the trailing part gathers the
        trailing whitespace, the final punctuation (: ? ! ; . and the ellipsis
        character), a final "..." and the spaces before them. Example:
        "  Name :" -> ("Name", "  ", " :"). Used to look up the bare core in the
        dictionaries and put the decoration back around its translation.
        """
        # Step 1: strip the leading and trailing whitespace, and the leading symbols that
        # only decorate the text ("✔  Elements...", "⚠ Zones...", "• item"); opening
        # brackets and quotes stay with the text.
        start = len(text) - len(text.lstrip())
        while start < len(text) and not text[start].isalnum() and text[start] not in "([{\"'«“‘¿¡<$€£#@":
            start += 1
        end_ = len(text.rstrip())
        if start > end_:
            start = end_
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
        if not self.collect or not self.language or self.quiet:
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
        with self.lock:
            self._write_missing()

    def _write_missing(self):
        """Body of write_missing (self.lock held)."""
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
    MAX_TEXT = 600            # Longest text sent at once (longer ones go by sentences, see _Engine._pieces)
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
        self.pending_low = []     # Same, sent after self.pending (Treeview detail rows)
        # (translator, source, target) -> {text with its numbers replaced by "#": (example
        # text, example translation)}: "16 found" is derived from a cached "24 found"
        self.number_patterns = {}
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
        # Texts still waiting for another output language or translator are no longer
        # displayed: drop them, so the texts of the new language are sent at once.
        # Both queues are filtered (main texts and secondary Treeview detail rows).
        with self.lock:
            for queue in (self.pending, self.pending_low):
                kept = [q for q in queue if q[0] == self.provider and q[2] == self.target]
                if len(kept) != len(queue):
                    for q in queue:
                        if q[0] != self.provider or q[2] != self.target:
                            self.requests.discard(q)
                    queue[:] = kept
            if not self.pending and not self.pending_low:
                self.event.clear()
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
        A "/" surrounded by spaces is a separator between words, not a path
        ("Check / Element ID" is a label): only "\\" or a "/" touching a character counts.
        """
        import re
        t = text.strip()
        # Step 1: length and letter count.
        if len(t) < 2 or len(t) > _Automatic.MAX_TEXT or sum(1 for c in t if c.isalpha()) < 2:
            return False
        # Step 2: paths, URLs, e-mail addresses, file names.
        if re.search(r"\\|\S/|/\S|://|@|\.(py|json|txt|csv|pln|pla|tpl|ifc|dwg|dxf|xlsx?|gsm|png|jpe?g|pdf)\b", t, re.I):
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
        for t, r in self.memory[key].items():
            self._learn_numbers(key, t, r)

    # --- texts with numbers: "16 found" derived from "24 found" ----------------------------
    _NUMBER = r"\d+(?:[.,]\d+)*"

    def _learn_numbers(self, key, text, translation):
        """Remember a translated text containing numbers as an example for its pattern.

        Only examples whose numbers all appear, in the same order, in the translation
        are kept, and never with the numbers 0 or 1 (their singular would be copied to
        the plural). Must be called with self.lock held (or before the worker starts).
        """
        import re
        if not translation or not any(c.isdigit() for c in text):
            return
        numbers = re.findall(self._NUMBER, text)
        if not numbers or any(n in ("0", "1") for n in numbers):
            return
        pos = 0
        for n in numbers:
            pos = translation.find(n, pos)
            if pos < 0:
                return
            pos += len(n)
        pattern = re.sub(self._NUMBER, "#", text)
        self.number_patterns.setdefault(key, {})[pattern] = (text, translation)

    def _derive(self, key, text):
        """Translation of 'text' derived from a cached example with the same pattern.

        "16 found" + example ("24 found" -> "24 résultats trouvés") gives
        "16 résultats trouvés". None when there is no usable example (or when 'text'
        contains 0 or 1). Must be called with self.lock held.
        """
        import re
        if not any(c.isdigit() for c in text):
            return None
        numbers = re.findall(self._NUMBER, text)
        if any(n in ("0", "1") for n in numbers):
            return None
        example = self.number_patterns.get(key, {}).get(re.sub(self._NUMBER, "#", text))
        if example is None:
            return None
        source_numbers = re.findall(self._NUMBER, example[0])
        if len(source_numbers) != len(numbers):
            return None
        # Replace the example's numbers, in order, by the text's numbers.
        out, pos = "", 0
        translation = example[1]
        for old, new in zip(source_numbers, numbers):
            i = translation.find(old, pos)
            if i < 0:
                return None
            out += translation[pos:i] + new
            pos = i + len(old)
        return out + translation[pos:]

    def _key(self):
        """(translator, source, target) of the current settings, or None when online
        translation cannot be used now (disabled, language unknown, same language)."""
        source = self.source or self.engine.detected_language()
        if not self.active_ or not source or source == self.target:
            return None
        return (self.provider, source, self.target)

    def lookup(self, text):
        """Cached online translation of 'text' (corrections applied), or None.

        Never queues anything: used for a decorated text that older versions sent and
        cached as a whole ("Name:"), before its bare core is requested.
        """
        key = self._key()
        if key is None or not self.translatable(text):
            return None
        with self.lock:
            self._load(key)
            t = self.memory[key].get(text)
        return self.engine.apply_corrections(text, t) if t else None

    def translate(self, text, low=False):
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
            # Step 3b: same text with other numbers already translated ("24 found").
            d = self._derive(key, text)
            if d is not None:
                return self.engine.apply_corrections(text, d), False
            # Step 4: during an outage, do not queue; remember to retry when it ends.
            if time.time() < self.outage_until:
                self.outage_failures = True
                return None, False
            # Step 5: queue the request once and wake the worker thread; secondary texts
            # (low) wait behind the others.
            queued = (self.provider, source, self.target, text)
            if queued not in self.requests:
                self.requests.add(queued)
                (self.pending_low if low else self.pending).append(queued)
                self.event.set()
        return None, True

    def cached(self, text):
        """Online translation of 'text' already received (cache), without any request.

        Output: the corrected translation, or None (disabled, not translatable, same
        language, unknown or known failure). Used for the strings of Script UI pages.
        """
        if not self.active_ or not self.translatable(text):
            return None
        source = self.source or self.engine.detected_language()
        if source == self.target:
            return None
        key = (self.provider, source, self.target)
        with self.lock:
            self._load(key)
            t = self.memory[key].get(text)
            if t is None:
                # Same text with other numbers already translated ("24 found")
                t = self._derive(key, text)
        return self.engine.apply_corrections(text, t) if t else None

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
                # Main texts first, then the secondary ones (Treeview detail rows).
                lot = self.pending[:self.LOT]
                del self.pending[:self.LOT]
                rest = self.LOT - len(lot)
                lot += self.pending_low[:rest]
                del self.pending_low[:rest]
                if not self.pending and not self.pending_low:
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
                        self._learn_numbers(key, t, r)
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
_revision = 0       # Increased each time the displayed texts must be recomputed (_poll_changes)
_tk_revision = 0    # Last revision applied to the Tk windows


def _poll_changes():
    """Reload the files if they changed and take the online translations that arrived.

    Output: the current revision number; it is increased when the displayed texts must
    be recomputed (language, dictionaries, corrections changed, or online translations
    arrived). Called by the Tk timer and by the Script UI page server: each one compares
    the revision with the last one it applied, so neither misses a change seen by the other.
    """
    global _revision
    with _engine.lock:
        change = _engine.reload_if_changed()
        if _engine.auto.to_reapply():
            _engine.cache = {}   # Online translations have arrived: forget the cached originals
            change = True
        if change:
            _revision += 1
        return _revision


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
        # Treeview item values: (tree path, item id) -> [original tuple, displayed tuple]
        self._tree_values = {}
        # Listbox items: listbox path -> {displayed text: original text}
        self._lists = {}
        # Combobox lists: combobox path -> {"values": [originals, displayed] or None,
        # "map": {displayed: original}}
        self._combos = {}
        # Tcl variable of a combobox (-textvariable) -> combobox path
        self._input_vars = {}
        # Tk class of each widget path met by a list command (cached "winfo class")
        self._classes = {}
        # Read-only Text widgets: path -> {displayed line or piece: original}
        self._texts = {}

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

    def _tr(self, value, low=False):
        """Translate 'value' if it is a str, otherwise return it unchanged."""
        if isinstance(value, str):
            return _engine.translate(value, low)
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
        # _tkinter stops a command at its first None argument (tkinter passes optional
        # arguments as None, e.g. Treeview.set): only the arguments before it count.
        n = next((i for i, a in enumerate(argv) if a is None), len(argv))
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
            # A combobox: its list of values and its variable (see _combo_options).
            if a0 == "ttk::combobox":
                self._classes[path] = "TCombobox"
                self._combo_options(argv, 2, n, path)
        return None

    # --- lists: ttk.Treeview rows, Listbox items, ttk.Combobox values ---------------------
    def _class(self, path):
        """Tk class of the widget 'path' ("Treeview", "Listbox", "TCombobox"...), cached."""
        c = self._classes.get(path)
        if c is None:
            try:
                c = str(self._real.call("winfo", "class", path))
            except Exception:
                c = ""
            self._classes[path] = c
        return c

    def _tr_list(self, value, low=False):
        """Translate every element of a Tcl list given as a string or a tuple.

        Output: (originals, displayed), two tuples of str of the same length. The
        displayed tuple is passed to Tcl as is (_tkinter turns it into a Tcl list).
        """
        if value is None or value == "":
            return (), ()
        originals = tuple(str(v) for v in self._real.splitlist(value))
        return originals, tuple(self._tr(v, low) for v in originals)

    def _as_tuple(self, value):
        """A Tcl list result (tuple, string or Tcl object) as a tuple of str."""
        try:
            return tuple(str(v) for v in self._real.splitlist(value))
        except Exception:
            return (str(value),)

    def _render_values(self, path, item, res):
        """Give back the original values of a Treeview item if 'res' is what we display."""
        e = self._tree_values.get((path, item))
        if e is not None and self._as_tuple(res) == e[1]:
            return e[0]
        return res

    def _column_index(self, path, column):
        """Index in the item values of the Treeview column 'column' (id, name or "#n").

        Returns None for the tree column "#0" or an unknown column.
        """
        try:
            cid = str(self._real.call(path, "column", column, "-id"))
            columns = self._as_tuple(self._real.call(path, "cget", "-columns"))
            return columns.index(cid) if cid in columns else None
        except Exception:
            return None

    def _treeview(self, argv, path, sub, n):
        """Handle the Treeview commands that carry row texts.

        insert parent index ?-id id? -text t -values {v ...}: translated; the item id
        (the command's result) keys what is recorded. item id ?-option value...?: set or
        query (-text, -values, or the whole description). set id ?column ?value??: one
        value or all of them. Output: None, a result converter, or False when the
        command is not one of those (the generic handling then continues).
        """
        if sub == "insert" and n >= 4:
            text = values = None
            # Rows under another row are details (element labels, layers...): they are
            # sent online after the main texts.
            low = str(argv[2]) != ""
            i = 4
            while i + 1 < n:
                if argv[i] == "-text" and isinstance(argv[i + 1], str):
                    original = argv[i + 1]
                    argv[i + 1] = self._tr(original, low)
                    text = (original, argv[i + 1])
                elif argv[i] == "-values":
                    values = self._tr_list(argv[i + 1], low)
                    argv[i + 1] = values[1]
                i += 2
            if text is None and values is None:
                return None

            def note(r, text=text, values=values):
                # The result of "insert" is the new item id.
                item = str(r)
                if text is not None:
                    self._note(("ttext", path, item, "-text"), text[0], text[1])
                if values is not None:
                    self._tree_values[(path, item)] = [values[0], values[1]]
                return r
            return note
        if sub == "item" and n >= 3:
            item = str(argv[2])
            if n == 3:
                # Whole description: "-text t -image ... -values {...} ...".
                def whole(r):
                    parts = list(self._real.splitlist(r))
                    for k in range(0, len(parts) - 1, 2):
                        if str(parts[k]) == "-text":
                            parts[k + 1] = self._render(("ttext", path, item, "-text"), parts[k + 1])
                        elif str(parts[k]) == "-values":
                            parts[k + 1] = self._render_values(path, item, parts[k + 1])
                    return tuple(parts)
                return whole
            if n == 4:
                if argv[3] == "-text":
                    key = ("ttext", path, item, "-text")
                    return lambda r: self._render(key, r)
                if argv[3] == "-values":
                    return lambda r: self._render_values(path, item, r)
                return None
            i = 3
            while i + 1 < n:
                if argv[i] == "-text" and isinstance(argv[i + 1], str):
                    original = argv[i + 1]
                    argv[i + 1] = self._tr(original)
                    self._note(("ttext", path, item, "-text"), original, argv[i + 1])
                elif argv[i] == "-values":
                    originals, displayed = self._tr_list(argv[i + 1])
                    argv[i + 1] = displayed
                    self._tree_values[(path, item)] = [originals, displayed]
                i += 2
            return None
        if sub == "set" and n >= 3:
            item = str(argv[2])
            if n == 3:
                # All values: "column value column value ...".
                def every(r):
                    e = self._tree_values.get((path, item))
                    parts = list(self._real.splitlist(r))
                    if e is not None:
                        for k in range(0, len(parts) - 1, 2):
                            idx = self._column_index(path, parts[k])
                            if idx is not None and idx < len(e[1]) and str(parts[k + 1]) == e[1][idx]:
                                parts[k + 1] = e[0][idx]
                    return tuple(parts)
                return every
            idx = self._column_index(path, argv[3])
            if n == 4:
                def one(r, idx=idx):
                    e = self._tree_values.get((path, item))
                    if e is not None and idx is not None and idx < len(e[1]) and str(r) == e[1][idx]:
                        return e[0][idx]
                    return r
                return one
            # Setting one value: translate it and update the recorded values.
            original = str(argv[4])
            argv[4] = self._tr(original)
            if idx is not None:
                e = self._tree_values.get((path, item))
                if e is None:
                    try:
                        current = self._as_tuple(self._real.call(path, "item", item, "-values"))
                    except Exception:
                        current = ()
                    e = [current, current]
                    self._tree_values[(path, item)] = e
                originals, displayed = list(e[0]), list(e[1])
                while len(originals) <= idx:
                    originals.append("")
                    displayed.append("")
                originals[idx], displayed[idx] = original, argv[4]
                e[0], e[1] = tuple(originals), tuple(displayed)
            return None
        return False

    def _listbox(self, argv, path, sub, n):
        """Handle Listbox insert (translate the items) and get (give back the originals).

        Items are matched by their displayed text (indexes move when items are
        inserted or deleted). Output: None, a converter, or False (not handled here).
        """
        if sub == "insert" and n >= 4:
            items = self._lists.setdefault(path, {})
            for k in range(3, n):
                original = str(argv[k])
                argv[k] = self._tr(original)
                items[argv[k]] = original
            return None
        if sub == "get" and n >= 3:
            items = self._lists.get(path, {})

            def originals(r):
                if isinstance(r, tuple):
                    return tuple(items.get(str(x), x) for x in r)
                return items.get(str(r), r)
            return originals
        return False

    def _combo_options(self, argv, start, n, path):
        """Translate the -values of a combobox in argv[start:n] and note its -textvariable."""
        combo = self._combos.setdefault(path, {"values": None, "map": {}})
        i = start
        while i + 1 < n:
            if argv[i] == "-values":
                originals, displayed = self._tr_list(argv[i + 1])
                argv[i + 1] = displayed
                combo["values"] = [originals, displayed]
                combo["map"] = {d: o for o, d in zip(originals, displayed)}
            elif argv[i] == "-textvariable" and argv[i + 1]:
                self._input_vars[str(argv[i + 1])] = path
            i += 2

    def _combobox(self, argv, path, sub, n):
        """Handle the combobox commands: configure -values, cget -values, get, set.

        get gives back the original of a value of the list (a text typed by the user is
        returned unchanged); set with a value of the list displays its translation.
        Output: None, a converter, or False (not handled here).
        """
        combo = self._combos.setdefault(path, {"values": None, "map": {}})
        if sub == "configure" and n >= 4:
            self._combo_options(argv, 2, n, path)
            return None
        if sub == "cget" and n == 3 and argv[2] == "-values":
            def values(r):
                v = combo["values"]
                return v[0] if v is not None and self._as_tuple(r) == v[1] else r
            return values
        if sub == "get" and n == 2:
            return lambda r: combo["map"].get(str(r), r)
        if sub == "set" and n == 3:
            v = combo["values"]
            if v is not None and str(argv[2]) in v[0]:
                argv[2] = v[1][v[0].index(str(argv[2]))]
            return None
        return False

    # --- Text widgets made read-only by the script ------------------------------------------
    # A Text widget is a typing area: the user's text is never touched. But a script that
    # writes its own text into it and then sets "-state disabled" uses it as a display
    # (result panels, help, logs): at that moment its content is translated, line by line
    # (piece by piece when a line mixes tags). "get" gives the originals back.
    _TEXT_MAX_LINES = 500     # Beyond, only the first lines are translated (big logs)

    def _text_widget(self, argv, path, sub, n):
        """Handle the Text commands: configure -state disabled (translate), get (originals).

        Output: None, a converter, or False (generic handling continues).
        """
        if sub == "configure" and n >= 4:
            for i in range(2, n - 1, 2):
                if argv[i] == "-state" and str(argv[i + 1]) == "disabled":
                    self._text_translate(path)
            return False
        if sub == "get" and n >= 3 and self._texts.get(path):
            shown = self._texts[path]

            def originals(r):
                if not isinstance(r, str):
                    return r
                return "\n".join(shown.get(line, line) for line in r.split("\n"))
            return originals
        return False

    def _text_runs(self, path, line):
        """Pieces of line 'line' of a Text widget with the same tags: [(start index, text, tags)]."""
        real = self._real
        start = "%d.0" % line
        current = set(str(t) for t in self._as_tuple(real.call(path, "tag", "names", start)))
        runs = []
        items = self._as_tuple(real.call(path, "dump", "-text", "-tag", start, "%d.end" % line))
        for k in range(0, len(items) - 2, 3):
            key, value, index = items[k], items[k + 1], items[k + 2]
            if key == "tagon":
                current.add(value)
            elif key == "tagoff":
                current.discard(value)
            elif key == "text" and value:
                tags = tuple(sorted(current))
                if runs and runs[-1][2] == tags:
                    runs[-1] = (runs[-1][0], runs[-1][1] + value, tags)
                else:
                    runs.append((index, value, tags))
        return runs

    def _text_translate(self, path, only_disabled=False):
        """Translate the content of the Text widget 'path' in place.

        Each line (or each piece of a line with its own tags) is replaced by the
        translation of its original; the tags are kept. Used when the script makes the
        widget read-only, and again by reapply() (language change, online arrivals) with
        only_disabled=True: a widget the script made editable again is left alone.
        """
        real = self._real
        disabled = str(real.call(path, "cget", "-state")) == "disabled"
        if only_disabled and not disabled:
            return
        if disabled:
            real.call(path, "configure", "-state", "normal")
        try:
            self._text_lines(path)
        finally:
            if disabled:
                real.call(path, "configure", "-state", "disabled")

    def _text_lines(self, path):
        """Body of _text_translate (the widget is in the normal state)."""
        real = self._real
        shown = self._texts.setdefault(path, {})
        if len(shown) > 5000:
            shown.clear()
        last = int(str(real.call(path, "index", "end")).split(".")[0]) - 1
        for line in range(1, min(last, self._TEXT_MAX_LINES) + 1):
            text = str(real.call(path, "get", "%d.0" % line, "%d.end" % line))
            if not any(c.isalpha() for c in text):
                continue
            runs = self._text_runs(path, line)
            if len(runs) <= 1:
                runs = [("%d.0" % line, text, runs[0][2] if runs else ())]
            # From the last piece to the first: earlier indexes stay valid.
            new_line = []
            for index, piece, tags in reversed(runs):
                original = shown.get(piece, piece)
                new = _engine.translate(original) if any(c.isalpha() for c in piece) else piece
                new_line.insert(0, new)
                if new != piece:
                    end_ = "%s+%dc" % (index, len(piece))
                    real.call(path, "delete", index, end_)
                    real.call(path, "insert", index, new, tags)
                if new != original:
                    shown[new] = original
            if len(runs) > 1:
                whole = "".join(new_line)
                originals = "".join(shown.get(p, p) for p in new_line)
                if whole != originals:
                    shown[whole] = originals

    def _sub_command(self, argv, path, sub, n):
        """Handle ".path sub-command ..." for the sub-commands that carry displayed texts.

        Inputs: argv = command (mutable), path = widget path, sub = sub-command,
        n = len(argv). Output: None or a result post-processing function (see _before).
        Covers configure / cget, menu add / insert / entryconfigure / entrycget,
        ttk.Notebook add / tab, ttk.Treeview heading, canvas create text /
        itemconfigure / itemcget.
        """
        # --- lists (Treeview rows, Listbox items, Combobox values): only for the
        # sub-commands they use, so that other widgets never pay the "winfo class" call
        if sub in ("insert", "item", "set", "get", "configure", "cget"):
            cls = self._class(path)
            handler = {"Treeview": self._treeview, "Listbox": self._listbox, "TCombobox": self._combobox,
                       "Text": self._text_widget}.get(cls)
            if handler is not None:
                res = handler(argv, path, sub, n)
                if res is not False:
                    return res

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
        # Variable of a combobox: a value of its list is displayed translated.
        combo = self._combos.get(self._input_vars.get(str(name), ""))
        if combo is not None and combo["values"] is not None and isinstance(value, str) and value in combo["values"][0]:
            return function(name, combo["values"][1][combo["values"][0].index(value)], *rest)
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
        # Variable of a combobox: the translation of a value of its list reads as the original.
        combo = self._combos.get(self._input_vars.get(str(name), ""))
        if combo is not None and isinstance(r, str):
            return combo["map"].get(r, r)
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
                elif kind == "ttext":
                    current = real.call(path, "item", sub_id, opt)
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
                elif kind == "ttext":
                    real.call(path, "item", sub_id, opt, new)
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
        # Part 3: Treeview item values (only rows still showing what we displayed).
        for key in list(self._tree_values.keys()):
            originals, old = self._tree_values[key]
            new = tuple(_engine.translate(o) for o in originals)
            if new == old:
                continue
            path, item = key
            try:
                if self._as_tuple(real.call(path, "item", item, "-values")) != old:
                    del self._tree_values[key]
                    continue
                real.call(path, "item", item, "-values", new)
                self._tree_values[key][1] = new
            except Exception:
                # Item deleted, tree destroyed...
                self._tree_values.pop(key, None)
        # Part 4: Listbox items, replaced in place (the selection of the item is kept).
        for path in list(self._lists.keys()):
            items = self._lists[path]
            try:
                for i in range(int(real.call(path, "size"))):
                    current = str(real.call(path, "get", i))
                    original = items.get(current)
                    if original is None:
                        continue
                    new = _engine.translate(original)
                    if new == current:
                        continue
                    selected = bool(real.call(path, "selection", "includes", i))
                    real.call(path, "delete", i)
                    real.call(path, "insert", i, new)
                    if selected:
                        real.call(path, "selection", "set", i)
                    items[new] = original
            except Exception:
                self._lists.pop(path, None)
        # Part 5: Combobox lists, and the displayed value when it is one of them.
        for path in list(self._combos.keys()):
            combo = self._combos[path]
            v = combo["values"]
            if v is None:
                continue
            new = tuple(_engine.translate(o) for o in v[0])
            if new == v[1]:
                continue
            try:
                if self._as_tuple(real.call(path, "cget", "-values")) != v[1]:
                    combo["values"] = None
                    continue
                shown = str(real.call(path, "get"))
                real.call(path, "configure", "-values", new)
                if shown in v[1]:
                    real.call(path, "set", new[v[1].index(shown)])
                combo["map"] = {d: o for o, d in zip(v[0], new)}
                v[1] = new
            except Exception:
                self._combos.pop(path, None)
        # Part 6: read-only Text widgets (only while still read-only).
        for path in list(self._texts.keys()):
            try:
                self._text_translate(path, only_disabled=True)
            except Exception:
                self._texts.pop(path, None)


# =====================================================================================
#  Script UI pages (HTML palettes shown by Tapir's ShowScriptUI command)
# =====================================================================================
#
# The script writes its page itself and sends it to ArchiCAD in an HTTP request whose
# JSON body is {"command": "API.ExecuteAddOnCommand", "parameters": {"addOnCommandId":
# {"commandName": "ShowScriptUI", ...}, "addOnCommandParameters": {"htmlContent": ...,
# "title": ...}}}. That request leaves from THIS process, so the layer patches
# http.client (urllib, the archicad package) and urllib3 (requests) and rewrites it:
#   1. the texts of the page are looked up at once: the HTML texts and attributes
#      (online translation requested if needed) and the strings of its JavaScript
#      (dictionaries and cache only: they may be identifiers or CSS classes);
#   2. a script (_PAGE_SCRIPT) is put at the head of the page with the translations
#      found. In the palette it translates every displayed text, including the texts the
#      page's own JavaScript writes later (MutationObserver), and gives the originals
#      back to the page's code (textContent, innerText, getAttribute, title, value...), so
#      the page's logic and the result it submits never see the translation;
#   3. a small HTTP server of this process (_PageServer, 127.0.0.1, random port and key)
#      answers the page for the texts that were not known in advance and tells it when
#      the language, the dictionaries or online translations change: the open palette
#      follows the Tapir Translator menu like the tkinter windows do.
# If the script has ended (the server is gone), the page keeps the translations it got.

_PAGE_ATTRIBUTES = ("title", "placeholder", "alt", "aria-label")   # Displayed attributes
_PAGE_MARK = "__tapirTranslator"   # Name of the page script's object; a page holding it is not rewritten again

_PAGE_SCRIPT = r"""
(function () {
"use strict";
if (window.__tapirTranslator) { return; }
try {
var C = __CONFIG__;
var map = Object.create(null);
Object.keys(C.map || {}).forEach(function (k) { map[k] = C.map[k]; });
var ATTRS = ["title", "placeholder", "alt", "aria-label"];
var BUTTONS = {button: true, submit: true, reset: true};
var RAW = "script,style,textarea,noscript";
var OFF = "[translate='no'],.notranslate,[contenteditable=''],[contenteditable='true'],[contenteditable='plaintext-only']";
var LETTER;
try { LETTER = new RegExp("\\p{L}", "u"); } catch (e) { LETTER = /[A-Za-z\u00C0-\u024F\u0370-\u052F\u3040-\u30FF\u4E00-\u9FFF\uAC00-\uD7AF]/; }
var SPLIT = /^(\s*)([\s\S]*?)(\s*)$/;
var dData = Object.getOwnPropertyDescriptor(CharacterData.prototype, "data");
var getAttr = Element.prototype.getAttribute;
var setAttr = Element.prototype.setAttribute;
var hasAttr = Element.prototype.hasAttribute;
var texts = new WeakMap();   // Text node -> {o: original, s: shown}
var attrs = new WeakMap();   // Element -> {attribute: {o: original, s: shown}}
var seen = Object.create(null);
var seenList = [];           // Every displayed text (trimmed), asked to the layer
var fresh = false;           // Texts seen since the last request to the layer
var anyTranslated = false;   // Read-back of originals is only needed once something is translated
var busy = false, failures = 0, rev = C.rev, timer = 0;

window.__tapirTranslator = {
  refresh: function () { walk(document); },
  state: function () { return {lang: C.lang, rev: rev, seen: seenList.length, failures: failures, server: !!C.url}; }
};

function lookup(core) {
  if (!core || core.length > 4000 || !LETTER.test(core)) { return null; }
  if (!seen[core]) { seen[core] = true; seenList.push(core); fresh = true; }
  var t = map[core];
  return typeof t === "string" ? t : null;
}
function shownOf(original) {
  var m = SPLIT.exec(original);
  var t = lookup(m[2]);
  return t === null ? original : m[1] + t + m[3];
}
function textOff(el) { return el.closest(RAW + "," + OFF) !== null; }
function attrOff(el) { return el.closest(OFF) !== null || (el.parentElement !== null && el.parentElement.closest(RAW) !== null); }

function doText(node, checked) {
  var p = node.parentNode;
  if (!p || p.nodeType !== 1 || (!checked && textOff(p))) { return; }
  var cur = dData.get.call(node);
  var st = texts.get(node);
  var original = st && cur === st.s ? st.o : cur;   // Otherwise the page changed the text: new original
  var shown = shownOf(original);
  if (shown !== original) {
    anyTranslated = true;
    // <option> without value: select.value is its text, keep it in the original language
    if (p.tagName === "OPTION" && !hasAttr.call(p, "value")) { setAttr.call(p, "value", original.replace(/[ \t\n\f\r]+/g, " ").replace(/^ | $/g, "")); }
  }
  if (st || shown !== original) { texts.set(node, {o: original, s: shown}); }
  if (cur !== shown) { dData.set.call(node, shown); }
}
function doAttr(el, name) {
  if (name === "value" && !(el.tagName === "INPUT" && BUTTONS[String(getAttr.call(el, "type") || "").toLowerCase()])) { return; }
  if (!hasAttr.call(el, name)) { return; }
  var cur = getAttr.call(el, name);
  var all = attrs.get(el);
  var st = all ? all[name] : undefined;
  var original = st && cur === st.s ? st.o : cur;
  var shown = shownOf(original);
  if (shown !== original) { anyTranslated = true; }
  if (st || shown !== original) {
    if (!all) { all = Object.create(null); attrs.set(el, all); }
    all[name] = {o: original, s: shown};
  }
  if (cur !== shown) { setAttr.call(el, name, shown); }
}
function doAttrs(el) {
  for (var i = 0; i < ATTRS.length; i++) { doAttr(el, ATTRS[i]); }
  if (el.tagName === "INPUT") { doAttr(el, "value"); }
}
var FILTER = {acceptNode: function (n) {
  if (n.nodeType === 1 && n.matches(RAW + "," + OFF)) {
    if (n.tagName === "TEXTAREA" && !n.matches(OFF)) { doAttrs(n); }   // its placeholder, not its text
    return NodeFilter.FILTER_REJECT;
  }
  return NodeFilter.FILTER_ACCEPT;
}};
function walk(root) {
  if (!root) { return; }
  if (root.nodeType === 3) { doText(root, false); return; }
  if (root.nodeType === 1) {
    if (textOff(root)) { if (root.tagName === "TEXTAREA" && !attrOff(root)) { doAttrs(root); } return; }
    doAttrs(root);
  } else if (root.nodeType !== 9 && root.nodeType !== 11) { return; }
  var w = document.createTreeWalker(root, NodeFilter.SHOW_ELEMENT | NodeFilter.SHOW_TEXT, FILTER);
  for (var n = w.nextNode(); n; n = w.nextNode()) {
    if (n.nodeType === 3) { doText(n, true); } else { doAttrs(n); }
  }
}

// --- the page's code reads back the ORIGINAL texts ---
function originalText(node) {
  var st = texts.get(node);
  var cur = dData.get.call(node);
  return st && cur === st.s ? st.o : cur;
}
function originalAttr(el, name, value) {
  var all = attrs.get(el);
  var a = all ? all[String(name).toLowerCase()] : undefined;
  return a && value === a.s ? a.o : value;
}
function joinTexts(root) {
  var w = document.createTreeWalker(root, NodeFilter.SHOW_TEXT);
  var s = "";
  for (var n = w.nextNode(); n; n = w.nextNode()) { s += originalText(n); }
  return s;
}
function collapse(s) { return s.replace(/[ \t\n\f\r]+/g, " ").replace(/^ | $/g, ""); }
function withOriginals(root, read) {
  // innerText / innerHTML need the layout or the serialiser: the originals are put back for the time of the read
  var undo = [];
  var w = document.createTreeWalker(root, NodeFilter.SHOW_ELEMENT | NodeFilter.SHOW_TEXT);
  for (var n = root; n; n = w.nextNode()) {
    if (n.nodeType === 3) {
      var st = texts.get(n);
      if (st && st.o !== st.s && dData.get.call(n) === st.s) { dData.set.call(n, st.o); undo.push([n, st.s]); }
    } else if (n.nodeType === 1) {
      var all = attrs.get(n);
      if (all) {
        for (var k in all) {
          var a = all[k];
          if (a.o !== a.s && getAttr.call(n, k) === a.s) { setAttr.call(n, k, a.o); undo.push([n, a.s, k]); }
        }
      }
    }
  }
  try { return read(); } finally {
    for (var i = undo.length - 1; i >= 0; i--) {
      var u = undo[i];
      if (u.length === 3) { setAttr.call(u[0], u[2], u[1]); } else { dData.set.call(u[0], u[1]); }
    }
  }
}
function getter(proto, name, make) {
  var d = proto ? Object.getOwnPropertyDescriptor(proto, name) : null;
  if (!d || !d.get || !d.configurable) { return; }
  var g = d.get, replacement = make(g);
  Object.defineProperty(proto, name, {configurable: true, enumerable: d.enumerable, set: d.set,
    get: function () { try { return anyTranslated ? replacement.call(this) : g.call(this); } catch (e) { return g.call(this); } }});
}
function attrGetter(proto, prop, attr) {
  getter(proto, prop, function (g) { return function () { return originalAttr(this, attr, g.call(this)); }; });
}
function serialised(g) { return function () { var self = this; return withOriginals(this, function () { return g.call(self); }); }; }
getter(CharacterData.prototype, "data", function (g) { return function () { return this.nodeType === 3 ? originalText(this) : g.call(this); }; });
getter(Node.prototype, "nodeValue", function (g) { return function () { return this.nodeType === 3 ? originalText(this) : g.call(this); }; });
getter(Node.prototype, "textContent", function (g) { return function () {
  if (this.nodeType === 3) { return originalText(this); }
  return this.nodeType === 1 || this.nodeType === 11 ? joinTexts(this) : g.call(this); }; });
getter(HTMLElement.prototype, "innerText", serialised);
getter(Element.prototype, "innerHTML", serialised);
getter(Element.prototype, "outerHTML", serialised);
getter(HTMLOptionElement.prototype, "text", function (g) { return function () { return collapse(joinTexts(this)); }; });
getter(HTMLOptionElement.prototype, "label", function (g) { return function () { return hasAttr.call(this, "label") ? g.call(this) : this.text; }; });
getter(Document.prototype, "title", function (g) { return function () {
  var t = this.querySelector ? this.querySelector("title") : null;
  return t ? collapse(joinTexts(t)) : g.call(this); }; });
attrGetter(HTMLElement.prototype, "title", "title");
attrGetter(HTMLInputElement.prototype, "placeholder", "placeholder");
attrGetter(HTMLTextAreaElement.prototype, "placeholder", "placeholder");
attrGetter(HTMLImageElement.prototype, "alt", "alt");
attrGetter(Element.prototype, "ariaLabel", "aria-label");
attrGetter(HTMLInputElement.prototype, "value", "value");   // Buttons: the value is the displayed attribute
Element.prototype.getAttribute = function (name) {
  var v = getAttr.apply(this, arguments);
  try { return originalAttr(this, name, v); } catch (e) { return v; }
};
["alert", "confirm", "prompt"].forEach(function (name) {
  var f = window[name];
  if (typeof f !== "function") { return; }
  window[name] = function (message) {
    var a = Array.prototype.slice.call(arguments);
    if (typeof message === "string") { a[0] = shownOf(message); later(); }
    return f.apply(window, a);
  };
});

// --- the layer (server of the script's process): unknown texts, language and dictionary changes ---
function receive(r, list) {
  if (!r || typeof r !== "object" || !r.map) { return; }
  var next = r.map, changed = false;
  if (r.lang !== C.lang) { changed = true; C.lang = r.lang; map = Object.create(null); }
  for (var i = 0; i < list.length; i++) {
    var k = list[i];
    var v = Object.prototype.hasOwnProperty.call(next, k) ? next[k] : undefined;
    if (map[k] !== v) { changed = true; if (v === undefined) { delete map[k]; } else { map[k] = v; } }
  }
  if (typeof r.rev === "number") { rev = r.rev; }
  if (changed) { walk(document); }
}
function later() { if (fresh && !timer && C.url && failures < 5) { timer = setTimeout(send, 40); } }
function send() {
  timer = 0;
  if (busy || !C.url || failures >= 5) { return; }
  busy = true; fresh = false;
  var list = seenList.slice();
  var x = new XMLHttpRequest();
  x.open("POST", C.url + "/tr?k=" + C.key, true);
  x.setRequestHeader("Content-Type", "text/plain;charset=UTF-8");
  x.timeout = 15000;
  x.onload = function () {
    busy = false;
    if (x.status === 200) { failures = 0; try { receive(JSON.parse(x.responseText), list); } catch (e) {} } else { failures++; }
    later();
  };
  x.onerror = x.ontimeout = function () { busy = false; failures++; fresh = true; later(); };
  x.send(JSON.stringify({texts: list}));
}
function poll() {
  if (busy || !C.url || failures >= 5) { return; }
  var x = new XMLHttpRequest();
  x.open("GET", C.url + "/rev?k=" + C.key, true);
  x.timeout = 5000;
  x.onload = function () {
    if (x.status !== 200) { failures++; return; }
    failures = 0;
    try { var r = JSON.parse(x.responseText); if (r.rev !== rev || r.lang !== C.lang) { fresh = true; send(); } } catch (e) {}
  };
  x.onerror = x.ontimeout = function () { failures++; };
  x.send();
}

var observer = new MutationObserver(function (records) {
  for (var i = 0; i < records.length; i++) {
    var r = records[i];
    if (r.type === "childList") {
      for (var j = 0; j < r.addedNodes.length; j++) { if (r.addedNodes[j].isConnected) { walk(r.addedNodes[j]); } }
    } else if (r.type === "characterData") {
      if (r.target.nodeType === 3 && r.target.isConnected) { doText(r.target, false); }
    } else if (r.type === "attributes" && r.target.isConnected && !attrOff(r.target)) {
      doAttr(r.target, r.attributeName);
    }
  }
  later();
});
observer.observe(document, {subtree: true, childList: true, characterData: true, attributes: true,
                            attributeFilter: ATTRS.concat(["value"])});
walk(document);
document.addEventListener("DOMContentLoaded", function () { walk(document); later(); });
if (C.url) {
  var pollTimer = setInterval(function () { if (failures >= 5) { clearInterval(pollTimer); } else { poll(); } }, 700);
}
later();
} catch (e) {
  try { console.error("Tapir Translator: " + e); } catch (e2) {}
}
})();
"""


def _js_unescape(raw):
    """Decode the escapes of a JavaScript string literal body (\\n, \\', \\u00e9, \\u{1F600}...)."""
    import re
    simple = {"n": "\n", "t": "\t", "r": "\r", "b": "\b", "f": "\f", "v": "\v", "0": "\0"}

    def one(m):
        s = m.group(1)
        if s in simple:
            return simple[s]
        if s in ("\n", "\r\n", "\r"):
            return ""   # Line continuation
        try:
            if s.startswith("u{"):
                return chr(int(s[2:-1], 16))
            if s[0] in "ux" and len(s) > 1:
                return chr(int(s[1:], 16))
        except ValueError:
            pass
        return s
    return re.sub(r"\\(u\{[0-9a-fA-F]+\}|u[0-9a-fA-F]{4}|x[0-9a-fA-F]{2}|\r\n|[\s\S])", one, raw)


def _js_strings(source):
    """Return the string literals of a JavaScript (or JSON) source.

    Comments are skipped; '...' and "..." strings are decoded; a `...` template is
    kept only without ${...} parts (a filled template is only known once displayed, the
    page asks for it then). A quote inside a regular expression literal may cut a
    string short: harmless, these strings are only looked up in the dictionaries.
    """
    out = []
    i, n = 0, len(source)
    while i < n:
        c = source[i]
        if c == "/" and source.startswith("//", i):
            j = source.find("\n", i)
            i = n if j < 0 else j
        elif c == "/" and source.startswith("/*", i):
            j = source.find("*/", i + 2)
            i = n if j < 0 else j + 2
        elif c in "'\"":
            j = i + 1
            while j < n and source[j] != c and source[j] != "\n":
                j += 2 if source[j] == "\\" else 1
            out.append(_js_unescape(source[i + 1:j]))
            i = j + 1
        elif c == "`":
            j, depth, filled = i + 1, 0, False
            while j < n:
                d = source[j]
                if d == "\\":
                    j += 2
                    continue
                if depth == 0 and d == "`":
                    break
                if depth == 0 and source.startswith("${", j):
                    depth, filled = 1, True
                    j += 2
                    continue
                if depth > 0:
                    depth += 1 if d == "{" else -1 if d == "}" else 0
                j += 1
            if not filled:
                out.append(_js_unescape(source[i + 1:j]))
            i = j + 1
        else:
            i += 1
    return out


def _page_texts(page):
    """Return (displayed, literals): the texts of an HTML page to look up in advance.

    displayed = texts between tags and displayed attributes (title, placeholder, alt,
    aria-label, value of input buttons), outside <script> and <style>;
    literals = string literals of the page's scripts, plus the texts of the HTML
    fragments among them (markup the page's code puts in innerHTML).
    Each list is stripped, without duplicates, texts with at least one letter.
    """
    from html.parser import HTMLParser
    displayed, literals = [], []

    class Parser(HTMLParser):
        def __init__(self, sink):
            HTMLParser.__init__(self, convert_charrefs=True)
            self.sink = sink     # List receiving the texts outside scripts
            self.raw = None      # "script" / "style" while inside one
            self.code = []

        def handle_starttag(self, tag, attrs):
            values = dict((k, v) for k, v in attrs if v)
            for name in _PAGE_ATTRIBUTES:
                if name in values:
                    self.sink.append(values[name])
            if tag == "input" and values.get("type", "").lower() in ("button", "submit", "reset") \
                    and "value" in values:
                self.sink.append(values["value"])
            if tag in ("script", "style"):
                self.raw, self.code = tag, []

        def handle_startendtag(self, tag, attrs):
            self.handle_starttag(tag, attrs)
            if tag in ("script", "style"):
                self.raw = None

        def handle_endtag(self, tag):
            if self.raw and tag == self.raw:
                if tag == "script":
                    for s in _js_strings("".join(self.code)):
                        literals.append(s)
                        if "<" in s and ">" in s:
                            sub = Parser(literals)
                            sub.feed(s)
                            sub.close()
                self.raw = None

        def handle_data(self, data):
            if self.raw:
                self.code.append(data)
            else:
                self.sink.append(data)

    parser = Parser(displayed)
    parser.feed(page)
    parser.close()

    def clean(items):
        result, known = [], set()
        for t in items:
            t = t.strip()
            if t and len(t) <= 2000 and t not in known and any(c.isalpha() for c in t):
                known.add(t)
                result.append(t)
        return result
    return clean(displayed), clean(literals)


def _page_insert(page, script):
    """Put 'script' at the head of the page: after <head>, else <html>, else <!doctype>."""
    import re
    for pattern in (r"<head\b[^>]*>", r"<html\b[^>]*>", r"<!doctype\b[^>]*>"):
        m = re.search(pattern, page, re.I)
        if m:
            return page[:m.end()] + script + page[m.end():]
    return script + page


class _PageServer(object):
    """HTTP server of the script's process that answers its Script UI pages.

    127.0.0.1 only, random port, random key in every URL (other local pages cannot use
    it). CORS headers (the palette's page has its own origin) and the Private Network
    Access header. Routes:
      GET  /rev?k=<key>  -> {"rev": n, "lang": "fr"}: the page asks again when they change;
      POST /tr?k=<key>   {"texts": [...]} -> {"rev", "lang", "map": {original: translation}}
                         (only the texts whose translation differs); unknown texts go to
                         the online translator like the tkinter ones.
    The threads are daemons: the server never keeps the script alive.
    """

    MAX_BODY = 4 * 1024 * 1024
    MAX_TEXTS = 5000

    def __init__(self):
        import http.server
        import json
        import secrets
        import threading
        from urllib.parse import parse_qs, urlsplit
        self.key = secrets.token_hex(16)
        key, max_body, max_texts = self.key, self.MAX_BODY, self.MAX_TEXTS

        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass    # The default writes every request to stderr, i.e. the script's output

            def _headers(self, code, length):
                self.send_response(code)
                self.send_header("Access-Control-Allow-Origin", "*")
                self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
                self.send_header("Access-Control-Allow-Headers", "Content-Type")
                self.send_header("Access-Control-Allow-Private-Network", "true")
                self.send_header("Cache-Control", "no-store")
                self.send_header("Content-Type", "application/json; charset=utf-8")
                self.send_header("Content-Length", str(length))
                self.end_headers()

            def _answer(self, code, value):
                body = json.dumps(value, ensure_ascii=False).encode("utf-8")
                self._headers(code, len(body))
                self.wfile.write(body)

            def _route(self):
                parts = urlsplit(self.path)
                return parts.path, parse_qs(parts.query).get("k", [""])[0] == key

            def do_OPTIONS(self):
                self._headers(204, 0)

            def do_GET(self):
                try:
                    path, allowed = self._route()
                    if not allowed:
                        return self._answer(403, {"error": "key"})
                    if path != "/rev":
                        return self._answer(404, {"error": "path"})
                    self._answer(200, {"rev": _poll_changes(), "lang": _engine.language})
                except Exception as e:
                    _log("Script UI server (GET): %r" % e)

            def do_POST(self):
                try:
                    path, allowed = self._route()
                    if not allowed:
                        return self._answer(403, {"error": "key"})
                    if path != "/tr":
                        return self._answer(404, {"error": "path"})
                    length = int(self.headers.get("Content-Length") or 0)
                    if length <= 0 or length > max_body:
                        return self._answer(413, {"error": "size"})
                    data = json.loads(self.rfile.read(length).decode("utf-8"))
                    texts = data.get("texts") if isinstance(data, dict) else None
                    if not isinstance(texts, list):
                        return self._answer(400, {"error": "texts"})
                    revision = _poll_changes()
                    found = {}
                    for t in texts[:max_texts]:
                        if isinstance(t, str):
                            r = _engine.translate(t)
                            if isinstance(r, str) and r != t:
                                found[t] = r
                    self._answer(200, {"rev": revision, "lang": _engine.language, "map": found})
                    _engine.write_missing()
                except Exception as e:
                    _log("Script UI server (POST): %r" % e)

        self.httpd = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.httpd.daemon_threads = True
        self.port = self.httpd.server_address[1]
        threading.Thread(target=self.httpd.serve_forever, name="TapirTranslatorPages", daemon=True).start()


_pages = None            # The _PageServer of this process (started with the first Script UI page)
_pages_failed = False    # The server could not start: pages get only the translations known in advance


def _page_server():
    """Return the _PageServer of this process, started on first use; None if it cannot start."""
    global _pages, _pages_failed
    if _pages is None and not _pages_failed:
        try:
            _pages = _PageServer()
            _log("Script UI page server on 127.0.0.1:%d" % _pages.port)
        except Exception as e:
            _pages_failed = True
            _log("Script UI page server not started: %r" % e)
    return _pages


def _flush_online():
    """At exit: let the online translations already asked for arrive (at most 3 s).

    A script may show its page and end at once, and an image drawn with a translation
    still waiting keeps its original text; waiting a little puts the translations in
    the cache, so they are displayed the next time.
    """
    try:
        import time
        auto = _engine.auto
        deadline = time.time() + 3.0
        while time.time() < deadline:
            with auto.lock:
                busy = auto.active_ and bool(auto.requests) and time.time() >= auto.outage_until
            if not busy:
                break
            time.sleep(0.1)
        _engine.write_missing()
    except Exception:
        pass


def _page_translate(page):
    """Return the page with the translation script at its head (see the section comment)."""
    import json
    if _PAGE_MARK in page:
        return page
    _poll_changes()
    displayed, literals = _page_texts(page)
    found = {}
    for t in displayed:
        r = _engine.translate(t)
        if isinstance(r, str) and r != t:
            found[t] = r
    for t in literals:
        if t not in found:
            r = _engine.translate(t, request=False)
            if isinstance(r, str) and r != t:
                found[t] = r
    server = _page_server()
    config = {"map": found, "lang": _engine.language, "rev": _revision,
              "url": "http://127.0.0.1:%d" % server.port if server else "",
              "key": server.key if server else ""}
    # The configuration is JavaScript inside <script>: "</" and "<!--" must not appear as such.
    data = json.dumps(config).replace("</", "<\\/").replace("<!--", "<\\!--")
    _log("Script UI page (tool=%s, language=%s): %d displayed texts, %d script strings, %d translated in advance"
         % (_engine._tool_name(), _engine.language or "original", len(displayed), len(literals), len(found)))
    return _page_insert(page, "<script>" + _PAGE_SCRIPT.replace("__CONFIG__", data) + "</script>")


def _page_parameters(data):
    """Return the parameters dict of a ShowScriptUI command in a request body, or None."""
    if not isinstance(data, dict) or data.get("command") != "API.ExecuteAddOnCommand":
        return None
    p = data.get("parameters")
    if not isinstance(p, dict):
        return None
    command = p.get("addOnCommandId")
    params = p.get("addOnCommandParameters")
    if isinstance(command, dict) and command.get("commandName") == "ShowScriptUI" and isinstance(params, dict):
        return params
    return None


def _page_body(body):
    """Return the rewritten body of a ShowScriptUI request, or None to send it unchanged."""
    if isinstance(body, str):
        raw = body.encode("utf-8")
    elif isinstance(body, (bytes, bytearray)):
        raw = bytes(body)
    else:
        return None   # File or iterator: never a Tapir command
    if b"ShowScriptUI" not in raw or b"htmlContent" not in raw:
        return None
    import json
    data = json.loads(raw.decode("utf-8"))
    params = _page_parameters(data)
    if params is None or not isinstance(params.get("htmlContent"), str):
        return None
    params["htmlContent"] = _page_translate(params["htmlContent"])
    if isinstance(params.get("title"), str):
        params["title"] = _engine.translate(params["title"])   # Palette title
    new = json.dumps(data).encode("ascii")    # ASCII: \u escapes, any encoding of the body works
    return new.decode("ascii") if isinstance(body, str) else new


def _page_request(args, kwargs):
    """Rewrite the arguments (after method and url) of HTTPConnection.request.

    Output: (args, kwargs), unchanged unless the body is a ShowScriptUI command; the
    Content-Length header, when given, follows the new body.
    """
    body = args[0] if args else kwargs.get("body")
    new = _page_body(body)
    if new is None:
        return args, kwargs
    if args:
        args = (new,) + tuple(args[1:])
    else:
        kwargs = dict(kwargs, body=new)
    headers = args[1] if len(args) > 1 else kwargs.get("headers")
    if headers and hasattr(headers, "items"):
        fixed = dict((k, str(len(new)) if str(k).lower() == "content-length" else v) for k, v in headers.items())
        if len(args) > 1:
            args = (args[0], fixed) + tuple(args[2:])
        else:
            kwargs["headers"] = fixed
    return args, kwargs


# =====================================================================================
#  Project data: the names ArchiCAD sends are never translated
# =====================================================================================
#
# Views, folders, layouts, stories... have the names the user gave them in ArchiCAD; a
# tool lists them, shows them in its panels and sentences. Those names must stay as they
# are. They reach the script in the answers of ArchiCAD's JSON API (Tapir commands
# included), read in this very process: the layer reads those answers too (http.client)
# and keeps every text value (never the error messages). A text equal to one of them is
# never translated; in a sentence sent online, only the pieces around such a name are.

_project_data = set()     # Texts received from ArchiCAD (stripped)
_project_regex = None     # Compiled pattern of the names searched in sentences (see _project_split)
_project_regex_size = -1  # Size of _project_data when _project_regex was built


def _project_collect(raw):
    """Add the text values of an answer of ArchiCAD's JSON API to _project_data."""
    import json
    try:
        data = json.loads(raw.decode("utf-8") if isinstance(raw, (bytes, bytearray)) else raw)
    except Exception:
        return
    found = set()
    stack = [data]
    while stack:
        v = stack.pop()
        if isinstance(v, dict):
            for k, x in v.items():
                if "error" not in str(k).lower() and "message" not in str(k).lower():
                    stack.append(x)
        elif isinstance(v, list):
            stack.extend(v)
        elif isinstance(v, str):
            t = v.strip()
            if 2 <= len(t) <= 300 and any(c.isalpha() for c in t):
                found.add(t)
    new = found - _project_data
    if new and _engine is not None:
        with _engine.lock:
            _project_data.update(new)
            for t in new:
                _engine.cache.pop(t, None)


def _project_split(text):
    """Split 'text' around the project names it contains: [(piece, is_name)], or None.

    Only names of 4 characters or more are searched, whole words, case-sensitive (a short
    name such as "Plan" would cut ordinary sentences).
    """
    global _project_regex, _project_regex_size
    import re
    if len(_project_data) != _project_regex_size:
        names = sorted((t for t in _project_data if len(t) >= 4), key=len, reverse=True)
        _project_regex = re.compile(r"(?<!\w)(?:%s)(?!\w)" % "|".join(re.escape(t) for t in names)) if names else None
        _project_regex_size = len(_project_data)
    if _project_regex is None:
        return None
    parts, last = [], 0
    for m in _project_regex.finditer(text):
        if m.start() > last:
            parts.append((text[last:m.start()], False))
        parts.append((m.group(0), True))
        last = m.end()
    if not parts:
        return None
    if last < len(text):
        parts.append((text[last:], False))
    return parts


def _is_api_request(args, kwargs):
    """True if the body of an HTTPConnection.request is a command of ArchiCAD's JSON API."""
    body = args[0] if args else kwargs.get("body")
    if isinstance(body, str):
        body = body.encode("utf-8", "replace")
    return isinstance(body, (bytes, bytearray)) and b'"command"' in body and b'"API.' in body


def _hook_http(module):
    """Patch module.HTTPConnection.request (http.client, urllib3.connection).

    Every request goes through _page_request; only ShowScriptUI commands are changed.
    urllib3 1.x inherits the request of http.client (already patched: nothing to do).
    For http.client, the answers to ArchiCAD's JSON API commands are also read to
    collect the project names (see _project_collect).
    """
    cls = getattr(module, "HTTPConnection", None)
    original = getattr(cls, "request", None) if cls is not None else None
    if original is None or getattr(original, "_tapir_translator", False):
        return
    import functools

    @functools.wraps(original)
    def request(self, method, url, *args, **kwargs):
        try:
            self._tapir_api = _is_api_request(args, kwargs)
            args, kwargs = _page_request(args, kwargs)
        except Exception as e:
            _log("Script UI page not translated: %r" % e)
        return original(self, method, url, *args, **kwargs)

    request._tapir_translator = True
    cls.request = request
    if module.__name__ != "http.client":
        return

    # The answer of an API command: its body is collected while the script reads it.
    getresponse_original = cls.getresponse

    @functools.wraps(getresponse_original)
    def getresponse(self, *args, **kwargs):
        response = getresponse_original(self, *args, **kwargs)
        try:
            if self.__dict__.pop("_tapir_api", False):
                response._tapir_chunks = []
        except Exception:
            pass
        return response
    cls.getresponse = getresponse

    response_cls = module.HTTPResponse
    read_original = response_cls.read

    @functools.wraps(read_original)
    def read(self, amt=None):
        data = read_original(self, amt)
        chunks = self.__dict__.get("_tapir_chunks")
        if chunks is not None:
            try:
                if data:
                    chunks.append(bytes(data))
                if amt is None or not data or len(data) < amt or self.isclosed():
                    del self._tapir_chunks
                    _project_collect(b"".join(chunks))
            except Exception as e:
                _log("project names not read: %r" % e)
        return data
    response_cls.read = read


# =====================================================================================
#  Texts drawn into images with Pillow (buttons drawn by the script)
# =====================================================================================

#
# Some scripts draw their buttons as images (rounded "capsules"): Tk then only sees a
# picture, the text never goes through the Tcl proxy. While a Tk window of the script is
# open (a script that renders images to files without a window is not affected):
#   1. the text given to ImageDraw.text / multiline_text, and to the measuring methods
#      (textbbox, multiline_textbbox, textlength, so that the script centres the
#      translated text), is translated;
#   2. the "recipe" of every image made by Image.new is recorded: the drawing calls
#      (with the ORIGINAL texts), then resize / copy. An ImageTk.PhotoImage made from an
#      image whose recipe holds texts is remembered;
#   3. when the displayed texts must change (language, dictionaries, online translations
#      arrived: same revision as the Tk windows), _pil_refresh() replays the recipe with
#      the new translations and pastes the result into the same PhotoImage: the button
#      changes in place. A text that was centred in its image stays centred.
# An image changed by anything not recorded (paste, putpixel...) or not made by
# Image.new is never replayed: it keeps what it was drawn with.

_PIL_DRAWING = ("arc", "bitmap", "chord", "circle", "ellipse", "line", "pieslice", "point", "polygon",
                "rectangle", "regular_polygon", "rounded_rectangle", "shape", "text", "multiline_text")
# Position of the text among the arguments after self, for the text methods.
_PIL_TEXT_POSITION = {"text": 1, "multiline_text": 1, "textbbox": 1, "multiline_textbbox": 1, "textlength": 0}
# Image methods that change an image in place without being recorded: its recipe is dropped.
_PIL_MUTATING = ("paste", "putpixel", "putalpha", "putdata", "alpha_composite", "frombytes", "putpalette",
                 "thumbnail")
_pil_original = {}       # Name -> unpatched function, used by the replay
_pil_photos = []         # [(weak reference to an ImageTk.PhotoImage, recipe)]
_pil_state = None        # threading.local(): "on" while inside a patched Pillow call (nested calls pass through)
# The recipe of an image {"base", "ops", "broken"} is kept on the image itself, in the
# attribute _tapir_recipe (PIL images cannot be dictionary keys: they compare by content).


def _pil_init():
    """Create the shared state of the Pillow hooks (once)."""
    global _pil_state
    if _pil_state is None:
        import threading
        _pil_state = threading.local()


def _pil_recipe(image):
    """The recipe recorded for a PIL image, or None (never raises)."""
    try:
        return image.__dict__.get("_tapir_recipe")
    except Exception:
        return None


def _pil_outer(function):
    """Run function() as an outermost Pillow call (inner calls of Pillow are not recorded)."""
    _pil_state.on = True
    try:
        return function()
    finally:
        _pil_state.on = False


def _hook_pil_image(module):
    """Patch PIL.Image: Image.new starts a recipe, resize / copy carry it, in-place changes drop it."""
    if getattr(module, "_tapir_translator", False):
        return
    module._tapir_translator = True
    _pil_init()
    import functools
    cls = module.Image
    new_original = module.new
    _pil_original["new"] = new_original

    @functools.wraps(new_original)
    def new(*args, **kwargs):
        if getattr(_pil_state, "on", False) or not _proxies:
            return new_original(*args, **kwargs)
        image = _pil_outer(lambda: new_original(*args, **kwargs))
        try:
            image._tapir_recipe = {"base": (args, kwargs), "ops": [], "broken": False}
        except Exception:
            pass
        return image
    module.new = new

    def derived(name):
        original = getattr(cls, name)
        _pil_original[name] = original

        @functools.wraps(original)
        def method(self, *args, **kwargs):
            if getattr(_pil_state, "on", False):
                return original(self, *args, **kwargs)
            result = _pil_outer(lambda: original(self, *args, **kwargs))
            recipe = _pil_recipe(self)
            if recipe is not None and not recipe["broken"] and result is not None:
                try:
                    result._tapir_recipe = {"base": recipe["base"], "broken": False,
                                            "ops": recipe["ops"] + [{"kind": name, "args": args, "kwargs": kwargs}]}
                except Exception:
                    pass
            return result
        setattr(cls, name, method)
    for name in ("resize", "copy"):
        derived(name)

    def mutating(name):
        original = getattr(cls, name, None)
        if original is None:
            return

        @functools.wraps(original)
        def method(self, *args, **kwargs):
            if not getattr(_pil_state, "on", False):
                recipe = _pil_recipe(self)
                if recipe is not None:
                    recipe["broken"] = True
            return original(self, *args, **kwargs)
        setattr(cls, name, method)
    for name in _PIL_MUTATING:
        mutating(name)


def _hook_pil(module):
    """Patch PIL.ImageDraw.ImageDraw: drawn texts translated, drawing calls recorded."""
    cls = getattr(module, "ImageDraw", None)
    if cls is None or getattr(cls, "_tapir_translator", False):
        return
    cls._tapir_translator = True
    _pil_init()
    import functools

    init_original = cls.__init__

    @functools.wraps(init_original)
    def init(self, im, mode=None, *args, **kwargs):
        init_original(self, im, mode, *args, **kwargs)
        self._tapir_mode = mode      # Drawing mode, needed to draw again (replay)
    cls.__init__ = init

    def wrap(name):
        original = getattr(cls, name, None)
        if original is None:
            return
        _pil_original[name] = original
        position = _PIL_TEXT_POSITION.get(name)

        @functools.wraps(original)
        def method(self, *args, **kwargs):
            if getattr(_pil_state, "on", False) or not _proxies:
                return original(self, *args, **kwargs)
            source = None
            try:
                if position is not None:
                    if len(args) > position and isinstance(args[position], str):
                        source = args[position]
                        args = args[:position] + (_engine.translate(source),) + args[position + 1:]
                    elif isinstance(kwargs.get("text"), str):
                        source = kwargs["text"]
                        kwargs = dict(kwargs, text=_engine.translate(source))
                if name in _PIL_DRAWING:
                    recipe = _pil_recipe(getattr(self, "_image", None))
                    if recipe is not None and not recipe["broken"]:
                        recipe["ops"].append({"kind": "draw", "name": name, "mode": getattr(self, "_tapir_mode", None),
                                              "args": args, "kwargs": kwargs, "original": source,
                                              "shown": args[position] if source is not None and len(args) > position
                                              else kwargs.get("text")})
            except Exception as e:
                _log("drawn text: %r" % e)
            return _pil_outer(lambda: original(self, *args, **kwargs))
        setattr(cls, name, method)

    for name in set(_PIL_DRAWING) | set(_PIL_TEXT_POSITION):
        wrap(name)


def _hook_pil_tk(module):
    """Patch PIL.ImageTk.PhotoImage: remember the photos made from images with drawn texts."""
    cls = getattr(module, "PhotoImage", None)
    if cls is None or getattr(cls, "_tapir_translator", False):
        return
    cls._tapir_translator = True
    _pil_init()
    import functools
    import weakref
    init_original = cls.__init__
    paste_original = cls.paste
    _pil_original["photo_paste"] = paste_original

    def remember(photo, image):
        # Forget what this photo showed before, then remember its new recipe (texts only).
        _pil_photos[:] = [e for e in _pil_photos if e[0]() is not None and e[0]() is not photo]
        recipe = _pil_recipe(image)
        if recipe is not None and not recipe["broken"] and any(op.get("original") for op in recipe["ops"]):
            _pil_photos.append((weakref.ref(photo), recipe))

    @functools.wraps(init_original)
    def init(self, image=None, size=None, **kw):
        init_original(self, image, size, **kw)
        try:
            if _proxies and image is not None and not isinstance(image, str):
                remember(self, image)
        except Exception as e:
            _log("drawn image: %r" % e)
    cls.__init__ = init

    @functools.wraps(paste_original)
    def paste(self, im, *args, **kwargs):
        # The script shows another image in this photo: follow it.
        try:
            if _proxies:
                remember(self, im)
        except Exception:
            pass
        return paste_original(self, im, *args, **kwargs)
    cls.paste = paste


def _pil_centre(draw, op, size_, new):
    """xy for 'new' in place of op["shown"]: the same centre if the text was centred in its image.

    size_ = (width, height) of the image being drawn.
    """
    args, kwargs = op["args"], op["kwargs"]
    names = ("xy", "text", "fill", "font", "anchor", "spacing", "align", "direction", "features", "language",
             "stroke_width", "stroke_fill", "embedded_color")
    params = dict(zip(names, args))
    params.update(kwargs)
    xy = params.get("xy")
    if not isinstance(xy, (tuple, list)) or len(xy) != 2:
        return xy
    measure = _pil_original.get("multiline_textbbox" if op["name"] == "multiline_text" else "textbbox")
    keys = ("font", "anchor", "spacing", "align", "direction", "features", "language", "stroke_width",
            "embedded_color", "font_size")
    options = dict((k, params[k]) for k in keys if k in params)
    # Measured at the origin, as scripts do to centre a text (no rounding of xy on the way).
    # Each direction separately: a text centred in it stays centred, otherwise it keeps
    # its position (left aligned label, text at the top...).
    old = measure(draw, (0, 0), op["shown"], **options)
    now = measure(draw, (0, 0), new, **options)
    result = list(xy)
    for axis, size in ((0, size_[0]), (1, size_[1])):
        low, high = old[axis], old[axis + 2]
        if abs(xy[axis] + (low + high) / 2.0 - size / 2.0) <= max(2.0, size * 0.03):
            result[axis] = xy[axis] + ((low + high) - (now[axis] + now[axis + 2])) / 2.0
    return tuple(result)


def _pil_replay(recipe):
    """Draw the image of 'recipe' again with the current translations (returns the new image)."""
    image_draw = sys.modules["PIL.ImageDraw"]
    args, kwargs = recipe["base"]
    image = _pil_original["new"](*args, **kwargs)
    draws = {}
    for op in recipe["ops"]:
        if op["kind"] != "draw":
            image = _pil_original[op["kind"]](image, *op["args"], **op["kwargs"])
            draws = {}
            continue
        draw = draws.get(op["mode"])
        if draw is None:
            draw = draws[op["mode"]] = image_draw.Draw(image, op["mode"])
        args, kwargs = op["args"], op["kwargs"]
        if op.get("original") is not None:
            new = _engine.translate(op["original"])
            if new != op["shown"]:
                xy = _pil_centre(draw, op, image.size, new)
                position = _PIL_TEXT_POSITION[op["name"]]
                if len(args) > position:
                    args = (xy,) + args[1:position] + (new,) + args[position + 1:]
                else:
                    kwargs = dict(kwargs, text=new)
                    args = (xy,) + tuple(args[1:])
                op["args"], op["kwargs"], op["shown"] = args, kwargs, new
        _pil_outer(lambda: _pil_original[op["name"]](draw, *args, **kwargs))
    return image


def _pil_refresh():
    """Draw again, in place, the photos whose texts are translated differently now (Tk thread)."""
    for entry in list(_pil_photos):
        photo = entry[0]()
        recipe = entry[1]
        if photo is None:
            _pil_photos.remove(entry)
            continue
        try:
            texts = [op for op in recipe["ops"] if op.get("original") is not None]
            if all(_engine.translate(op["original"]) == op["shown"] for op in texts):
                continue
            image = _pil_replay(recipe)
            _pil_original["photo_paste"](photo, image)
        except Exception as e:
            _log("drawn image not updated: %r" % e)
            _pil_photos.remove(entry)


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
        global _tk_revision
        try:
            # Steps 1-2: settings / dictionaries / corrections changed, or online translations
            # arrived (or an outage / detection wait ended)? The Script UI page server may have
            # noticed it first: the revision number tells.
            revision = _poll_changes()
            # Step 3: re-translate every open window.
            if revision != _tk_revision:
                _tk_revision = revision
                for inter in list(_proxies):
                    inter.reapply()
                _pil_refresh()    # Images with drawn texts (buttons drawn with Pillow)
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
    """Import loader wrapper used for the hooked modules (see _HOOKS).

    It delegates to the real loader, and once the module has been executed it calls
    its hook on it, so the module is patched before the script can use it.
    """

    def __init__(self, loader, hook):
        """Wrap the real loader 'loader' found by the standard PathFinder; hook(module) patches."""
        self._loader = loader
        self._hook = hook

    def create_module(self, spec):
        """Delegate module creation to the real loader."""
        return self._loader.create_module(spec)

    def exec_module(self, module):
        """Execute the module with the real loader, then patch it (errors are logged)."""
        self._loader.exec_module(module)
        try:
            self._hook(module)
        except Exception as e:
            _log("hook of %s: %r" % (getattr(module, "__name__", "?"), e))

    def __getattr__(self, name):
        """Forward every other loader attribute to the real loader."""
        return getattr(self._loader, name)


class _Hook(object):
    """Meta-path finder that waits for the import of the hooked modules to patch them.

    Nothing is imported in advance: start-up stays light and scripts without a
    window are not affected.
    """

    def find_spec(self, name, path=None, target=None):
        """Return the spec of a module of _HOOKS with a wrapping loader; None otherwise.

        The spec is found with the standard PathFinder, then its loader is replaced
        by a _Loader. Returning None lets the normal import machinery proceed.
        """
        hook = _HOOKS.get(name)
        if hook is None:
            return None
        try:
            from importlib.machinery import PathFinder
            spec = PathFinder.find_spec(name, path)
            if spec is None or spec.loader is None:
                return None
            spec.loader = _Loader(spec.loader, hook)
            return spec
        except Exception as e:
            _log("crochet : %r" % e)
            return None


# Modules patched when they are imported: tkinter (windows), http.client and urllib3
# (requests that carry a Script UI page, see _hook_http).
_HOOKS = {"tkinter": _hook_up, "http.client": _hook_http, "urllib3.connection": _hook_http,
          "PIL.Image": _hook_pil_image, "PIL.ImageDraw": _hook_pil, "PIL.ImageTk": _hook_pil_tk}


def _install():
    """Create the engine, load the files and hook tkinter and the HTTP clients.

    Step 1: create the global _engine and load settings / dictionaries.
    Step 2: patch now the hooked modules already imported; install the import hook at
    the head of sys.meta_path for the others.
    Step 3: write the missing texts at process exit.
    """
    global _engine
    root = _folder()
    _engine = _Engine(root)
    _engine.reload_if_changed()
    for name, hook in _HOOKS.items():
        if name in sys.modules:
            hook(sys.modules[name])
    sys.meta_path.insert(0, _Hook())
    import atexit
    atexit.register(lambda: _engine.write_missing())
    atexit.register(_flush_online)    # Runs first (atexit: last registered, first run)


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
