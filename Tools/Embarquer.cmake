# *****************************************************************************
# Tapir Translator - Embarquer.cmake: embeds two files in the add-on as C++ arrays
#
#   Generates TT_Embarque.cpp, which holds couche/sitecustomize.py (the Python
#   translation layer) and dictionnaires/_commun.json (the common dictionary)
#   as byte arrays, so the add-on can write them to %APPDATA% at start-up
#   without shipping extra files. Run in CMake script mode by a custom
#   command of CMakeLists.txt whenever one of the two files changes:
#     cmake -DSORTIE=<file.cpp> -DCOUCHE=<sitecustomize.py> -DCOMMUN=<_commun.json> -P Embarquer.cmake
#   Generated symbols (namespace TT::Embarque, declared in Src/TT_Embarque.hpp):
#     Couche, CoucheTaille, Commun, CommunTaille
# *****************************************************************************

# Converts one file into C++ source text:
#   const unsigned char <nom>[] = { 0x.., ..., 0x00 };
#   const unsigned long <nom>Taille = <size>UL;
#   nom      : C++ name of the array
#   chemin   : file to embed
#   resultat : name of the variable that receives the text (in the caller's scope)
# A 0x00 is appended after the data (so the array is never empty); the size
# does not count it.
function (Tableau nom chemin resultat)
	# Step 1: read the file as a hexadecimal string (2 characters per byte)
	file (READ "${chemin}" hex HEX)
	string (LENGTH "${hex}" longueur)
	math (EXPR taille "${longueur} / 2")
	# Step 2: "0xNN," for each byte, then a line break (and tab) every 32 bytes
	string (REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," octets "${hex}")
	string (REPEAT "0x[0-9a-f][0-9a-f]," 32 motif)
	string (REGEX REPLACE "(${motif})" "\\1\n\t" octets "${octets}")
	# Step 3: declarations returned to the caller
	set (${resultat} "const unsigned char ${nom}[] = {\n\t${octets}0x00\n};\nconst unsigned long ${nom}Taille = ${taille}UL;\n" PARENT_SCOPE)
endfunction ()

# Step 1: one array per embedded file
Tableau (Couche "${COUCHE}" tableauCouche)
Tableau (Commun "${COMMUN}" tableauCommun)

# Step 2: complete content of the generated .cpp file
set (contenu "// Genere par Tools/Embarquer.cmake : ne pas modifier.\n#include \"TT_Embarque.hpp\"\n\nnamespace TT {\nnamespace Embarque {\n\n${tableauCouche}\n${tableauCommun}\n}\n}\n")

# Step 3: write only if the content changed (otherwise the file keeps its
# date and the add-on is not recompiled for nothing)
if (EXISTS "${SORTIE}")
	file (READ "${SORTIE}" ancien)
	if (ancien STREQUAL contenu)
		return ()
	endif ()
endif ()
file (WRITE "${SORTIE}" "${contenu}")
