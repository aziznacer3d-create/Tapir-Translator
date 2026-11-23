// *****************************************************************************
// Tapir Translator - fichiers embarques dans l'add-on
//
//   Le .cpp correspondant est genere au build par Tools/Embarquer.cmake a
//   partir de couche/sitecustomize.py et dictionnaires/_commun.json : l'add-on
//   tient en un seul .apx, et la couche ecrite sur le disque est toujours
//   celle de la version de l'add-on.
// *****************************************************************************

#ifndef TT_EMBARQUE_HPP
#define TT_EMBARQUE_HPP

namespace TT {
namespace Embarque {

extern const unsigned char Couche[];
extern const unsigned long CoucheTaille;

extern const unsigned char Commun[];
extern const unsigned long CommunTaille;

}	// namespace Embarque
}	// namespace TT

#endif
