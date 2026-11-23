# Tapir Translator

Add-on ArchiCAD (C++, DevKit AC29 et AC30) qui traduit **en direct** l'interface des scripts Python lancés
par la palette [Tapir](https://github.com/ENZYME-APD/tapir-archicad-automation), **sans modifier les
scripts**. Il traduit d'abord avec des dictionnaires, puis, pour les phrases qu'ils ne connaissent pas,
avec le **traducteur en ligne** de votre choix (DeepL, Microsoft Translator, Google Cloud Translation,
MyMemory, LibreTranslate). Chaque traduction en ligne est gardée en cache sur le poste : elle n'est
demandée qu'une fois. Libre de droit (domaine public, voir `LICENSE`).

MDID : 623201637 / 1856068953.

## Installation

Un seul fichier : `Tapir_Translator.apx` (environ 2 Mo, moteur compris), à copier dans le dossier des
add-ons d'ArchiCAD. Au premier démarrage avec la traduction en ligne cochée, l'add-on extrait le moteur
une seule fois dans `%LOCALAPPDATA%\Tapir Translator\moteur\<empreinte>\` (les anciennes versions y sont
supprimées).

## Traducteurs en ligne

| Traducteur | Gratuit (ordre de grandeur) | À renseigner dans `cles.json` |
|---|---|---|
| DeepL API Free | ~500 000 caractères / mois | `cle` (se termine par `:fx`) |
| Microsoft Translator (Azure, niveau F0) | ~2 millions de caractères / mois | `cle` et `region` |
| Google Cloud Translation (v2) | selon l'offre Google Cloud | `cle` |
| MyMemory | ~5 000 caractères / jour (50 000 avec un e-mail) | rien (`email` facultatif) |
| LibreTranslate | selon le serveur | `url` (et `cle` si demandée) |

- Menu **Tapir Translator > Traducteur en ligne** : choix du traducteur (le choisir active la
  traduction en ligne) et **Clés des traducteurs...**, qui ouvre `%APPDATA%\Tapir Translator\cles.json`
  dans le Bloc-notes. Les clés restent sur le poste ; les scripts ne les voient jamais (seul le moteur
  les lit, à chaque requête : une clé ajoutée sert tout de suite).
- Le moteur `TapirTranslatorMoteur.exe` (127.0.0.1:8779) appelle les services par WinHTTP : proxy et
  certificats de Windows. L'add-on le relance s'il s'arrête.
- Erreur du service (clé absente ou refusée, quota atteint, réseau) : le texte d'origine reste affiché,
  la cause est écrite une fois dans `journal.txt`, et la traduction est retentée toutes les 15 s.
- Confidentialité : les textes affichés hors dictionnaire partent au service choisi (jamais les chemins,
  noms de fichiers, identifiants et codes). D'après les conditions de DeepL (section 3.3.2), les textes
  envoyés avec l'offre API **Free** peuvent être conservés par DeepL.
- Cache : `%APPDATA%\Tapir Translator\cache\<traducteur>\<source>-<cible>.json`, partagé par tous les
  scripts et relu même sans Internet ni moteur. Un dictionnaire l'emporte toujours sur le cache.
- **Quota MyMemory** : quand MyMemory répond « MYMEMORY WARNING », le moteur ouvre une fenêtre qui
  propose de saisir une adresse e-mail (quota porté d'environ 5 000 à 50 000 caractères par jour,
  d'après la documentation MyMemory). Pas de mot de passe : un compte MyMemory n'augmente pas le quota
  (sa clé ne sert qu'aux mémoires privées). L'adresse est écrite dans `cles.json` et sert aussitôt.
  « Plus tard » : pas de nouvelle question pendant une heure.

## Corrections de termes

Les traducteurs en ligne connaissent mal le vocabulaire Archicad (« story » devient « histoire » au lieu
de « étage »). `%APPDATA%\Tapir Translator\corrections.json` (menu **Traducteur en ligne > Corrections
des termes...**) corrige les traductions en ligne, langue de sortie par langue de sortie :

```json
{
  "fr": [
    {"source": ["story", "stories"], "remplacer": {
      "toutes les histoires": "tous les étages", "une histoire": "un étage",
      "histoires": "étages", "histoire": "étage"}},
    {"source": "layer", "mauvais": "couche", "correct": "calque"}
  ]
}
```

- `source` (facultatif) : la règle ne joue que si l'original contient l'un de ces mots.
- `mauvais` → `correct`, ou `remplacer` pour plusieurs formes ; en fr, es et pt, le pluriel en -s d'un
  mot seul suit tout seul ; la majuscule initiale de la traduction est gardée.
- Les expressions les plus longues passent d'abord : y mettre les accords (« toutes les histoires » →
  « tous les étages »).
- Appliqué en direct (fenêtres ouvertes et cache compris), jamais aux dictionnaires. Le fichier est
  pré-rempli avec les pièges courants en fr, de, es, it, pt (story, slab, beam, layer, pen, fill,
  railing, curtain wall, morph, mesh, layout) et n'est jamais écrasé.
- Pour une phrase entière, utiliser plutôt `dictionnaires\_perso.json`.

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
5. Les textes absents des dictionnaires partent en arrière-plan au moteur local, qui les envoie au
   traducteur en ligne choisi. Le texte d'origine reste affiché le temps de la réponse, puis la
   traduction apparaît ; elle est gardée dans le cache.

## Langues

Les 20 langues des versions d'ArchiCAD 29 : français, anglais, allemand, espagnol, portugais, italien,
hongrois, tchèque, polonais, russe, ukrainien, grec, turc, néerlandais, suédois, norvégien, finnois,
japonais, coréen, chinois. Le dictionnaire commun couvre fr, en, de, es, pt, it et hu ; les traducteurs
en ligne traduisent le reste.

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

## Moteur hors ligne (option de build, désactivée)

Abandonné par défaut (trop gros) : il reste disponible avec `-DTT_MODELE_LOCAL=ON` (fournisseur `local`).
Le `.apx` embarque alors le modèle et pèse environ 506 Mo.

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
| Traducteur en ligne > DeepL / Microsoft Translator / Google Cloud Translation / MyMemory / LibreTranslate | traducteur des phrases hors dictionnaire |
| Traducteur en ligne > Clés des traducteurs... | ouvre `cles.json` dans le Bloc-notes |
| Traducteur en ligne > Corrections des termes... | ouvre `corrections.json` dans le Bloc-notes |
| Options > Traduction en ligne | lance / arrête le moteur (phrases hors dictionnaire) |
| Options > Collecter les textes non traduits | coché = notation dans `manquants\` |
| Options > Ouvrir le dossier des dictionnaires | ouvre `%APPDATA%\Tapir Translator` |
| Options > État de Tapir Translator | Rapport de session : dossiers, PYTHONPATH, langues, traducteur choisi et état de chaque clé (jamais la clé), moteur, dictionnaires, textes manquants, fin du journal |
| Langue de Tapir Translator > Comme ArchiCAD / 20 langues | langue de l'outil lui-même (voir ci-dessous) |

La couche et le moteur écrivent leurs erreurs dans `%APPDATA%\Tapir Translator\journal.txt` : elles
n'atteignent jamais le script.

## Langue de l'outil

L'interface de Tapir Translator lui-même (menus, aides de la barre d'état, fenêtre du quota MyMemory,
messages du Rapport de session) existe dans les 20 langues d'ArchiCAD.

- Par défaut (**Comme ArchiCAD**), elle suit la langue de la version d'ArchiCAD installée (code fourni
  par l'API : FRA → français, GER / AUT / CHE → allemand, INT / USA / UKI / AUS / NZE → anglais,
  BRA / POR → portugais, CHI / TAI → chinois, JPN → japonais...).
- Le menu **Langue de Tapir Translator** permet d'en forcer une autre (réglage `interface` de
  `reglages.json`). Les messages et la fenêtre changent tout de suite ; les menus, fixés par ArchiCAD à
  son démarrage, changent au **prochain démarrage**.
- Les textes sont dans `Src/TT_Textes.cpp` (une ligne par texte, une colonne par langue). Les menus sont
  générés au build par `Tools/GenererMenus.cpp` dans `RFIX/Tapir_TranslatorMenus.grc` (STR#
  32500 + 10 × langue, aides à + 1000) ; l'add-on n'enregistre que ceux de la langue choisie.
- Le diagnostic détaillé (Options > État) reste en français, ou en anglais pour les autres langues.
- Traductions à faire relire par des locuteurs natifs.

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
- v0.5.0 : moteur hors ligne retiré du build par défaut ; 5 traducteurs en ligne au choix (DeepL,
  Microsoft, Google, MyMemory, LibreTranslate) avec cles.json ; cache par traducteur ; relance
  automatique du moteur ; anciennes extractions supprimées ; correction de `{n}` (« Only the selected
  éléments »). `.apx` de 2 Mo. Banc **324 / 324** (faux serveur aux formats des 5 services). MyMemory
  vérifié en réel ; DeepL, Microsoft, Google, LibreTranslate pas encore avec de vraies clés.
- v0.6.0 : fenêtre « quota MyMemory » (saisie de l'e-mail, sans mot de passe) ; corrections de termes
  `corrections.json` appliquées en direct aux traductions en ligne. Banc **349 / 349**. MyMemory testé
  dans ArchiCAD par l'utilisateur (v0.5.0) : fonctionne.
- v0.7.0 : interface de l'outil dans les 20 langues d'ArchiCAD, suit la langue d'ArchiCAD par défaut (menu
  Langue de Tapir Translator pour en forcer une) ; menus générés au build. Banc **361 / 361**.
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
