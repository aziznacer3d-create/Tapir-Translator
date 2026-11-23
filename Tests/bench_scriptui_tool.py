# -*- coding: utf-8 -*-
"""Fake Tapir script with a Script UI page (HTML palette), for bench_scriptui.py.

It sends its page with ShowScriptUI exactly like the Tapir scripts do (urllib, JSON body
"API.ExecuteAddOnCommand"), then waits for the user's choice with GetScriptUIResult,
like marketplace/relink-drawing-keep-placement. Most of the page's texts are written by
its JavaScript after loading, and the page compares a displayed text with its original
("Collapse all") and submits displayed texts: the translation must not change them.

Usage: python bench_scriptui_tool.py --port <ArchiCAD port> [--client urllib|requests]
Each result received is printed as one JSON line "RESULT {...}".
"""

import argparse
import json
import sys
import time
import urllib.request

PAGE = r"""<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<title>Relink Drawing</title>
<style>body { font: 13px sans-serif; } .fold { color: #555; }</style>
</head>
<body>
<h1>Relink Drawing</h1>
<p id="intro">Pick the target view.</p>
<input id="filter" placeholder="Filter views">
<button id="btnFold" title="Collapse or expand all folders">Collapse all</button>
<button id="btnLink" class="primary">Relink</button>
<button id="btnClose">Close</button>
<input type="button" id="btnCancel" value="Cancel">
<select id="kind"><option>All types</option><option>Walls</option></select>
<div id="list"></div>
<div id="count"></div>
<div id="empty"></div>
<script>
// Collapse all: comment with an apostrophe that's not a string
const $ = id => document.getElementById(id);
const VIEWS = [{"name": "Ground Floor"}, {"name": "Section"}];
function render() {
  const box = $("list"); box.innerHTML = "";
  VIEWS.forEach(v => { const d = document.createElement("div"); d.className = "fold"; d.textContent = v.name; box.appendChild(d); });
  $("count").textContent = `${VIEWS.length} views`;
  $("empty").innerHTML = '<div class="fold">No view matches the filter.</div>';
}
$("btnFold").onclick = () => {
  $("btnFold").textContent = $("btnFold").textContent === "Collapse all" ? "Expand all" : "Collapse all";
};
$("btnLink").onclick = () => window.ACAPI.SubmitResult(JSON.stringify({
  action: "link", label: $("btnLink").textContent, inner: $("btnLink").innerText,
  kind: $("kind").value, option: $("kind").options[0].text, tip: $("btnFold").title,
  placeholder: $("filter").getAttribute("placeholder"), cancel: $("btnCancel").value,
  pageTitle: document.title, intro: $("intro").innerHTML }));
$("btnClose").onclick = () => window.ACAPI.SubmitResult(JSON.stringify({ action: "close" }));
render();
</script>
</body>
</html>
"""


def command(port, name, parameters, client):
    """Run a Tapir command through ArchiCAD's JSON API (like aclib.RunTapirCommand)."""
    body = {"command": "API.ExecuteAddOnCommand",
            "parameters": {"addOnCommandId": {"commandNamespace": "TapirCommand", "commandName": name},
                           "addOnCommandParameters": parameters}}
    url = "http://127.0.0.1:%d" % port
    if client == "requests":
        import requests
        answer = requests.post(url, data=json.dumps(body).encode("utf-8"),
                               headers={"Content-Type": "application/json"}, timeout=30).json()
    else:
        request = urllib.request.Request(url)
        request.add_header("Content-Type", "application/json")
        with urllib.request.urlopen(request, json.dumps(body).encode("utf-8"), timeout=30) as response:
            answer = json.loads(response.read())
    return answer["result"]["addOnCommandResponse"]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--client", default="urllib")
    args = parser.parse_args()
    shown = command(args.port, "ShowScriptUI",
                    {"htmlContent": PAGE, "title": "Relink Drawing", "width": 420, "height": 400}, args.client)
    print("SHOWN %s" % json.dumps(shown), flush=True)
    deadline = time.time() + 600
    while time.time() < deadline:
        r = command(args.port, "GetScriptUIResult", {"uiId": shown.get("uiId")}, args.client)
        if r.get("result") is not None:
            print("RESULT %s" % r["result"], flush=True)
            if json.loads(r["result"]).get("action") == "close":
                break
        if not r.get("isOpen", True):
            break
        time.sleep(0.3)
    return 0


if __name__ == "__main__":
    sys.exit(main())
