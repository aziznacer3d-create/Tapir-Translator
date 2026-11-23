# Tapir Translator

Add-on ArchiCAD (C++, DevKit AC29 et AC30) qui traduit **en direct** l'interface des scripts Python lancés
par la palette [Tapir](https://github.com/ENZYME-APD/tapir-archicad-automation), **sans modifier les
scripts**. Il traduit d'abord avec des dictionnaires, puis, pour les phrases qu'ils ne connaissent pas,
avec un **moteur de traduction hors ligne** embarqué dans le `.apx`. Libre de droit (domaine public,
voir `LICENSE`) ; les composants tiers sont sous licences libres permissives (voir `licences/`).

MDID : 623201637 / 1856068953.

## Installation

Un seul fichier : `Tapir_Translator.apx` (environ 506 Mo, moteur et modèle compris), à copier dans le
dossier des add-ons d'ArchiCAD. Au premier démarrage avec la traduction automatique cochée, l'add-on
extrait le moteur et le modèle une seule fois dans `%LOCALAPPDATA%\Tapir Translator\moteur\<empreinte>\`.

## Principe

1. Au démarrage d'ArchiCAD, l'add-on (préchargé) écrit la couche de traduction
   `couche/sitecustomize.py`, embarquée dans le `.apx`, dans `%APPDATA%\Tapir Translator\couche\`.
2. Il met ce dossier en tête du `PYTHONPATH` d'ArchiCAD et pose `TAPIR_TRANSLATOR_DIR`. Tapir lance
   chaque script par `uv run --script <script.py>` en gardant ce `PYTHONPATH` (il y ajoute ses helpers
   devant). Python importe alors **tout seul** le module `sitecustomize` avant le script : un seul
   processus, aucun second script.
3. La couche ne fait rien hors d'un script Tapir (pas de `TAPIR_SCRIPT_ID`). Dans un script Tapir, elle
   attend l'import de `tkinter` et met un intermédiaire devant l'interpréteur Tcl de chaque fenêtre Tk.
4. Le menu **Tapir Translator** écrit les langues dans `reglages.json`. La couche relit ce fichier et
   les dictionnaires toutes les 0,3 s : les fenêtres déjà ouvertes changent de langue.
5. Les textes absents des dictionnaires partent en arrière-plan au moteur local
   `TapirTranslatorMoteur.exe` (127.0.0.1:8779), lancé par l'add-on et arrêté avec ArchiCAD. Le texte
   d'origine reste affiché le temps du calcul, puis la traduction apparaît ; elle est gardée dans
   `cache\<source>-<cible>.json` (partagé par tous les scripts, réutilisé même moteur arrêté).

## Langues

Les 20 langues des versions d'ArchiCAD 29 : français, anglais, allemand, espagnol, portugais, italien,
hongrois, tchèque, polonais, russe, ukrainien, grec, turc, néerlandais, suédois, norvégien, finnois,
japonais, coréen, chinois. Le dictionnaire commun couvre fr, en, de, es, pt, it et hu ; le moteur traduit
toutes les paires directement.

## Ce qui est traduit

Seuls les textes **affichés** sont traduits :

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
Combobox (`values`). Jamais envoyés au moteur : chemins, noms de fichiers, identifiants et codes
(`MUR-01`), textes déjà valables dans la langue de sortie.

## Dictionnaires

Ils sont dans `%APPDATA%\Tapir Translator\dictionnaires\`, du moins au plus prioritaire :

- `_commun.json` : environ 400 notions (interface courante + vocabulaire Archicad + libellés fréquents
  des scripts Tapir). **Remplacé à chaque version de l'add-on.** Traductions à faire relire par des
  locuteurs natifs, en particulier le hongrois ;
- `_perso.json` : vos ajouts communs à tous les scripts, jamais touché par l'add-on ;
- `<script>.json` : le nom du `.py` lancé par Tapir, sans `.py`.

Un dictionnaire l'emporte toujours sur le moteur : c'est le moyen de corriger une traduction
automatique (surtout pour les boutons très courts, le point faible du modèle).

**Format multilingue** : une notion par ligne, une colonne par langue. Il traduit d'une langue
d'entrée quelconque vers une langue de sortie quelconque.

```json
{
  "format": 2,
  "termes": [
    {"en": "Close", "fr": "Fermer", "de": "Schließen", "es": "Cerrar", "pt": "Fechar", "it": "Chiudi", "hu": "Bezárás"},
    {"en": "{n} walls", "fr": "{n} murs", "de": "{n} Wände"}
  ]
}
```

Le comportement dépend de la langue d'entrée :

- **choisie** : seule cette colonne est lue ;
- **détection automatique** : toutes les colonnes sont lues, et un texte déjà valable dans la langue de
  sortie reste tel quel. La langue du script est reconnue d'après les mots connus, puis gardée.

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
- Si la langue d'entrée est la langue de sortie, rien n'est traduit.

**Collecte** : avec *Collecter les textes non traduits* coché, chaque texte affiché sans traduction est
noté dans `manquants\<script>.json`, au même format.

## Moteur de traduction hors ligne

- Modèle : [facebook/m2m100_418M](https://huggingface.co/facebook/m2m100_418M) (Meta, **MIT**), converti
  au format CTranslate2 et quantifié en int8 : 490 Mo. Un seul réseau pour les 100 langues du modèle,
  traduction directe entre toutes les paires.
- Exécution : [CTranslate2](https://github.com/OpenNMT/CTranslate2) v4.9.1 (**MIT**), compilé en
  statique avec Ruy (Apache 2.0), sans MKL ni OpenMP ; découpage SentencePiece (Apache 2.0). Le moteur
  ne dépend d'aucune DLL et tourne dans son propre processus (un plantage n'atteint pas ArchiCAD).
- Le moteur reprend la ponctuation finale et la casse initiale de l'original.
- Mesuré sur le poste de développement : modèle chargé en 0,4 s, 8 phrases en moins d'1 s.

Préparer le moteur et le modèle (une fois, dans `_externe/`, exclu de git) :

```
# modèle (fichiers de https://huggingface.co/facebook/m2m100_418M/tree/main) dans _externe/m2m100_418M_hf
pip install ctranslate2==4.9.1 "transformers<5" sentencepiece torch   (environnement isolé)
ct2-transformers-converter --model _externe/m2m100_418M_hf --output_dir modele --quantization int8
copier _externe/m2m100_418M_hf/sentencepiece.bpe.model dans modele/

# CTranslate2 v4.9.1 (git clone --recursive) dans _externe/CTranslate2
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DWITH_MKL=OFF
      -DWITH_RUY=ON -DOPENMP_RUNTIME=NONE -DBUILD_SHARED_LIBS=OFF -DBUILD_CLI=OFF
      -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded -DCMAKE_INSTALL_PREFIX=../ct2_install
cmake --build build --config Release && cmake --install build --config Release

# SentencePiece
vcpkg install sentencepiece:x64-windows-static
```

Sans ces éléments, le projet compile quand même : le moteur ne connaît alors que `--essai`, et le
`.apx` n'embarque pas de modèle.

## Menu

| Commande | Effet |
|---|---|
| Langue d'entrée > Détection automatique / 20 langues | langue dans laquelle les scripts sont écrits |
| Langue de sortie > Aucune traduction / 20 langues | langue affichée, en direct (fenêtres ouvertes comprises) |
| Options > Traduction automatique hors ligne | lance / arrête le moteur (phrases hors dictionnaire) |
| Options > Collecter les textes non traduits | coché = notation dans `manquants\` |
| Options > Ouvrir le dossier des dictionnaires | ouvre `%APPDATA%\Tapir Translator` |
| Options > État de Tapir Translator | Rapport de session : dossiers, PYTHONPATH, langues, moteur, charge du `.apx`, dictionnaires, textes manquants, fin du journal |

La couche et le moteur écrivent leurs erreurs dans `%APPDATA%\Tapir Translator\journal.txt` : elles
n'atteignent jamais le script.

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

Après l'édition de liens, `Tools/Empaqueter.cpp` ajoute à la fin du `.apx` le moteur, le modèle
(`modele/`) et les textes de licence (`licences/`). Le projet désactive la régénération automatique de
CMake : relancer la configuration après une modification de `CMakeLists.txt`.

## Banc de test (hors ArchiCAD)

```
build\RelWithDebInfo\BancTraducteur.exe
```

Il lance le faux script `Tests/banc_outil.py` (interface tkinter en français) par `uv run --script`,
comme la palette Tapir, avec l'environnement préparé par le code de l'add-on (`Src/TT_Coeur.cpp`). Il
vérifie l'empaquetage du `.apx`, les dictionnaires, le moteur d'essai, puis le vrai modèle de bout en
bout. `uv` doit être dans le `PATH`.

## État (23/11/2026)

- v0.1.0 : couche tkinter, menu, collecte.
- v0.2.0 : langue d'entrée + langue de sortie, dictionnaire commun multilingue.
- v0.3.0 : chaîne de traduction automatique (moteur d'essai), cache.
- v0.4.0 : moteur réel M2M100 embarqué dans le `.apx`, 20 langues d'ArchiCAD (arabe retiré),
  `_perso.json`, menu Options corrigé, licences tierces jointes. Banc **300 / 300**. Compile en AC29 et
  AC30. Dictionnaires testés dans ArchiCAD par l'utilisateur ; **moteur réel pas encore testé dans
  ArchiCAD**.
- Pas encore traduits : les interfaces HTML servies par un script (serveur local), les boutons des boîtes
  de message (Windows les affiche dans sa propre langue) et les fenêtres « Script UI » de Tapir.

## Fichiers

- `Src/AddOnMain.cpp` : menu, démarrage, lancement du moteur, Rapport de session.
- `Src/TT_Coeur.*` : réglages, extraction de la couche, environnement (sans ArchiCAD, partagé avec le banc).
- `Src/TT_Charge.*`, `Tools/Empaqueter.cpp` : charge embarquée à la fin du `.apx`.
- `Moteur/` : serveur de traduction, traducteur M2M100, traducteur d'essai.
- `couche/sitecustomize.py` : la couche de traduction (Python : elle tourne dans l'interpréteur du script).
- `dictionnaires/_commun.json` : le dictionnaire commun embarqué.
- `licences/` : textes de licence des composants tiers (embarqués dans le `.apx`).
- `Tests/` : le banc C++, le faux script et son dictionnaire.
