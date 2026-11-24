# -*- coding: utf-8 -*-
"""Test bench of the tkinter extras, outside ArchiCAD: read-only Text panels, texts drawn
with Pillow, long and multi-line texts with the online translator.

    python Tests/bench_tk_extras.py      exit code 0 = OK (needs tkinter and Pillow)

A fake engine answers "[<language>] <text>"; the layer runs in bench_tk_extras_tool.py
as Tapir runs it (PYTHONPATH, TAPIR_SCRIPT_ID, TAPIR_TRANSLATOR_DIR...).
"""

import json
import os
import shutil
import subprocess
import sys
import tempfile
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
received = []


class Engine(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers["Content-Length"])).decode("utf-8"))
        received.extend(body["texts"])
        answer = json.dumps({"translations": ["[%s] %s" % (body["target"], t.strip()) for t in body["texts"]]},
                            ensure_ascii=False).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(answer)))
        self.end_headers()
        self.wfile.write(answer)


NAMES = {"root": {"name": "Etudes de faisabilite", "children": [{"name": "Plan du rez-de-chaussee"},
                                                                {"name": "Coupe longitudinale"}]},
         "error": {"message": "Element introuvable dans le projet"}}


class Archicad(BaseHTTPRequestHandler):
    """Fake ArchiCAD JSON API: answers a navigator tree (project names) and an error message."""

    def log_message(self, *a):
        pass

    def do_POST(self):
        self.rfile.read(int(self.headers["Content-Length"]))
        answer = json.dumps({"succeeded": True, "result": {"addOnCommandResponse": NAMES}}).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(answer)))
        self.end_headers()
        self.wfile.write(answer)


def main():
    data = tempfile.mkdtemp(prefix="tt_tkextras_")
    os.makedirs(os.path.join(data, "dictionaries"))
    shutil.copy(os.path.join(REPO, "dictionaries", "_common.json"), os.path.join(data, "dictionaries"))
    with open(os.path.join(data, "settings.json"), "w", encoding="utf-8") as f:
        json.dump({"output": "en", "input": "fr", "online": True, "provider": "bench", "collect": False}, f)
    engine = ThreadingHTTPServer(("127.0.0.1", 0), Engine)
    archicad = ThreadingHTTPServer(("127.0.0.1", 0), Archicad)
    for s in (engine, archicad):
        threading.Thread(target=s.serve_forever, daemon=True).start()
    env = dict(os.environ, PYTHONPATH=os.path.join(REPO, "layer"), TAPIR_SCRIPT_ID="bench-tk-extras",
               TAPIR_TRANSLATOR_DIR=data, TAPIR_TRANSLATOR_ENGINE_PORT=str(engine.server_address[1]),
               BENCH_ARCHICAD_PORT=str(archicad.server_address[1]), PYTHONIOENCODING="utf-8")
    p = subprocess.run([sys.executable, os.path.join(HERE, "bench_tk_extras_tool.py")], env=env,
                       capture_output=True, text=True, encoding="utf-8", timeout=60)
    line = next((x for x in p.stdout.splitlines() if x.startswith("RESULTS ")), None)
    if line is None:
        print("FAIL the tool printed no result\n" + p.stdout + p.stderr)
        return 1
    r = json.loads(line[len("RESULTS "):])
    checks = []

    def check(name, condition, detail=""):
        checks.append(bool(condition))
        print("%s %s%s" % ("OK  " if condition else "FAIL", name, (" : %r" % (detail,)) if not condition else ""))

    shown = r["text_shown"].split("\n")
    check("Text panel: lines translated", shown[0] == "[en] Analyse de la selection"
          and shown[1] == "  [en] vues placees dans aucune mise en page", shown)
    # Since 0.10.1 the final ":" is put back with the spacing of the output language
    # (English: no space before it).
    check("Text panel: a line mixing tags is translated piece by piece",
          shown[2] == "[en] Nom du dossier: [en] valeur du champ", shown[2])
    check("Text panel: data without letters untouched", shown[3] == "1:500 / 25", shown[3])
    check("Text panel: tags kept", "titre" in r["tags_line1"] and "titre" in r["tags_line3"]
          and "titre" not in r["tags_line3_value"], (r["tags_line1"], r["tags_line3"], r["tags_line3_value"]))
    check("Text panel: still read-only", r["state"] == "disabled", r["state"])
    check("Text panel: get gives back the originals", r["text_read"] == "Analyse de la selection\n"
          "  vues placees dans aucune mise en page\nNom du dossier : valeur du champ\n1:500 / 25\n", r["text_read"])
    check("Text panel: refreshed by the script, translated at once", r["refill_shown"] == r["text_shown"], r["refill_shown"])
    check("Text panel: live language change", r["text_shown_de"].startswith("[de] Analyse de la selection"),
          r["text_shown_de"])
    check("Text panel: get after the change gives back the originals", r["text_read_de"] == r["text_read"], r["text_read_de"])
    check("Pillow: first drawing in the original while the translation is on its way",
          r["pil_first"] == r["ref"][0], r["pil_first"])
    check("Pillow: drawn text translated (textbbox of the translation)", r["pil_later"] == r["ref"][1], r["pil_later"])
    check("Long text: translated sentence by sentence", r["long_shown"].startswith("[en] Phrase numero 0")
          and r["long_shown"].count("[en] ") == 12, r["long_shown"][:200])
    check("Long text: the script reads its original", r["long_read_ok"])
    check("Multi-line text: translated line by line",
          r["multi_shown"] == "[en] Premiere ligne du message\n[en] Deuxieme ligne du message", r["multi_shown"])
    fr_, en_ = r["capsule_en"]
    check("Drawn button: redrawn in place when the online translation arrives", en_ < 1.0 and fr_ > 5 * max(en_, 0.2),
          r["capsule_en"])
    en_, de_ = r["capsule_de"]
    check("Drawn button: redrawn in place after a language change", de_ < 1.0 and en_ > 5 * max(de_, 0.2),
          r["capsule_de"])
    names = ["Etudes de faisabilite", "Plan du rez-de-chaussee", "Coupe longitudinale"]
    check("Project names in a list never translated", r["tree_shown"] == names, r["tree_shown"])
    check("Tool words of a list still translated (common dictionary)", r["tree_types"] == ["Folder"] * 3, r["tree_types"])
    check("Sentence with a project name: only the pieces around it translated",
          r["sentence_shown"] == "[en] Ouvert dans ArchiCAD: Plan du rez-de-chaussee [en] (Dossier)", r["sentence_shown"])
    check("Error message from ArchiCAD translated", r["error_shown"] == "[en] Element introuvable dans le projet",
          r["error_shown"])
    check("Project names never sent online", not any(n in t for t in received for n in names),
          [t for t in received if any(n in t for n in names)])
    check("Nothing longer than 600 characters sent", all(len(t) <= 600 for t in received),
          max(len(t) for t in received) if received else 0)
    check("Data without letters never sent", "1:500 / 25" not in received)
    print("%d/%d checks OK" % (sum(checks), len(checks)))
    return 0 if all(checks) else 1


if __name__ == "__main__":
    sys.exit(main())
