# *****************************************************************************
# Tapir Translator - Embed.cmake: embeds two files in the add-on as C++ arrays
#
#   Generates TT_Embedded.cpp, which holds layer/sitecustomize.py (the Python
#   translation layer) and dictionaries/_common.json (the common dictionary)
#   as byte arrays, so the add-on can write them to %APPDATA% at start-up
#   without shipping extra files. Run in CMake script mode by a custom
#   command of CMakeLists.txt whenever one of the two files changes:
#     cmake -DSORTIE=<file.cpp> -DCOUCHE=<sitecustomize.py> -DCOMMUN=<_common.json> -P Embed.cmake
#   Generated symbols (namespace TT::Embedded, declared in Src/TT_Embedded.hpp):
#     Layer, LayerSize, Common, CommonSize
# *****************************************************************************

# Converts one file into C++ source text:
#   const unsigned char <name>[] = { 0x.., ..., 0x00 };
#   const unsigned long <name>Size = <size>UL;
#   name      : C++ name of the array
#   path   : file to embed
#   result : name of the variable that receives the text (in the caller's scope)
# A 0x00 is appended after the data (so the array is never empty); the size
# does not count it.
function (EmbedArray name path result)
	# Step 1: read the file as a hexadecimal string (2 characters per byte)
	file (READ "${path}" hex HEX)
	string (LENGTH "${hex}" length)
	math (EXPR size "${length} / 2")
	# Step 2: "0xNN," for each byte, then a line break (and tab) every 32 bytes
	string (REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
	string (REPEAT "0x[0-9a-f][0-9a-f]," 32 pattern)
	string (REGEX REPLACE "(${pattern})" "\\1\n\t" bytes "${bytes}")
	# Step 3: declarations returned to the caller
	set (${result} "const unsigned char ${name}[] = {\n\t${bytes}0x00\n};\nconst unsigned long ${name}Size = ${size}UL;\n" PARENT_SCOPE)
endfunction ()

# Step 1: one array per embedded file
EmbedArray (Layer "${LAYER}" layerArray)
EmbedArray (Common "${COMMON}" commonArray)

# Step 2: complete content of the generated .cpp file
set (content "// Generated par Tools/Embed.cmake : ne pas modifier.\n#include \"TT_Embedded.hpp\"\n\nnamespace TT {\nnamespace Embedded {\n\n${layerArray}\n${commonArray}\n}\n}\n")

# Step 3: write only if the content changed (otherwise the file keeps its
# date and the add-on is not recompiled for nothing)
if (EXISTS "${OUTPUT}")
	file (READ "${OUTPUT}" old)
	if (old STREQUAL content)
		return ()
	endif ()
endif ()
file (WRITE "${OUTPUT}" "${content}")
