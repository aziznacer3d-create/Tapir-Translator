# /// script
# requires-python = ">=3.10"
# dependencies = []
# ///
"""
Fake Tapir script used by the Tapir Translator test bench (Tests/BancTraducteur.cpp).

What this file is (and is not)
------------------------------
This file is NOT the translation layer: it is the tool to be translated, written like an
ordinary Tapir script (a tkinter interface in French, with no knowledge at all of the
translation). The inline "# /// script" block above is the PEP 723 metadata that uv reads.
The bench starts it with "uv run --script banc_outil.py --port N", exactly like the Tapir
palette does, so the translation layer (sitecustomize.py, placed on the PYTHONPATH by the
add-on code) is imported automatically by Python before this script runs.

What it reports
---------------
Every 150 ms it writes the state of its interface to stdout, but only when it changed:
  V|<key>|<text displayed by Tk>|<text seen by the script>
  FIN_ETAT
The displayed text is read directly in the real Tcl interpreter (bypassing the layer):
it is what the user sees on screen. The seen text goes through the normal tkinter API
(and therefore through the layer): it is what the script logic receives, and it must
always stay in the original language. Newlines are written as a literal backslash-n.
Other lines it prints:
  COUCHE|active or COUCHE|absente  -> whether the layer is installed in front of Tcl
  FIN_OUTIL                        -> the Tk main loop has ended

Commands (file <BANC_ECHANGE>\\commande.txt, written by the bench, polled every tick):
  maj  -> the script itself changes some texts (status, label) while being translated
  fin  -> close the window and exit
"""
import os
import sys
import tkinter as tk
from tkinter import ttk

# Exchange folder shared with the bench (holds commande.txt).
ECHANGE = os.environ.get("BANC_ECHANGE", ".")
# BANC_UI=en: the same interface written in English instead of French, to emulate a script
# whose source language is not declared (the layer must detect it). Only the texts listed
# here change; every other text stays in French.
T = ({"Fermer": "Close", "Nom :": "Name:", "Murs": "Walls", "Prêt": "Ready", "Terminé": "Done",
      "Quitter": "Quit", "Fichier": "File", "Tous les étages": "All stories"}
     if os.environ.get("BANC_UI") == "en" else {})


# Returns the text in the interface language chosen by BANC_UI (French by default).
def t(texte):
    return T.get(texte, texte)


# --- Interface: one widget of each kind the layer is supposed to translate (or not) ---
root = tk.Tk()
root.title("Outil de démonstration")

# Status shown through a StringVar (the layer translates the displayed variable only).
statut = tk.StringVar(value=t("Prêt"))
tk.Label(root, textvariable=statut).pack()

# Plain "text" options of Tk and ttk widgets.
btn_fermer = tk.Button(root, text=t("Fermer"))
btn_fermer.pack()
lbl_nom = ttk.Label(root, text=t("Nom :"))
lbl_nom.pack()
cb_murs = tk.Checkbutton(root, text=t("Murs"))
cb_murs.pack()
# "Options" is already valid English: it must never be sent to the online engine.
cadre = ttk.LabelFrame(root, text="Options")
cadre.pack()
# Multi-line text, and a text with a number (matches a "{n} ..." dictionary pattern).
lbl_multi = tk.Label(root, text="Première ligne\nDeuxième ligne")
lbl_multi.pack()
lbl_modele = tk.Label(root, text="3 murs créés")
lbl_modele.pack()
# Text absent from every dictionary: only the online/test engine can translate it.
lbl_inconnu = tk.Label(root, text="Texte inconnu")
lbl_inconnu.pack()
# Data shown in Labels (a path, an element code): never sent to the automatic engine.
lbl_chemin = tk.Label(root, text="C:\\Projets\\plan.pln")
lbl_chemin.pack()
lbl_code = tk.Label(root, text="MUR-01")
# Sentence where an Archicad term is mistranslated by online translators ("story" -> "histoire");
# used to test the term corrections (corrections.json).
lbl_etages = tk.Label(root, text=t("Tous les étages"))
lbl_etages.pack()
lbl_code.pack()

# Menu bar: cascade label and command labels.
barre = tk.Menu(root, tearoff=0)
menu_fichier = tk.Menu(barre, tearoff=0)
menu_fichier.add_command(label="Ouvrir...")
menu_fichier.add_separator()
menu_fichier.add_command(label=t("Quitter"))
barre.add_cascade(label=t("Fichier"), menu=menu_fichier)
root.config(menu=barre)

# Notebook tabs.
carnet = ttk.Notebook(root)
p1 = ttk.Frame(carnet)
p2 = ttk.Frame(carnet)
carnet.add(p1, text="Général")
carnet.add(p2, text="Avancé")
carnet.pack()

# Treeview: column headings are translated, row contents are data and are not.
arbre = ttk.Treeview(root, columns=("valeur",))
arbre.heading("#0", text="Nom")
arbre.heading("valeur", text="Valeur")
ligne = arbre.insert("", "end", text="Murs", values=("Dalles",))
arbre.pack()

# Canvas text item.
canevas = tk.Canvas(root, width=120, height=40)
texte_canevas = canevas.create_text(-5, 20, text="Plan", anchor="w")
canevas.pack()

# Combobox values and Entry contents are data: never translated.
combo = ttk.Combobox(root, values=["Murs", "Dalles"])
combo.set("Murs")
combo.pack()
saisie = tk.Entry(root)
saisie.insert(0, "Murs")
saisie.pack()


# Returns the real Tcl interpreter. When the layer is active, root.tk is its proxy and the
# original interpreter is kept in its "_reel" attribute; otherwise root.tk is the real one.
def reel():
    return getattr(root.tk, "_reel", root.tk)


# Builds the interface state as a list of (key, displayed text, text seen by the script):
# the displayed text is read from the real interpreter, the seen text through tkinter.
def etat():
    r = reel()
    e = []
    e.append(("titre", r.call("wm", "title", "."), root.title()))
    e.append(("statut", r.globalgetvar(str(statut)), statut.get()))
    for cle, w in (("fermer", btn_fermer), ("nom", lbl_nom), ("murs", cb_murs), ("cadre", cadre),
                   ("multi", lbl_multi), ("modele", lbl_modele), ("inconnu", lbl_inconnu),
                   ("chemin", lbl_chemin), ("code", lbl_code), ("etages", lbl_etages)):
        e.append((cle, r.call(str(w), "cget", "-text"), w.cget("text")))
    e.append(("menu_fichier", r.call(str(barre), "entrycget", 0, "-label"), barre.entrycget(0, "label")))
    e.append(("menu_ouvrir", r.call(str(menu_fichier), "entrycget", 0, "-label"), menu_fichier.entrycget(0, "label")))
    e.append(("menu_quitter", r.call(str(menu_fichier), "entrycget", 2, "-label"), menu_fichier.entrycget(2, "label")))
    e.append(("onglet", r.call(str(carnet), "tab", 0, "-text"), carnet.tab(0, "text")))
    e.append(("entete", r.call(str(arbre), "heading", "#0", "-text"), arbre.heading("#0", "text")))
    e.append(("ligne_arbre", r.call(str(arbre), "item", ligne, "-text"), arbre.item(ligne, "text")))
    e.append(("canevas", r.call(str(canevas), "itemcget", texte_canevas, "-text"), canevas.itemcget(texte_canevas, "text")))
    # For the Combobox, "displayed" = its list of values joined by spaces, "seen" = current value.
    e.append(("combo", " ".join(str(v) for v in combo.cget("values")), combo.get()))
    e.append(("saisie", saisie.get(), saisie.get()))
    # The script logic compares original texts: it must keep working while translated
    # ("ok" when the comparisons still succeed, "CASSEE" otherwise).
    e.append(("logique", "ok" if btn_fermer.cget("text") == t("Fermer") and statut.get() in (t("Prêt"), t("Terminé")) else "CASSEE", ""))
    return e


# Last state written to stdout and last command executed (to act on changes only).
dernier = None
derniere_commande = ""


# Periodic tick (every 150 ms):
#   1. read commande.txt and execute the command if it changed ("maj" or "fin");
#   2. build the interface state;
#   3. if it differs from the last one, write it as V|... lines followed by FIN_ETAT;
#   4. schedule the next tick.
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


# Tell the bench whether the layer replaced root.tk with its proxy.
print("COUCHE|%s" % ("active" if hasattr(root.tk, "_reel") else "absente"), flush=True)
root.after(100, tic)
root.after(60000, root.destroy)   # safety: never run for more than one minute
root.mainloop()
print("FIN_OUTIL", flush=True)
