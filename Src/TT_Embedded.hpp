// *****************************************************************************
// Tapir Translator - files embedded in the add-on binary
//
//   ROLE IN THE SYSTEM
//   The matching .cpp is generated at build time by Tools/Embed.cmake from
//   layer/sitecustomize.py and dictionaries/_common.json, as byte arrays.
//   This way the add-on fits in a single .apx, and the layer written to disk by
//   TT::ExtractLayer is always the one of this add-on version.
//   (The much larger engine and model are not compiled in: they are appended
//   to the .apx after linking, see TT_Payload.hpp.)
// *****************************************************************************

#ifndef TT_EMBEDDED_HPP
#define TT_EMBEDDED_HPP

namespace TT {
namespace Embedded {

// Raw bytes of layer/sitecustomize.py (the Python translation layer) and their count.
extern const unsigned char Layer[];
extern const unsigned long LayerSize;

// Raw bytes of dictionaries/_common.json (the common multilingual dictionary) and their count.
extern const unsigned char Common[];
extern const unsigned long CommonSize;

}	// namespace Embedded
}	// namespace TT

#endif
