"""Turn user/web/prelude.js into user/web/js_prelude.c (a C string the
browser runs before each page's scripts). Run after changing prelude.js:
    python tools/genprelude.py
"""
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
src = os.path.join(ROOT, "user", "web", "prelude.js")
out = os.path.join(ROOT, "user", "web", "js_prelude.c")

lines = open(src, encoding="utf-8").read().split("\n")
body = []
for line in lines:
    if line.strip().startswith("//"):
        continue  # (comments stay in prelude.js)
    esc = line.replace("\\", "\\\\").replace('"', '\\"')
    body.append('    "' + esc + '\\n"')
text = ("/* web/js_prelude.c - GENERATED from web/prelude.js by tools/genprelude.py:\n"
        "   do not edit; change prelude.js and run the script. Part of the browser. */\n\n"
        "static const char JS_PRELUDE[] =\n" + "\n".join(body) + ";\n")
open(out, "w", encoding="utf-8", newline="\n").write(text)
print(f"wrote {out}: {len(body)} lines")
