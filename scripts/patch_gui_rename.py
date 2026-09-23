from pathlib import Path

p = Path("client/gui.py")
t = p.read_text(encoding="utf-8")

repls = [
    (
        '"""COMStorage desktop explorer — dual-pane file manager over USB CDC."""',
        '"""RNDISStorage desktop explorer — dual-pane file manager over HTTP/WebDAV."""',
    ),
    (
        "from comstorage import (\n"
        "    ComStorageClient,\n"
        "    ComStorageError,\n"
        "    filter_dongle_ports,\n"
        "    join_remote,\n"
        "    list_serial_ports,\n"
        "    parent_remote,\n"
        "    score_dongle_port,\n"
        ")",
        "from rndisstorage import (\n"
        "    DEFAULT_BASE_URL,\n"
        "    RndisStorageClient,\n"
        "    RndisStorageError,\n"
        "    discover_default_urls,\n"
        "    join_remote,\n"
        "    normalize_base_url,\n"
        "    parent_remote,\n"
        ")",
    ),
    (
        'CONFIG_PATH = Path.home() / ".comstorage_gui.json"',
        'CONFIG_PATH = Path.home() / ".rndisstorage_gui.json"',
    ),
    (
        'return {"dark_mode": True, "show_all_ports": False}',
        'return {"dark_mode": True, "base_url": DEFAULT_BASE_URL}',
    ),
    ("class ComStorageApp(tk.Tk):", "class RndisStorageApp(tk.Tk):"),
    ('self.title("COMStorage")', 'self.title("RNDISStorage")'),
    (
        'self.show_all_ports = tk.BooleanVar(value=bool(self.cfg.get("show_all_ports", False)))',
        'self.base_url_var = tk.StringVar(value=str(self.cfg.get("base_url", DEFAULT_BASE_URL)))',
    ),
    (
        "self.client: Optional[ComStorageClient] = None",
        "self.client: Optional[RndisStorageClient] = None",
    ),
    ("self._port_map: dict[str, str] = {}", "self._url_hints: list[str] = []"),
    ("self.refresh_ports()", "self.refresh_urls()"),
    ('text="COMStorage"', 'text="RNDISStorage"'),
    (
        'text="USB CDC file bridge  ·  lab PoC"',
        'text="WebDAV / HTTP file bridge  ·  lab PoC"',
    ),
    ("app = ComStorageApp()", "app = RndisStorageApp()"),
    ("ComStorageError", "RndisStorageError"),
    ("ComStorageClient", "RndisStorageClient"),
]

for a, b in repls:
    if a not in t:
        print("MISSING:", repr(a[:80]))
    else:
        t = t.replace(a, b)
        print("OK:", repr(a[:50]))

p.write_text(t, encoding="utf-8")
print("written", p.stat().st_size)
