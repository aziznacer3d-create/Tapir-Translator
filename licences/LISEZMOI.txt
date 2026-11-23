Tapir Translator - composants tiers embarques dans Tapir_Translator.apx
=========================================================================

Tapir Translator lui-meme est dans le domaine public (Unlicense, fichier LICENSE du depot).
Les composants ci-dessous sont sous licences libres (permissives) : aucune n'est non commerciale
ni en partage a l'identique. Elles demandent seulement de joindre leur texte, ce que fait ce dossier.

Composant                         Version / origine                                   Licence
--------------------------------  --------------------------------------------------  -------------
Modele M2M100 418M (Meta)         huggingface.co/facebook/m2m100_418M, int8 CT2       MIT
CTranslate2 (OpenNMT / SYSTRAN)   v4.9.1, statique, sans MKL ni OpenMP                MIT
Ruy (Google)                      sous-module de CTranslate2 v4.9.1                   Apache 2.0
cpu_features (Google)             sous-module de CTranslate2 v4.9.1                   Apache 2.0
spdlog (Gabi Melman)              sous-module de CTranslate2 v4.9.1                   MIT
BS::thread_pool, half_float,      en-tetes fournis avec CTranslate2 v4.9.1            MIT
nlohmann/json
SentencePiece (Google)            vcpkg x64-windows-static                            Apache 2.0
Abseil (Google)                   vcpkg x64-windows-static (dependance SentencePiece) Apache 2.0
Protocol Buffers lite (Google)    vcpkg x64-windows-static (dependance SentencePiece) BSD 3 clauses
utf8_range                        vcpkg x64-windows-static (dependance Protobuf)      MIT

Aucun des composants n'est modifie. Le modele est seulement converti de format (PyTorch ->
CTranslate2) et quantifie en int8.
