// *****************************************************************************
// Tapir Translator - files embedded in the add-on binary
//
//   ROLE IN THE SYSTEM
//   The matching .cpp is generated at build time by Tools/Embarquer.cmake from
//   couche/sitecustomize.py and dictionnaires/_commun.json, as byte arrays.
//   This way the add-on fits in a single .apx, and the layer written to disk by
//   TT::ExtraireCouche is always the one of this add-on version.
//   (The much larger engine and model are not compiled in: they are appended
//   to the .apx after linking, see TT_Charge.hpp.)
// *****************************************************************************

#ifndef TT_EMBARQUE_HPP
#define TT_EMBARQUE_HPP

namespace TT {
namespace Embarque {

// Raw bytes of couche/sitecustomize.py (the Python translation layer) and their count.
extern const unsigned char Couche[];
extern const unsigned long CoucheTaille;

// Raw bytes of dictionnaires/_commun.json (the common multilingual dictionary) and their count.
extern const unsigned char Commun[];
extern const unsigned long CommunTaille;

}	// namespace Embarque
}	// namespace TT

#endif
