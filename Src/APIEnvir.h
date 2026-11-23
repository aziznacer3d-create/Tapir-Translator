// *****************************************************************************
// General settings for Add-On developments (standard ArchiCAD DevKit header)
//
//   ROLE IN THE SYSTEM
//   Must be included first by every source file that uses the ArchiCAD API
//   (here: AddOnMain.cpp), before ACAPinc.h. It defines the platform macros the
//   DevKit headers expect and pulls in the platform base header.
// *****************************************************************************

#ifndef	_APIENVIR_H_
#define	_APIENVIR_H_


// Step 1: on the Microsoft compiler, make sure the DevKit's WINDOWS macro is defined.
#if defined (_MSC_VER)
	#if !defined (WINDOWS)
		#define WINDOWS
	#endif
#endif

// Step 2: platform base header (Win32 API through the DevKit's wrapper on Windows).
#if defined (WINDOWS)
	#include "Win32Interface.hpp"
#endif

#if defined (macintosh)
	#include <CoreServices/CoreServices.h>
#endif

// Step 3: mark this code as an ArchiCAD extension (add-on) for the DevKit headers.
#if !defined (ACExtension)
	#define ACExtension
#endif


#endif
