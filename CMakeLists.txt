# *****************************************************************************
# Tapir Translator - CMake build (Windows: Visual Studio 2022, x64)
#
#   Targets and build steps:
#     1. LocalizedMenus   runs GenerateMenus to (re)generate
#                         RFIX/Tapir_TranslatorMenus.grc (menus in 20 languages)
#     2. AddOnResources   compiles the GRC resources with the DevKit's
#                         CompileResources.py (needs Python 3)
#     3. (custom command) Tools/Embed.cmake turns layer/sitecustomize.py
#                         and dictionaries/_common.json into generated/TT_Embedded.cpp
#     4. AddOn            the add-on itself, Tapir_Translator.apx (Src/*.cpp)
#     5. TapirTranslatorEngine  the local translation engine (Engine/*.cpp),
#                         optionally with the offline model (TT_LOCAL_MODEL)
#     6. Packer       POST_BUILD of AddOn: appends the engine, the licenses
#                         (and the model) to the end of the .apx
#     7. TranslatorBench   test bench, outside ArchiCAD
#
#   Usage:
#     cmake -S . -B build -G "Visual Studio 17 2022" -A x64
#           [-DAC_API_DEVKIT_DIR=".../API AC30"] [-DTT_MODELE_LOCAL=ON]
#     cmake --build build --config RelWithDebInfo
#   Automatic regeneration is disabled (CMAKE_SUPPRESS_REGENERATION): run the
#   configure step again after editing this file.
# *****************************************************************************

cmake_minimum_required (VERSION 3.16)

# Find Python 3 (needed to run CompileResources.py)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
message(STATUS "Using Python: ${Python3_EXECUTABLE} (version ${Python3_VERSION})")

# Compiler options of the add-on (from the ArchiCAD DevKit template): C++20,
# warnings as errors (/W3 /WX on Windows), UTF-8 sources, wchar_t as in the
# DevKit (/Zc:wchar_t-). The macOS branch is kept from the template.
function (SetCompilerOptions target)
	target_compile_features (${target} PUBLIC cxx_std_20)
	target_compile_options (${target} PUBLIC "$<$<CONFIG:Debug>:-DDEBUG>")
	if (WIN32)
		target_compile_options (${target} PUBLIC /W3 /WX /wd4996 /Zc:wchar_t- /utf-8)
	else ()
		target_compile_options (${target} PUBLIC -Wall -Werror -fvisibility=hidden
			-Wno-multichar
			-Wno-ctor-dtor-privacy
			-Wno-invalid-offsetof
			-Wno-ignored-qualifiers
			-Wno-reorder
			-Wno-overloaded-virtual
			-Wno-unused-parameter
			-Wno-missing-field-initializers
			-Wno-unknown-pragmas
			-Wno-missing-braces
			-Wno-unused-private-field
			-Wno-return-std-move
			-Wno-unused-value
			-Wno-switch
			-Wno-deprecated
			-Wno-shorten-64-to-32)
	endif ()
endfunction ()

# Adds one DevKit module folder (Support/Modules/<module>) to the include path.
function (AddGSModuleIncludeFolder target folder)
	target_include_directories (${target} PUBLIC "${folder}")
endfunction ()

# Links every library of one DevKit module (on Windows: <module>/Win/*.lib).
function (AddGSModuleLinkLibrary target library)
	if (WIN32)
		file (GLOB libs "${library}/Win/*.lib")
		foreach (lib ${libs})
			target_link_libraries (${target} ${lib})
		endforeach ()
	else ()
		target_link_libraries (${target} ${library})
	endif ()
endfunction ()


# Global settings: IDE folders, no automatic regeneration, three configurations,
# VS 2022 toolset (v143) and the DLL C runtime for the add-on
set_property (GLOBAL PROPERTY USE_FOLDERS ON)

get_filename_component (CMAKE_CURRENT_FOLDER_NAME "${CMAKE_CURRENT_LIST_DIR}" NAME)
set (CMAKE_SUPPRESS_REGENERATION 1)
set (CMAKE_CONFIGURATION_TYPES Debug;Release;RelWithDebInfo)
if (WIN32)
	set (CMAKE_GENERATOR_TOOLSET v143)
	set (CMAKE_MSVC_RUNTIME_LIBRARY MultiThreadedDLL)
else ()
	set (CMAKE_OSX_DEPLOYMENT_TARGET ___MACOSX_DEPLOYMENT_TARGET___)
	if (CMAKE_GENERATOR STREQUAL Xcode)
		set (CMAKE_OSX_ARCHITECTURES "$(ARCHS_STANDARD)" CACHE INTERNAL "" FORCE)
		set (CMAKE_XCODE_ATTRIBUTE_ONLY_ACTIVE_ARCH "NO")
	endif ()
endif ()

# Points to the ArchiCAD API DevKit ("API AC29" folder shipped alongside this project);
# pass -DAC_API_DEVKIT_DIR=... to build for ArchiCAD 30.
# Then the add-on name (output file name) and the language of the R<language> resource folder.
set (AC_API_DEVKIT_DIR "${CMAKE_CURRENT_LIST_DIR}/../API AC29" CACHE PATH "API DevKit directory.")
set (AC_ADDON_NAME "Tapir_Translator" CACHE STRING "Add-On name.")
set (AC_ADDON_LANGUAGE "INT" CACHE STRING "Add-On language code.")

message (STATUS "APIDevKit directory: ${AC_API_DEVKIT_DIR}")
# Detect the ArchiCAD version from ACAPinc.h (ServerMainVers_NN); stop if the
# DevKit cannot be found
set (ACAPINC_FILE_LOCATION ${AC_API_DEVKIT_DIR}/Support/Inc/ACAPinc.h)
if (EXISTS ${ACAPINC_FILE_LOCATION})
	file (READ ${ACAPINC_FILE_LOCATION} ACAPIncContent)
	string (REGEX MATCHALL "#define[ \t]+ServerMainVers_([0-9][0-9])" VersionList ${ACAPIncContent})
	set (ARCHICAD_VERSION ${CMAKE_MATCH_1})
	message (STATUS "Archicad Version: ${ARCHICAD_VERSION}")
else ()
	message (FATAL_ERROR "Failed to detect Archicad version, please check the value of the AC_API_DEVKIT_DIR variable.")
endif ()

# Preprocessor definitions required by the DevKit headers
if (WIN32)
	add_definitions (-DUNICODE -D_UNICODE -D_ITERATOR_DEBUG_LEVEL=0 -DGS_WIN)
else ()
	add_definitions (-Dmacintosh=1 -DGS_MAC)
endif ()
add_definitions (-DACExtension)

project (${AC_ADDON_NAME})

set (AddOnSourcesFolder ./Src)
set (AddOnResourcesFolder .)

# AddOnResources
# Step 2 of the build: compile the GRC / RC2 resources (menus, strings, icons)
# into <name>.res, which is linked into the .apx. A stamp file tells CMake
# when the resources are up to date.

set (ResourceObjectsDir ${CMAKE_BINARY_DIR}/ResourceObjects)

file (GLOB AddOnImageFiles
	${AddOnResourcesFolder}/RFIX/Images/*.svg
)
if (WIN32)
	file (GLOB AddOnResourceFiles
		${AddOnResourcesFolder}/R${AC_ADDON_LANGUAGE}/*.grc
		${AddOnResourcesFolder}/RFIX/*.grc
		${AddOnResourcesFolder}/RFIX.win/*.rc2
	)
else ()
	file (GLOB AddOnResourceFiles
		${AddOnResourcesFolder}/R${AC_ADDON_LANGUAGE}/*.grc
		${AddOnResourcesFolder}/RFIX/*.grc
		${AddOnResourcesFolder}/RFIX.mac/*.plist
	)
endif ()

source_group ("Images" FILES ${AddOnImageFiles})
source_group ("Resources" FILES ${AddOnResourceFiles})
add_custom_target (
	AddOnResources ALL
	DEPENDS "${ResourceObjectsDir}/AddOnResources.stamp"
	SOURCES ${AddOnResourceFiles} ${AddOnImageFiles}
)

get_filename_component (AddOnSourcesFolderAbsolute "${CMAKE_CURRENT_LIST_DIR}/${AddOnSourcesFolder}" ABSOLUTE)
get_filename_component (AddOnResourcesFolderAbsolute "${CMAKE_CURRENT_LIST_DIR}/${AddOnResourcesFolder}" ABSOLUTE)
get_filename_component (APIDevKitToolsFolderAbsolute "${AC_API_DEVKIT_DIR}/Support/Tools" ABSOLUTE)
# The AC30 DevKit no longer ships Support/Tools/CompileResources.py (it is in each example, with another
# signature): the project's AC29 copy (Tools/) is used instead; it calls the same ResConv (same options in AC30).
if (NOT EXISTS "${APIDevKitToolsFolderAbsolute}/CompileResources.py")
	get_filename_component (APIDevKitToolsFolderAbsolute "${CMAKE_CURRENT_LIST_DIR}/Tools" ABSOLUTE)
endif ()
if (WIN32)
	add_custom_command (
		OUTPUT "${ResourceObjectsDir}/AddOnResources.stamp"
		DEPENDS ${AddOnResourceFiles} ${AddOnImageFiles}
		COMMENT "Compiling resources..."
		COMMAND ${CMAKE_COMMAND} -E make_directory "${ResourceObjectsDir}"
		COMMAND ${Python3_EXECUTABLE} "${APIDevKitToolsFolderAbsolute}/CompileResources.py" "${AC_ADDON_LANGUAGE}" "${AC_API_DEVKIT_DIR}" "${AddOnSourcesFolderAbsolute}" "${AddOnResourcesFolderAbsolute}" "${ResourceObjectsDir}" "${ResourceObjectsDir}/${AC_ADDON_NAME}.res"
		COMMAND ${CMAKE_COMMAND} -E touch "${ResourceObjectsDir}/AddOnResources.stamp"
	)
else ()
	add_custom_command (
		OUTPUT "${ResourceObjectsDir}/AddOnResources.stamp"
		DEPENDS ${AddOnResourceFiles} ${AddOnImageFiles}
		COMMENT "Compiling resources..."
		COMMAND ${CMAKE_COMMAND} -E make_directory "${ResourceObjectsDir}"
		COMMAND ${Python3_EXECUTABLE} "${APIDevKitToolsFolderAbsolute}/CompileResources.py" "${AC_ADDON_LANGUAGE}" "${AC_API_DEVKIT_DIR}" "${AddOnSourcesFolderAbsolute}" "${AddOnResourcesFolderAbsolute}" "${ResourceObjectsDir}" "${CMAKE_BINARY_DIR}/$<CONFIG>/${AC_ADDON_NAME}.bundle/Contents/Resources"
		COMMAND ${CMAKE_COMMAND} -E copy "${AC_API_DEVKIT_DIR}/Support/Inc/PkgInfo" "${CMAKE_BINARY_DIR}/$<CONFIG>/${AC_ADDON_NAME}.bundle/Contents/PkgInfo"
		COMMAND ${CMAKE_COMMAND} -E touch "${ResourceObjectsDir}/AddOnResources.stamp"
	)
endif ()

# AddOn
# Step 4 of the build: the add-on DLL (Tapir_Translator.apx) from every
# Src/*.c / *.cpp / *.h / *.hpp file plus the generated TT_Embedded.cpp
# (.c files are compiled as C++).

file (GLOB AddOnHeaderFiles
	${AddOnSourcesFolder}/*.h
	${AddOnSourcesFolder}/*.hpp
)
file (GLOB AddOnSourceFiles
	${AddOnSourcesFolder}/*.c
	${AddOnSourcesFolder}/*.cpp
)
file (GLOB AllCFiles
	${AddOnSourcesFolder}/*.c
)
set_source_files_properties(${AllCFiles} PROPERTIES LANGUAGE CXX)
# Step 3 of the build: Python layer and common dictionary embedded in the .apx as byte arrays
# (Tools/Embed.cmake, run in script mode; regenerated when one of the inputs changes)
set (EmbeddedCpp "${CMAKE_BINARY_DIR}/generated/TT_Embedded.cpp")
set (LayerSource "${CMAKE_CURRENT_LIST_DIR}/layer/sitecustomize.py")
set (CommonSource "${CMAKE_CURRENT_LIST_DIR}/dictionaries/_common.json")
add_custom_command (
	OUTPUT "${EmbeddedCpp}"
	COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/generated"
	COMMAND ${CMAKE_COMMAND} "-DOUTPUT=${EmbeddedCpp}" "-DLAYER=${LayerSource}" "-DCOMMON=${CommonSource}"
			-P "${CMAKE_CURRENT_LIST_DIR}/Tools/Embed.cmake"
	DEPENDS "${LayerSource}" "${CommonSource}" "${CMAKE_CURRENT_LIST_DIR}/Tools/Embed.cmake"
	COMMENT "Embedding the translation layer..."
)

set (
	AddOnFiles
	${AddOnHeaderFiles}
	${AddOnSourceFiles}
	"${EmbeddedCpp}"
)
source_group ("Sources" FILES ${AddOnFiles})
# A DLL on Windows (renamed .apx below), a bundle on macOS
if (WIN32)
	add_library (AddOn SHARED ${AddOnFiles})
else ()
	add_library (AddOn MODULE ${AddOnFiles})
endif ()

set_target_properties (AddOn PROPERTIES OUTPUT_NAME ${AC_ADDON_NAME})
if (WIN32)
	set_target_properties (AddOn PROPERTIES SUFFIX ".apx")
else ()
	set_target_properties (AddOn PROPERTIES BUNDLE TRUE)
	set_target_properties (AddOn PROPERTIES MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_LIST_DIR}/${AddOnResourcesFolder}/RFIX.mac/Info.plist")
	set_target_properties (AddOn PROPERTIES LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>")
endif ()

# Windows: link the compiled resources and export the two entry points that
# ArchiCAD looks for in an add-on (by ordinal 1 and 2)
if (WIN32)
	target_link_options (AddOn PUBLIC "${ResourceObjectsDir}/${AC_ADDON_NAME}.res")
	target_link_options (AddOn PUBLIC /export:GetExportedFuncAddrs,@1 /export:SetImportedFuncAddrs,@2)
endif ()

# Include paths: add-on sources, DevKit headers, and Engine/ (Json.hpp etc. shared with the engine)
target_include_directories (AddOn PUBLIC
	${AddOnSourcesFolder}
	${AC_API_DEVKIT_DIR}/Support/Inc
	${CMAKE_CURRENT_LIST_DIR}/Engine
)

# DevKit static API library (+ Shell32 on Windows)
if (WIN32)
	target_link_libraries (AddOn
		"${AC_API_DEVKIT_DIR}/Support/Lib/ACAP_STAT.lib"
		Shell32.lib
	)
else ()
	find_library (CocoaFramework Cocoa)
	target_link_libraries (AddOn
		"${AC_API_DEVKIT_DIR}/Support/Lib/libACAP_STAT.a"
		${CocoaFramework}
	)
endif ()

SetCompilerOptions (AddOn)
add_dependencies (AddOn AddOnResources)

# Every DevKit module (GSRoot, DGLib...) on the include path and linked
get_filename_component (APIDevKitModulesDir "${AC_API_DEVKIT_DIR}/Support/Modules" ABSOLUTE)

file (GLOB GSIncludeFolders
	${APIDevKitModulesDir}/*
)
foreach (includeFolder ${GSIncludeFolders})
	AddGSModuleIncludeFolder (AddOn ${includeFolder})
endforeach ()

if (WIN32)
	set (APIDevKitLinkLibDir "${APIDevKitModulesDir}")
else ()
	get_filename_component (APIDevKitLinkLibDir "${AC_API_DEVKIT_DIR}/Support/Frameworks" ABSOLUTE)
endif ()

file (GLOB GSLinkLibFolders
	${APIDevKitLinkLibDir}/*
)
foreach (linkLibFolder ${GSLinkLibFolders})
	AddGSModuleLinkLibrary (AddOn ${linkLibFolder})
endforeach ()
# Force-include the DevKit's GSNew.hpp and GSMalloc.hpp in every add-on source,
# as in the DevKit's own CMake template
get_filename_component(new_hpp "${APIDevKitModulesDir}/GSRoot/GSNew.hpp" REALPATH)
get_filename_component(malloc_hpp "${APIDevKitModulesDir}/GSRoot/GSMalloc.hpp" REALPATH)
if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    target_compile_options(
        AddOn PRIVATE
        "SHELL:/FI \"${new_hpp}\""
        "SHELL:/FI \"${malloc_hpp}\""
    )
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang\$")
    target_compile_options(
        AddOn PRIVATE
        "SHELL:-include \"${new_hpp}\""
        "SHELL:-include \"${malloc_hpp}\""
    )
else()
    message(FATAL_ERROR "Unknown compiler ID.")
endif()

# Step 7 of the build - test bench (outside ArchiCAD): runs a fake Tapir script through
# "uv run --script", like the Tapir palette, with the environment prepared by the same code
# as the add-on (Src/TT_Core.cpp). Built with /W3 /WX.
if (WIN32)
	add_executable (TranslatorBench
		Tests/TranslatorBench.cpp
		Src/TT_Core.cpp
		Src/TT_Core.hpp
		"${EmbeddedCpp}"
	)
	target_include_directories (TranslatorBench PRIVATE Src Engine)
	target_compile_features (TranslatorBench PRIVATE cxx_std_20)
	target_compile_options (TranslatorBench PRIVATE /W3 /WX /utf-8)
	target_compile_definitions (TranslatorBench PRIVATE "BENCH_TESTS_DIR=\"${CMAKE_CURRENT_LIST_DIR}/Tests\"")
endif ()

# Step 5 of the build - translation engine (local HTTP server on 127.0.0.1:8779, started by
# the add-on): relays to the online translators. Linked with Winsock (server) and WinHTTP
# (client). TT_LOCAL_MODEL adds the offline M2M100 model (off by default).
option (TT_LOCAL_MODEL "Offline M2M100 engine (CTranslate2) in the engine and the .apx (~500 MB)" OFF)
if (WIN32)
	add_executable (TapirTranslatorEngine
		Engine/EngineMain.cpp
		Engine/TestTranslator.cpp
		Engine/OnlineTranslators.cpp
		Engine/Dialogue.cpp
		Engine/Dialogue.hpp
		Src/TT_Texts.cpp
		Engine/Http.cpp
		Engine/Http.hpp
		Engine/Translator.hpp
		Engine/Json.hpp
	)
	target_compile_features (TapirTranslatorEngine PRIVATE cxx_std_20)
	target_compile_options (TapirTranslatorEngine PRIVATE /W3 /WX /utf-8)
	target_link_libraries (TapirTranslatorEngine PRIVATE Ws2_32 Winhttp)
	target_include_directories (TapirTranslatorEngine PRIVATE Src)
	# Static CRT: the engine depends on no DLL (it runs from its extraction folder)
	set_property (TARGET TapirTranslatorEngine PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
	add_dependencies (TranslatorBench TapirTranslatorEngine)

	# Offline translator: CTranslate2 (MIT, built statically without MKL, see README) +
	# SentencePiece (Apache 2.0, vcpkg x64-windows-static). Static CRT: no DLL to ship.
	# Used only if TT_LOCAL_MODEL is ON and both libraries are found; otherwise the
	# stub TranslatorM2M100Missing.cpp is compiled instead.
	set (TT_CT2 "${CMAKE_CURRENT_LIST_DIR}/_external/ct2_install" CACHE PATH "CTranslate2 installation (static).")
	set (TT_CT2_BUILD "${CMAKE_CURRENT_LIST_DIR}/_external/CTranslate2/build" CACHE PATH "CTranslate2 build folder (Ruy, cpu_features).")
	set (TT_VCPKG_STATIC "C:/vcpkg/installed/x64-windows-static" CACHE PATH "vcpkg x64-windows-static (SentencePiece).")
	if (TT_LOCAL_MODEL AND EXISTS "${TT_CT2}/lib/ctranslate2.lib" AND EXISTS "${TT_VCPKG_STATIC}/lib/sentencepiece.lib")
		message (STATUS "Engine: CTranslate2 + SentencePiece (M2M100 translator)")
		# Third-party libraries built with CTranslate2 (Ruy, cpu_features...): Release
		# builds only, test frameworks excluded; abseil / utf8 libraries needed by SentencePiece
		file (GLOB_RECURSE CT2_DEPENDANCES "${TT_CT2_BUILD}/third_party/*.lib")
		list (FILTER CT2_DEPENDANCES INCLUDE REGEX "/Release/")
		list (FILTER CT2_DEPENDANCES EXCLUDE REGEX "googletest|gtest|gmock|cxxopts")
		file (GLOB VCPKG_LIBS "${TT_VCPKG_STATIC}/lib/absl_*.lib" "${TT_VCPKG_STATIC}/lib/utf8_*.lib")
		target_sources (TapirTranslatorEngine PRIVATE Engine/TranslatorM2M100.cpp)
		target_include_directories (TapirTranslatorEngine SYSTEM PRIVATE "${TT_CT2}/include" "${TT_VCPKG_STATIC}/include")
		target_link_libraries (TapirTranslatorEngine PRIVATE
			"${TT_CT2}/lib/ctranslate2.lib" ${CT2_DEPENDANCES}
			"${TT_VCPKG_STATIC}/lib/sentencepiece.lib" "${TT_VCPKG_STATIC}/lib/libprotobuf-lite.lib" ${VCPKG_LIBS})
		set_property (TARGET TapirTranslatorEngine PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
	else ()
		message (STATUS "Engine: online translators only (TT_LOCAL_MODEL=OFF)")
		target_sources (TapirTranslatorEngine PRIVATE Engine/TranslatorM2M100Missing.cpp)
	endif ()
endif ()

# Step 6 of the build - payload appended to the end of the .apx: engine (and model if it is
# present in TT_MODEL_DIR). The user copies only the .apx; the add-on extracts the payload
# on first start.
set (TT_MODEL_DIR "${CMAKE_CURRENT_LIST_DIR}/model" CACHE PATH "Folder of the translation model to embed.")
if (WIN32)
	add_executable (Packer
		Tools/Packer.cpp
		Src/TT_Payload.cpp
		Src/TT_Core.cpp
		"${EmbeddedCpp}"
	)
	target_include_directories (Packer PRIVATE Src)
	target_compile_features (Packer PRIVATE cxx_std_20)
	target_compile_options (Packer PRIVATE /W3 /WX /utf-8)

	# Payload folder, rebuilt from scratch after each link of the add-on
	set (PayloadFolder "${CMAKE_BINARY_DIR}/charge/$<CONFIG>")
	set (ModelCommands "")
	if (TT_LOCAL_MODEL AND EXISTS "${TT_MODEL_DIR}")
		message (STATUS "Model embedded: ${TT_MODEL_DIR}")
		# Copy the model, and the licenses of the offline engine's third-party components
		# (CTranslate2, Ruy, SentencePiece, model...)
		set (ModelCommands
			COMMAND ${CMAKE_COMMAND} -E copy_directory "${TT_MODEL_DIR}" "${PayloadFolder}/model"
			COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_LIST_DIR}/licenses" "${PayloadFolder}/licenses")
	else ()
		message (STATUS "No model embedded: the .apx stays small")
	endif ()
	# After the link: empty the payload folder, copy the engine and the project licence
	# (+ model and licenses), then append the folder to the .apx with Packer
	add_custom_command (TARGET AddOn POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E rm -rf "${PayloadFolder}"
		COMMAND ${CMAKE_COMMAND} -E make_directory "${PayloadFolder}"
		COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:TapirTranslatorEngine>" "${PayloadFolder}/"
		COMMAND ${CMAKE_COMMAND} -E make_directory "${PayloadFolder}/licenses"
		COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_LIST_DIR}/LICENSE" "${PayloadFolder}/licenses/Tapir_Translator_Unlicense.txt"
		${ModelCommands}
		COMMAND "$<TARGET_FILE:Packer>" "$<TARGET_FILE:AddOn>" "${PayloadFolder}"
		COMMENT "Embedding the translation engine into the .apx..."
	)
	# The engine and the packer must exist before the add-on's POST_BUILD runs;
	# the test bench also reads payloads and the UI texts
	add_dependencies (AddOn TapirTranslatorEngine Packer)
	target_sources (TranslatorBench PRIVATE Src/TT_Payload.cpp Src/TT_Texts.cpp)
endif ()

# Step 1 of the build - the add-on's menus in the 20 ArchiCAD languages: RFIX/Tapir_TranslatorMenus.grc
# is generated from Src/TT_Texts.cpp before the resources are compiled (rewritten only if it changes).
if (WIN32)
	add_executable (GenerateMenus Tools/GenerateMenus.cpp Src/TT_Texts.cpp Src/TT_Texts.hpp)
	target_include_directories (GenerateMenus PRIVATE Src)
	target_compile_features (GenerateMenus PRIVATE cxx_std_20)
	target_compile_options (GenerateMenus PRIVATE /W3 /WX /utf-8)
	add_custom_target (LocalizedMenus
		COMMAND "$<TARGET_FILE:GenerateMenus>" "${CMAKE_CURRENT_LIST_DIR}/RFIX/Tapir_TranslatorMenus.grc"
		DEPENDS GenerateMenus
		COMMENT "Tapir Translator menus in the 20 languages..."
	)
	add_dependencies (AddOnResources LocalizedMenus)
endif ()
