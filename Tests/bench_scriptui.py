# -*- coding: utf-8 -*-
"""Test bench of the Script UI pages (HTML palettes of Tapir), outside ArchiCAD.

It plays ArchiCAD + Tapir (JSON API: ShowScriptUI, GetScriptUIResult) and the local
translation engine (POST /translate, answers "[<language>] <text>"), prepares a data
folder like the add-on does, and runs the fake script bench_scriptui_tool.py with the
layer (layer/sitecustomize.py) on its PYTHONPATH, as Tapir does.

    python Tests/bench_scriptui.py            automatic checks (Python side), exit code 0 = OK
    python Tests/bench_scriptui.py --serve    same, then keeps the page open for a browser:
                                              http://127.0.0.1:<port>/page shows the page
                                              received by "ArchiCAD" with an ACAPI stand-in;
                                              runChecks() in that page runs the page checks.

Only the standard library is needed ("requests" is also tried if it is installed).
"""

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlsplit

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
TOOL = os.path.join(HERE, "bench_scriptui_tool.py")

TOOL_DICTIONARY = {
    "source_language": "en",
    "translations": {
        "fr": {"Relink Drawing": "Relier le dessin", "Relink": "Relier", "Pick the target view.": "Choisissez la vue cible.",
               "No view matches the filter.": "Aucune vue ne correspond au filtre.", "{n} views": "{n} vues",
               "All types": "Tous les types"},
        "de": {"Relink": "Neu verknüpfen"},
    },
}

# Script added to the page served to the browser: stands in for Tapir's window.ACAPI and
# checks the page as a user would see it. The DISPLAYED text is read with Range.toString
# and Attr.value (the page script does not change them); the page's code reads with
# textContent, title, value... and must get the originals.
BENCH_SCRIPT = r"""<script>
window.ACAPI = { SubmitResult: function (s) { return fetch("/submit", {method: "POST", body: s}); } };
window.runChecks = async function () {
  const $ = id => document.getElementById(id);
  const sleep = ms => new Promise(r => setTimeout(r, ms));
  const shown = el => { const r = document.createRange(); r.selectNodeContents(el); return r.toString().trim(); };
  const attr = (el, n) => el.attributes[n] ? el.attributes[n].value : null;
  const state = async () => (await fetch("/state")).json();
  const wait = async (test, ms) => { const end = Date.now() + ms; while (Date.now() < end) { if (test()) return true; await sleep(100); } return false; };
  const out = {};
  out.onlineArrived = await wait(() => shown($("list").children[0]) === "[fr] Ground Floor" && shown($("count")) === "2 vues", 8000);
  out.displayed = { h1: shown(document.querySelector("h1")), intro: shown($("intro")), btnFold: shown($("btnFold")),
    btnLink: shown($("btnLink")), btnClose: shown($("btnClose")), btnCancel: attr($("btnCancel"), "value"),
    option0: shown($("kind").options[0]), option1: shown($("kind").options[1]), count: shown($("count")),
    empty: shown($("empty")), list0: shown($("list").children[0]), list1: shown($("list").children[1]),
    placeholder: attr($("filter"), "placeholder"), tip: attr($("btnFold"), "title"),
    docTitle: shown(document.querySelector("title")) };
  out.readBack = { btnClose: $("btnClose").textContent, data: $("btnClose").firstChild.data, tip: $("btnFold").title,
    placeholder: $("filter").getAttribute("placeholder"), kind: $("kind").value, option: $("kind").options[0].text,
    docTitle: document.title, cancel: $("btnCancel").value, inner: $("btnLink").innerText, html: $("intro").innerHTML };
  $("btnFold").click(); await sleep(50);
  out.toggle1 = { shown: shown($("btnFold")), read: $("btnFold").textContent };
  $("btnFold").click(); await sleep(50);
  out.toggle2 = { shown: shown($("btnFold")), read: $("btnFold").textContent };
  $("btnLink").click();
  await wait(() => false, 1200);
  out.submitted = (await state()).results;
  await fetch("/lang?to=de"); await sleep(2500);
  out.german = { btnClose: shown($("btnClose")), btnLink: shown($("btnLink")), count: shown($("count")), read: $("btnClose").textContent };
  await fetch("/lang?to=original"); await sleep(2500);
  out.original = { btnClose: shown($("btnClose")), count: shown($("count")), intro: shown($("intro")) };
  await fetch("/lang?to=fr"); await sleep(2500);
  out.frenchAgain = { btnClose: shown($("btnClose")), intro: shown($("intro")) };
  out.translator = window.__tapirTranslator ? window.__tapirTranslator.state() : null;
  return out;
};
</script>"""


class Bench(object):
    def __init__(self):
        self.lock = threading.Lock()
        self.shown = None          # Parameters of the last ShowScriptUI
        self.raw_bodies = []       # Raw ShowScriptUI request bodies (to check Content-Length)
        self.results = []          # Results submitted by the page
        self.pending = None        # Result waiting for GetScriptUIResult
        self.open_ = True
        self.engine_texts = []     # Texts received by the fake engine
        self.data = tempfile.mkdtemp(prefix="tt_scriptui_")
        os.makedirs(os.path.join(self.data, "dictionaries"))
        shutil.copy(os.path.join(REPO, "dictionaries", "_common.json"), os.path.join(self.data, "dictionaries"))
        with open(os.path.join(self.data, "dictionaries", "bench_scriptui_tool.json"), "w", encoding="utf-8") as f:
            json.dump(TOOL_DICTIONARY, f, ensure_ascii=False)
        self.set_language("fr")

    def set_language(self, language):
        with open(os.path.join(self.data, "settings.json"), "w", encoding="utf-8") as f:
            json.dump({"output": language, "input": "en", "online": True, "provider": "bench", "collect": True}, f)

    def servers(self):
        bench = self

        class Engine(BaseHTTPRequestHandler):
            def log_message(self, *a):
                pass

            def do_POST(self):
                body = json.loads(self.rfile.read(int(self.headers["Content-Length"])).decode("utf-8"))
                with bench.lock:
                    bench.engine_texts.extend(body["texts"])
                answer = json.dumps({"translations": ["[%s] %s" % (body["target"], t) for t in body["texts"]]},
                                    ensure_ascii=False).encode("utf-8")
                self.send_response(200)
                self.send_header("Content-Type", "application/json; charset=utf-8")
                self.send_header("Content-Length", str(len(answer)))
                self.end_headers()
                self.wfile.write(answer)

        class Archicad(BaseHTTPRequestHandler):
            def log_message(self, *a):
                pass

            def reply(self, value, kind="application/json; charset=utf-8"):
                body = value if isinstance(value, bytes) else json.dumps(value, ensure_ascii=False).encode("utf-8")
                self.send_response(200)
                self.send_header("Content-Type", kind)
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def do_GET(self):
                parts = urlsplit(self.path)
                if parts.path == "/page":
                    page = bench.shown["htmlContent"] if bench.shown else "<p>no page yet</p>"
                    i = page.lower().rfind("</body>")
                    page = page[:i] + BENCH_SCRIPT + page[i:] if i >= 0 else page + BENCH_SCRIPT
                    return self.reply(page.encode("utf-8"), "text/html; charset=utf-8")
                if parts.path == "/lang":
                    bench.set_language(parse_qs(parts.query)["to"][0])
                    return self.reply({"ok": True})
                if parts.path == "/state":
                    with bench.lock:
                        return self.reply({"results": bench.results, "engine": bench.engine_texts})
                if parts.path == "/close":
                    bench.open_ = False
                    return self.reply({"ok": True})
                self.send_error(404)

            def do_POST(self):
                raw = self.rfile.read(int(self.headers["Content-Length"]))
                if self.path == "/submit":
                    with bench.lock:
                        bench.results.append(json.loads(raw.decode("utf-8")))
                        bench.pending = raw.decode("utf-8")
                    return self.reply({"ok": True})
                body = json.loads(raw.decode("utf-8"))     # Fails if Content-Length does not match the body
                name = body["parameters"]["addOnCommandId"]["commandName"]
                params = body["parameters"]["addOnCommandParameters"]
                if name == "ShowScriptUI":
                    with bench.lock:
                        bench.shown = params
                        bench.raw_bodies.append(raw)
                        bench.open_ = True
                    response = {"uiId": "1"}
                elif name == "GetScriptUIResult":
                    with bench.lock:
                        response = {"result": bench.pending, "isOpen": bench.open_}
                        bench.pending = None
                else:
                    response = {}
                self.reply({"succeeded": True, "result": {"addOnCommandResponse": response}})

        self.engine = ThreadingHTTPServer(("127.0.0.1", 0), Engine)
        self.archicad = ThreadingHTTPServer(("127.0.0.1", 0), Archicad)
        for s in (self.engine, self.archicad):
            threading.Thread(target=s.serve_forever, daemon=True).start()

    def run_tool(self, client):
        env = dict(os.environ)
        env.update({"PYTHONPATH": os.path.join(REPO, "layer"), "TAPIR_SCRIPT_ID": "bench-scriptui",
                    "TAPIR_TRANSLATOR_DIR": self.data,
                    "TAPIR_TRANSLATOR_ENGINE_PORT": str(self.engine.server_address[1]),
                    "PYTHONIOENCODING": "utf-8"})
        env.pop("TAPIR_TRANSLATOR_FORCE", None)
        with self.lock:
            self.shown, self.open_, self.pending = None, True, None
        return subprocess.Popen([sys.executable, TOOL, "--port", str(self.archicad.server_address[1]),
                                 "--client", client], env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                text=True, encoding="utf-8")


def config_of(page):
    """The configuration object given to the page script ({"map", "lang", "url", ...})."""
    start = page.index("var C = ") + len("var C = ")
    end = page.index(";\nvar map", start)
    return json.loads(page[start:end].replace("<\\/", "</").replace("<\\!--", "<!--"))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--serve", action="store_true")
    args = parser.parse_args()
    bench = Bench()
    bench.servers()
    checks = []

    def check(name, condition, detail=""):
        checks.append(bool(condition))
        print("%s %s%s" % ("OK  " if condition else "FAIL", name, (" : %s" % detail) if detail and not condition else ""))

    clients = ["urllib"]
    try:
        import requests  # noqa: F401
        clients.append("requests")
    except ImportError:
        print("(requests not installed: urllib only)")

    tool = None
    for client in clients:
        tool = bench.run_tool(client)
        end = time.time() + 20
        while bench.shown is None and time.time() < end and tool.poll() is None:
            time.sleep(0.05)
        check("[%s] ShowScriptUI received and readable (Content-Length follows the new body)" % client,
              bench.shown is not None, tool.stderr.read() if tool.poll() is not None else "timeout")
        if bench.shown is None:
            break
        page = bench.shown["htmlContent"]
        check("[%s] translation script at the head of the page" % client,
              "__tapirTranslator" in page and page.index("__tapirTranslator") < page.index("<body>"))
        config = config_of(page)
        m = config["map"]
        check("[%s] palette title translated" % client, bench.shown.get("title") == "Relier le dessin", bench.shown.get("title"))
        check("[%s] HTML texts translated in advance (common dictionary)" % client, m.get("Close") == "Fermer", m.get("Close"))
        check("[%s] HTML texts translated in advance (script dictionary)" % client,
              m.get("Pick the target view.") == "Choisissez la vue cible.", m.get("Pick the target view."))
        check("[%s] input button value translated in advance" % client, m.get("Cancel") == "Annuler", m.get("Cancel"))
        check("[%s] JavaScript strings translated in advance" % client,
              m.get("Expand all") == "Tout déplier" and m.get("No view matches the filter.") == "Aucune vue ne correspond au filtre.",
              {k: m.get(k) for k in ("Expand all", "No view matches the filter.")})
        check("[%s] language and server given to the page" % client,
              config["lang"] == "fr" and config["url"].startswith("http://127.0.0.1:") and len(config["key"]) == 32, config)
        if client != clients[-1] or not args.serve:
            with bench.lock:
                bench.open_ = False
            tool.wait(timeout=20)
            out = tool.stdout.read()
            check("[%s] the script ends normally" % client, tool.returncode == 0, out + tool.stderr.read())

    time.sleep(0.5)
    with bench.lock:
        sent = list(bench.engine_texts)
    check("displayed texts unknown to the dictionaries sent to the engine",
          "Filter views" in sent and "Collapse or expand all folders" in sent, sent)
    leaked = [t for t in sent if t in ("btnFold", "fold", "primary", "link", "close", "kind", "list", "count", "/submit")]
    check("JavaScript strings never sent to the engine", not leaked, leaked)
    missing_path = os.path.join(bench.data, "missing", "bench_scriptui_tool.json")
    missing = {}
    if os.path.isfile(missing_path):
        with open(missing_path, encoding="utf-8") as f:
            missing = json.load(f).get("translations", {}).get("fr", {})
    check("JavaScript strings never recorded as missing", "btnFold" not in missing and "primary" not in missing, list(missing))

    if args.serve and tool is not None and tool.poll() is None:
        url = "http://127.0.0.1:%d/page" % bench.archicad.server_address[1]
        print("PAGE %s" % url, flush=True)
        print("DATA %s" % bench.data, flush=True)
        end = time.time() + 900
        while tool.poll() is None and time.time() < end:
            time.sleep(0.2)
        with bench.lock:
            print("RESULTS %s" % json.dumps(bench.results, ensure_ascii=False))
    print("%d/%d checks OK" % (sum(checks), len(checks)))
    return 0 if all(checks) else 1


if __name__ == "__main__":
    sys.exit(main())
