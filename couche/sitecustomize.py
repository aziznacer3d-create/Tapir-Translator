# -*- coding: utf-8 -*-
"""
Tapir Translator - translation layer for the tkinter interfaces of Tapir scripts.

This file is written by the Tapir Translator add-on into
%APPDATA%\\Tapir Translator\\couche\\ (it is overwritten every time ArchiCAD starts:
do not edit it there, edit the source file in the repository instead).

How it works, step by step
--------------------------
1. Loading. At ArchiCAD start-up the (preloaded) add-on writes this file into the
   "couche" folder, puts that folder at the head of ArchiCAD's PYTHONPATH and sets
   TAPIR_TRANSLATOR_DIR. Tapir runs every script with "uv run --script <script.py>"
   and keeps this PYTHONPATH, so Python's "site" machinery imports this
   "sitecustomize" module by itself at interpreter start-up, BEFORE the script runs.
   The script is never modified and Tapir only starts one process.
   If another sitecustomize.py exists further down sys.path (the machine's own Python
   environment), it is executed afterwards (see _chainer) so it is not shadowed.

2. Activation. The layer does nothing outside a Tapir script (no TAPIR_SCRIPT_ID
   environment variable, unless TAPIR_TRANSLATOR_FORCE=1). Nothing heavy is imported
   at start-up: a meta-path finder (_Crochet) waits for the script to import tkinter
   and then patches tkinter.Tk.__init__ / destroy (_brancher).

3. Tcl interpreter proxy. Every Tk root window gets a proxy (_Intermediaire) in place
   of its "tk" attribute (the Tcl interpreter). Every Tcl command goes through
   proxy.call(); the proxy translates only the DISPLAYED texts (widget -text, menu
   -label, window titles, notebook tabs, Treeview column headings, canvas texts,
   message boxes, file dialog titles, variables shown by a Label / Button...). It
   remembers the original of each displayed text, so values read back by the script
   (cget, StringVar.get, entrycget, wm title) are returned in the ORIGINAL language:
   the script's logic never sees the translation. Data (view names, lists, Treeview
   rows, Entry, Text, Combobox -values) is never touched.

4. Dictionaries. _Moteur loads, from lowest to highest priority, the add-on's common
   dictionary (_commun.json), the user's dictionary (_perso.json) and the script's own
   dictionary (<tool>.json). Each one may hold multilingual "termes" rows and per-
   language "traductions" tables. Entries with {placeholders} become regex templates;
   trailing punctuation, case and multi-line texts are handled as fall-backs. The
   source language of the script is the one chosen in the menu, else the one declared
   by the script's dictionary, else it is detected automatically by "voting".

5. Online translation. Texts unknown to every dictionary are sent in the background
   (worker thread, never in the Tk loop) by _Automatique to the local engine
   TapirTranslatorMoteur.exe (HTTP on 127.0.0.1, started by the add-on), which calls
   the online translator chosen in the menu. The original text stays on screen until
   the answer arrives, then the translation appears by itself.

6. Cache. Online results are stored in cache\\<translator>\\<source>-<target>.json,
   shared by every script, so the next time the display is immediate. A dictionary
   always wins over the cache.

7. Corrections. corrections.json fixes wrong ArchiCAD terms produced by online
   translators (e.g. "story" -> "histoire" instead of "etage"). Corrections are applied
   when an online/cached translation is displayed, never to dictionary entries.

8. Live reload. Every _PERIODE_MS milliseconds (Tk "after" timer) the layer checks
   whether reglages.json (written by the add-on menu), the dictionaries or
   corrections.json changed, and whether online translations arrived; if so every
   open window re-applies its texts (_Intermediaire.reappliquer), so the language
   changes without restarting the tool.

Every internal error is written to journal.txt and never reaches the script.
"""

import os
import sys

VERSION_COUCHE = "0.6.0"
_PERIODE_MS = 300            # Period (ms) of the live check: settings / dictionaries reload, arrived translations
_TAILLE_JOURNAL = 512 * 1024  # Beyond this size (bytes) the log file is deleted and starts again from zero

# Widget / menu options whose value is a displayed text to translate.
_OPTIONS_TEXTE = ("-text", "-label")
# Placeholder names in dictionary templates ("{n} elements") that may only match numbers.
_NOMS_NOMBRE = ("n", "nb", "nombre", "count", "i", "x", "total", "n1", "n2")
# Menu entry types accepted by "<menu> add <type>" / "<menu> insert <index> <type>".
_TYPES_MENU = ("command", "cascade", "checkbutton", "radiobutton", "separator")
# Tk widget classes (lower case, "ttk::x" normalised to "tx") whose -textvariable is only
# displayed (never typed in by the user), so the variable's value may be translated.
_CLASSES_AFFICHAGE = ("label", "button", "checkbutton", "radiobutton", "labelframe",
                      "message", "menubutton", "tlabel", "tbutton", "tcheckbutton",
                      "tradiobutton", "tlabelframe", "tmenubutton")
# Tk standard dialog commands (message box, file / directory / colour choosers).
_DIALOGUES = ("tk_messageBox", "tk_getOpenFile", "tk_getSaveFile", "tk_chooseDirectory",
              "tk_chooseColor")
# Options of those dialogs whose value is a displayed text to translate.
_OPTIONS_DIALOGUE = ("-title", "-message", "-detail")


# =====================================================================================
#  Log file (journal.txt)
# =====================================================================================

def _dossier():
    """Return the Tapir Translator data folder (%APPDATA%\\Tapir Translator).

    The add-on passes it through the TAPIR_TRANSLATOR_DIR environment variable.
    Returns "" when the variable is missing (the layer then stays inactive).
    """
    return os.environ.get("TAPIR_TRANSLATOR_DIR", "")


def _journal(message):
    """Append one timestamped line to journal.txt in the data folder.

    Input: message (str). Output: none. The line contains the date, the process id
    and the message. This function never raises: logging must never disturb the
    translated script.
    """
    try:
        # Step 1: no data folder -> nowhere to write, do nothing.
        racine = _dossier()
        if not racine:
            return
        chemin = os.path.join(racine, "journal.txt")
        # Step 2: keep the log small; delete it once it exceeds _TAILLE_JOURNAL.
        try:
            if os.path.getsize(chemin) > _TAILLE_JOURNAL:
                os.remove(chemin)
        except OSError:
            pass
        # Step 3: append the line (imported lazily to keep start-up light).
        import time
        with open(chemin, "a", encoding="utf-8") as f:
            f.write("%s [%d] %s\n" % (time.strftime("%Y-%m-%d %H:%M:%S"), os.getpid(), message))
    except Exception:
        pass


# =====================================================================================
#  Translation engine (dictionaries + settings)
# =====================================================================================

class _Moteur:
    """Translation engine of one script process.

    It reads the settings (reglages.json), the dictionaries and corrections.json,
    builds the lookup tables for the current output language and translates texts
    with them. Texts unknown to the dictionaries are delegated to the online
    translation helper (self.auto, an _Automatique). A single instance exists per
    process (module global _moteur), shared by every Tk window.
    """

    def __init__(self, racine):
        """Create an empty engine for the data folder 'racine'.

        Nothing is read here: recharger_si_change() loads the files.
        """
        self.racine = racine
        self.outil = None         # Tool (script) name, computed lazily by _nom_outil()
        self.langue = ""          # Output language code; "" = original language, no translation
        self.collecte = False     # True: record texts missing from the dictionaries (manquants\<tool>.json)
        self.langue_source = ""   # Source language of the script (menu choice or script dictionary), "" = unknown
        self.table = {}           # Original text -> translation (current output language)
        self.table_min = {}       # Same table with case-folded keys (case-insensitive fall-back)
        self.corrections = []     # (pattern on the original or None, pattern to replace, replacement)
        self.modeles = []         # (regex, template) for texts containing {placeholders}
        self.manquants = {}       # Language -> {missing text: ""}
        self.manquants_modifies = False   # True when self.manquants has entries not yet written to disk
        self.empreinte = None     # Dates/sizes of the files read, used to detect changes (live reload)
        self.cache = {}           # Original text -> displayed result, emptied on every reload
        self.entree = ""          # Input language explicitly chosen in the menu ("" = not chosen)
        self.colonnes = {}        # Language -> set of texts known by the dictionaries ("termes" columns)
        self.votes = {}           # Language -> number of script texts recognised in that language
        self.vus = set()          # Texts already counted in the votes (each text votes once)
        self.detectee = ""        # Automatically detected script language, frozen once found
        self.auto = _Automatique(racine, self)   # Online translation through the local engine

    # --- files ------------------------------------------------------------------------
    def _nom_outil(self):
        """Return the tool name used for the per-script dictionary and missing-text file.

        Priority: TAPIR_TRANSLATOR_OUTIL environment variable, else the script file
        name without extension (sys.argv[0]), else "script". Computed once and cached.
        """
        if self.outil is None:
            nom = os.environ.get("TAPIR_TRANSLATOR_OUTIL", "")
            if not nom:
                try:
                    nom = os.path.splitext(os.path.basename(sys.argv[0]))[0]
                except Exception:
                    nom = ""
            self.outil = nom or "script"
        return self.outil

    def _fichiers(self):
        """Return the list of watched files, in a fixed order.

        Order: reglages.json, _commun.json, _perso.json, <tool>.json, corrections.json.
        The dictionaries are listed from the lowest to the highest priority.
        recharger_si_change() unpacks this list positionally.
        """
        outil = self._nom_outil()
        dico = os.path.join(self.racine, "dictionnaires")
        # From the lowest to the highest priority
        return [os.path.join(self.racine, "reglages.json"),
                os.path.join(dico, "_commun.json"),     # The add-on's dictionary (replaced at every version)
                os.path.join(dico, "_perso.json"),      # The user's dictionary (never touched by the add-on)
                os.path.join(dico, outil + ".json"),    # The dictionary of this particular script
                os.path.join(self.racine, "corrections.json")]   # Terms to fix in online translations

    def _calculer_empreinte(self):
        """Return a fingerprint of the watched files.

        Output: tuple with one (modification time in ns, size) pair per file, or None
        for a missing file. Comparing two fingerprints tells whether anything changed.
        """
        emp = []
        for chemin in self._fichiers():
            try:
                s = os.stat(chemin)
                emp.append((s.st_mtime_ns, s.st_size))
            except OSError:
                emp.append(None)
        return tuple(emp)

    @staticmethod
    def _lire_json(chemin):
        """Read a JSON file and return its content, or None.

        Returns None silently for a missing file; for an unreadable or invalid file
        the error is logged and None is returned. "utf-8-sig" accepts files saved with
        a BOM (e.g. by Notepad).
        """
        import json
        try:
            with open(chemin, "r", encoding="utf-8-sig") as f:
                return json.load(f)
        except FileNotFoundError:
            return None
        except Exception as e:
            _journal("fichier illisible %s : %r" % (chemin, e))
            return None

    def recharger_si_change(self):
        """Reload settings, dictionaries and corrections if any watched file changed.

        Output: True if the language or a translation may have changed (the caller
        must then re-apply the texts of the open windows), False if nothing changed.
        Called once at start-up and then every _PERIODE_MS by the Tk timer.
        """
        # Step 1: compare the file fingerprint with the last one; unchanged -> nothing to do.
        emp = self._calculer_empreinte()
        if emp == self.empreinte:
            return False
        self.empreinte = emp
        reglages_f, commun_f, perso_f, outil_f, corrections_f = self._fichiers()

        # Step 2: settings written by the add-on menu: output language ("origine" means
        # no translation) and the "collect missing texts" switch.
        reglages = self._lire_json(reglages_f) or {}
        langue = str(reglages.get("langue", "") or "")
        if langue == "origine":
            langue = ""
        self.collecte = bool(reglages.get("collecte", False))

        # Step 3: read the three dictionaries; anything that is not a JSON object is ignored.
        commun = self._lire_json(commun_f)
        perso = self._lire_json(perso_f)
        outil = self._lire_json(outil_f)
        commun = commun if isinstance(commun, dict) else {}
        perso = perso if isinstance(perso, dict) else {}
        outil = outil if isinstance(outil, dict) else {}
        # Step 4: source language of the script. Only the language declared by the script's
        # own dictionary counts: the common dictionary serves scripts written in any language.
        # Priority: input language chosen in the menu; else the one of the script's
        # dictionary; else (empty) automatic detection later on.
        entree = str(reglages.get("entree", "") or "")
        source = entree or str(outil.get("langue_source", "") or "")
        self.entree = entree

        # Step 5: build the lookup table, from the lowest to the highest priority: common
        # (add-on), perso (user), script dictionary; in each one, "termes" first, then
        # "traductions" (so later entries overwrite earlier ones). Empty translations are skipped.
        table = {}
        if langue:
            for d in (commun, perso, outil):
                table.update(self._depuis_termes(d.get("termes"), langue, source, bool(entree)))
                trad = d.get("traductions", {})
                if isinstance(trad, dict) and isinstance(trad.get(langue), dict):
                    for k, v in trad[langue].items():
                        if isinstance(k, str) and isinstance(v, str) and v != "":
                            table[k] = v
        self.langue_source = source
        # Step 6: requested language == script language -> nothing to translate.
        if langue and source and langue == source:
            langue = ""          # The requested language is the script's own language
        if langue != self.langue:
            _journal("outil=%s langue=%s (source=%s) collecte=%s" %
                     (self._nom_outil(), langue or "origine", source or "?", self.collecte))
        self.langue = langue
        self.table = table if langue else {}
        # Step 7: case-insensitive fall-back table ("FERMER", "fermer" -> "CLOSE", "close");
        # templates and 1-character keys are excluded; the first key wins on a collision.
        self.table_min = {}
        for k, v in self.table.items():
            if len(k) >= 2 and "{" not in k:
                self.table_min.setdefault(k.casefold(), v)
        # Step 8: compile the {placeholder} templates and the term corrections, and empty
        # the result cache (every text will be translated again with the new tables).
        self.modeles = self._compiler_modeles(self.table)
        self.corrections = self._compiler_corrections(self._lire_json(corrections_f), langue) if langue else []
        self.cache = {}

        # Step 9: dictionary columns: the texts known per language, used to recognise the
        # script's language (automatic detection by votes). Templates and "_" keys are skipped.
        self.colonnes = {}
        for d in (commun, perso, outil):
            for t in d.get("termes", []) if isinstance(d.get("termes"), list) else []:
                if isinstance(t, dict):
                    for code, texte in t.items():
                        if isinstance(texte, str) and not code.startswith("_") and "{" not in texte:
                            self.colonnes.setdefault(code, set()).add(texte)
        # Step 10: configure online translation: enabled only if the "moteur" setting is on
        # AND a translation is requested. The engine port comes from the environment
        # (set by the add-on) or else from the settings; default translator "mymemory".
        port = os.environ.get("TAPIR_TRANSLATOR_MOTEUR_PORT", "") or str(reglages.get("port", "") or "")
        self.auto.configurer(bool(reglages.get("moteur", False)) and bool(langue), source, langue, port,
                             str(reglages.get("fournisseur", "") or "mymemory"))
        return True

    # --- term corrections (corrections.json) ----------------------------------------------
    @staticmethod
    def _compiler_corrections(donnees, langue):
        """Compile the corrections.json rules of one output language.

        Input: donnees = parsed corrections.json, {"fr": [rule, ...], "de": [...], ...};
        langue = output language code. A rule looks like:
             {"source": "story", "mauvais": "histoire", "correct": "étage"}
             {"source": ["story", "stories"], "remplacer": {"histoire": "étage", "histoires": "étages"}}
             {"mauvais": "Toutes les histoires", "correct": "Tous les étages"}
        "source" (optional): the rule only applies if the original text contains one of
        these words (whole words, case-insensitive).
        In fr, es and pt, a single-word "mauvais"/"correct" pair also covers the -s plural.
        Output: list of (source regex or None, regex of the wrong term, correct term),
        longest patterns first so that longer expressions are replaced before their parts.
        """
        import re
        regles = []
        if not isinstance(donnees, dict) or not isinstance(donnees.get(langue), list):
            return regles
        for r in donnees[langue]:
            if not isinstance(r, dict):
                continue
            # Step 1: collect the (wrong, correct) pairs of the rule, either from the
            # "remplacer" mapping or from "mauvais"/"correct" (+ automatic -s plural).
            paires = []
            if isinstance(r.get("remplacer"), dict):
                paires = [(k, v) for k, v in r["remplacer"].items() if isinstance(k, str) and isinstance(v, str) and k]
            elif isinstance(r.get("mauvais"), str) and isinstance(r.get("correct"), str) and r["mauvais"]:
                paires = [(r["mauvais"], r["correct"])]
                m, c = r["mauvais"], r["correct"]
                if langue in ("fr", "es", "pt") and " " not in m and " " not in c and not m.endswith("s"):
                    paires.append((m + "s", c + "s"))
            if not paires:
                continue
            # Step 2: optional condition on the original text ("source": str or list of words).
            sources = r.get("source")
            if isinstance(sources, str):
                sources = [sources]
            motif_source = None
            if isinstance(sources, list) and sources:
                mots = [re.escape(s) for s in sources if isinstance(s, str) and s]
                if mots:
                    motif_source = re.compile(r"(?<!\w)(?:%s)(?!\w)" % "|".join(mots), re.I)
            # Step 3: one whole-word, case-insensitive regex per pair; the longest expressions
            # first ("toutes les histoires" before "histoires")
            for mauvais, correct in sorted(paires, key=lambda p: -len(p[0])):
                regles.append((motif_source, re.compile(r"(?<!\w)%s(?!\w)" % re.escape(mauvais), re.I), correct))
        # Step 4: global order across all rules: longest pattern first.
        regles.sort(key=lambda r: -len(r[1].pattern))
        return regles

    @staticmethod
    def _meme_casse(modele, texte):
        """Give 'texte' the same case style as 'modele' (the matched wrong term).

        All upper case (2+ characters) -> upper case; initial capital -> initial
        capital; otherwise 'texte' is returned unchanged. Used by corriger().
        """
        if modele.isupper() and len(modele) > 1:
            return texte.upper()
        if modele[:1].isupper():
            return texte[:1].upper() + texte[1:]
        return texte

    def corriger(self, original, traduit):
        """Apply corrections.json to an online translation (never to dictionary entries).

        Inputs: original = the source text (used by the "source" conditions),
        traduit = the online/cached translation. Output: the corrected translation.
        Each replacement keeps the case style of the replaced word.
        """
        if not self.corrections or not isinstance(traduit, str) or not traduit:
            return traduit
        for motif_source, motif, correct in self.corrections:
            # Skip the rule if its "source" words are not in the original text.
            if motif_source is not None and not motif_source.search(original):
                continue
            traduit = motif.sub(lambda m, c=correct: self._meme_casse(m.group(0), c), traduit)
        return traduit

    def _voter(self, texte):
        """Count in which language the script's texts are written (automatic detection).

        Every dictionary language whose column contains 'texte' gets one vote.
        """
        for code, textes in self.colonnes.items():
            if texte in textes:
                self.votes[code] = self.votes.get(code, 0) + 1

    def langue_detectee(self):
        """Return the automatically detected script language ("" while unknown).

        The result is frozen as soon as it is recognised without ambiguity: otherwise a
        change of mind would send the already translated texts to another language pair.
        """
        if self.detectee or not self.votes:
            return self.detectee
        classement = sorted(self.votes.items(), key=lambda kv: -kv[1])
        premier = classement[0]
        second = classement[1][1] if len(classement) > 1 else 0
        # At least 2 recognised texts and a clear lead (identical words count for
        # several languages: "Options", "Zone"...)
        if premier[1] >= 2 and premier[1] > second:
            self.detectee = premier[0]
            _journal("langue du script reconnue : %s (%s)" % (self.detectee, dict(classement)))
        return self.detectee

    @staticmethod
    def _depuis_termes(termes, langue, source, seule=False):
        """Build an {original: translation} table from a multilingual "termes" list.

        Format: "termes": [{"en": "Close", "fr": "Fermer", ...}, ...]; 'langue' is the
        output language, 'source' the script language ("" if unknown).
        seule=True (input language chosen in the menu): only the source column is read.
        Otherwise every text of every language gives the translation into the target
        language, whatever the script's language; on a duplicate (same text in two
        notions), the script's declared language comes first, then the column order.
        """
        res = {}
        if not isinstance(termes, list):
            return res
        # Step 1: list of source columns to read: the declared source first, then (unless
        # seule) every other language code found in the rows ("_" keys are comments).
        langues = []
        if source:
            langues.append(source)
        if not seule:
            for t in termes:
                if isinstance(t, dict):
                    for code in t:
                        if code not in langues and not code.startswith("_"):
                            langues.append(code)
        # Step 2: automatic detection: a text already valid in the target language is not
        # translated (a script written in English keeps "Surface", even though "Surface" (fr)
        # means "Area"). With a chosen input language, we know what the text is.
        deja = set()
        if not seule:
            for t in termes:
                if isinstance(t, dict) and isinstance(t.get(langue), str):
                    deja.add(t[langue])
        # Step 3: map each text of each source column to the row's target text; the first
        # mapping found for a text wins (setdefault).
        for code in langues:
            if code == langue:
                continue
            for t in termes:
                if not isinstance(t, dict):
                    continue
                cible = t.get(langue)
                texte = t.get(code)
                if (isinstance(cible, str) and cible and isinstance(texte, str) and texte
                        and texte != cible and texte not in deja):
                    res.setdefault(texte, cible)
        return res

    def _casse(self, texte):
        """Case-insensitive dictionary look-up, adapting the case of the result.

        Output: the translation with the case style of 'texte' (all caps, lower-case
        initial, capital initial), or None if the text is unknown.
        """
        t = self.table_min.get(texte.casefold())
        if t is None:
            return None
        if texte.isupper():
            return t.upper()
        # (German keeps the capital letter of its nouns)
        if texte[:1].islower() and t[:1].isupper() and self.langue != "de":
            return t[:1].lower() + t[1:]
        if texte[:1].isupper() and t[:1].islower():
            return t[:1].upper() + t[1:]
        return t

    @staticmethod
    def _compiler_modeles(table):
        """Compile the dictionary keys containing {placeholders} into regex templates.

        Input: the lookup table. Output: list of (compiled regex matching the whole
        text, translation template for str.format), longest patterns first.
        Example: "{n} elements selected" -> "^(?P<n>number) elements selected$"; the
        translation "{n} éléments sélectionnés" is then filled with the captured values.
        """
        import re
        modeles = []
        for cle, val in table.items():
            if "{" not in cle:
                continue
            # Step 1: placeholder names and the literal pieces between them.
            noms = re.findall(r"\{([A-Za-z_][A-Za-z0-9_]*)\}", cle)
            if not noms:
                continue
            morceaux = re.split(r"\{[A-Za-z_][A-Za-z0-9_]*\}", cle)
            # Step 2: build the pattern: escaped literal, then a capture group per placeholder.
            motif = ""
            for i, m in enumerate(morceaux):
                motif += re.escape(m)
                if i < len(noms):
                    nom = noms[i]
                    if nom in noms[:i]:
                        # Same placeholder used twice: back-reference to the first capture.
                        motif += "(?P=%s)" % nom
                    elif nom in _NOMS_NOMBRE:
                        # {n} = a number: "{n} elements" must not swallow
                        # "Only the selected elements" ("Only the selected éléments")
                        motif += r"(?P<%s>[-+]?\d+(?:[.,  ]\d+)*)" % nom
                    else:
                        # {name} = a short text without line break (lazy, at most 80 characters)
                        motif += r"(?P<%s>[^\n]{1,80}?)" % nom
            # Step 3: anchor the pattern on the whole text; an invalid pattern is skipped.
            try:
                modeles.append((re.compile("^" + motif + "$", re.S), val))
            except re.error:
                pass
        # Step 4: the longest (most precise) templates first
        modeles.sort(key=lambda m: -len(m[0].pattern))
        return modeles

    # --- translation ------------------------------------------------------------------
    def traduire(self, texte):
        """Translate one displayed text into the current output language.

        Input: any value (only non-blank str are translated). Output: the text to
        display; the original text itself when no translation is requested, when the
        text is unknown, or while an online translation is pending.
        Order: result cache -> dictionaries (_traduire) -> text already valid in the
        output language -> online translation (self.auto) -> original text.
        """
        # Step 1: no output language, not a string, or blank -> unchanged.
        if not self.langue or not isinstance(texte, str) or not texte.strip():
            return texte
        # Step 2: result cache (emptied at every reload and when online translations arrive).
        r = self.cache.get(texte)
        if r is not None:
            return r
        # Step 3: each new text votes once for the script's language (automatic detection).
        if texte not in self.vus:
            self.vus.add(texte)
            self._voter(texte)
        # Step 4: dictionaries (exact, punctuation, templates, case, line by line).
        r = self._traduire(texte, True)
        if r is None and texte.strip() in self.colonnes.get(self.langue, ()):
            r = texte    # Already valid in the output language ("Options", "Zone"...)
        if r is None:
            # Step 5: sentence outside the dictionaries: online translation through the local
            # engine (if enabled). While pending, the original text stays displayed and is not
            # cached: it will be translated again when the answer arrives.
            r, en_attente = self.auto.traduire(texte)
            if en_attente:
                return texte
            if r is None:
                # No translation at all: keep the original and record it as missing.
                r = texte
                self._noter_manquant(texte)
        # Step 6: remember the final result for this text.
        self.cache[texte] = r
        return r

    def _traduire(self, texte, lignes):
        """Dictionary-only translation of one text; returns None if unknown.

        Inputs: texte = text to translate; lignes = True to allow splitting a
        multi-line text and translating each line separately (False for the lines
        themselves, to avoid recursion). Missing lines are recorded via _noter_manquant.
        """
        # Step 1: exact match.
        t = self.table.get(texte)
        if t is not None:
            return t
        # Step 2: surrounding spaces and trailing punctuation: "Nom :" -> "Name:" if "Nom" is known.
        # The core is looked up exactly, then as a template, then case-insensitively.
        coeur, avant, apres = self._decouper(texte)
        if coeur != texte and coeur:
            t = self.table.get(coeur)
            if t is None:
                t = self._modele(coeur)
            if t is None:
                t = self._casse(coeur)
            if t is not None:
                # Adapt the punctuation spacing to the output language.
                if self.langue == "fr":
                    # French typography: a space before : ? ! ;
                    if apres[:1] in (":", "?", "!", ";"):
                        apres = " " + apres
                else:
                    # Other languages: no-break spaces become normal spaces and the space
                    # before : ? ! ; is removed.
                    apres = apres.replace(" ", " ")
                    for p in (" :", " ?", " !", " ;"):
                        if apres.startswith(p):
                            apres = apres[1:]
                            break
                return avant + t + apres
        # Step 3: whole text as a {placeholder} template.
        t = self._modele(texte)
        if t is not None:
            return t
        # Step 4: whole text, case-insensitive.
        t = self._casse(texte)
        if t is not None:
            return t
        # Step 5: multi-line text: translate line by line; succeed if at least one line
        # was translated (unknown lines stay as they are and are recorded as missing).
        if lignes and "\n" in texte:
            parties = texte.split("\n")
            res = []
            change = False
            for p in parties:
                tp = self._traduire(p, False) if p.strip() else None
                if tp is None:
                    res.append(p)
                    if p.strip():
                        self._noter_manquant(p)
                else:
                    res.append(tp)
                    change = True
            if change:
                return "\n".join(res)
        return None

    @staticmethod
    def _decouper(texte):
        """Split a text into (core, leading part, trailing part).

        The leading part is the leading whitespace; the trailing part gathers the
        trailing whitespace, the final punctuation (: ? ! ; . and the ellipsis
        character), a final "..." and the spaces before them. Example:
        "  Name :" -> ("Name", "  ", " :"). Used to look up the bare core in the
        dictionaries and put the decoration back around its translation.
        """
        # Step 1: strip the leading and trailing whitespace.
        debut = len(texte) - len(texte.lstrip())
        fin = len(texte.rstrip())
        coeur = texte[debut:fin]
        avant = texte[:debut]
        apres = texte[fin:]
        # Step 2: move the final punctuation characters one by one into the trailing part,
        # stopping at ".." so that a "..." ending is handled as a whole below.
        while coeur and coeur[-1] in ":?!;.…" and not coeur.endswith(".."):
            # (the ".." test keeps the "..." of menu entries that open a dialog together)
            apres = coeur[-1] + apres
            coeur = coeur[:-1]
        # Step 3: a final "..." goes to the trailing part as a whole.
        if coeur.endswith("..."):
            apres = "..." + apres
            coeur = coeur[:-3]
        # Step 4: spaces left between the core and the punctuation ("Nom :") also go to
        # the trailing part.
        espace = len(coeur) - len(coeur.rstrip())
        if espace:
            apres = coeur[len(coeur) - espace:] + apres
            coeur = coeur.rstrip()
        return coeur, avant, apres

    def _modele(self, texte):
        """Translate 'texte' with the first {placeholder} template that matches it.

        Output: the template filled with the captured values, or None if no template
        matches (or if filling the first matching template fails).
        """
        for regex, gabarit in self.modeles:
            m = regex.match(texte)
            if m:
                try:
                    return gabarit.format(**m.groupdict())
                except Exception:
                    return None
        return None

    # --- missing texts ----------------------------------------------------------------
    def _noter_manquant(self, texte):
        """Record a text missing from the dictionaries (only when collection is enabled).

        Texts without any letter or longer than 2000 characters are ignored. The texts
        are kept in memory and written later by ecrire_manquants().
        """
        if not self.collecte or not self.langue:
            return
        if not any(c.isalpha() for c in texte) or len(texte) > 2000:
            return
        d = self.manquants.setdefault(self.langue, {})
        if texte not in d:
            d[texte] = ""
            self.manquants_modifies = True

    def ecrire_manquants(self):
        """Write the recorded missing texts to manquants\\<tool>.json.

        The file has the same shape as a script dictionary ("langue_source" and
        "traductions": {language: {text: ""}}) so the user can fill it in and use it
        as a dictionary. Existing entries are kept (merge). Called by the Tk timer, when
        a window is destroyed and at process exit; does nothing if nothing new.
        """
        if not self.manquants_modifies:
            return
        import json
        self.manquants_modifies = False
        try:
            # Step 1: read the existing file (if any) to merge with it.
            dossier = os.path.join(self.racine, "manquants")
            os.makedirs(dossier, exist_ok=True)
            chemin = os.path.join(dossier, self._nom_outil() + ".json")
            existant = self._lire_json(chemin)
            if not isinstance(existant, dict):
                existant = {}
            # Step 2: add the new texts with an empty translation, never overwriting one.
            existant.setdefault("langue_source", self.langue_source or "")
            trad = existant.setdefault("traductions", {})
            for langue, textes in self.manquants.items():
                cible = trad.setdefault(langue, {})
                for t in textes:
                    cible.setdefault(t, "")
            # Step 3: atomic write (temporary file, then replace).
            tmp = chemin + ".tmp"
            with open(tmp, "w", encoding="utf-8") as f:
                json.dump(existant, f, ensure_ascii=False, indent=2, sort_keys=True)
            os.replace(tmp, chemin)
        except Exception as e:
            _journal("ecriture des manquants impossible : %r" % e)


class _Automatique(object):
    """Online translation of the texts the dictionaries do not know.

    The texts are sent in the background (separate worker thread, never in the Tk
    loop) to the local engine TapirTranslatorMoteur.exe (HTTP on 127.0.0.1, started by
    the add-on), which forwards them to the online translator chosen in the menu
    (DeepL, Microsoft, Google, MyMemory, LibreTranslate). The original text stays
    displayed while waiting, then the translation appears by itself (the Tk timer asks
    a_reappliquer() and re-applies the texts).
    Results are kept in cache\\<translator>\\<source>-<target>.json, shared by every
    script: the next time, the display is immediate. A dictionary always wins over
    the online translation.

    Threading: the Tk thread calls traduire() / a_reappliquer(); the worker thread
    runs _travailler(). Shared state is protected by self.verrou.
    """

    LOT = 16                  # Maximum number of texts per request to the engine
    PAUSE_PANNE = 15.0        # Engine/service unavailable: new attempt after this delay (s)
    DELAI_DETECTION = 2.0     # Script language unknown: wait this long before sending without a language (s)

    def __init__(self, racine, moteur):
        """Create the helper (inactive until configurer() enables it).

        Inputs: racine = data folder; moteur = the owning _Moteur (used for language
        detection and term corrections). The worker thread is started lazily.
        """
        import threading
        self.racine = racine
        self.moteur = moteur
        self.actif = False        # Online translation enabled
        self.source = ""          # Source language ("" = use the detected language)
        self.cible = ""           # Target language
        self.port = "8779"        # Port of the local engine
        self.verrou = threading.Lock()       # Protects the state shared with the worker thread
        self.evenement = threading.Event()   # Set when texts wait in self.attente
        self.memoire = {}         # (translator, source, target) -> {text: translation, "" = failure}
        self.attente = []         # [(translator, source, target, text)] waiting to be sent
        self.demandes = set()     # Requests already in the queue or in progress
        self.modifies = set()     # (translator, source, target) caches to save to disk
        self.nouveau = False      # New translations have arrived (display to recompute)
        self.panne_jusqua = 0.0   # time.time() until which no new request is made (outage)
        self.panne_signalee = False   # The current outage has already been logged
        self.fournisseur = "mymemory"  # Online translator chosen in the menu
        self.echecs_en_panne = False   # Texts were refused during the outage (retry when it ends)
        self.debut_detection = None    # time.time() of the first text sent while the language was unknown
        self.attend_detection = False  # Texts are waiting for the language detection
        self.fil = None           # Worker thread (created on first activation)

    def configurer(self, actif, source, cible, port, fournisseur="mymemory"):
        """Apply the current settings (called by _Moteur.recharger_si_change).

        Inputs: actif = online translation on/off, source / cible = language codes,
        port = engine port ("" keeps the current one), fournisseur = translator name.
        Starts the daemon worker thread the first time the helper is enabled.
        """
        self.actif = actif
        self.source = source
        self.cible = cible
        self.fournisseur = fournisseur or "mymemory"
        if port:
            self.port = port
        if actif and self.fil is None:
            import threading
            self.fil = threading.Thread(target=self._travailler, name="TapirTranslatorAuto", daemon=True)
            self.fil.start()

    @staticmethod
    def traduisible(texte):
        """Tell whether a text may be sent to the online translator.

        Data displayed in Labels (paths, file names, identifiers, codes) is never sent.
        Output: False for texts shorter than 2 / longer than 600 characters or with
        fewer than 2 letters, for texts containing a path separator, URL, "@" or a known
        file extension, and for single words containing digits, "_" or an inner ".",
        or made of at most 6 capital letters (codes); True otherwise.
        """
        import re
        t = texte.strip()
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

    def _chemin(self, fournisseur, source, cible):
        """Return the cache file path cache\\<translator>\\<source or auto>-<target>.json."""
        # One cache per translator: switching translator gives back its own translations
        return os.path.join(self.racine, "cache", fournisseur, "%s-%s.json" % (source or "auto", cible))

    def _charger(self, cle):
        """Load the cache file of 'cle' = (translator, source, target) into self.memoire.

        Done once per key; a missing or invalid file gives an empty table.
        Must be called with self.verrou held.
        """
        if cle in self.memoire:
            return
        d = _Moteur._lire_json(self._chemin(*cle))
        trad = d.get("traductions") if isinstance(d, dict) else None
        self.memoire[cle] = dict(trad) if isinstance(trad, dict) else {}

    def traduire(self, texte):
        """Look up or request the online translation of one text (Tk thread).

        Output: (translation or None, en_attente). en_attente = True means the text
        has been queued (or waits for the language detection): the caller displays
        the original for now and must not cache the result.
        (None, False) means "no online translation" (disabled, not translatable,
        same language, known failure, or engine in outage).
        """
        # Step 1: disabled, or data that must not leave the computer.
        if not self.actif or not self.traduisible(texte):
            return None, False
        import time
        # Step 2: source language: from the settings, else the detected one.
        source = self.source or self.moteur.langue_detectee()
        if not source:
            # Script language not recognised yet: wait a little for the other texts
            # (after DELAI_DETECTION the text is sent with an empty source, "auto").
            if self.debut_detection is None:
                self.debut_detection = time.time()
            if time.time() - self.debut_detection < self.DELAI_DETECTION:
                self.attend_detection = True
                return None, True
        if source == self.cible:
            return None, False
        cle = (self.fournisseur, source, self.cible)
        with self.verrou:
            # Step 3: already in the cache ("" = known failure -> None).
            self._charger(cle)
            t = self.memoire[cle].get(texte)
            if t is not None:
                # Term corrections are applied at display time: a newly added correction
                # also applies to the translations already in the cache
                return (self.moteur.corriger(texte, t) if t else None), False
            # Step 4: during an outage, do not queue; remember to retry when it ends.
            if time.time() < self.panne_jusqua:
                self.echecs_en_panne = True
                return None, False
            # Step 5: queue the request once and wake the worker thread.
            demande = (self.fournisseur, source, self.cible, texte)
            if demande not in self.demandes:
                self.demandes.add(demande)
                self.attente.append(demande)
                self.evenement.set()
        return None, True

    def a_reappliquer(self):
        """Called by the Tk loop: True if the display must be recomputed.

        That is the case when new translations arrived, when an outage has ended
        (texts refused during it can now be requested), or when texts waiting for the
        language detection can now be sent (language found or delay elapsed).
        """
        import time
        r = False
        with self.verrou:
            if self.nouveau:
                self.nouveau = False
                r = True
            if self.echecs_en_panne and time.time() >= self.panne_jusqua:
                self.echecs_en_panne = False
                r = True
        if self.attend_detection and (self.moteur.langue_detectee() or
                                      time.time() - (self.debut_detection or 0) >= self.DELAI_DETECTION):
            self.attend_detection = False
            r = True
        return r

    def _travailler(self):
        """Worker thread loop: send the queued texts to the local engine.

        Protocol: POST http://127.0.0.1:<port>/traduire with JSON
        {"fournisseur", "source", "cible", "textes": [...]}; the answer is
        {"traductions": [...]} in the same order. HTTP 400 (language refused, unknown
        translator) is stored as a permanent failure; any other error (503: quota, key,
        service network; engine unreachable) starts an outage of PAUSE_PANNE seconds.
        """
        import json
        import time
        import urllib.error
        import urllib.request
        # No proxy for 127.0.0.1 (the computer's HTTP_PROXY must not be used)
        ouvreur = urllib.request.build_opener(urllib.request.ProxyHandler({}))
        while True:
            # Step 1: wait for work, then take at most LOT texts from the queue.
            self.evenement.wait()
            time.sleep(0.05)   # Let the other texts of the same window arrive
            with self.verrou:
                lot = self.attente[:self.LOT]
                del self.attente[:self.LOT]
                if not self.attente:
                    self.evenement.clear()
            # Step 2: group the texts by (translator, source, target): one request per group.
            groupes = {}
            for fournisseur, source, cible, texte in lot:
                groupes.setdefault((fournisseur, source, cible), []).append(texte)
            for (fournisseur, source, cible), textes in groupes.items():
                cle = (fournisseur, source, cible)
                try:
                    # Step 3: send the request and check that one translation came back per text
                    # (blank answers are stored as "" = failure).
                    corps = json.dumps({"fournisseur": fournisseur, "source": source, "cible": cible,
                                        "textes": textes}, ensure_ascii=False).encode("utf-8")
                    requete = urllib.request.Request(
                        "http://127.0.0.1:%s/traduire" % self.port, data=corps,
                        headers={"Content-Type": "application/json; charset=utf-8"})
                    with ouvreur.open(requete, timeout=120) as reponse:
                        res = json.loads(reponse.read().decode("utf-8"))
                    trads = res.get("traductions") if isinstance(res, dict) else None
                    if not isinstance(trads, list) or len(trads) != len(textes):
                        raise ValueError("reponse incomplete")
                    resultats = [t if isinstance(t, str) and t.strip() else "" for t in trads]
                except Exception as e:
                    # Step 4: failure. The engine's JSON body may carry an "erreur" message.
                    detail = repr(e)
                    if isinstance(e, urllib.error.HTTPError):
                        try:
                            detail = json.loads(e.read().decode("utf-8")).get("erreur", detail)
                        except Exception:
                            pass
                        if e.code == 400:
                            # Language refused, unknown translator: failure stored, no new attempt
                            _journal("traduction refusee (%s) : %s" % (fournisseur, detail))
                            with self.verrou:
                                self._charger(cle)
                                for t in textes:
                                    self.memoire[cle][t] = ""
                                    self.demandes.discard((fournisseur, source, cible, t))
                            continue
                    # 503 (quota, key, service network) or engine unreachable: the original text
                    # is kept, new attempt later; logged only once while the outage lasts
                    with self.verrou:
                        for t in textes:
                            self.demandes.discard((fournisseur, source, cible, t))
                        if not self.panne_signalee:
                            self.panne_signalee = True
                            _journal("traduction en ligne indisponible (%s, port %s) : %s ; nouvel essai toutes les %d s"
                                     % (fournisseur, self.port, detail, int(self.PAUSE_PANNE)))
                        self.panne_jusqua = time.time() + self.PAUSE_PANNE
                        self.echecs_en_panne = True
                    continue
                # Step 5: success: end of outage (logged), store the results in memory, mark
                # the cache file to save and tell the Tk loop that new translations arrived.
                with self.verrou:
                    if self.panne_signalee:
                        self.panne_signalee = False
                        _journal("traduction en ligne retablie (%s)" % fournisseur)
                    self._charger(cle)
                    m = self.memoire[cle]
                    for t, r in zip(textes, resultats):
                        m[t] = r
                        self.demandes.discard((fournisseur, source, cible, t))
                    self.modifies.add(cle)
                    self.nouveau = True
            # Step 6: save the modified caches to disk (outside the lock).
            self._enregistrer()

    def _enregistrer(self):
        """Write the modified in-memory caches to their JSON files (worker thread).

        Step 1 copies the modified tables under the lock; the files are then written
        without holding it. Each file is merged with its current content on disk and
        written atomically through a per-process temporary file.
        """
        import json
        with self.verrou:
            paires = list(self.modifies)
            self.modifies.clear()
            copies = {p: dict(self.memoire.get(p, {})) for p in paires}
        for (fournisseur, source, cible), traductions in copies.items():
            try:
                chemin = self._chemin(fournisseur, source, cible)
                os.makedirs(os.path.dirname(chemin), exist_ok=True)
                # Step 2: another script may have written the same cache: merge (our values win)
                existant = _Moteur._lire_json(chemin)
                if isinstance(existant, dict) and isinstance(existant.get("traductions"), dict):
                    fusion = dict(existant["traductions"])
                    fusion.update(traductions)
                    traductions = fusion
                # Step 3: atomic write; the pid in the temporary name avoids clashes between processes.
                tmp = chemin + ".%d.tmp" % os.getpid()
                with open(tmp, "w", encoding="utf-8") as f:
                    json.dump({"fournisseur": fournisseur, "source": source, "cible": cible,
                               "traductions": traductions},
                              f, ensure_ascii=False, indent=1, sort_keys=True)
                os.replace(tmp, chemin)
            except Exception as e:
                _journal("cache de traduction non enregistre : %r" % e)


_moteur = None          # The single _Moteur of this process (created by _installer)
_intermediaires = []   # One _Intermediaire per open Tk root window


# =====================================================================================
#  Proxy in front of the Tcl interpreter of a Tk window
# =====================================================================================

class _Intermediaire(object):
    """Proxy that replaces root.tk (the Tcl interpreter object of a Tk root window).

    tkinter sends every Tcl command through root.tk.call(...); this proxy inspects
    each command before it runs (_avant), translates the displayed texts in its
    arguments and records (original, displayed) pairs in a registry, so that the
    values read back by the script are given back in the original language. Tcl
    variables shown by display widgets (-textvariable) are handled through the
    setvar/getvar methods. Everything that is not intercepted goes unchanged to the
    real interpreter (self._reel).
    """

    def __init__(self, reel):
        """Wrap the real Tcl interpreter 'reel' (the original root.tk)."""
        self._reel = reel
        # Key -> [original text, displayed text]. The key is
        # (kind, widget path, sub-id, option), kind being "opt", "titre", "menu",
        # "onglet", "entete" or "canevas".
        self._registre = {}
        self._variables = {}   # Name of a displayed Tcl variable -> [original, displayed]

    def __getattr__(self, nom):
        """Forward every attribute not defined here to the real interpreter."""
        return getattr(self._reel, nom)

    # --- call ---------------------------------------------------------------------------
    def call(self, *args):
        """Run a Tcl command, translating its displayed texts on the way.

        Step 1: _avant() may rewrite the arguments (translations) and may return a
        post-processing function. Step 2: the real command runs. Step 3: the
        post-processing function (if any) converts the result, e.g. gives back the
        original text of a cget. Interception errors are logged and ignored: the
        command then runs with its arguments as they are at that point.
        """
        # tkinter may pass the whole command as a single tuple argument.
        if len(args) == 1 and isinstance(args[0], tuple):
            args = args[0]
        apres = None
        try:
            argv = list(args)
            apres = self._avant(argv)
            args = tuple(argv)
        except Exception as e:
            _journal("interception call : %r" % e)
            apres = None
        res = self._reel.call(*args)
        if apres is not None:
            try:
                res = apres(res)
            except Exception as e:
                _journal("apres call : %r" % e)
        return res

    def _tr(self, valeur):
        """Translate 'valeur' if it is a str, otherwise return it unchanged."""
        if isinstance(valeur, str):
            return _moteur.traduire(valeur)
        return valeur

    def _noter(self, cle, origine, affiche):
        """Record in the registry that 'origine' is displayed as 'affiche' at 'cle'."""
        if isinstance(origine, str):
            self._registre[cle] = [origine, affiche]

    def _rendre(self, cle, res):
        """Convert a value read from Tcl back to the original text.

        If 'res' (str or Tcl object compared through str()) equals the text displayed
        at 'cle', the recorded original is returned; otherwise 'res' unchanged.
        """
        e = self._registre.get(cle)
        if e is not None and isinstance(res, str) and res == e[1]:
            return e[0]
        if e is not None and not isinstance(res, str) and str(res) == e[1]:
            return e[0]
        return res

    def _options(self, argv, debut):
        """Translate the text options (-text, -label) of argv[debut:] in place.

        argv is read as "-option value" pairs from index 'debut'. Output: list of
        (option, original, displayed) for the options that were translated.
        """
        faits = []
        i = debut
        n = len(argv)
        while i + 1 < n:
            opt = argv[i]
            if isinstance(opt, str) and opt in _OPTIONS_TEXTE and isinstance(argv[i + 1], str):
                origine = argv[i + 1]
                affiche = self._tr(origine)
                argv[i + 1] = affiche
                faits.append((opt, origine, affiche))
            i += 2
        return faits

    def _variable_texte(self, argv, debut, classe):
        """Handle a -textvariable option found in argv[debut:] ("-option value" pairs).

        If the widget class 'classe' only displays its variable (Label, Button...),
        the variable is bound (_lier_variable) and its value becomes translated.
        Variables of input widgets (Entry...) are never touched.
        """
        i = debut
        n = len(argv)
        while i + 1 < n:
            if argv[i] == "-textvariable":
                nom = str(argv[i + 1])
                if nom and self._est_affichage(classe):
                    self._lier_variable(nom)
            i += 2

    @staticmethod
    def _est_affichage(classe):
        """True if the Tk class 'classe' is a display-only widget (_CLASSES_AFFICHAGE).

        Accepts creation commands ("label", "ttk::label") and winfo class names
        ("Label", "TLabel"): "ttk::x" is normalised to "tx", comparison is case-insensitive.
        """
        c = str(classe).lower()
        if c.startswith("ttk::"):
            c = "t" + c[5:]
        return c in _CLASSES_AFFICHAGE

    def _lier_variable(self, nom):
        """Start translating the Tcl variable 'nom' (done once per variable).

        If the variable already holds a string, it is translated and the displayed
        value is written to Tcl directly (bypassing the proxy). Otherwise the entry is
        [None, None] and its value will be picked up by reappliquer().
        """
        if nom in self._variables:
            return
        try:
            val = self._reel.globalgetvar(nom)
        except Exception:
            val = None
        if isinstance(val, str):
            aff = self._tr(val)
            self._variables[nom] = [val, aff]
            if aff != val:
                self._reel.globalsetvar(nom, aff)
        else:
            self._variables[nom] = [None, None]

    def _avant(self, argv):
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
            chemin = str(argv[2])
            cle = ("titre", chemin, None, None)
            # Wm.title() queries with a 4th argument set to None
            if n >= 4 and argv[3] is not None:
                # Setting the title: translate and record it.
                origine = argv[3]
                argv[3] = self._tr(origine)
                self._noter(cle, origine, argv[3])
                return None
            # Reading the title: give back the original.
            return lambda r: self._rendre(cle, r)

        # Case 2: message boxes and file dialogs (-title, -message, -detail); nothing recorded
        if a0 in _DIALOGUES:
            i = 1
            while i + 1 < n:
                if argv[i] in _OPTIONS_DIALOGUE:
                    argv[i + 1] = self._tr(argv[i + 1])
                i += 2
            return None

        # Case 3: command of an existing widget: ".path sub-command ..."
        if a0.startswith("."):
            return self._sous_commande(argv, a0, a1, n)

        # Case 4: creation of a widget: "class .path -option value ..."
        if a1.startswith("."):
            chemin = a1
            for opt, origine, affiche in self._options(argv, 2):
                self._noter(("opt", chemin, None, opt), origine, affiche)
            self._variable_texte(argv, 2, a0)
        return None

    def _sous_commande(self, argv, chemin, sous, n):
        """Handle ".path sub-command ..." for the sub-commands that carry displayed texts.

        Inputs: argv = command (mutable), chemin = widget path, sous = sub-command,
        n = len(argv). Output: None or a result post-processing function (see _avant).
        Covers configure / cget, menu add / insert / entryconfigure / entrycget,
        ttk.Notebook add / tab, ttk.Treeview heading, canvas create text /
        itemconfigure / itemcget.
        """
        # --- widget options: configure / cget
        if sous == "configure":
            if n >= 4:
                # Setting options: translate them, and handle a -textvariable (the widget
                # class is asked to Tcl to know if it is a display widget).
                for opt, origine, affiche in self._options(argv, 2):
                    self._noter(("opt", chemin, None, opt), origine, affiche)
                if "-textvariable" in argv[2::2]:
                    try:
                        classe = self._reel.call("winfo", "class", chemin)
                    except Exception:
                        classe = ""
                    self._variable_texte(argv, 2, classe)
            elif n == 3 and argv[2] in _OPTIONS_TEXTE:
                # "configure -text" (query) returns a 5-item description: give back the original value.
                cle = ("opt", chemin, None, argv[2])

                def rendre(r, cle=cle):
                    # r = (option, database name, class, default, value)
                    try:
                        if isinstance(r, tuple) and len(r) == 5:
                            return r[:4] + (self._rendre(cle, r[4]),)
                    except Exception:
                        pass
                    return r
                return rendre
            return None

        if sous == "cget" and n == 3 and argv[2] in _OPTIONS_TEXTE:
            # The script reads its own text back: give back the original.
            cle = ("opt", chemin, None, argv[2])
            return lambda r: self._rendre(cle, r)

        # --- menus: add / insert / entryconfigure / entrycget
        # Menu entries are recorded by their numeric index.
        if sous == "add" and n >= 3 and argv[2] in _TYPES_MENU:
            faits = self._options(argv, 3)
            if not faits:
                return None

            def noter(r, faits=faits):
                # After the add, the new entry is the last one ("end").
                idx = self._reel.call(chemin, "index", "end")
                for opt, origine, affiche in faits:
                    self._noter(("menu", chemin, str(idx), opt), origine, affiche)
                return r
            return noter
        if sous == "insert" and n >= 4 and argv[3] in _TYPES_MENU:
            faits = self._options(argv, 4)
            if not faits:
                return None
            position = argv[2]

            def noter(r, faits=faits, position=position):
                # After the insert, the new entry sits at the requested position.
                idx = self._reel.call(chemin, "index", position)
                for opt, origine, affiche in faits:
                    self._noter(("menu", chemin, str(idx), opt), origine, affiche)
                return r
            return noter
        if sous == "entryconfigure" and n >= 5:
            # The entry may be given as a label or pattern: resolve it to a numeric index.
            try:
                idx = str(self._reel.call(chemin, "index", argv[2]))
            except Exception:
                idx = str(argv[2])
            for opt, origine, affiche in self._options(argv, 3):
                self._noter(("menu", chemin, idx, opt), origine, affiche)
            return None
        if sous == "entrycget" and n == 4 and argv[3] in _OPTIONS_TEXTE:
            try:
                idx = str(self._reel.call(chemin, "index", argv[2]))
            except Exception:
                idx = str(argv[2])
            cle = ("menu", chemin, idx, argv[3])
            return lambda r: self._rendre(cle, r)

        # --- ttk.Notebook tabs: add .child -text ... / tab id -text ...
        # (a tab is designated by its window or by its number: the number is recorded)
        if sous == "add" and n >= 3 and str(argv[2]).startswith("."):
            enfant = argv[2]
            faits = self._options(argv, 3)
            if not faits:
                return None

            def noter(r, faits=faits, enfant=enfant):
                # After the add, ask the notebook for the tab number of the child window.
                idx = str(self._reel.call(chemin, "index", enfant))
                for opt, origine, affiche in faits:
                    self._noter(("onglet", chemin, idx, opt), origine, affiche)
                return r
            return noter
        if sous == "tab" and n >= 4:
            try:
                onglet = str(self._reel.call(chemin, "index", argv[2]))
            except Exception:
                onglet = str(argv[2])
            # "tab id -text" alone is a query (give back the original); otherwise options are set.
            if n == 4 and argv[3] in _OPTIONS_TEXTE:
                cle = ("onglet", chemin, onglet, argv[3])
                return lambda r: self._rendre(cle, r)
            for opt, origine, affiche in self._options(argv, 3):
                self._noter(("onglet", chemin, onglet, opt), origine, affiche)
            return None

        # --- ttk.Treeview column headings (the rows are data: never touched)
        if sous == "heading" and n >= 4:
            colonne = str(argv[2])
            # "heading col -text" alone is a query; otherwise options are set.
            if n == 4 and argv[3] in _OPTIONS_TEXTE:
                cle = ("entete", chemin, colonne, argv[3])
                return lambda r: self._rendre(cle, r)
            for opt, origine, affiche in self._options(argv, 3):
                self._noter(("entete", chemin, colonne, opt), origine, affiche)
            return None

        # --- canvas texts: create text x y -text ... / itemconfigure / itemcget
        if sous == "create" and n >= 3 and argv[2] == "text":
            # Coordinates may be negative ("-5"): the 1st option starts with "-letter"
            debut = 3
            while debut < n and not (isinstance(argv[debut], str) and len(argv[debut]) > 1
                                     and argv[debut][0] == "-" and argv[debut][1].isalpha()):
                debut += 1
            faits = self._options(argv, debut)
            if not faits:
                return None

            def noter(r, faits=faits):
                # The result of "create" is the new item id, used as sub-id.
                for opt, origine, affiche in faits:
                    self._noter(("canevas", chemin, str(r), opt), origine, affiche)
                return r
            return noter
        if sous == "itemconfigure" and n >= 5:
            item = str(argv[2])
            for opt, origine, affiche in self._options(argv, 3):
                self._noter(("canevas", chemin, item, opt), origine, affiche)
            return None
        if sous == "itemcget" and n == 4 and argv[3] in _OPTIONS_TEXTE:
            cle = ("canevas", chemin, str(argv[2]), argv[3])
            return lambda r: self._rendre(cle, r)
        return None

    # --- Tcl variables (StringVar...) ----------------------------------------------------
    def _ecrire_variable(self, fonction, nom, valeur, reste):
        """Write a Tcl variable, translating the value if the variable is displayed.

        Inputs: fonction = real setvar/globalsetvar, nom = variable name, valeur =
        value set by the script, reste = extra arguments. For a bound variable the
        new original and displayed values are recorded, and the translation is written.
        """
        e = self._variables.get(str(nom))
        if e is not None and isinstance(valeur, str):
            aff = self._tr(valeur)
            e[0], e[1] = valeur, aff
            return fonction(nom, aff, *reste)
        return fonction(nom, valeur, *reste)

    def _lire_variable(self, fonction, nom, reste):
        """Read a Tcl variable; give back the original if the displayed translation is read.

        Inputs: fonction = real getvar/globalgetvar, nom = variable name, reste = extra
        arguments. If the value differs from the recorded displayed text (changed by
        another way), it is returned unchanged.
        """
        r = fonction(nom, *reste)
        e = self._variables.get(str(nom))
        if e is not None and e[1] is not None and isinstance(r, str) and r == e[1]:
            return e[0]
        return r

    def globalsetvar(self, nom, valeur, *reste):
        """Intercepted interpreter method used by tkinter (Variable.set)."""
        return self._ecrire_variable(self._reel.globalsetvar, nom, valeur, reste)

    def setvar(self, nom, valeur, *reste):
        """Intercepted interpreter method (Misc.setvar)."""
        return self._ecrire_variable(self._reel.setvar, nom, valeur, reste)

    def globalgetvar(self, nom, *reste):
        """Intercepted interpreter method used by tkinter (StringVar.get)."""
        return self._lire_variable(self._reel.globalgetvar, nom, reste)

    def getvar(self, nom, *reste):
        """Intercepted interpreter method (Misc.getvar)."""
        return self._lire_variable(self._reel.getvar, nom, reste)

    # --- live language change -------------------------------------------------------------
    def reappliquer(self):
        """Re-translate every recorded text and variable of this window (live update).

        Called by the Tk timer after a settings / dictionary change or when online
        translations arrived. For each registry entry: compute the new translation of
        the original; if it differs from what we displayed, check that the widget still
        shows our text (otherwise the script changed it by another way and the entry is
        dropped) and write the new text. Entries of destroyed widgets are dropped.
        """
        reel = self._reel
        # Part 1: widget texts recorded in the registry.
        for cle in list(self._registre.keys()):
            # Step 1: new translation of the original; unchanged -> nothing to do.
            origine, ancien = self._registre[cle]
            nouveau = _moteur.traduire(origine)
            if nouveau == ancien:
                continue
            genre, chemin, sous_id, opt = cle
            try:
                # Step 2: read the text currently displayed, with the real interpreter
                # (no translation / restoration on the way).
                if genre == "opt":
                    actuel = reel.call(chemin, "cget", opt)
                elif genre == "titre":
                    actuel = reel.call("wm", "title", chemin)
                elif genre == "menu":
                    actuel = reel.call(chemin, "entrycget", sous_id, opt)
                elif genre == "onglet":
                    actuel = reel.call(chemin, "tab", sous_id, opt)
                elif genre == "entete":
                    actuel = reel.call(chemin, "heading", sous_id, opt)
                elif genre == "canevas":
                    actuel = reel.call(chemin, "itemcget", sous_id, opt)
                else:
                    continue
                if str(actuel) != ancien:
                    # The text changed without going through us: do not overwrite it
                    del self._registre[cle]
                    continue
                # Step 3: write the new translation with the matching set command.
                if genre == "opt":
                    reel.call(chemin, "configure", opt, nouveau)
                elif genre == "titre":
                    reel.call("wm", "title", chemin, nouveau)
                elif genre == "menu":
                    reel.call(chemin, "entryconfigure", sous_id, opt, nouveau)
                elif genre == "onglet":
                    reel.call(chemin, "tab", sous_id, opt, nouveau)
                elif genre == "entete":
                    reel.call(chemin, "heading", sous_id, opt, nouveau)
                elif genre == "canevas":
                    reel.call(chemin, "itemconfigure", sous_id, opt, nouveau)
                # Step 4: remember what is displayed now.
                self._registre[cle][1] = nouveau
            except Exception:
                # Widget destroyed, menu entry gone...
                self._registre.pop(cle, None)
        # Part 2: displayed Tcl variables.
        for nom in list(self._variables.keys()):
            e = self._variables[nom]
            if e[0] is None:
                # Variable bound before it had a string value: take its current value
                # as the original (it is not translated yet).
                try:
                    val = reel.globalgetvar(nom)
                except Exception:
                    continue
                if isinstance(val, str):
                    e[0], e[1] = val, val
                else:
                    continue
            nouveau = _moteur.traduire(e[0])
            if nouveau == e[1]:
                continue
            # Only overwrite the variable if it still holds the text we displayed.
            try:
                if reel.globalgetvar(nom) == e[1]:
                    reel.globalsetvar(nom, nouveau)
                    e[1] = nouveau
            except Exception:
                self._variables.pop(nom, None)


# =====================================================================================
#  Hooking into tkinter
# =====================================================================================

def _surveiller(racine):
    """Start the live-update timer on the Tk root window 'racine'.

    Every _PERIODE_MS milliseconds, as long as the window lives, the timer reloads
    the settings and dictionaries if they changed, checks whether online translations
    arrived, re-applies the texts of every open window if needed and writes the
    missing texts. The timer re-arms itself with Tk's "after" (runs in the Tk thread).
    """
    def tic():
        """One timer tick (see _surveiller); errors are logged, the timer goes on."""
        try:
            # Step 1: settings / dictionaries / corrections changed?
            change = _moteur.recharger_si_change()
            # Step 2: online translations arrived (or an outage / detection wait ended)?
            if _moteur.auto.a_reappliquer():
                _moteur.cache = {}   # Online translations have arrived: forget the cached originals
                change = True
            # Step 3: re-translate every open window.
            if change:
                for inter in list(_intermediaires):
                    inter.reappliquer()
            # Step 4: save the missing texts collected so far (no-op if nothing new).
            _moteur.ecrire_manquants()
        except Exception as e:
            _journal("surveillance : %r" % e)
        # Step 5: schedule the next tick (fails silently once the window is destroyed).
        try:
            racine.after(_PERIODE_MS, tic)
        except Exception:
            pass
    try:
        racine.after(_PERIODE_MS, tic)
    except Exception as e:
        _journal("after impossible : %r" % e)


def _brancher(tkinter):
    """Patch the tkinter module so that every Tk root window gets a proxy.

    Input: the imported tkinter module. Done once per module (marker attribute
    _tapir_translator). Tk.__init__ is wrapped to replace self.tk by an
    _Intermediaire and start the live-update timer; Tk.destroy is wrapped to
    unregister the proxy and write the missing texts.
    """
    # Step 1: patch only once.
    if getattr(tkinter, "_tapir_translator", False):
        return
    tkinter._tapir_translator = True
    init_origine = tkinter.Tk.__init__

    # Step 2: wrapped constructor: run the original, then insert the proxy.
    def init(self, *args, **kwargs):
        """Tk.__init__ replacement: original constructor, then proxy + timer."""
        init_origine(self, *args, **kwargs)
        try:
            if not isinstance(self.tk, _Intermediaire):
                inter = _Intermediaire(self.tk)
                self.tk = inter
                _intermediaires.append(inter)
                _surveiller(self)
        except Exception as e:
            _journal("branchement Tk : %r" % e)

    init.__wrapped__ = init_origine
    tkinter.Tk.__init__ = init

    destroy_origine = tkinter.Tk.destroy

    # Step 3: wrapped destroy: forget the proxy, save the missing texts, then destroy.
    def destroy(self):
        """Tk.destroy replacement: unregister the proxy, write missing texts, destroy."""
        inter = self.__dict__.get("tk")
        try:
            if isinstance(inter, _Intermediaire) and inter in _intermediaires:
                _intermediaires.remove(inter)
            _moteur.ecrire_manquants()
        except Exception:
            pass
        return destroy_origine(self)

    tkinter.Tk.destroy = destroy
    _journal("tkinter branche (outil=%s, couche %s)" % (_moteur._nom_outil(), VERSION_COUCHE))


class _Chargeur(object):
    """Import loader wrapper used for the tkinter module.

    It delegates to the real loader, and once the module has been executed it calls
    _brancher() on it, so tkinter is patched before the script can use it.
    """

    def __init__(self, chargeur):
        """Wrap the real loader 'chargeur' found by the standard PathFinder."""
        self._chargeur = chargeur

    def create_module(self, spec):
        """Delegate module creation to the real loader."""
        return self._chargeur.create_module(spec)

    def exec_module(self, module):
        """Execute the module with the real loader, then patch it (errors are logged)."""
        self._chargeur.exec_module(module)
        try:
            _brancher(module)
        except Exception as e:
            _journal("branchement : %r" % e)

    def __getattr__(self, nom):
        """Forward every other loader attribute to the real loader."""
        return getattr(self._chargeur, nom)


class _Crochet(object):
    """Meta-path finder that waits for the import of tkinter to patch it.

    Nothing is imported in advance: start-up stays light and scripts without a
    window are not affected.
    """

    def find_spec(self, nom, chemin=None, cible=None):
        """Return the tkinter spec with a wrapping loader; None for any other module.

        The spec is found with the standard PathFinder, then its loader is replaced
        by a _Chargeur. Returning None lets the normal import machinery proceed.
        """
        if nom != "tkinter":
            return None
        try:
            from importlib.machinery import PathFinder
            spec = PathFinder.find_spec(nom, chemin)
            if spec is None or spec.loader is None:
                return None
            spec.loader = _Chargeur(spec.loader)
            return spec
        except Exception as e:
            _journal("crochet : %r" % e)
            return None


def _installer():
    """Create the engine, load the files and hook tkinter.

    Step 1: create the global _moteur and load settings / dictionaries.
    Step 2: if tkinter is already imported, patch it now; otherwise install the
    import hook at the head of sys.meta_path.
    Step 3: write the missing texts at process exit.
    """
    global _moteur
    racine = _dossier()
    _moteur = _Moteur(racine)
    _moteur.recharger_si_change()
    if "tkinter" in sys.modules:
        _brancher(sys.modules["tkinter"])
    else:
        sys.meta_path.insert(0, _Crochet())
    import atexit
    atexit.register(lambda: _moteur.ecrire_manquants())


def _chainer():
    """Run another sitecustomize.py (from the computer's Python environment), if any.

    Python only imports the first "sitecustomize" module found on sys.path, i.e. this
    one: without this chaining, an existing sitecustomize further down sys.path would
    be silently skipped. The first sitecustomize.py found in another sys.path folder
    is executed under the module name "_sitecustomize_suivant"; errors are logged.
    """
    try:
        # Step 1: our own folder, to skip it.
        ici = os.path.normcase(os.path.abspath(os.path.dirname(__file__)))
        for p in list(sys.path):
            try:
                if not p or os.path.normcase(os.path.abspath(p)) == ici:
                    continue
            except Exception:
                continue
            # Step 2: first other sitecustomize.py found: execute it and stop.
            candidat = os.path.join(p, "sitecustomize.py")
            if os.path.isfile(candidat):
                import importlib.util
                spec = importlib.util.spec_from_file_location("_sitecustomize_suivant", candidat)
                if spec and spec.loader:
                    module = importlib.util.module_from_spec(spec)
                    spec.loader.exec_module(module)
                return
    except Exception as e:
        _journal("sitecustomize suivant : %r" % e)


def _demarrer():
    """Entry point, run when Python imports this module at start-up.

    The layer is installed only inside a Tapir script (TAPIR_SCRIPT_ID set, or
    TAPIR_TRANSLATOR_FORCE=1 for tests) and when the data folder is known. Any other
    sitecustomize is chained in every case.
    """
    actif = bool(os.environ.get("TAPIR_SCRIPT_ID")) or os.environ.get("TAPIR_TRANSLATOR_FORCE") == "1"
    if actif and _dossier():
        try:
            _installer()
        except Exception as e:
            _journal("installation impossible : %r" % e)
    _chainer()


# Executed at import time (Python's "site" module imports sitecustomize automatically).
_demarrer()
