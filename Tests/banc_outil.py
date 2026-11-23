# /// script
# requires-python = ">=3.10"
# dependencies = []
# ///
"""
Faux script Tapir pour le banc de test de Tapir Translator (Tests/BancTraducteur.cpp).

Ce fichier n'est PAS la couche de traduction : c'est l'outil a traduire, ecrit comme un
script Tapir ordinaire (interface tkinter en francais, aucune connaissance de la
traduction). Le banc le lance par "uv run --script banc_outil.py --port N", comme la
palette Tapir.

Toutes les 150 ms il ecrit sur sa sortie l'etat de son interface si celui-ci a change :
  V|<cle>|<texte affiche par Tk>|<texte vu par le script>
  FIN_ETAT
Le texte affiche est lu directement dans l'interpreteur Tcl (sans la couche), le texte
vu passe par l'API tkinter normale (ce que la logique du script recoit).

Commandes (fichier <BANC_ECHANGE>\\commande.txt, ecrit par le banc) :
  maj  -> le script change lui-meme des textes (statut, libelle) pendant la traduction
  fin  -> fermeture
"""
import os
import sys
import tkinter as tk
from tkinter import ttk

ECHANGE = os.environ.get("BANC_ECHANGE", ".")
# BANC_UI=en : la meme interface ecrite en anglais (script dont la langue n'est pas declaree)
T = ({"Fermer": "Close", "Nom :": "Name:", "Murs": "Walls", "Prêt": "Ready", "Terminé": "Done",
      "Quitter": "Quit", "Fichier": "File"} if os.environ.get("BANC_UI") == "en" else {})


def t(texte):
    return T.get(texte, texte)


root = tk.Tk()
root.title("Outil de démonstration")

statut = tk.StringVar(value=t("Prêt"))
tk.Label(root, textvariable=statut).pack()

btn_fermer = tk.Button(root, text=t("Fermer"))
btn_fermer.pack()
lbl_nom = ttk.Label(root, text=t("Nom :"))
lbl_nom.pack()
cb_murs = tk.Checkbutton(root, text=t("Murs"))
cb_murs.pack()
cadre = ttk.LabelFrame(root, text="Options")
cadre.pack()
lbl_multi = tk.Label(root, text="Première ligne\nDeuxième ligne")
lbl_multi.pack()
lbl_modele = tk.Label(root, text="3 murs créés")
lbl_modele.pack()
lbl_inconnu = tk.Label(root, text="Texte inconnu")
lbl_inconnu.pack()
# donnees affichees dans des Label : jamais envoyees au moteur automatique
lbl_chemin = tk.Label(root, text="C:\\Projets\\plan.pln")
lbl_chemin.pack()
lbl_code = tk.Label(root, text="MUR-01")
lbl_code.pack()

barre = tk.Menu(root, tearoff=0)
menu_fichier = tk.Menu(barre, tearoff=0)
menu_fichier.add_command(label="Ouvrir...")
menu_fichier.add_separator()
menu_fichier.add_command(label=t("Quitter"))
barre.add_cascade(label=t("Fichier"), menu=menu_fichier)
root.config(menu=barre)

carnet = ttk.Notebook(root)
p1 = ttk.Frame(carnet)
p2 = ttk.Frame(carnet)
carnet.add(p1, text="Général")
carnet.add(p2, text="Avancé")
carnet.pack()

arbre = ttk.Treeview(root, columns=("valeur",))
arbre.heading("#0", text="Nom")
arbre.heading("valeur", text="Valeur")
ligne = arbre.insert("", "end", text="Murs", values=("Dalles",))
arbre.pack()

canevas = tk.Canvas(root, width=120, height=40)
texte_canevas = canevas.create_text(-5, 20, text="Plan", anchor="w")
canevas.pack()

combo = ttk.Combobox(root, values=["Murs", "Dalles"])
combo.set("Murs")
combo.pack()
saisie = tk.Entry(root)
saisie.insert(0, "Murs")
saisie.pack()


def reel():
    return getattr(root.tk, "_reel", root.tk)


def etat():
    r = reel()
    e = []
    e.append(("titre", r.call("wm", "title", "."), root.title()))
    e.append(("statut", r.globalgetvar(str(statut)), statut.get()))
    for cle, w in (("fermer", btn_fermer), ("nom", lbl_nom), ("murs", cb_murs), ("cadre", cadre),
                   ("multi", lbl_multi), ("modele", lbl_modele), ("inconnu", lbl_inconnu),
                   ("chemin", lbl_chemin), ("code", lbl_code)):
        e.append((cle, r.call(str(w), "cget", "-text"), w.cget("text")))
    e.append(("menu_fichier", r.call(str(barre), "entrycget", 0, "-label"), barre.entrycget(0, "label")))
    e.append(("menu_ouvrir", r.call(str(menu_fichier), "entrycget", 0, "-label"), menu_fichier.entrycget(0, "label")))
    e.append(("menu_quitter", r.call(str(menu_fichier), "entrycget", 2, "-label"), menu_fichier.entrycget(2, "label")))
    e.append(("onglet", r.call(str(carnet), "tab", 0, "-text"), carnet.tab(0, "text")))
    e.append(("entete", r.call(str(arbre), "heading", "#0", "-text"), arbre.heading("#0", "text")))
    e.append(("ligne_arbre", r.call(str(arbre), "item", ligne, "-text"), arbre.item(ligne, "text")))
    e.append(("canevas", r.call(str(canevas), "itemcget", texte_canevas, "-text"), canevas.itemcget(texte_canevas, "text")))
    e.append(("combo", " ".join(str(v) for v in combo.cget("values")), combo.get()))
    e.append(("saisie", saisie.get(), saisie.get()))
    # la logique du script compare des textes d'origine : elle doit toujours marcher
    e.append(("logique", "ok" if btn_fermer.cget("text") == t("Fermer") and statut.get() in (t("Prêt"), t("Terminé")) else "CASSEE", ""))
    return e


dernier = None
derniere_commande = ""


def tic():
    global dernier, derniere_commande
    try:
        with open(os.path.join(ECHANGE, "commande.txt"), "r", encoding="utf-8") as f:
            commande = f.read().strip()
    except OSError:
        commande = ""
    if commande != derniere_commande:
        derniere_commande = commande
        if commande == "maj":
            statut.set(t("Terminé"))
            lbl_nom.config(text="Valeur")
        elif commande == "fin":
            root.destroy()
            return
    e = etat()
    if e != dernier:
        dernier = e
        lignes = []
        for cle, affiche, vu in e:
            lignes.append("V|%s|%s|%s" % (cle, str(affiche).replace("\n", "\\n"), str(vu).replace("\n", "\\n")))
        lignes.append("FIN_ETAT")
        sys.stdout.write("\n".join(lignes) + "\n")
        sys.stdout.flush()
    root.after(150, tic)


print("COUCHE|%s" % ("active" if hasattr(root.tk, "_reel") else "absente"), flush=True)
root.after(100, tic)
root.after(60000, root.destroy)   # securite : jamais plus d'une minute
root.mainloop()
print("FIN_OUTIL", flush=True)
