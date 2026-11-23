# -*- coding: utf-8 -*-
"""Fake tkinter Tapir script for bench_tk_extras.py (French texts, translated to English online).

It checks from the inside what the user sees and what the script reads back:
- a read-only Text panel (filled, then "-state disabled"), lines with and without tags;
- texts drawn into an image with Pillow (a "capsule" button), measured with textbbox;
- a label longer than what the online translators accept, and a multi-line label;
- a live language change (settings.json written as the add-on menu does).
Prints one line "RESULTS {json}" and quits.
"""

import json
import os
import tkinter as tk
import urllib.request
from tkinter import ttk

from PIL import Image, ImageChops, ImageDraw, ImageFont, ImageStat, ImageTk

FONT = ImageFont.load_default()
DRAW = ImageDraw.Draw(Image.new("RGB", (900, 60)))
REF_ORIGINAL = DRAW.textbbox((0, 0), "Supprimer la vue", font=FONT)      # no window yet: never translated
REF_EN = DRAW.textbbox((0, 0), "[en] Supprimer la vue", font=FONT)


def capsule(text, w=200, h=30, ss=4):
    """A button drawn like the capsules of real scripts: big, centred text, then reduced."""
    img = Image.new("RGBA", (w * ss, h * ss), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.rounded_rectangle((ss, ss, w * ss - ss - 1, h * ss - ss - 1), radius=h * ss // 2, fill="#333333")
    font = ImageFont.load_default(size=12 * ss)
    box = d.textbbox((0, 0), text, font=font)
    d.text(((w * ss - (box[2] - box[0])) / 2.0 - box[0], (h * ss - (box[3] - box[1])) / 2.0 - box[1]),
           text, font=font, fill="#ffffff")
    return img.resize((w, h), Image.LANCZOS)


# References drawn before any window exists (never translated)
CAPSULES = dict((k, capsule(t)) for k, t in (("fr", "Supprimer la vue"), ("en", "[en] Supprimer la vue"),
                                              ("de", "[de] Supprimer la vue")))


def on_white(image):
    """The image as seen on a white background (the colour of fully transparent pixels does not count)."""
    background = Image.new("RGBA", image.size, (255, 255, 255, 255))
    background.alpha_composite(image.convert("RGBA"))
    return background.convert("RGB")


def difference(a, b):
    return sum(ImageStat.Stat(ImageChops.difference(on_white(a), on_white(b))).mean)


# Project names: the script reads them from "ArchiCAD" (fake server of the bench)
request = urllib.request.Request("http://127.0.0.1:%s" % os.environ["BENCH_ARCHICAD_PORT"])
request.add_header("Content-Type", "application/json")
body = json.dumps({"command": "API.ExecuteAddOnCommand", "parameters": {"addOnCommandId": {
    "commandNamespace": "TapirCommand", "commandName": "GetNavigatorItemTree"}, "addOnCommandParameters": {}}})
with urllib.request.urlopen(request, body.encode("utf-8")) as response:
    TREE = json.loads(response.read())["result"]["addOnCommandResponse"]

LONG = " ".join("Phrase numero %d de la legende, assez longue pour depasser la limite." % i for i in range(12))
MULTI = "Premiere ligne du message\nDeuxieme ligne du message"
results = {"ref": [list(REF_ORIGINAL), list(REF_EN)]}

root = tk.Tk()
real = root.tk._real if hasattr(root.tk, "_real") else root.tk
txt = tk.Text(root, height=8, width=80)
txt.pack()
txt.tag_configure("titre", font=("Segoe UI", 10, "bold"))
long_label = tk.Label(root, text=LONG, wraplength=700)
long_label.pack()
multi_label = tk.Label(root, text=MULTI)
multi_label.pack()
# The references go through Tk too (Tk keeps semi-transparent pixels its own way)
CAPSULES = dict((k, ImageTk.getimage(ImageTk.PhotoImage(v))) for k, v in CAPSULES.items())
canvas = tk.Canvas(root, width=200, height=30)
canvas.pack()
photo = ImageTk.PhotoImage(capsule("Supprimer la vue"))     # online translation pending: drawn in French
canvas.create_image(0, 0, image=photo, anchor="nw")
tree = ttk.Treeview(root, columns=("type",))
tree.pack()
names = [TREE["root"]["name"]] + [c["name"] for c in TREE["root"]["children"]]
for n in names:
    tree.insert("", "end", text=n, values=("Dossier",))
sentence = tk.Label(root, text="Ouvert dans ArchiCAD : %s (Dossier)" % names[1])
sentence.pack()
error_label = tk.Label(root, text=TREE["error"]["message"])
error_label.pack()


def fill():
    txt.configure(state="normal")
    txt.delete("1.0", "end")
    txt.insert("end", "Analyse de la selection\n", "titre")
    txt.insert("end", "  vues placees dans aucune mise en page\n")
    txt.insert("end", "Nom du dossier : ", "titre")
    txt.insert("end", "valeur du champ\n")
    txt.insert("end", "1:500 / 25\n")
    txt.configure(state="disabled")


fill()
results["pil_first"] = list(DRAW.textbbox((0, 0), "Supprimer la vue", font=FONT))   # online translation pending


def shown_text():
    return str(real.call(str(txt), "get", "1.0", "end-1c"))


def step1():
    results["text_shown"] = shown_text()
    results["text_read"] = txt.get("1.0", "end-1c")
    results["tags_line1"] = [str(t) for t in real.splitlist(real.call(str(txt), "tag", "names", "1.3"))]
    results["tags_line3"] = [str(t) for t in real.splitlist(real.call(str(txt), "tag", "names", "3.3"))]
    results["tags_line3_value"] = [str(t) for t in real.splitlist(real.call(str(txt), "tag", "names", "3.end-2c"))]
    results["state"] = str(txt.cget("state"))
    results["pil_later"] = list(DRAW.textbbox((0, 0), "Supprimer la vue", font=FONT))
    results["long_shown"] = str(real.call(str(long_label), "cget", "-text"))
    results["long_read_ok"] = long_label.cget("text") == LONG
    results["multi_shown"] = str(real.call(str(multi_label), "cget", "-text"))
    shown_photo = ImageTk.getimage(photo)
    if os.environ.get("BENCH_SAVE"):     # Debugging: the drawn button and its references as PNG
        shown_photo.save(os.path.join(os.environ["BENCH_SAVE"], "capsule_shown_en.png"))
        for k, v in CAPSULES.items():
            v.save(os.path.join(os.environ["BENCH_SAVE"], "capsule_ref_%s.png" % k))
    results["capsule_en"] = [difference(shown_photo, CAPSULES[k]) for k in ("fr", "en")]
    results["tree_shown"] = [str(real.call(str(tree), "item", i, "-text")) for i in tree.get_children()]
    results["tree_types"] = [str(real.splitlist(real.call(str(tree), "item", i, "-values"))[0]) for i in tree.get_children()]
    results["sentence_shown"] = str(real.call(str(sentence), "cget", "-text"))
    results["error_shown"] = str(real.call(str(error_label), "cget", "-text"))
    fill()   # the script refreshes its panel: translated at once from the cache
    results["refill_shown"] = shown_text()
    settings = os.path.join(os.environ["TAPIR_TRANSLATOR_DIR"], "settings.json")
    with open(settings, encoding="utf-8") as f:
        data = json.load(f)
    data["output"] = "de"
    with open(settings, "w", encoding="utf-8") as f:
        json.dump(data, f)
    root.after(2500, step2)


def step2():
    results["text_shown_de"] = shown_text()
    results["text_read_de"] = txt.get("1.0", "end-1c")
    results["capsule_de"] = [difference(ImageTk.getimage(photo), CAPSULES[k]) for k in ("en", "de")]
    print("RESULTS " + json.dumps(results, ensure_ascii=False), flush=True)
    root.destroy()


root.after(3000, step1)
root.mainloop()
