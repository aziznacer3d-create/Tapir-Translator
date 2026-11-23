# -*- coding: utf-8 -*-
"""
Tapir Translator - couche de traduction des interfaces tkinter des scripts Tapir.

Ce fichier est ecrit par l'add-on Tapir Translator dans
%APPDATA%\\Tapir Translator\\couche\\ (il est remplace a chaque demarrage d'ArchiCAD :
ne pas le modifier la, modifier la source du depot).

Principe
--------
L'add-on ajoute ce dossier en tete du PYTHONPATH d'ArchiCAD. Tapir lance chaque script
par "uv run --script <script.py>" et garde ce PYTHONPATH : Python importe alors tout
seul ce module "sitecustomize" au demarrage, AVANT le script. Le script n'est pas
modifie et Tapir ne lance qu'un seul processus.

La couche ne fait rien hors d'un script Tapir (variable TAPIR_SCRIPT_ID absente).
Elle n'agit qu'au moment ou le script importe tkinter : chaque fenetre Tk recoit un
intermediaire devant l'interpreteur Tcl, qui traduit les textes AFFICHES (option -text
des widgets, -label des menus, titres de fenetre, onglets, en-tetes de colonnes, textes
de canevas, boites de message, titres des dialogues de fichiers, variables affichees
par un Label / Button). Les valeurs relues par le script (cget, StringVar.get,
entrycget, wm title) lui sont rendues dans la langue d'origine : sa logique ne voit
jamais la traduction.

Rien n'est traduit qui ne soit dans un dictionnaire : les donnees (noms de vues,
listes, Treeview, Entry, Text, Combobox -values) ne sont pas touchees.

La langue est lue dans reglages.json (ecrit par le menu de l'add-on) et relue en
direct ; un dictionnaire modifie pendant que l'outil tourne est applique aussitot.

Toute erreur interne est ecrite dans journal.txt et n'atteint jamais le script.
"""

import os
import sys

VERSION_COUCHE = "0.5.0"
_PERIODE_MS = 300            # relecture des reglages / dictionnaires, traductions arrivees
_TAILLE_JOURNAL = 512 * 1024  # au-dela, le journal repart a zero

_OPTIONS_TEXTE = ("-text", "-label")
# parties variables des dictionnaires qui ne peuvent etre que des nombres
_NOMS_NOMBRE = ("n", "nb", "nombre", "count", "i", "x", "total", "n1", "n2")
_TYPES_MENU = ("command", "cascade", "checkbutton", "radiobutton", "separator")
# classes Tk dont une -textvariable est affichee telle quelle (pas saisie)
_CLASSES_AFFICHAGE = ("label", "button", "checkbutton", "radiobutton", "labelframe",
                      "message", "menubutton", "tlabel", "tbutton", "tcheckbutton",
                      "tradiobutton", "tlabelframe", "tmenubutton")
_DIALOGUES = ("tk_messageBox", "tk_getOpenFile", "tk_getSaveFile", "tk_chooseDirectory",
              "tk_chooseColor")
_OPTIONS_DIALOGUE = ("-title", "-message", "-detail")


# =====================================================================================
#  Journal
# =====================================================================================

def _dossier():
    return os.environ.get("TAPIR_TRANSLATOR_DIR", "")


def _journal(message):
    try:
        racine = _dossier()
        if not racine:
            return
        chemin = os.path.join(racine, "journal.txt")
        try:
            if os.path.getsize(chemin) > _TAILLE_JOURNAL:
                os.remove(chemin)
        except OSError:
            pass
        import time
        with open(chemin, "a", encoding="utf-8") as f:
            f.write("%s [%d] %s\n" % (time.strftime("%Y-%m-%d %H:%M:%S"), os.getpid(), message))
    except Exception:
        pass


# =====================================================================================
#  Moteur de traduction (dictionnaires + reglages)
# =====================================================================================

class _Moteur:
    def __init__(self, racine):
        self.racine = racine
        self.outil = None
        self.langue = ""          # "" = langue d'origine, aucune traduction
        self.collecte = False
        self.langue_source = ""
        self.table = {}           # texte d'origine -> traduction (langue courante)
        self.table_min = {}       # meme table, cles sans casse
        self.modeles = []         # (regex, gabarit) pour les textes a variables
        self.manquants = {}       # langue -> {texte: ""}
        self.manquants_modifies = False
        self.empreinte = None     # dates des fichiers lus, pour la relecture en direct
        self.cache = {}
        self.entree = ""
        self.colonnes = {}        # langue -> textes connus des dictionnaires
        self.votes = {}           # langue -> nombre de textes du script reconnus
        self.vus = set()
        self.detectee = ""
        self.auto = _Automatique(racine, self)

    # --- fichiers ---------------------------------------------------------------------
    def _nom_outil(self):
        if self.outil is None:
            nom = os.environ.get("TAPIR_TRANSLATOR_OUTIL", "")
            if not nom:
                try:
                    nom = os.path.splitext(os.path.basename(sys.argv[0]))[0]
                except Exception:
                    nom = ""
            self.outil = nom or "script"
        return self.outil

    def _fichiers(self):
        outil = self._nom_outil()
        dico = os.path.join(self.racine, "dictionnaires")
        # du moins prioritaire au plus prioritaire
        return [os.path.join(self.racine, "reglages.json"),
                os.path.join(dico, "_commun.json"),     # celui de l'add-on (remplace a chaque version)
                os.path.join(dico, "_perso.json"),      # celui de l'utilisateur (jamais touche)
                os.path.join(dico, outil + ".json")]

    def _calculer_empreinte(self):
        emp = []
        for chemin in self._fichiers():
            try:
                s = os.stat(chemin)
                emp.append((s.st_mtime_ns, s.st_size))
            except OSError:
                emp.append(None)
        return tuple(emp)

    @staticmethod
    def _lire_json(chemin):
        import json
        try:
            with open(chemin, "r", encoding="utf-8-sig") as f:
                return json.load(f)
        except FileNotFoundError:
            return None
        except Exception as e:
            _journal("fichier illisible %s : %r" % (chemin, e))
            return None

    def recharger_si_change(self):
        """Relit reglages et dictionnaires s'ils ont change. Rend True si la langue
        ou une traduction a pu changer."""
        emp = self._calculer_empreinte()
        if emp == self.empreinte:
            return False
        self.empreinte = emp
        reglages_f, commun_f, perso_f, outil_f = self._fichiers()

        reglages = self._lire_json(reglages_f) or {}
        langue = str(reglages.get("langue", "") or "")
        if langue == "origine":
            langue = ""
        self.collecte = bool(reglages.get("collecte", False))

        commun = self._lire_json(commun_f)
        perso = self._lire_json(perso_f)
        outil = self._lire_json(outil_f)
        commun = commun if isinstance(commun, dict) else {}
        perso = perso if isinstance(perso, dict) else {}
        outil = outil if isinstance(outil, dict) else {}
        # seule la langue declaree par le dictionnaire du script compte : le dictionnaire
        # commun sert des scripts ecrits dans n'importe quelle langue
        # langue d'entree choisie dans le menu ; sinon celle du dictionnaire du script ;
        # sinon detection automatique
        entree = str(reglages.get("entree", "") or "")
        source = entree or str(outil.get("langue_source", "") or "")
        self.entree = entree

        # du moins prioritaire au plus prioritaire : commun (add-on), perso (utilisateur),
        # dictionnaire du script ; dans chacun, termes puis traductions
        table = {}
        if langue:
            for d in (commun, perso, outil):
                table.update(self._depuis_termes(d.get("termes"), langue, source, bool(entree)))
                trad = d.get("traductions", {})
                if isinstance(trad, dict) and isinstance(trad.get(langue), dict):
                    for k, v in trad[langue].items():
                        if isinstance(k, str) and isinstance(v, str) and v != "":
                            table[k] = v
        self.langue_source = source
        if langue and source and langue == source:
            langue = ""          # la langue demandee est celle du script
        if langue != self.langue:
            _journal("outil=%s langue=%s (source=%s) collecte=%s" %
                     (self._nom_outil(), langue or "origine", source or "?", self.collecte))
        self.langue = langue
        self.table = table if langue else {}
        # repli sans tenir compte de la casse ("FERMER", "fermer" -> "CLOSE", "close")
        self.table_min = {}
        for k, v in self.table.items():
            if len(k) >= 2 and "{" not in k:
                self.table_min.setdefault(k.casefold(), v)
        self.modeles = self._compiler_modeles(self.table)
        self.cache = {}

        # colonnes des dictionnaires : reconnaitre la langue du script (detection automatique)
        self.colonnes = {}
        for d in (commun, perso, outil):
            for t in d.get("termes", []) if isinstance(d.get("termes"), list) else []:
                if isinstance(t, dict):
                    for code, texte in t.items():
                        if isinstance(texte, str) and not code.startswith("_") and "{" not in texte:
                            self.colonnes.setdefault(code, set()).add(texte)
        port = os.environ.get("TAPIR_TRANSLATOR_MOTEUR_PORT", "") or str(reglages.get("port", "") or "")
        self.auto.configurer(bool(reglages.get("moteur", False)) and bool(langue), source, langue, port,
                             str(reglages.get("fournisseur", "") or "mymemory"))
        return True

    def _voter(self, texte):
        """Compte dans quelle langue les textes du script sont ecrits (detection automatique)."""
        for code, textes in self.colonnes.items():
            if texte in textes:
                self.votes[code] = self.votes.get(code, 0) + 1

    def langue_detectee(self):
        """Langue du script, figee des qu'elle est reconnue sans ambiguite : sinon un
        changement d'avis renverrait les textes deja traduits vers une autre paire."""
        if self.detectee or not self.votes:
            return self.detectee
        classement = sorted(self.votes.items(), key=lambda kv: -kv[1])
        premier = classement[0]
        second = classement[1][1] if len(classement) > 1 else 0
        # au moins 2 textes reconnus et une avance nette (les mots identiques comptent
        # pour plusieurs langues : "Options", "Zone"...)
        if premier[1] >= 2 and premier[1] > second:
            self.detectee = premier[0]
            _journal("langue du script reconnue : %s (%s)" % (self.detectee, dict(classement)))
        return self.detectee

    @staticmethod
    def _depuis_termes(termes, langue, source, seule=False):
        """Format multilingue : "termes": [{"en": "Close", "fr": "Fermer", ...}, ...].
        seule=True (langue d'entree choisie) : seule la colonne source est lue.
        Sinon chaque texte de chaque langue donne la traduction dans la langue cible,
        quelle que soit la langue du script ; en cas de doublon (meme texte dans deux
        notions), la langue declaree du script passe d'abord, puis l'ordre des colonnes."""
        res = {}
        if not isinstance(termes, list):
            return res
        langues = []
        if source:
            langues.append(source)
        if not seule:
            for t in termes:
                if isinstance(t, dict):
                    for code in t:
                        if code not in langues and not code.startswith("_"):
                            langues.append(code)
        # detection automatique : un texte deja valable dans la langue cible n'est pas
        # traduit (un script ecrit en anglais garde "Surface", meme si "Surface" (fr)
        # veut dire "Area"). Avec une langue d'entree choisie, on sait ce qu'est le texte.
        deja = set()
        if not seule:
            for t in termes:
                if isinstance(t, dict) and isinstance(t.get(langue), str):
                    deja.add(t[langue])
        for code in langues:
            if code == langue:
                continue
            for t in termes:
                if not isinstance(t, dict):
                    continue
                cible = t.get(langue)
                texte = t.get(code)
                if (isinstance(cible, str) and cible and isinstance(texte, str) and texte
                        and texte != cible and texte not in deja):
                    res.setdefault(texte, cible)
        return res

    def _casse(self, texte):
        t = self.table_min.get(texte.casefold())
        if t is None:
            return None
        if texte.isupper():
            return t.upper()
        # (l'allemand garde la majuscule de ses noms)
        if texte[:1].islower() and t[:1].isupper() and self.langue != "de":
            return t[:1].lower() + t[1:]
        if texte[:1].isupper() and t[:1].islower():
            return t[:1].upper() + t[1:]
        return t

    @staticmethod
    def _compiler_modeles(table):
        import re
        modeles = []
        for cle, val in table.items():
            if "{" not in cle:
                continue
            noms = re.findall(r"\{([A-Za-z_][A-Za-z0-9_]*)\}", cle)
            if not noms:
                continue
            morceaux = re.split(r"\{[A-Za-z_][A-Za-z0-9_]*\}", cle)
            motif = ""
            for i, m in enumerate(morceaux):
                motif += re.escape(m)
                if i < len(noms):
                    nom = noms[i]
                    if nom in noms[:i]:
                        motif += "(?P=%s)" % nom
                    elif nom in _NOMS_NOMBRE:
                        # {n} = un nombre : "{n} elements" ne doit pas avaler
                        # "Only the selected elements" ("Only the selected éléments")
                        motif += r"(?P<%s>[-+]?\d+(?:[.,  ]\d+)*)" % nom
                    else:
                        # {nom} = un texte court sans retour a la ligne
                        motif += r"(?P<%s>[^\n]{1,80}?)" % nom
            try:
                modeles.append((re.compile("^" + motif + "$", re.S), val))
            except re.error:
                pass
        # les modeles les plus longs (les plus precis) d'abord
        modeles.sort(key=lambda m: -len(m[0].pattern))
        return modeles

    # --- traduction -------------------------------------------------------------------
    def traduire(self, texte):
        if not self.langue or not isinstance(texte, str) or not texte.strip():
            return texte
        r = self.cache.get(texte)
        if r is not None:
            return r
        if texte not in self.vus:
            self.vus.add(texte)
            self._voter(texte)
        r = self._traduire(texte, True)
        if r is None and texte.strip() in self.colonnes.get(self.langue, ()):
            r = texte    # deja valable dans la langue de sortie ("Options", "Zone"...)
        if r is None:
            # phrase hors dictionnaire : traduction automatique hors ligne (si activee).
            # En attente, le texte d'origine reste affiche et n'est pas memorise : il sera
            # retraduit quand la reponse arrivera.
            r, en_attente = self.auto.traduire(texte)
            if en_attente:
                return texte
            if r is None:
                r = texte
                self._noter_manquant(texte)
        self.cache[texte] = r
        return r

    def _traduire(self, texte, lignes):
        t = self.table.get(texte)
        if t is not None:
            return t
        # espaces et ponctuation de fin : "Nom :" -> "Name:" si "Nom" est connu
        coeur, avant, apres = self._decouper(texte)
        if coeur != texte and coeur:
            t = self.table.get(coeur)
            if t is None:
                t = self._modele(coeur)
            if t is None:
                t = self._casse(coeur)
            if t is not None:
                if self.langue == "fr":
                    # typographie francaise : espace avant : ? ! ;
                    if apres[:1] in (":", "?", "!", ";"):
                        apres = " " + apres
                else:
                    apres = apres.replace(" ", " ")
                    for p in (" :", " ?", " !", " ;"):
                        if apres.startswith(p):
                            apres = apres[1:]
                            break
                return avant + t + apres
        t = self._modele(texte)
        if t is not None:
            return t
        t = self._casse(texte)
        if t is not None:
            return t
        if lignes and "\n" in texte:
            parties = texte.split("\n")
            res = []
            change = False
            for p in parties:
                tp = self._traduire(p, False) if p.strip() else None
                if tp is None:
                    res.append(p)
                    if p.strip():
                        self._noter_manquant(p)
                else:
                    res.append(tp)
                    change = True
            if change:
                return "\n".join(res)
        return None

    @staticmethod
    def _decouper(texte):
        debut = len(texte) - len(texte.lstrip())
        fin = len(texte.rstrip())
        coeur = texte[debut:fin]
        avant = texte[:debut]
        apres = texte[fin:]
        while coeur and coeur[-1] in ":?!;.…" and not coeur.endswith(".."):
            # garder "..." des menus qui ouvrent un dialogue
            apres = coeur[-1] + apres
            coeur = coeur[:-1]
        if coeur.endswith("..."):
            apres = "..." + apres
            coeur = coeur[:-3]
        espace = len(coeur) - len(coeur.rstrip())
        if espace:
            apres = coeur[len(coeur) - espace:] + apres
            coeur = coeur.rstrip()
        return coeur, avant, apres

    def _modele(self, texte):
        for regex, gabarit in self.modeles:
            m = regex.match(texte)
            if m:
                try:
                    return gabarit.format(**m.groupdict())
                except Exception:
                    return None
        return None

    # --- textes manquants -------------------------------------------------------------
    def _noter_manquant(self, texte):
        if not self.collecte or not self.langue:
            return
        if not any(c.isalpha() for c in texte) or len(texte) > 2000:
            return
        d = self.manquants.setdefault(self.langue, {})
        if texte not in d:
            d[texte] = ""
            self.manquants_modifies = True

    def ecrire_manquants(self):
        if not self.manquants_modifies:
            return
        import json
        self.manquants_modifies = False
        try:
            dossier = os.path.join(self.racine, "manquants")
            os.makedirs(dossier, exist_ok=True)
            chemin = os.path.join(dossier, self._nom_outil() + ".json")
            existant = self._lire_json(chemin)
            if not isinstance(existant, dict):
                existant = {}
            existant.setdefault("langue_source", self.langue_source or "")
            trad = existant.setdefault("traductions", {})
            for langue, textes in self.manquants.items():
                cible = trad.setdefault(langue, {})
                for t in textes:
                    cible.setdefault(t, "")
            tmp = chemin + ".tmp"
            with open(tmp, "w", encoding="utf-8") as f:
                json.dump(existant, f, ensure_ascii=False, indent=2, sort_keys=True)
            os.replace(tmp, chemin)
        except Exception as e:
            _journal("ecriture des manquants impossible : %r" % e)


class _Automatique(object):
    """Traduction automatique hors ligne des textes que les dictionnaires ne connaissent pas.

    Les textes partent en arriere-plan (fil separe, jamais dans la boucle Tk) vers le
    moteur local TapirTranslatorMoteur.exe (127.0.0.1, lance par l'add-on). Le texte
    d'origine reste affiche le temps du calcul, puis la traduction s'affiche toute seule.
    Les resultats sont gardes dans cache\\<source>-<cible>.json, partage par tous les
    scripts : la fois suivante, l'affichage est immediat. Un dictionnaire l'emporte
    toujours sur le moteur."""

    LOT = 16                  # textes par requete
    PAUSE_PANNE = 15.0        # moteur injoignable : nouvel essai apres ce delai (s)
    DELAI_DETECTION = 2.0     # langue du script inconnue : attente avant envoi sans langue (s)

    def __init__(self, racine, moteur):
        import threading
        self.racine = racine
        self.moteur = moteur
        self.actif = False
        self.source = ""
        self.cible = ""
        self.port = "8779"
        self.verrou = threading.Lock()
        self.evenement = threading.Event()
        self.memoire = {}         # (source, cible) -> {texte: traduction, "" = echec}
        self.attente = []         # [(source, cible, texte)] a envoyer
        self.demandes = set()     # deja dans la file ou en cours
        self.modifies = set()     # paires a enregistrer
        self.nouveau = False      # des traductions sont arrivees
        self.panne_jusqua = 0.0
        self.panne_signalee = False
        self.fournisseur = "mymemory"
        self.echecs_en_panne = False
        self.debut_detection = None
        self.attend_detection = False
        self.fil = None

    def configurer(self, actif, source, cible, port, fournisseur="mymemory"):
        self.actif = actif
        self.source = source
        self.cible = cible
        self.fournisseur = fournisseur or "mymemory"
        if port:
            self.port = port
        if actif and self.fil is None:
            import threading
            self.fil = threading.Thread(target=self._travailler, name="TapirTranslatorAuto", daemon=True)
            self.fil.start()

    @staticmethod
    def traduisible(texte):
        """Les donnees affichees dans des Label (chemins, fichiers, identifiants, codes)
        ne partent pas au moteur."""
        import re
        t = texte.strip()
        if len(t) < 2 or len(t) > 600 or sum(1 for c in t if c.isalpha()) < 2:
            return False
        if re.search(r"[\\/]|://|@|\.(py|json|txt|csv|pln|pla|tpl|ifc|dwg|dxf|xlsx?|gsm|png|jpe?g|pdf)\b", t, re.I):
            return False
        if " " not in t:
            if any(c.isdigit() for c in t) or "_" in t or "." in t.strip("."):
                return False
            if t.isupper() and len(t) <= 6:
                return False
        return True

    def _chemin(self, fournisseur, source, cible):
        # un cache par traducteur : changer de traducteur redonne ses propres traductions
        return os.path.join(self.racine, "cache", fournisseur, "%s-%s.json" % (source or "auto", cible))

    def _charger(self, cle):
        if cle in self.memoire:
            return
        d = _Moteur._lire_json(self._chemin(*cle))
        trad = d.get("traductions") if isinstance(d, dict) else None
        self.memoire[cle] = dict(trad) if isinstance(trad, dict) else {}

    def traduire(self, texte):
        """Rend (traduction ou None, en_attente)."""
        if not self.actif or not self.traduisible(texte):
            return None, False
        import time
        source = self.source or self.moteur.langue_detectee()
        if not source:
            # langue du script pas encore reconnue : on attend un peu les autres textes
            if self.debut_detection is None:
                self.debut_detection = time.time()
            if time.time() - self.debut_detection < self.DELAI_DETECTION:
                self.attend_detection = True
                return None, True
        if source == self.cible:
            return None, False
        cle = (self.fournisseur, source, self.cible)
        with self.verrou:
            self._charger(cle)
            t = self.memoire[cle].get(texte)
            if t is not None:
                return (t or None), False
            if time.time() < self.panne_jusqua:
                self.echecs_en_panne = True
                return None, False
            demande = (self.fournisseur, source, self.cible, texte)
            if demande not in self.demandes:
                self.demandes.add(demande)
                self.attente.append(demande)
                self.evenement.set()
        return None, True

    def a_reappliquer(self):
        """Appele par la boucle Tk : True si l'affichage doit etre recalcule."""
        import time
        r = False
        with self.verrou:
            if self.nouveau:
                self.nouveau = False
                r = True
            if self.echecs_en_panne and time.time() >= self.panne_jusqua:
                self.echecs_en_panne = False
                r = True
        if self.attend_detection and (self.moteur.langue_detectee() or
                                      time.time() - (self.debut_detection or 0) >= self.DELAI_DETECTION):
            self.attend_detection = False
            r = True
        return r

    def _travailler(self):
        import json
        import time
        import urllib.error
        import urllib.request
        # pas de proxy pour 127.0.0.1 (HTTP_PROXY du poste)
        ouvreur = urllib.request.build_opener(urllib.request.ProxyHandler({}))
        while True:
            self.evenement.wait()
            time.sleep(0.05)   # laisser arriver les textes de la meme fenetre
            with self.verrou:
                lot = self.attente[:self.LOT]
                del self.attente[:self.LOT]
                if not self.attente:
                    self.evenement.clear()
            groupes = {}
            for fournisseur, source, cible, texte in lot:
                groupes.setdefault((fournisseur, source, cible), []).append(texte)
            for (fournisseur, source, cible), textes in groupes.items():
                cle = (fournisseur, source, cible)
                try:
                    corps = json.dumps({"fournisseur": fournisseur, "source": source, "cible": cible,
                                        "textes": textes}, ensure_ascii=False).encode("utf-8")
                    requete = urllib.request.Request(
                        "http://127.0.0.1:%s/traduire" % self.port, data=corps,
                        headers={"Content-Type": "application/json; charset=utf-8"})
                    with ouvreur.open(requete, timeout=120) as reponse:
                        res = json.loads(reponse.read().decode("utf-8"))
                    trads = res.get("traductions") if isinstance(res, dict) else None
                    if not isinstance(trads, list) or len(trads) != len(textes):
                        raise ValueError("reponse incomplete")
                    resultats = [t if isinstance(t, str) and t.strip() else "" for t in trads]
                except Exception as e:
                    detail = repr(e)
                    if isinstance(e, urllib.error.HTTPError):
                        try:
                            detail = json.loads(e.read().decode("utf-8")).get("erreur", detail)
                        except Exception:
                            pass
                        if e.code == 400:
                            # langue refusee, traducteur inconnu : echec memorise, pas de nouvel essai
                            _journal("traduction refusee (%s) : %s" % (fournisseur, detail))
                            with self.verrou:
                                self._charger(cle)
                                for t in textes:
                                    self.memoire[cle][t] = ""
                                    self.demandes.discard((fournisseur, source, cible, t))
                            continue
                    # 503 (quota, cle, reseau du service) ou moteur injoignable : texte d'origine
                    # garde, nouvel essai plus tard ; signale une seule fois tant que ca dure
                    with self.verrou:
                        for t in textes:
                            self.demandes.discard((fournisseur, source, cible, t))
                        if not self.panne_signalee:
                            self.panne_signalee = True
                            _journal("traduction en ligne indisponible (%s, port %s) : %s ; nouvel essai toutes les %d s"
                                     % (fournisseur, self.port, detail, int(self.PAUSE_PANNE)))
                        self.panne_jusqua = time.time() + self.PAUSE_PANNE
                        self.echecs_en_panne = True
                    continue
                with self.verrou:
                    if self.panne_signalee:
                        self.panne_signalee = False
                        _journal("traduction en ligne retablie (%s)" % fournisseur)
                    self._charger(cle)
                    m = self.memoire[cle]
                    for t, r in zip(textes, resultats):
                        m[t] = r
                        self.demandes.discard((fournisseur, source, cible, t))
                    self.modifies.add(cle)
                    self.nouveau = True
            self._enregistrer()

    def _enregistrer(self):
        import json
        with self.verrou:
            paires = list(self.modifies)
            self.modifies.clear()
            copies = {p: dict(self.memoire.get(p, {})) for p in paires}
        for (fournisseur, source, cible), traductions in copies.items():
            try:
                chemin = self._chemin(fournisseur, source, cible)
                os.makedirs(os.path.dirname(chemin), exist_ok=True)
                # un autre script a pu ecrire le meme cache : on fusionne
                existant = _Moteur._lire_json(chemin)
                if isinstance(existant, dict) and isinstance(existant.get("traductions"), dict):
                    fusion = dict(existant["traductions"])
                    fusion.update(traductions)
                    traductions = fusion
                tmp = chemin + ".%d.tmp" % os.getpid()
                with open(tmp, "w", encoding="utf-8") as f:
                    json.dump({"fournisseur": fournisseur, "source": source, "cible": cible,
                               "traductions": traductions},
                              f, ensure_ascii=False, indent=1, sort_keys=True)
                os.replace(tmp, chemin)
            except Exception as e:
                _journal("cache de traduction non enregistre : %r" % e)


_moteur = None
_intermediaires = []   # un par fenetre Tk ouverte


# =====================================================================================
#  Intermediaire devant l'interpreteur Tcl d'une fenetre Tk
# =====================================================================================

class _Intermediaire(object):
    """Remplace root.tk. Tout ce qui n'est pas intercepte part tel quel vers
    l'interpreteur reel (self._reel)."""

    def __init__(self, reel):
        self._reel = reel
        # cle -> [texte d'origine, texte affiche]
        self._registre = {}
        self._variables = {}   # nom de variable Tcl affichee -> [origine, affiche]

    def __getattr__(self, nom):
        return getattr(self._reel, nom)

    # --- call ---------------------------------------------------------------------------
    def call(self, *args):
        if len(args) == 1 and isinstance(args[0], tuple):
            args = args[0]
        apres = None
        try:
            argv = list(args)
            apres = self._avant(argv)
            args = tuple(argv)
        except Exception as e:
            _journal("interception call : %r" % e)
            apres = None
        res = self._reel.call(*args)
        if apres is not None:
            try:
                res = apres(res)
            except Exception as e:
                _journal("apres call : %r" % e)
        return res

    def _tr(self, valeur):
        if isinstance(valeur, str):
            return _moteur.traduire(valeur)
        return valeur

    def _noter(self, cle, origine, affiche):
        if isinstance(origine, str):
            self._registre[cle] = [origine, affiche]

    def _rendre(self, cle, res):
        e = self._registre.get(cle)
        if e is not None and isinstance(res, str) and res == e[1]:
            return e[0]
        if e is not None and not isinstance(res, str) and str(res) == e[1]:
            return e[0]
        return res

    def _options(self, argv, debut):
        """Traduit les options texte de argv[debut:] ; rend [(option, origine, affiche)]."""
        faits = []
        i = debut
        n = len(argv)
        while i + 1 < n:
            opt = argv[i]
            if isinstance(opt, str) and opt in _OPTIONS_TEXTE and isinstance(argv[i + 1], str):
                origine = argv[i + 1]
                affiche = self._tr(origine)
                argv[i + 1] = affiche
                faits.append((opt, origine, affiche))
            i += 2
        return faits

    def _variable_texte(self, argv, debut, classe):
        """-textvariable d'un widget d'affichage : la variable devient traduite."""
        i = debut
        n = len(argv)
        while i + 1 < n:
            if argv[i] == "-textvariable":
                nom = str(argv[i + 1])
                if nom and self._est_affichage(classe):
                    self._lier_variable(nom)
            i += 2

    @staticmethod
    def _est_affichage(classe):
        c = str(classe).lower()
        if c.startswith("ttk::"):
            c = "t" + c[5:]
        return c in _CLASSES_AFFICHAGE

    def _lier_variable(self, nom):
        if nom in self._variables:
            return
        try:
            val = self._reel.globalgetvar(nom)
        except Exception:
            val = None
        if isinstance(val, str):
            aff = self._tr(val)
            self._variables[nom] = [val, aff]
            if aff != val:
                self._reel.globalsetvar(nom, aff)
        else:
            self._variables[nom] = [None, None]

    def _avant(self, argv):
        n = len(argv)
        if n < 2 or not isinstance(argv[0], str):
            return None
        a0 = argv[0]
        a1 = argv[1] if isinstance(argv[1], str) else str(argv[1])

        # titre de fenetre
        if a0 == "wm" and a1 == "title" and n >= 3:
            chemin = str(argv[2])
            cle = ("titre", chemin, None, None)
            # Wm.title() interroge avec un 4e argument None
            if n >= 4 and argv[3] is not None:
                origine = argv[3]
                argv[3] = self._tr(origine)
                self._noter(cle, origine, argv[3])
                return None
            return lambda r: self._rendre(cle, r)

        # boites de message et dialogues de fichiers
        if a0 in _DIALOGUES:
            i = 1
            while i + 1 < n:
                if argv[i] in _OPTIONS_DIALOGUE:
                    argv[i + 1] = self._tr(argv[i + 1])
                i += 2
            return None

        # commande d'un widget existant : ".chemin sous-commande ..."
        if a0.startswith("."):
            return self._sous_commande(argv, a0, a1, n)

        # creation d'un widget : "classe .chemin -option valeur ..."
        if a1.startswith("."):
            chemin = a1
            for opt, origine, affiche in self._options(argv, 2):
                self._noter(("opt", chemin, None, opt), origine, affiche)
            self._variable_texte(argv, 2, a0)
        return None

    def _sous_commande(self, argv, chemin, sous, n):
        if sous == "configure":
            if n >= 4:
                for opt, origine, affiche in self._options(argv, 2):
                    self._noter(("opt", chemin, None, opt), origine, affiche)
                if "-textvariable" in argv[2::2]:
                    try:
                        classe = self._reel.call("winfo", "class", chemin)
                    except Exception:
                        classe = ""
                    self._variable_texte(argv, 2, classe)
            elif n == 3 and argv[2] in _OPTIONS_TEXTE:
                cle = ("opt", chemin, None, argv[2])

                def rendre(r, cle=cle):
                    # r = (option, nom bd, classe, defaut, valeur)
                    try:
                        if isinstance(r, tuple) and len(r) == 5:
                            return r[:4] + (self._rendre(cle, r[4]),)
                    except Exception:
                        pass
                    return r
                return rendre
            return None

        if sous == "cget" and n == 3 and argv[2] in _OPTIONS_TEXTE:
            cle = ("opt", chemin, None, argv[2])
            return lambda r: self._rendre(cle, r)

        # menus : add / insert / entryconfigure / entrycget
        if sous == "add" and n >= 3 and argv[2] in _TYPES_MENU:
            faits = self._options(argv, 3)
            if not faits:
                return None

            def noter(r, faits=faits):
                idx = self._reel.call(chemin, "index", "end")
                for opt, origine, affiche in faits:
                    self._noter(("menu", chemin, str(idx), opt), origine, affiche)
                return r
            return noter
        if sous == "insert" and n >= 4 and argv[3] in _TYPES_MENU:
            faits = self._options(argv, 4)
            if not faits:
                return None
            position = argv[2]

            def noter(r, faits=faits, position=position):
                idx = self._reel.call(chemin, "index", position)
                for opt, origine, affiche in faits:
                    self._noter(("menu", chemin, str(idx), opt), origine, affiche)
                return r
            return noter
        if sous == "entryconfigure" and n >= 5:
            try:
                idx = str(self._reel.call(chemin, "index", argv[2]))
            except Exception:
                idx = str(argv[2])
            for opt, origine, affiche in self._options(argv, 3):
                self._noter(("menu", chemin, idx, opt), origine, affiche)
            return None
        if sous == "entrycget" and n == 4 and argv[3] in _OPTIONS_TEXTE:
            try:
                idx = str(self._reel.call(chemin, "index", argv[2]))
            except Exception:
                idx = str(argv[2])
            cle = ("menu", chemin, idx, argv[3])
            return lambda r: self._rendre(cle, r)

        # onglets de ttk.Notebook : add .enfant -text ... / tab id -text ...
        # (un onglet se designe par sa fenetre ou par son numero : on note le numero)
        if sous == "add" and n >= 3 and str(argv[2]).startswith("."):
            enfant = argv[2]
            faits = self._options(argv, 3)
            if not faits:
                return None

            def noter(r, faits=faits, enfant=enfant):
                idx = str(self._reel.call(chemin, "index", enfant))
                for opt, origine, affiche in faits:
                    self._noter(("onglet", chemin, idx, opt), origine, affiche)
                return r
            return noter
        if sous == "tab" and n >= 4:
            try:
                onglet = str(self._reel.call(chemin, "index", argv[2]))
            except Exception:
                onglet = str(argv[2])
            if n == 4 and argv[3] in _OPTIONS_TEXTE:
                cle = ("onglet", chemin, onglet, argv[3])
                return lambda r: self._rendre(cle, r)
            for opt, origine, affiche in self._options(argv, 3):
                self._noter(("onglet", chemin, onglet, opt), origine, affiche)
            return None

        # en-tetes de colonnes de ttk.Treeview (les lignes sont des donnees : pas touchees)
        if sous == "heading" and n >= 4:
            colonne = str(argv[2])
            if n == 4 and argv[3] in _OPTIONS_TEXTE:
                cle = ("entete", chemin, colonne, argv[3])
                return lambda r: self._rendre(cle, r)
            for opt, origine, affiche in self._options(argv, 3):
                self._noter(("entete", chemin, colonne, opt), origine, affiche)
            return None

        # textes de canevas
        if sous == "create" and n >= 3 and argv[2] == "text":
            # les coordonnees peuvent etre negatives ("-5") : la 1re option commence par "-lettre"
            debut = 3
            while debut < n and not (isinstance(argv[debut], str) and len(argv[debut]) > 1
                                     and argv[debut][0] == "-" and argv[debut][1].isalpha()):
                debut += 1
            faits = self._options(argv, debut)
            if not faits:
                return None

            def noter(r, faits=faits):
                for opt, origine, affiche in faits:
                    self._noter(("canevas", chemin, str(r), opt), origine, affiche)
                return r
            return noter
        if sous == "itemconfigure" and n >= 5:
            item = str(argv[2])
            for opt, origine, affiche in self._options(argv, 3):
                self._noter(("canevas", chemin, item, opt), origine, affiche)
            return None
        if sous == "itemcget" and n == 4 and argv[3] in _OPTIONS_TEXTE:
            cle = ("canevas", chemin, str(argv[2]), argv[3])
            return lambda r: self._rendre(cle, r)
        return None

    # --- variables Tcl (StringVar...) -----------------------------------------------------
    def _ecrire_variable(self, fonction, nom, valeur, reste):
        e = self._variables.get(str(nom))
        if e is not None and isinstance(valeur, str):
            aff = self._tr(valeur)
            e[0], e[1] = valeur, aff
            return fonction(nom, aff, *reste)
        return fonction(nom, valeur, *reste)

    def _lire_variable(self, fonction, nom, reste):
        r = fonction(nom, *reste)
        e = self._variables.get(str(nom))
        if e is not None and e[1] is not None and isinstance(r, str) and r == e[1]:
            return e[0]
        return r

    def globalsetvar(self, nom, valeur, *reste):
        return self._ecrire_variable(self._reel.globalsetvar, nom, valeur, reste)

    def setvar(self, nom, valeur, *reste):
        return self._ecrire_variable(self._reel.setvar, nom, valeur, reste)

    def globalgetvar(self, nom, *reste):
        return self._lire_variable(self._reel.globalgetvar, nom, reste)

    def getvar(self, nom, *reste):
        return self._lire_variable(self._reel.getvar, nom, reste)

    # --- changement de langue en direct ----------------------------------------------------
    def reappliquer(self):
        reel = self._reel
        for cle in list(self._registre.keys()):
            origine, ancien = self._registre[cle]
            nouveau = _moteur.traduire(origine)
            if nouveau == ancien:
                continue
            genre, chemin, sous_id, opt = cle
            try:
                if genre == "opt":
                    actuel = reel.call(chemin, "cget", opt)
                elif genre == "titre":
                    actuel = reel.call("wm", "title", chemin)
                elif genre == "menu":
                    actuel = reel.call(chemin, "entrycget", sous_id, opt)
                elif genre == "onglet":
                    actuel = reel.call(chemin, "tab", sous_id, opt)
                elif genre == "entete":
                    actuel = reel.call(chemin, "heading", sous_id, opt)
                elif genre == "canevas":
                    actuel = reel.call(chemin, "itemcget", sous_id, opt)
                else:
                    continue
                if str(actuel) != ancien:
                    # le texte a change sans passer par nous : on ne l'ecrase pas
                    del self._registre[cle]
                    continue
                if genre == "opt":
                    reel.call(chemin, "configure", opt, nouveau)
                elif genre == "titre":
                    reel.call("wm", "title", chemin, nouveau)
                elif genre == "menu":
                    reel.call(chemin, "entryconfigure", sous_id, opt, nouveau)
                elif genre == "onglet":
                    reel.call(chemin, "tab", sous_id, opt, nouveau)
                elif genre == "entete":
                    reel.call(chemin, "heading", sous_id, opt, nouveau)
                elif genre == "canevas":
                    reel.call(chemin, "itemconfigure", sous_id, opt, nouveau)
                self._registre[cle][1] = nouveau
            except Exception:
                # widget detruit, element de menu disparu...
                self._registre.pop(cle, None)
        for nom in list(self._variables.keys()):
            e = self._variables[nom]
            if e[0] is None:
                try:
                    val = reel.globalgetvar(nom)
                except Exception:
                    continue
                if isinstance(val, str):
                    e[0], e[1] = val, val
                else:
                    continue
            nouveau = _moteur.traduire(e[0])
            if nouveau == e[1]:
                continue
            try:
                if reel.globalgetvar(nom) == e[1]:
                    reel.globalsetvar(nom, nouveau)
                    e[1] = nouveau
            except Exception:
                self._variables.pop(nom, None)


# =====================================================================================
#  Branchement sur tkinter
# =====================================================================================

def _surveiller(racine):
    """Relit reglages et dictionnaires toutes les _PERIODE_MS tant que la fenetre vit."""
    def tic():
        try:
            change = _moteur.recharger_si_change()
            if _moteur.auto.a_reappliquer():
                _moteur.cache = {}   # des traductions automatiques sont arrivees
                change = True
            if change:
                for inter in list(_intermediaires):
                    inter.reappliquer()
            _moteur.ecrire_manquants()
        except Exception as e:
            _journal("surveillance : %r" % e)
        try:
            racine.after(_PERIODE_MS, tic)
        except Exception:
            pass
    try:
        racine.after(_PERIODE_MS, tic)
    except Exception as e:
        _journal("after impossible : %r" % e)


def _brancher(tkinter):
    if getattr(tkinter, "_tapir_translator", False):
        return
    tkinter._tapir_translator = True
    init_origine = tkinter.Tk.__init__

    def init(self, *args, **kwargs):
        init_origine(self, *args, **kwargs)
        try:
            if not isinstance(self.tk, _Intermediaire):
                inter = _Intermediaire(self.tk)
                self.tk = inter
                _intermediaires.append(inter)
                _surveiller(self)
        except Exception as e:
            _journal("branchement Tk : %r" % e)

    init.__wrapped__ = init_origine
    tkinter.Tk.__init__ = init

    destroy_origine = tkinter.Tk.destroy

    def destroy(self):
        inter = self.__dict__.get("tk")
        try:
            if isinstance(inter, _Intermediaire) and inter in _intermediaires:
                _intermediaires.remove(inter)
            _moteur.ecrire_manquants()
        except Exception:
            pass
        return destroy_origine(self)

    tkinter.Tk.destroy = destroy
    _journal("tkinter branche (outil=%s, couche %s)" % (_moteur._nom_outil(), VERSION_COUCHE))


class _Chargeur(object):
    def __init__(self, chargeur):
        self._chargeur = chargeur

    def create_module(self, spec):
        return self._chargeur.create_module(spec)

    def exec_module(self, module):
        self._chargeur.exec_module(module)
        try:
            _brancher(module)
        except Exception as e:
            _journal("branchement : %r" % e)

    def __getattr__(self, nom):
        return getattr(self._chargeur, nom)


class _Crochet(object):
    """Attend l'import de tkinter pour le brancher (rien n'est importe avant)."""

    def find_spec(self, nom, chemin=None, cible=None):
        if nom != "tkinter":
            return None
        try:
            from importlib.machinery import PathFinder
            spec = PathFinder.find_spec(nom, chemin)
            if spec is None or spec.loader is None:
                return None
            spec.loader = _Chargeur(spec.loader)
            return spec
        except Exception as e:
            _journal("crochet : %r" % e)
            return None


def _installer():
    global _moteur
    racine = _dossier()
    _moteur = _Moteur(racine)
    _moteur.recharger_si_change()
    if "tkinter" in sys.modules:
        _brancher(sys.modules["tkinter"])
    else:
        sys.meta_path.insert(0, _Crochet())
    import atexit
    atexit.register(lambda: _moteur.ecrire_manquants())


def _chainer():
    """Un autre sitecustomize (environnement Python du poste) doit encore s'executer."""
    try:
        ici = os.path.normcase(os.path.abspath(os.path.dirname(__file__)))
        for p in list(sys.path):
            try:
                if not p or os.path.normcase(os.path.abspath(p)) == ici:
                    continue
            except Exception:
                continue
            candidat = os.path.join(p, "sitecustomize.py")
            if os.path.isfile(candidat):
                import importlib.util
                spec = importlib.util.spec_from_file_location("_sitecustomize_suivant", candidat)
                if spec and spec.loader:
                    module = importlib.util.module_from_spec(spec)
                    spec.loader.exec_module(module)
                return
    except Exception as e:
        _journal("sitecustomize suivant : %r" % e)


def _demarrer():
    actif = bool(os.environ.get("TAPIR_SCRIPT_ID")) or os.environ.get("TAPIR_TRANSLATOR_FORCE") == "1"
    if actif and _dossier():
        try:
            _installer()
        except Exception as e:
            _journal("installation impossible : %r" % e)
    _chainer()


_demarrer()
