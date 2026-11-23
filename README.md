# Tapir Translator

Add-on ArchiCAD (C++, DevKit AC29 et AC30) qui traduit **en direct** l'interface des scripts Python lancés
par la palette [Tapir](https://github.com/ENZYME-APD/tapir-archicad-automation), **sans modifier les
scripts**. Libre de droit (domaine public, voir `LICENSE`).

MDID : 623201637 / 1856068953.

## Principe

1. Au démarrage d'ArchiCAD, l'add-on (préchargé) écrit la couche de traduction
   `couche/sitecustomize.py`, embarquée dans le `.apx`, dans `%APPDATA%\Tapir Translator\couche\`.
2. Il met ce dossier en tête du `PYTHONPATH` d'ArchiCAD et pose `TAPIR_TRANSLATOR_DIR`. Tapir lance
   chaque script par `uv run --script <script.py>` en gardant ce `PYTHONPATH` (il y ajoute ses helpers
   devant). Python importe alors **tout seul** le module `sitecustomize` avant le script : un seul
   processus, aucun second script.
3. La couche ne fait rien hors d'un script Tapir (pas de `TAPIR_SCRIPT_ID`). Dans un script Tapir, elle
   attend l'import de `tkinter` et met un intermédiaire devant l'interpréteur Tcl de chaque fenêtre Tk.
4. Le menu **Tapir Translator** écrit la langue dans `reglages.json`. La couche relit ce fichier et les
   dictionnaires toutes les 0,6 s : les fenêtres déjà ouvertes changent de langue, et un dictionnaire
   modifié s'applique aussitôt.

## Ce qui est traduit

Seuls les textes **affichés** et présents dans un dictionnaire sont traduits :

- l'option `text` des widgets (Label, Button, Checkbutton, Radiobutton, LabelFrame…, Tk et ttk) ;
- les `label` des menus ;
- les titres de fenêtre ;
- les onglets de `ttk.Notebook` et les en-têtes de colonnes de `ttk.Treeview` ;
- les textes de `Canvas` ;
- les variables (`StringVar`) affichées par un Label ou un Button ;
- les titres et messages de `messagebox` et des dialogues de fichiers.

Le script relit toujours ses textes dans leur langue d'origine (`cget`, `StringVar.get`, `entrycget`,
`title()`, `tab`, `heading`, `itemcget`). Une comparaison comme `if bouton.cget("text") == "Fermer"`
marche donc encore une fois l'interface traduite.

Jamais traduits, parce que ce sont des données : lignes de Treeview, Listbox, Entry, Text, valeurs de
Combobox (`values`).

## Dictionnaires

Ils sont dans `%APPDATA%\Tapir Translator\dictionnaires\` :

- `_commun.json` : environ 380 notions (interface courante + vocabulaire Archicad) en fr, en, de, es,
  pt, it, hu et ar. Il est écrit une seule fois par l'add-on : vos modifications sont gardées. Traductions
  à faire relire par des locuteurs natifs, en particulier le hongrois et l'arabe ;
- `<script>.json` : le nom du `.py` lancé par Tapir, sans `.py`. Il l'emporte sur `_commun.json`.

**Format multilingue** (celui de `_commun.json`) : une notion par ligne, une colonne par langue. Il
traduit d'une langue d'entrée quelconque vers une langue de sortie quelconque.

```json
{
  "format": 2,
  "termes": [
    {"en": "Close", "fr": "Fermer", "de": "Schließen", "es": "Cerrar", "pt": "Fechar", "it": "Chiudi", "hu": "Bezárás", "ar": "إغلاق"},
    {"en": "{n} walls", "fr": "{n} murs", "de": "{n} Wände"}
  ]
}
```

Le comportement dépend de la langue d'entrée :

- **choisie** : seule cette colonne est lue ;
- **détection automatique** : toutes les colonnes sont lues, et un texte déjà valable dans la langue de
  sortie reste tel quel. Par exemple, un script écrit en anglais garde « Surface » en anglais.

**Format par script** (une langue source, une table par langue cible) :

```json
{
  "langue_source": "fr",
  "traductions": {
    "en": { "Fermer": "Close", "{n} murs créés": "{n} walls created" },
    "de": { "Fermer": "Schließen" }
  }
}
```

- `{nom}` marque une partie variable (même nom dans la traduction).
- Les espaces et la ponctuation de fin se raccordent tout seuls : `"Nom"` traduit aussi `"Nom :"` en
  `"Name:"`, et `"Ouvrir"` traduit aussi `"Ouvrir..."`.
- Le français reçoit son espace avant `:` `?` `!` `;` (`"Name:"` donne `"Nom :"`).
- La casse est tolérée : `"FERMER"` donne `"CLOSE"`.
- Un texte sur plusieurs lignes se traduit ligne par ligne s'il n'a pas d'entrée complète.
- Si la langue d'entrée (choisie, ou à défaut la `langue_source` du script) est la langue de sortie,
  rien n'est traduit.

**Collecte** : avec *Collecter les textes non traduits* coché, chaque texte affiché sans traduction est
noté dans `manquants\<script>.json`, au même format. Il suffit de le compléter et de le copier dans
`dictionnaires\`.

## Menu

| Commande | Effet |
|---|---|
| Langue d'entrée > Détection automatique / Français / English / Deutsch / Español / Português / Italiano / Magyar / العربية | langue dans laquelle les scripts sont écrits |
| Langue de sortie > Aucune traduction / mêmes langues | langue affichée, en direct (fenêtres ouvertes comprises) |
| Collecter les textes non traduits | coché = notation dans `manquants\` |
| Ouvrir le dossier des dictionnaires | ouvre `%APPDATA%\Tapir Translator` |
| État de Tapir Translator | Rapport de session : dossiers, PYTHONPATH, langue, dictionnaires, textes manquants, fin du journal de la couche |

Chaque démarrage écrit aussi quelques lignes `Tapir Translator : …` dans le Rapport de session. La
couche écrit ses propres erreurs dans `journal.txt` : elles n'atteignent jamais le script.

## Build (Windows)

```
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config RelWithDebInfo
```

Le résultat est `build/RelWithDebInfo/Tapir_Translator.apx` (ArchiCAD 29). Pour ArchiCAD 30 :

```
cmake -S . -B build_ac30 -G "Visual Studio 17 2022" -A x64 -DAC_API_DEVKIT_DIR="E:/CODE ARCHICAD/R&D/API AC30"
cmake --build build_ac30 --config RelWithDebInfo
```

La couche Python et `_commun.json` sont embarqués au build (`Tools/Embarquer.cmake`, qui génère
`build/genere/TT_Embarque.cpp`).

## Banc de test (hors ArchiCAD)

```
build\RelWithDebInfo\BancTraducteur.exe
```

Il lance le faux script `Tests/banc_outil.py` (interface tkinter en français) par `uv run --script`,
comme la palette Tapir. L'environnement est préparé par le même code que l'add-on (`Src/TT_Coeur.cpp`).
Pendant que le script tourne, le banc change la langue et le dictionnaire, puis compare le texte
affiché par Tk et le texte vu par le script. `uv` doit être dans le `PATH`.

## État (23/11/2026)

- v0.1.0 : couche tkinter, menu, collecte, banc 123 / 123.
- v0.2.0 : langue d'entrée + langue de sortie, dictionnaire commun multilingue (8 langues, ~380 notions),
  repli sans casse, typographie française, banc **218 / 218**. Compile en AC29 et AC30.
  **Jamais testé dans ArchiCAD.**
- Pas encore traduits : les interfaces HTML servies par un script (serveur local), les boutons des boîtes
  de message (Windows les affiche dans sa propre langue) et les fenêtres « Script UI » de Tapir.
- Le banc ne couvre ni `messagebox` ni les dialogues de fichiers, parce qu'ils sont modaux.

## Fichiers

- `Src/AddOnMain.cpp` : menu, démarrage, Rapport de session.
- `Src/TT_Coeur.*` : réglages, extraction de la couche, environnement (sans ArchiCAD, partagé avec le banc).
- `couche/sitecustomize.py` : la couche de traduction (Python : elle tourne dans l'interpréteur du script).
- `dictionnaires/_commun.json` : le dictionnaire commun embarqué.
- `Tests/` : le banc C++, le faux script et son dictionnaire.
