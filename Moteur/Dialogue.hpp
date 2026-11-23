// *****************************************************************************
// Tapir Translator - moteur : fenetre "quota MyMemory atteint"
//
//   Ouverte par le moteur (processus separe d'ArchiCAD) quand MyMemory repond
//   "MYMEMORY WARNING" : propose de saisir une adresse e-mail (quota gratuit
//   porte de 5 000 a 50 000 caracteres par jour d'apres la documentation
//   MyMemory). Aucun mot de passe : un compte MyMemory n'augmente pas le quota
//   (la cle de compte ne sert qu'aux memoires privees).
// *****************************************************************************

#ifndef TT_MOTEUR_DIALOGUE_HPP
#define TT_MOTEUR_DIALOGUE_HPP

#include <string>

enum class ReponseQuota { Fermee, Enregistree, PlusTard };

// email : pre-rempli, recoit l'adresse saisie si Enregistree.
// avecEmail : le quota de 50 000 caracteres (avec e-mail) est lui aussi atteint -> simple information.
ReponseQuota DemanderEmailMyMemory (std::string& email, bool avecEmail);

#endif
