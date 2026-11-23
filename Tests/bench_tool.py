# /// script
# requires-python = ">=3.10"
# dependencies = []
# ///
"""
Fake Tapir script used by the Tapir Translator test bench (Tests/TranslatorBench.cpp).

What this file is (and is not)
------------------------------
This file is NOT the translation layer: it is the tool to be translated, written like an
ordinary Tapir script (a tkinter interface in French, with no knowledge at all of the
translation). The inline "# /// script" block above is the PEP 723 metadata that uv reads.
The bench starts it with "uv run --script bench_tool.py --port N", exactly like the Tapir
palette does, so the translation layer (sitecustomize.py, placed on the PYTHONPATH by the
add-on code) is imported automatically by Python before this script runs.

What it reports
---------------
Every 150 ms it writes the state of its interface to stdout, but only when it changed:
  V|<key>|<text displayed by Tk>|<text seen by the script>
  STATUS_END
The displayed text is read directly in the real Tcl interpreter (bypassing the layer):
it is what the user sees on screen. The seen text goes through the normal tkinter API
(and therefore through the layer): it is what the script logic receives, and it must
always stay in the original language. Newlines are written as a literal backslash-n.
Other lines it prints:
  LAYER|active or LAYER|missing  -> whether the layer is installed in front of Tcl
  TOOL_END                        -> the Tk main loop has ended

Commands (file <BENCH_EXCHANGE>\\command_.txt, written by the bench, polled every tick):
  caps  -> the script itself changes some texts (status, label) while being translated
  end_  -> close the window and exit
"""
import os
import sys
import tkinter as tk
from tkinter import ttk

# Exchange folder shared with the bench (holds command_.txt).
EXCHANGE = os.environ.get("BENCH_EXCHANGE", ".")
# BENCH_UI=en: the same interface written in English instead of French, to emulate a script
# whose source language is not declared (the layer must detect it). Only the texts listed
# here change; every other text stays in French.
T = ({"Fermer": "Close", "Nom :": "Name:", "Murs": "Walls", "Prêt": "Ready", "Terminé": "Done",
      "Quitter": "Quit", "Fichier": "File", "Tous les étages": "All stories"}
     if os.environ.get("BENCH_UI") == "en" else {})


# Returns the text in the interface language chosen by BENCH_UI (French by default).
def t(text):
    return T.get(text, text)


# --- Interface: one widget of each kind the layer is supposed to translate (or not) ---
root = tk.Tk()
root.title("Outil de démonstration")

# Status shown through a StringVar (the layer translates the displayed variable only).
status = tk.StringVar(value=t("Prêt"))
tk.Label(root, textvariable=status).pack()

# Plain "text" options of Tk and ttk widgets.
btn_close = tk.Button(root, text=t("Fermer"))
btn_close.pack()
lbl_name = ttk.Label(root, text=t("Nom :"))
lbl_name.pack()
cb_walls = tk.Checkbutton(root, text=t("Murs"))
cb_walls.pack()
# "Options" is already valid English: it must never be sent to the online engine.
frame_ = ttk.LabelFrame(root, text="Options")
frame_.pack()
# Multi-line text, and a text with a number (matches a "{n} ..." dictionary pattern).
lbl_multi = tk.Label(root, text="Première ligne\nDeuxième ligne")
lbl_multi.pack()
lbl_pattern = tk.Label(root, text="3 murs créés")
lbl_pattern.pack()
# Text absent from every dictionary: only the online/test engine can translate it.
lbl_unknown = tk.Label(root, text="Texte inconnu")
lbl_unknown.pack()
# Data shown in Labels (a path, an element code): never sent to the automatic engine.
lbl_path = tk.Label(root, text="C:\\Projets\\plan.pln")
lbl_path.pack()
lbl_code = tk.Label(root, text="MUR-01")
# Sentence where an Archicad term is mistranslated by online translators ("story" -> "story_");
# used to test the term corrections (corrections.json).
lbl_stories = tk.Label(root, text=t("Tous les étages"))
lbl_stories.pack()
lbl_code.pack()

# Menu bar: cascade label and command labels.
bar = tk.Menu(root, tearoff=0)
menu_file = tk.Menu(bar, tearoff=0)
menu_file.add_command(label="Ouvrir...")
menu_file.add_separator()
menu_file.add_command(label=t("Quitter"))
bar.add_cascade(label=t("Fichier"), menu=menu_file)
root.config(menu=bar)

# Notebook tabs.
notebook_ = ttk.Notebook(root)
p1 = ttk.Frame(notebook_)
p2 = ttk.Frame(notebook_)
notebook_.add(p1, text="Général")
notebook_.add(p2, text="Avancé")
notebook_.pack()

# Treeview: column headings are translated, row contents are data and are not.
tree = ttk.Treeview(root, columns=("valeur",))
tree.heading("#0", text="Nom")
tree.heading("valeur", text="Valeur")
line = tree.insert("", "end", text="Murs", values=("Dalles",))
tree.pack()

# Canvas text item.
canvas_ = tk.Canvas(root, width=120, height=40)
canvas_text = canvas_.create_text(-5, 20, text="Plan", anchor="w")
canvas_.pack()

# Combobox values and Entry contents are data: never translated.
combo = ttk.Combobox(root, values=["Murs", "Dalles"])
combo.set("Murs")
combo.pack()
entry = tk.Entry(root)
entry.insert(0, "Murs")
entry.pack()


# Returns the real Tcl interpreter. When the layer is active, root.tk is its proxy and the
# original interpreter is kept in its "_real" attribute; otherwise root.tk is the real one.
def real():
    return getattr(root.tk, "_real", root.tk)


# Builds the interface state as a list of (key, displayed text, text seen by the script):
# the displayed text is read from the real interpreter, the seen text through tkinter.
def state():
    r = real()
    e = []
    e.append(("titre", r.call("wm", "title", "."), root.title()))
    e.append(("statut", r.globalgetvar(str(status)), status.get()))
    for key, w in (("fermer", btn_close), ("nom", lbl_name), ("murs", cb_walls), ("cadre", frame_),
                   ("multi", lbl_multi), ("modele", lbl_pattern), ("inconnu", lbl_unknown),
                   ("chemin", lbl_path), ("code", lbl_code), ("etages", lbl_stories)):
        e.append((key, r.call(str(w), "cget", "-text"), w.cget("text")))
    e.append(("menu_fichier", r.call(str(bar), "entrycget", 0, "-label"), bar.entrycget(0, "label")))
    e.append(("menu_ouvrir", r.call(str(menu_file), "entrycget", 0, "-label"), menu_file.entrycget(0, "label")))
    e.append(("menu_quitter", r.call(str(menu_file), "entrycget", 2, "-label"), menu_file.entrycget(2, "label")))
    e.append(("onglet", r.call(str(notebook_), "tab", 0, "-text"), notebook_.tab(0, "text")))
    e.append(("entete", r.call(str(tree), "heading", "#0", "-text"), tree.heading("#0", "text")))
    e.append(("ligne_arbre", r.call(str(tree), "item", line, "-text"), tree.item(line, "text")))
    e.append(("canevas", r.call(str(canvas_), "itemcget", canvas_text, "-text"), canvas_.itemcget(canvas_text, "text")))
    # For the Combobox, "displayed" = its list of values joined by spaces, "seen" = current value.
    e.append(("combo", " ".join(str(v) for v in combo.cget("values")), combo.get()))
    e.append(("saisie", entry.get(), entry.get()))
    # The script logic compares original texts: it must keep working while translated
    # ("ok" when the comparisons still succeed, "BROKEN" otherwise).
    e.append(("logique", "ok" if btn_close.cget("text") == t("Fermer") and status.get() in (t("Prêt"), t("Terminé")) else "CASSEE", ""))
    return e


# Last state written to stdout and last command executed (to act on changes only).
last = None
last_command = ""


# Periodic tick (every 150 ms):
#   1. read command_.txt and execute the command if it changed ("caps" or "end_");
#   2. build the interface state;
#   3. if it differs from the last one, write it as V|... lines followed by STATUS_END;
#   4. schedule the next tick.
def tic():
    global last, last_command
    try:
        with open(os.path.join(EXCHANGE, "command.txt"), "r", encoding="utf-8") as f:
            command_ = f.read().strip()
    except OSError:
        command_ = ""
    if command_ != last_command:
        last_command = command_
        if command_ == "maj":
            status.set(t("Terminé"))
            lbl_name.config(text="Valeur")
        elif command_ == "fin":
            root.destroy()
            return
    e = state()
    if e != last:
        last = e
        lines = []
        for key, displayed, seen in e:
            lines.append("V|%s|%s|%s" % (key, str(displayed).replace("\n", "\\n"), str(seen).replace("\n", "\\n")))
        lines.append("FIN_ETAT")
        sys.stdout.write("\n".join(lines) + "\n")
        sys.stdout.flush()
    root.after(150, tic)


# Tell the bench whether the layer replaced root.tk with its proxy.
print("LAYER|%s" % ("active" if hasattr(root.tk, "_real") else "missing"), flush=True)
root.after(100, tic)
root.after(60000, root.destroy)   # safety: never run for more than one minute
root.mainloop()
print("FIN_OUTIL", flush=True)
