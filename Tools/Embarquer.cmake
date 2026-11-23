# Genere TT_Embarque.cpp : couche/sitecustomize.py et dictionnaires/_commun.json en tableaux d'octets.
#   cmake -DSORTIE=<fichier.cpp> -DCOUCHE=<sitecustomize.py> -DCOMMUN=<_commun.json> -P Embarquer.cmake

function (Tableau nom chemin resultat)
	file (READ "${chemin}" hex HEX)
	string (LENGTH "${hex}" longueur)
	math (EXPR taille "${longueur} / 2")
	# 0xNN, par octet, retour a la ligne tous les 32 octets
	string (REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," octets "${hex}")
	string (REPEAT "0x[0-9a-f][0-9a-f]," 32 motif)
	string (REGEX REPLACE "(${motif})" "\\1\n\t" octets "${octets}")
	set (${resultat} "const unsigned char ${nom}[] = {\n\t${octets}0x00\n};\nconst unsigned long ${nom}Taille = ${taille}UL;\n" PARENT_SCOPE)
endfunction ()

Tableau (Couche "${COUCHE}" tableauCouche)
Tableau (Commun "${COMMUN}" tableauCommun)

set (contenu "// Genere par Tools/Embarquer.cmake : ne pas modifier.\n#include \"TT_Embarque.hpp\"\n\nnamespace TT {\nnamespace Embarque {\n\n${tableauCouche}\n${tableauCommun}\n}\n}\n")

# N'ecrit que si le contenu change (pas de recompilation inutile)
if (EXISTS "${SORTIE}")
	file (READ "${SORTIE}" ancien)
	if (ancien STREQUAL contenu)
		return ()
	endif ()
endif ()
file (WRITE "${SORTIE}" "${contenu}")
