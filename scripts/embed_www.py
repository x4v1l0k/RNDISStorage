from pathlib import Path

html = Path("firmware/data/www/index.html").read_text(encoding="utf-8")
out = Path("firmware/include/www_index.h")
delim = "INDEXHTML"
assert delim not in html
content = (
    "#pragma once\n"
    "// Auto-embedded file explorer UI\n"
    f'static const char WWW_INDEX_HTML[] = R"{delim}(\n{html}\n){delim}";\n'
)
out.write_text(content, encoding="utf-8")
print("wrote", out, "bytes", out.stat().st_size)
