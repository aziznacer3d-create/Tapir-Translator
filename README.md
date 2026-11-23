# Tapir Translator

ArchiCAD add-on (C++, DevKit AC29 and AC30) that translates **live** the interface of the Python scripts
launched by the [Tapir](https://github.com/ENZYME-APD/tapir-archicad-automation) palette, **without
modifying the scripts**. It translates with dictionaries first, then, for the sentences they do not know,
with the **online translator** of your choice (DeepL, Microsoft Translator, Google Cloud Translation,
MyMemory, LibreTranslate). Every online translation is cached on the computer: it is requested only
once. Public domain (see `LICENSE`).

MDID: 623201637 / 1856068953.

## Installation

A single file: `Tapir_Translator.apx` (about 2 MB, engine included), to copy into ArchiCAD's add-ons
folder. At the first start with online translation enabled, the add-on extracts the engine once into
`%LOCALAPPDATA%\Tapir Translator\engine\<hash>\` (older versions are deleted there).

Upgrading from 0.7.x or older: the data written by those versions (French file names and JSON keys:
`reglages.json`, `cles.json`, `journal.txt`, `dictionnaires\`, `manquants\`, cache) is converted once,
automatically, at the first start of 0.8.0. Keys, settings, corrections, personal dictionaries and
cache are kept.

## Online translators

| Translator | Free (order of magnitude) | To fill in `keys.json` |
|---|---|---|
| DeepL API Free | ~500,000 characters / month | `key` (ends with `:fx`) |
| Microsoft Translator (Azure, F0 tier) | ~2 million characters / month | `key` and `region` |
| Google Cloud Translation (v2) | depends on the Google Cloud plan | `key` |
| MyMemory | ~5,000 characters / day (50,000 with an e-mail) | nothing (`email` optional) |
| LibreTranslate | depends on the server | `url` (and `key` if required) |

- Menu **Tapir Translator > Online translator**: choice of the translator (choosing one enables online
  translation) and **Translator keys...**, which opens `%APPDATA%\Tapir Translator\keys.json` in
  Notepad. The keys stay on the computer; the scripts never see them (only the engine reads them, at
  each request: a key you add is used at once).
- The engine `TapirTranslatorEngine.exe` (127.0.0.1:8779) calls the services through WinHTTP: Windows
  proxy and certificates. The add-on restarts it if it stops.
- Service error (missing or refused key, quota reached, network): the original text stays on screen,
  the cause is written once in `log.txt`, and the translation is retried every 15 s.
- Privacy: the displayed texts that are not in a dictionary are sent to the chosen service (never paths,
  file names, identifiers or codes). According to DeepL's terms (section 3.3.2), texts sent with the
  **Free** API plan may be kept by DeepL.
- Cache: `%APPDATA%\Tapir Translator\cache\<translator>\<source>-<target>.json`, shared by all scripts
  and read even without Internet or engine. A dictionary always wins over the cache.
- **MyMemory quota**: when MyMemory answers "MYMEMORY WARNING", the engine opens a window that offers
  to enter an e-mail address (quota raised from about 5,000 to 50,000 characters per day, according to
  the MyMemory documentation). No password: a MyMemory account does not raise the quota (its key is only
  used for private memories). The address is written in `keys.json` and used at once. "Later": no new
  question for one hour.

## Term corrections

Online translators know the Archicad vocabulary poorly ("story" becomes "histoire" instead of "étage" in
French). `%APPDATA%\Tapir Translator\corrections.json` (menu **Online translator > Term corrections...**)
corrects the online translations, output language by output language:

```json
{
  "fr": [
    {"source": ["story", "stories"], "replace": {
      "toutes les histoires": "tous les étages", "une histoire": "un étage",
      "histoires": "étages", "histoire": "étage"}},
    {"source": "layer", "wrong": "couche", "correct": "calque"}
  ]
}
```

- `source` (optional): the rule applies only if the original contains one of these words.
- `wrong` → `correct`, or `replace` for several forms; in fr, es and pt, the -s plural of a single word
  follows by itself; the initial capital of the translation is kept.
- Longer expressions are applied first: put the agreements there ("toutes les histoires" →
  "tous les étages").
- Applied live (open windows and cache included), never to the dictionaries. The file is pre-filled with
  the usual pitfalls in fr, de, es, it, pt (story, slab, beam, layer, pen, fill, railing, curtain wall,
  morph, mesh, layout) and is never overwritten.
- For a whole sentence, use `dictionaries\_user.json` instead.

## How it works

1. When ArchiCAD starts, the (preloaded) add-on writes the translation layer `layer/sitecustomize.py`,
   embedded in the `.apx`, into `%APPDATA%\Tapir Translator\layer\`.
2. It puts this folder at the head of ArchiCAD's `PYTHONPATH` and sets `TAPIR_TRANSLATOR_DIR`. Tapir
   launches each script with `uv run --script <script.py>` and keeps this `PYTHONPATH` (it prepends its
   own helpers). Python then imports the `sitecustomize` module **by itself** before the script: one
   process, no second script.
3. The layer does nothing outside a Tapir script (no `TAPIR_SCRIPT_ID`). In a Tapir script, it waits for
   the import of `tkinter` and puts a proxy in front of the Tcl interpreter of each Tk window.
4. The **Tapir Translator** menu writes the languages in `settings.json`. The layer re-reads this file
   and the dictionaries every 0.3 s: windows that are already open change language.
5. The texts missing from the dictionaries go, in the background, to the local engine, which sends them
   to the chosen online translator. The original text stays on screen while waiting, then the
   translation appears; it is kept in the cache.

## Languages

The 20 languages of the ArchiCAD 29 versions: French, English, German, Spanish, Portuguese, Italian,
Hungarian, Czech, Polish, Russian, Ukrainian, Greek, Turkish, Dutch, Swedish, Norwegian, Finnish,
Japanese, Korean, Chinese. The common dictionary covers fr, en, de, es, pt, it and hu; the online
translators translate the rest.

## What is translated

Only **displayed** texts are translated:

- the `text` option of widgets (Label, Button, Checkbutton, Radiobutton, LabelFrame…, Tk and ttk);
- menu `label`s;
- window titles;
- `ttk.Notebook` tabs and `ttk.Treeview` column headings;
- `Canvas` texts;
- variables (`StringVar`) displayed by a Label or a Button;
- titles and messages of `messagebox` and of the file dialogs.

The script always reads its texts back in their original language (`cget`, `StringVar.get`,
`entrycget`, `title()`, `tab`, `heading`, `itemcget`). A comparison such as
`if button.cget("text") == "Fermer"` therefore still works once the interface is translated.

Never translated, because they are data: Treeview rows, Listbox, Entry, Text, Combobox values
(`values`). Never sent to the engine: paths, file names, identifiers and codes (`WALL-01`), texts that
are already valid in the output language.

## Dictionaries

They are in `%APPDATA%\Tapir Translator\dictionaries\`, from lowest to highest priority:

- `_common.json`: about 400 notions (common interface + Archicad vocabulary + frequent labels of Tapir
  scripts). **Replaced with every version of the add-on.** Translations to be reviewed by native
  speakers, especially Hungarian;
- `_user.json`: your additions shared by all scripts, never touched by the add-on;
- `<script>.json`: the name of the `.py` launched by Tapir, without `.py`.

A dictionary always wins over the engine: it is the way to correct an automatic translation (especially
for very short buttons).

**Multilingual format**: one notion per row, one column per language. It translates from any input
language into any output language.

```json
{
  "format": 2,
  "terms": [
    {"en": "Close", "fr": "Fermer", "de": "Schließen", "es": "Cerrar", "pt": "Fechar", "it": "Chiudi", "hu": "Bezárás"},
    {"en": "{n} walls", "fr": "{n} murs", "de": "{n} Wände"}
  ]
}
```

The behaviour depends on the input language:

- **chosen**: only that column is read;
- **automatic detection**: every column is read, and a text that is already valid in the output language
  stays as it is. The language of the script is recognised from the known words, then kept.

**Per-script format** (one source language, one table per target language):

```json
{
  "source_language": "fr",
  "translations": {
    "en": { "Fermer": "Close", "{n} murs créés": "{n} walls created" },
    "de": { "Fermer": "Schließen" }
  }
}
```

- `{name}` marks a variable part (same name in the translation).
- Spaces and trailing punctuation are matched by themselves: `"Nom"` also translates `"Nom :"` into
  `"Name:"`, and `"Ouvrir"` also translates `"Ouvrir..."`.
- French gets its space before `:` `?` `!` `;` (`"Name:"` gives `"Nom :"`).
- Case is tolerated: `"FERMER"` gives `"CLOSE"`.
- If the input language is the output language, nothing is translated.

**Collection**: with *Collect untranslated texts* checked, every displayed text without a translation
is recorded in `missing\<script>.json`, in the same format.

## Offline engine (build option, disabled)

Dropped by default (too large): it is still available with `-DTT_LOCAL_MODEL=ON` (provider `local`).
The `.apx` then embeds the model and weighs about 506 MB.

- Model: [facebook/m2m100_418M](https://huggingface.co/facebook/m2m100_418M) (Meta, **MIT**), converted
  to the CTranslate2 format and quantized to int8: 490 MB. A single network for the 100 languages of the
  model, direct translation between all pairs.
- Runtime: [CTranslate2](https://github.com/OpenNMT/CTranslate2) v4.9.1 (**MIT**), built statically
  with Ruy (Apache 2.0), without MKL or OpenMP; SentencePiece tokenisation (Apache 2.0). The engine
  depends on no DLL and runs in its own process (a crash does not reach ArchiCAD).
- The engine restores the final punctuation and the initial case of the original.
- Measured on the development computer: model loaded in 0.4 s, 8 sentences in less than 1 s.

Preparing the engine and the model (once, in `_external/`, excluded from git):

```
# model (files from https://huggingface.co/facebook/m2m100_418M/tree/main) in _external/m2m100_418M_hf
pip install ctranslate2==4.9.1 "transformers<5" sentencepiece torch   (isolated environment)
ct2-transformers-converter --model _external/m2m100_418M_hf --output_dir model --quantization int8
copy _external/m2m100_418M_hf/sentencepiece.bpe.model into model/

# CTranslate2 v4.9.1 (git clone --recursive) in _external/CTranslate2
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DWITH_MKL=OFF
      -DWITH_RUY=ON -DOPENMP_RUNTIME=NONE -DBUILD_SHARED_LIBS=OFF -DBUILD_CLI=OFF
      -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded -DCMAKE_INSTALL_PREFIX=../ct2_install
cmake --build build --config Release && cmake --install build --config Release

# SentencePiece
vcpkg install sentencepiece:x64-windows-static
```

Without these, the project still builds: the engine then only knows `--test`, and the `.apx` embeds no
model.

## Menu

The menus exist in the 20 languages (see "Tool language"); their English names:

| Command | Effect |
|---|---|
| Input language > Automatic detection / 20 languages | language the scripts are written in |
| Output language > No translation / 20 languages | displayed language, live (open windows included) |
| Online translator > DeepL / Microsoft Translator / Google Cloud Translation / MyMemory / LibreTranslate | translator of the sentences missing from the dictionaries |
| Online translator > Translator keys... | opens `keys.json` in Notepad |
| Online translator > Term corrections... | opens `corrections.json` in Notepad |
| Options > Online translation | starts / stops the engine (sentences missing from the dictionaries) |
| Options > Collect untranslated texts | checked = recorded in `missing\` |
| Options > Open the dictionaries folder | opens `%APPDATA%\Tapir Translator` |
| Options > Tapir Translator status | Session Report: folders, PYTHONPATH, languages, chosen translator and state of each key (never the key itself), engine, dictionaries, missing texts, end of the log |
| Tapir Translator language > Same as ArchiCAD / 20 languages | language of the tool itself (see below) |

The layer and the engine write their errors in `%APPDATA%\Tapir Translator\log.txt`: they never reach
the script. The add-on also copies its Session Report lines there.

## Tool language

The interface of Tapir Translator itself (menus, status-bar help, MyMemory quota window, Session Report
messages) exists in the 20 ArchiCAD languages.

- By default (**Same as ArchiCAD**), it follows the language of the installed ArchiCAD version (code
  given by the API: FRA → French, GER / AUT / CHE → German, INT / USA / UKI / AUS / NZE → English,
  BRA / POR → Portuguese, CHI / TAI → Chinese, JPN → Japanese...).
- The **Tapir Translator language** menu forces another one (`interface` setting of `settings.json`).
  The messages, the window and the menu items change at once.
- The texts are in `Src/TT_Texts.cpp` (one row per text, one column per language). The menus are
  generated at build time by `Tools/GenerateMenus.cpp` into `RFIX/Tapir_TranslatorMenus.grc` (STR#
  32100 + 10 × language, help strings at + 300, all below 32767 because the ArchiCAD API reads them as
  16-bit signed integers); the add-on registers those of the chosen language and renames them when the
  language changes.
- The detailed technical lines of the diagnostic (Options > status: folders, paths, errors) are in English.
- Translations to be reviewed by native speakers.

## Build (Windows)

```
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config RelWithDebInfo
```

The result is `build/RelWithDebInfo/Tapir_Translator.apx` (ArchiCAD 29). For ArchiCAD 30:

```
cmake -S . -B build_ac30 -G "Visual Studio 17 2022" -A x64 -DAC_API_DEVKIT_DIR="E:/CODE ARCHICAD/R&D/API AC30"
cmake --build build_ac30 --config RelWithDebInfo
```

After linking, `Tools/Packer.cpp` appends the engine, the model (`model/`) and the license texts
(`licenses/`) to the end of the `.apx`. The project disables CMake's automatic regeneration: run the
configuration again after a change to `CMakeLists.txt`. Every `.grc` of `RFIX/` must also be included
in `RFIX.win/Tapir_Translator.rc2`, otherwise its resources are not in the `.apx`.

## Test bench (outside ArchiCAD)

```
build\RelWithDebInfo\TranslatorBench.exe
```

It launches the fake script `Tests/bench_tool.py` (tkinter interface in English, with a French variant) with `uv run --script`,
like the Tapir palette, with the environment prepared by the add-on code (`Src/TT_Core.cpp`). It checks
the packing of the `.apx` (engine and menu resources inside), the dictionaries, the test engine, the
online translators against a fake server speaking the formats of the 5 services, the corrections and
the conversion of old data. `uv` must be in the `PATH`.

## Status (23/11/2026)

- v0.1.0: tkinter layer, menu, collection.
- v0.2.0: input language + output language, multilingual common dictionary.
- v0.3.0: automatic translation chain (test engine), cache.
- v0.4.0: real M2M100 engine embedded in the `.apx`, the 20 ArchiCAD languages (Arabic removed),
  personal dictionary, third-party licenses included.
- v0.5.0: offline engine removed from the default build; 5 online translators to choose from (DeepL,
  Microsoft, Google, MyMemory, LibreTranslate) with a keys file; cache per translator; automatic
  restart of the engine; `{n}` fix. 2 MB `.apx`.
- v0.6.0: "MyMemory quota" window (e-mail entry, no password); term corrections applied live to online
  translations.
- v0.7.0: interface of the tool in the 20 ArchiCAD languages, following ArchiCAD's language by default;
  menus generated at build time.
- v0.7.1 – v0.7.5: menu fixes (resource IDs below 32767, menu resources linked into the `.apx`, add-on
  kept loaded after a menu command, menus renamed at once when the tool language changes); Session
  Report lines copied to the log.
- v0.8.0: the whole code base in English (identifiers, files, folders, messages, data names), with an
  automatic conversion of the data of older versions.
- Not translated yet: HTML interfaces served by a script (local server), message-box buttons (Windows
  displays them in its own language) and the "Script UI" windows of Tapir.

## Files

- `Src/AddOnMain.cpp`: menus, start-up, engine launch and watchdog, Session Report.
- `Src/TT_Core.*`: settings, layer extraction, environment, conversion of old data (no ArchiCAD
  dependency, shared with the bench).
- `Src/TT_Texts.*`: texts of the tool in the 20 languages.
- `Src/TT_Payload.*`, `Tools/Packer.cpp`: payload appended to the end of the `.apx`.
- `Engine/`: local translation server, online translators, MyMemory quota window, M2M100 and test
  translators.
- `layer/sitecustomize.py`: the translation layer (Python: it runs in the script's interpreter).
- `dictionaries/_common.json`: the embedded common dictionary.
- `licenses/`: license texts of the third-party components (embedded in the `.apx`).
- `Tests/`: the C++ bench, the fake script and its dictionary.
