# /// script
# requires-python = ">=3.10"
# dependencies = []
# ///
"""
Fake Tapir script used by the Tapir Translator test bench (Tests/TranslatorBench.cpp).

What this file is (and is not)
------------------------------
This file is NOT the translation layer: it is the tool to be translated, written like an
ordinary Tapir script (a tkinter interface in English, with no knowledge at all of the
translation). The inline "# /// script" block above is the PEP 723 metadata that uv reads.
The bench starts it with "uv run --script bench_tool.py --port N", exactly like the Tapir
palette does, so the translation layer (sitecustomize.py, placed on the PYTHONPATH by the
add-on code) is imported automatically by Python before this script runs.

What it reports
---------------
Every 150 ms it writes the state of its interface to stdout, but only when it changed:
  V|<key>|<text displayed by Tk>|<text seen by the script>
  STATE_END
The displayed text is read directly in the real Tcl interpreter (bypassing the layer):
it is what the user sees on screen. The seen text goes through the normal tkinter API
(and therefore through the layer): it is what the script logic receives, and it must
always stay in the original language. Newlines are written as a literal backslash-n.
Other lines it prints:
  LAYER|active or LAYER|missing  -> whether the layer is installed in front of Tcl
  TOOL_END                        -> the Tk main loop has ended

Commands (file <BENCH_EXCHANGE>\\command.txt, written by the bench, polled every tick):
  update -> the script itself changes some texts (status, label) while being translated
  end    -> close the window and exit
"""
import os
import sys
import tkinter as tk
from tkinter import ttk

# Exchange folder shared with the bench (holds command_.txt).
EXCHANGE = os.environ.get("BENCH_EXCHANGE", ".")
# BENCH_UI=fr: the same interface written in French instead of English, to emulate a script
# written in another language whose source language is not declared (the layer must
# recognise it). Every interface text listed here changes; the data (path, code, Treeview
# row, Combobox values, Entry contents) stays as it is.
T = ({"Demo tool": "Outil de démonstration", "Ready": "Prêt", "Done": "Terminé", "Close": "Fermer",
      "Name:": "Nom :", "Walls": "Murs", "First line\nSecond line": "Première ligne\nDeuxième ligne",
      "3 walls created": "3 murs créés", "Unknown text": "Texte inconnu", "All stories": "Tous les étages",
      "Open...": "Ouvrir...", "Quit": "Quitter", "File": "Fichier", "General": "Général",
      "Advanced": "Avancé", "Name": "Nom", "Value": "Valeur", "Floor plan": "Plan"}
     if os.environ.get("BENCH_UI") == "fr" else {})


# Returns the text in the interface language chosen by BENCH_UI (English by default).
def t(text):
    return T.get(text, text)


# --- Interface: one widget of each kind the layer is supposed to translate (or not) ---
root = tk.Tk()
root.title(t("Demo tool"))

# Status shown through a StringVar (the layer translates the displayed variable only).
status = tk.StringVar(value=t("Ready"))
tk.Label(root, textvariable=status).pack()

# Plain "text" options of Tk and ttk widgets.
btn_close = tk.Button(root, text=t("Close"))
btn_close.pack()
lbl_name = ttk.Label(root, text=t("Name:"))
lbl_name.pack()
cb_walls = tk.Checkbutton(root, text=t("Walls"))
cb_walls.pack()
# "Options" is also valid French: towards French it must never be sent to the online engine.
frame_ = ttk.LabelFrame(root, text="Options")
frame_.pack()
# Multi-line text, and a text with a number (matches a "{n} ..." dictionary pattern).
lbl_multi = tk.Label(root, text=t("First line\nSecond line"))
lbl_multi.pack()
lbl_pattern = tk.Label(root, text=t("3 walls created"))
lbl_pattern.pack()
# Text absent from every dictionary: only the online/test engine can translate it.
lbl_unknown = tk.Label(root, text=t("Unknown text"))
lbl_unknown.pack()
# Data shown in Labels (a path, an element code): never sent to the automatic engine.
lbl_path = tk.Label(root, text="C:\\Projects\\plan.pln")
lbl_path.pack()
lbl_code = tk.Label(root, text="WALL-01")
# Sentence with an Archicad term that online translators mistranslate into French
# ("story" -> "histoire" instead of "étage"); used to test the term corrections
# (corrections.json).
lbl_stories = tk.Label(root, text=t("All stories"))
lbl_stories.pack()
lbl_code.pack()

# Menu bar: cascade label and command labels.
bar = tk.Menu(root, tearoff=0)
menu_file = tk.Menu(bar, tearoff=0)
menu_file.add_command(label=t("Open..."))
menu_file.add_separator()
menu_file.add_command(label=t("Quit"))
bar.add_cascade(label=t("File"), menu=menu_file)
root.config(menu=bar)

# Notebook tabs.
notebook_ = ttk.Notebook(root)
p1 = ttk.Frame(notebook_)
p2 = ttk.Frame(notebook_)
notebook_.add(p1, text=t("General"))
notebook_.add(p2, text=t("Advanced"))
notebook_.pack()

# Treeview: column headings are translated, row contents are data and are not.
tree = ttk.Treeview(root, columns=("value",))
tree.heading("#0", text=t("Name"))
tree.heading("value", text=t("Value"))
line = tree.insert("", "end", text="Walls", values=("Slabs",))
tree.pack()

# Canvas text item.
canvas_ = tk.Canvas(root, width=120, height=40)
canvas_text = canvas_.create_text(-5, 20, text=t("Floor plan"), anchor="w")
canvas_.pack()

# Combobox (values translated since 0.10.0: the script reads back its originals, through get()
# and through its variable), Listbox items (translated), and an Entry (user input: never).
combo_var = tk.StringVar()
combo = ttk.Combobox(root, values=["Walls", "Slabs"], textvariable=combo_var)
combo.set("Walls")
combo.pack()
listbox = tk.Listbox(root, height=2)
listbox.insert("end", "Walls", "Slabs")
listbox.pack()
entry = tk.Entry(root)
entry.insert(0, "Walls")
entry.pack()
# A label with a "/" between words: a separator, not a path (it may be sent online).
lbl_slash = tk.Label(root, text="Check / Element ID")
lbl_slash.pack()
# Texts decorated with a leading symbol: only the words are translated, the symbol stays.
lbl_marked = tk.Label(root, text="✔  Close")
lbl_marked.pack()
lbl_marked_unknown = tk.Label(root, text="⚠  Unknown text")
lbl_marked_unknown.pack()


# Returns the real Tcl interpreter. When the layer is active, root.tk is its proxy and the
# original interpreter is kept in its "_real" attribute; otherwise root.tk is the real one.
def real():
    return getattr(root.tk, "_real", root.tk)


# Builds the interface state as a list of (key, displayed text, text seen by the script):
# the displayed text is read from the real interpreter, the seen text through tkinter.
def state():
    r = real()
    e = []
    e.append(("title", r.call("wm", "title", "."), root.title()))
    e.append(("status", r.globalgetvar(str(status)), status.get()))
    for key, w in (("close", btn_close), ("name", lbl_name), ("walls", cb_walls), ("frame", frame_),
                   ("multi", lbl_multi), ("pattern", lbl_pattern), ("unknown", lbl_unknown),
                   ("path", lbl_path), ("code", lbl_code), ("stories", lbl_stories)):
        e.append((key, r.call(str(w), "cget", "-text"), w.cget("text")))
    e.append(("menu_file", r.call(str(bar), "entrycget", 0, "-label"), bar.entrycget(0, "label")))
    e.append(("menu_open", r.call(str(menu_file), "entrycget", 0, "-label"), menu_file.entrycget(0, "label")))
    e.append(("menu_quit", r.call(str(menu_file), "entrycget", 2, "-label"), menu_file.entrycget(2, "label")))
    e.append(("tab", r.call(str(notebook_), "tab", 0, "-text"), notebook_.tab(0, "text")))
    e.append(("heading", r.call(str(tree), "heading", "#0", "-text"), tree.heading("#0", "text")))
    e.append(("tree_row", r.call(str(tree), "item", line, "-text"), tree.item(line, "text")))
    e.append(("tree_value", r.call(str(tree), "set", line, "value"), tree.set(line, "value")))
    e.append(("tree_values", " ".join(str(v) for v in r.splitlist(r.call(str(tree), "item", line, "-values"))),
              " ".join(tree.item(line, "values"))))
    e.append(("canvas", r.call(str(canvas_), "itemcget", canvas_text, "-text"), canvas_.itemcget(canvas_text, "text")))
    # Combobox: "displayed" = its list of values joined by spaces (read in Tcl), "seen" = the
    # value read by the script; its variable too.
    e.append(("combo", " ".join(str(v) for v in r.splitlist(r.call(str(combo), "cget", "-values"))), combo.get()))
    e.append(("combo_var", r.globalgetvar(str(combo_var)), combo_var.get()))
    e.append(("listbox", " ".join(str(v) for v in r.splitlist(r.call(str(listbox), "get", 0, "end"))),
              " ".join(listbox.get(0, "end"))))
    e.append(("entry", entry.get(), entry.get()))
    e.append(("slash", r.call(str(lbl_slash), "cget", "-text"), lbl_slash.cget("text")))
    e.append(("marked", r.call(str(lbl_marked), "cget", "-text"), lbl_marked.cget("text")))
    e.append(("marked_unknown", r.call(str(lbl_marked_unknown), "cget", "-text"), lbl_marked_unknown.cget("text")))
    # The script logic compares original texts: it must keep working while translated
    # ("ok" when the comparisons still succeed, "BROKEN" otherwise).
    e.append(("logic", "ok" if btn_close.cget("text") == t("Close") and status.get() in (t("Ready"), t("Done")) else "BROKEN", ""))
    return e


# Last state written to stdout and last command executed (to act on changes only).
last = None
last_command = ""


# Periodic tick (every 150 ms):
#   1. read command.txt and execute the command if it changed ("update" or "end");
#   2. build the interface state;
#   3. if it differs from the last one, write it as V|... lines followed by STATE_END;
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
        if command_ == "update":
            status.set(t("Done"))
            lbl_name.config(text=t("Value"))
        elif command_ == "end":
            root.destroy()
            return
    e = state()
    if e != last:
        last = e
        lines = []
        for key, displayed, seen in e:
            lines.append("V|%s|%s|%s" % (key, str(displayed).replace("\n", "\\n"), str(seen).replace("\n", "\\n")))
        lines.append("STATE_END")
        sys.stdout.write("\n".join(lines) + "\n")
        sys.stdout.flush()
    root.after(150, tic)


# Tell the bench whether the layer replaced root.tk with its proxy.
print("LAYER|%s" % ("active" if hasattr(root.tk, "_real") else "missing"), flush=True)
root.after(100, tic)
root.after(60000, root.destroy)   # safety: never run for more than one minute
root.mainloop()
print("TOOL_END", flush=True)
