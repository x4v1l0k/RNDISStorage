#!/usr/bin/env python3
"""RNDISStorage desktop explorer — dual-pane file manager over HTTP/WebDAV."""

from __future__ import annotations

import json
import os
import shutil
import stat
import subprocess
import sys
import tempfile
import threading
import tkinter as tk
from datetime import datetime
from pathlib import Path
from tkinter import filedialog, messagebox, simpledialog, ttk
from typing import Any, Callable, Optional

from rndisstorage import (
    DEFAULT_BASE_URL,
    RndisStorageClient,
    RndisStorageError,
    discover_default_urls,
    join_remote,
    normalize_base_url,
    parent_remote,
)

CONFIG_PATH = Path.home() / ".rndisstorage_gui.json"
MAX_EDIT_BYTES = 512 * 1024

# Visual system — technical lab aesthetic (no purple / no neon glow)
THEMES = {
    "dark": {
        "app_bg": "#0e1116",
        "header_bg": "#141a22",
        "header_line": "#243041",
        "surface": "#171d27",
        "surface_2": "#1c2430",
        "elevated": "#222b38",
        "border": "#2c3645",
        "border_soft": "#243041",
        "text": "#e8eef5",
        "text_muted": "#8b97a8",
        "text_dim": "#667384",
        "accent": "#3aa89a",
        "accent_dim": "#2a7d73",
        "accent_text": "#0b1210",
        "select_bg": "#2a5f58",
        "select_fg": "#f2fffc",
        "danger": "#d97868",
        "ok": "#6fbf8a",
        "warn": "#d4a84b",
        "tree_bg": "#121820",
        "tree_fg": "#d7dee8",
        "tree_head_bg": "#1a222e",
        "tree_head_fg": "#9aa8ba",
        "log_bg": "#0c1016",
        "log_fg": "#a8b4c4",
        "input_bg": "#1a222e",
        "input_fg": "#e8eef5",
        "btn_bg": "#243041",
        "btn_fg": "#e8eef5",
        "btn_active": "#2e3c4f",
        "pane_title": "#3aa89a",
        "sash": "#2c3645",
    },
    "light": {
        "app_bg": "#e6ebf0",
        "header_bg": "#f4f7fa",
        "header_line": "#cfd8e3",
        "surface": "#ffffff",
        "surface_2": "#f7f9fb",
        "elevated": "#ffffff",
        "border": "#c5d0dc",
        "border_soft": "#d7e0ea",
        "text": "#1a2330",
        "text_muted": "#5a6a7c",
        "text_dim": "#7a8796",
        "accent": "#1f7a6e",
        "accent_dim": "#186258",
        "accent_text": "#ffffff",
        "select_bg": "#d5ebe6",
        "select_fg": "#123d37",
        "danger": "#c45c4a",
        "ok": "#2f8f5b",
        "warn": "#b8860b",
        "tree_bg": "#ffffff",
        "tree_fg": "#1a2330",
        "tree_head_bg": "#eef2f6",
        "tree_head_fg": "#4a5a6c",
        "log_bg": "#f4f7fa",
        "log_fg": "#3a4a5c",
        "input_bg": "#ffffff",
        "input_fg": "#1a2330",
        "btn_bg": "#e8eef4",
        "btn_fg": "#1a2330",
        "btn_active": "#d5dee8",
        "pane_title": "#1f7a6e",
        "sash": "#b8c4d0",
    },
}


def fmt_size(n: int) -> str:
    if n < 1024:
        return f"{n} B"
    if n < 1024 * 1024:
        return f"{n / 1024:.1f} KB"
    return f"{n / (1024 * 1024):.2f} MB"


def fmt_mtime(ts: float) -> str:
    try:
        return datetime.fromtimestamp(ts).strftime("%Y-%m-%d %H:%M")
    except (OSError, OverflowError, ValueError):
        return ""


def fmt_mode(mode: int) -> str:
    return stat.filemode(mode)


def load_config() -> dict[str, Any]:
    if CONFIG_PATH.is_file():
        try:
            return json.loads(CONFIG_PATH.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError):
            pass
    return {"dark_mode": True, "base_url": DEFAULT_BASE_URL}


def save_config(cfg: dict[str, Any]) -> None:
    try:
        CONFIG_PATH.write_text(json.dumps(cfg, indent=2), encoding="utf-8")
    except OSError:
        pass


def pick_font(candidates: list[str], size: int, weight: str = "normal") -> tuple:
    # tk font tuples; first available family wins at runtime via Tk
    return (candidates[0], size, weight)


FONT_UI = ["Segoe UI", "IBM Plex Sans", "Helvetica Neue", "Helvetica", "Arial"]
FONT_BRAND = ["Segoe UI Semibold", "Segoe UI", "IBM Plex Sans", "Helvetica"]
FONT_MONO = ["Cascadia Mono", "Consolas", "Courier New", "monospace"]


class TextEditorDialog(tk.Toplevel):
    def __init__(
        self,
        master: tk.Misc,
        title: str,
        initial: str,
        colors: dict[str, str],
        readonly: bool = False,
    ) -> None:
        super().__init__(master)
        self.title(title)
        self.geometry("760x520")
        self.result: Optional[str] = None
        self.transient(master)
        self.grab_set()
        self.configure(bg=colors["app_bg"])

        bar = tk.Frame(self, bg=colors["header_bg"], padx=12, pady=10)
        bar.pack(fill=tk.X)
        tk.Label(
            bar,
            text=title,
            bg=colors["header_bg"],
            fg=colors["text"],
            font=(FONT_UI[0], 11, "bold"),
        ).pack(side=tk.LEFT)
        if not readonly:
            tk.Button(
                bar,
                text="Save",
                command=self._save,
                bg=colors["accent"],
                fg=colors["accent_text"],
                activebackground=colors["accent_dim"],
                relief=tk.FLAT,
                padx=14,
                pady=4,
                cursor="hand2",
            ).pack(side=tk.RIGHT, padx=(8, 0))
        tk.Button(
            bar,
            text="Close",
            command=self.destroy,
            bg=colors["btn_bg"],
            fg=colors["btn_fg"],
            activebackground=colors["btn_active"],
            relief=tk.FLAT,
            padx=12,
            pady=4,
            cursor="hand2",
        ).pack(side=tk.RIGHT)

        body = tk.Frame(self, bg=colors["app_bg"])
        body.pack(fill=tk.BOTH, expand=True, padx=10, pady=10)
        self.text = tk.Text(
            body,
            wrap=tk.NONE,
            undo=True,
            bg=colors["log_bg"],
            fg=colors["log_fg"],
            insertbackground=colors["accent"],
            relief=tk.FLAT,
            borderwidth=0,
            font=(FONT_MONO[0], 10),
            padx=10,
            pady=10,
        )
        ysb = ttk.Scrollbar(body, orient=tk.VERTICAL, command=self.text.yview)
        xsb = ttk.Scrollbar(body, orient=tk.HORIZONTAL, command=self.text.xview)
        self.text.configure(yscrollcommand=ysb.set, xscrollcommand=xsb.set)
        self.text.grid(row=0, column=0, sticky="nsew")
        ysb.grid(row=0, column=1, sticky="ns")
        xsb.grid(row=1, column=0, sticky="ew")
        body.rowconfigure(0, weight=1)
        body.columnconfigure(0, weight=1)
        self.text.insert("1.0", initial)
        if readonly:
            self.text.configure(state=tk.DISABLED)
        self.wait_window(self)

    def _save(self) -> None:
        self.result = self.text.get("1.0", "end-1c")
        self.destroy()


class RndisStorageApp(tk.Tk):
    def __init__(self) -> None:
        super().__init__()
        self.title("RNDISStorage")
        self.geometry("1280x780")
        self.minsize(1024, 640)

        self.cfg = load_config()
        self.dark_mode = tk.BooleanVar(value=bool(self.cfg.get("dark_mode", True)))
        self.base_url_var = tk.StringVar(value=str(self.cfg.get("base_url", DEFAULT_BASE_URL)))
        self.colors = THEMES["dark" if self.dark_mode.get() else "light"]

        self.client: Optional[RndisStorageClient] = None
        self.local_path = tk.StringVar(value=str(Path.home()))
        self.remote_path = tk.StringVar(value="/")
        self.status_var = tk.StringVar(value="Disconnected")
        self.storage_var = tk.StringVar(value="No device")
        self.conn_var = tk.StringVar(value="Offline")
        self.busy = False
        self._url_hints: list[str] = []
        self._sash_centered = False
        self.style = ttk.Style(self)

        self._build_ui()
        self.apply_theme()
        self.refresh_urls()
        self.refresh_local()
        self.protocol("WM_DELETE_WINDOW", self.on_close)
        self.after(80, self._center_sash)
        self.after(250, self._center_sash)
        self.paned.bind("<Map>", lambda _e: self.after(30, self._center_sash))
        self.bind("<Configure>", self._on_root_configure)

    # --- layout ---

    def _build_ui(self) -> None:
        self.configure(bg=self.colors["app_bg"])

        # Header / brand
        self.header = tk.Frame(self, bg=self.colors["header_bg"], height=72)
        self.header.pack(fill=tk.X)
        self.header.pack_propagate(False)

        brand_wrap = tk.Frame(self.header, bg=self.colors["header_bg"])
        brand_wrap.pack(side=tk.LEFT, padx=20, pady=14)
        self.lbl_brand = tk.Label(
            brand_wrap,
            text="RNDISStorage",
            bg=self.colors["header_bg"],
            fg=self.colors["text"],
            font=(FONT_BRAND[0], 18, "bold"),
        )
        self.lbl_brand.pack(anchor=tk.W)
        self.lbl_tag = tk.Label(
            brand_wrap,
            text="WebDAV / HTTP file bridge  ·  lab PoC",
            bg=self.colors["header_bg"],
            fg=self.colors["text_muted"],
            font=(FONT_UI[0], 9),
        )
        self.lbl_tag.pack(anchor=tk.W)

        right_hdr = tk.Frame(self.header, bg=self.colors["header_bg"])
        right_hdr.pack(side=tk.RIGHT, padx=20)
        self.lbl_conn_pill = tk.Label(
            right_hdr,
            textvariable=self.conn_var,
            bg=self.colors["elevated"],
            fg=self.colors["text_muted"],
            font=(FONT_UI[0], 9, "bold"),
            padx=12,
            pady=5,
        )
        self.lbl_conn_pill.pack(side=tk.RIGHT, padx=(12, 0))

        self.btn_theme = tk.Button(
            right_hdr,
            text="Dark" if self.dark_mode.get() else "Light",
            command=self.toggle_theme,
            relief=tk.FLAT,
            padx=12,
            pady=5,
            cursor="hand2",
            font=(FONT_UI[0], 9),
        )
        self.btn_theme.pack(side=tk.RIGHT)

        self.header_line = tk.Frame(self, bg=self.colors["header_line"], height=1)
        self.header_line.pack(fill=tk.X)

        # Connection strip
        self.strip = tk.Frame(self, bg=self.colors["surface"], padx=16, pady=12)
        self.strip.pack(fill=tk.X)

        tk.Label(
            self.strip,
            text="URL",
            bg=self.colors["surface"],
            fg=self.colors["text_dim"],
            font=(FONT_UI[0], 8, "bold"),
        ).pack(side=tk.LEFT, padx=(0, 8))

        self.url_combo = ttk.Combobox(
            self.strip, width=48, textvariable=self.base_url_var, font=(FONT_MONO[0], 9)
        )
        self.url_combo.pack(side=tk.LEFT, fill=tk.X, expand=True, padx=(0, 8))

        self.btn_refresh_urls = self._mk_btn(self.strip, "Hints", self.refresh_urls)
        self.btn_refresh_urls.pack(side=tk.LEFT, padx=4)
        self.btn_connect = self._mk_btn(self.strip, "Connect", self.toggle_connect, primary=True)
        self.btn_connect.pack(side=tk.LEFT, padx=4)

        self.lbl_storage = tk.Label(
            self.strip,
            textvariable=self.storage_var,
            bg=self.colors["surface"],
            fg=self.colors["text_muted"],
            font=(FONT_UI[0], 9),
        )
        self.lbl_storage.pack(side=tk.RIGHT, padx=(12, 0))

        # Dual explorers
        self.body = tk.Frame(self, bg=self.colors["app_bg"], padx=12, pady=10)
        self.body.pack(fill=tk.BOTH, expand=True)

        self.paned = ttk.Panedwindow(self.body, orient=tk.HORIZONTAL)
        self.paned.pack(fill=tk.BOTH, expand=True)

        self.left_pane = tk.Frame(self.paned, bg=self.colors["surface"])
        self.right_pane = tk.Frame(self.paned, bg=self.colors["surface"])
        self.paned.add(self.left_pane, weight=1)
        self.paned.add(self.right_pane, weight=1)

        self._build_local_pane(self.left_pane)
        self._build_remote_pane(self.right_pane)

        # Transfer bar
        self.mid = tk.Frame(self, bg=self.colors["app_bg"], pady=4)
        self.mid.pack(fill=tk.X, padx=12)
        mid_inner = tk.Frame(self.mid, bg=self.colors["app_bg"])
        mid_inner.pack()
        self.btn_upload = self._mk_btn(mid_inner, "  Upload →  ", self.upload_selected, primary=True)
        self.btn_upload.pack(side=tk.LEFT, padx=6)
        self.btn_download = self._mk_btn(mid_inner, "  ← Download  ", self.download_selected, primary=True)
        self.btn_download.pack(side=tk.LEFT, padx=6)

        self.progress = ttk.Progressbar(self, mode="determinate")
        self.progress.pack(fill=tk.X, padx=16, pady=(2, 6))

        # Footer / log
        self.footer = tk.Frame(self, bg=self.colors["surface"], padx=12, pady=8)
        self.footer.pack(fill=tk.BOTH)
        self.lbl_status = tk.Label(
            self.footer,
            textvariable=self.status_var,
            bg=self.colors["surface"],
            fg=self.colors["text_muted"],
            font=(FONT_UI[0], 9),
            anchor=tk.W,
        )
        self.lbl_status.pack(fill=tk.X, pady=(0, 6))

        log_frame = tk.Frame(self.footer, bg=self.colors["surface"])
        log_frame.pack(fill=tk.BOTH, expand=True)
        self.log = tk.Text(
            log_frame,
            height=7,
            wrap=tk.WORD,
            relief=tk.FLAT,
            borderwidth=0,
            font=(FONT_MONO[0], 9),
            padx=10,
            pady=8,
        )
        log_sb = ttk.Scrollbar(log_frame, orient=tk.VERTICAL, command=self.log.yview)
        self.log.configure(yscrollcommand=log_sb.set, state=tk.DISABLED)
        self.log.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
        log_sb.pack(side=tk.RIGHT, fill=tk.Y)

    def _mk_btn(
        self, parent: tk.Misc, text: str, command: Callable, primary: bool = False
    ) -> tk.Button:
        c = self.colors
        return tk.Button(
            parent,
            text=text,
            command=command,
            relief=tk.FLAT,
            padx=10,
            pady=5,
            cursor="hand2",
            font=(FONT_UI[0], 9),
            bg=c["accent"] if primary else c["btn_bg"],
            fg=c["accent_text"] if primary else c["btn_fg"],
            activebackground=c["accent_dim"] if primary else c["btn_active"],
            activeforeground=c["accent_text"] if primary else c["btn_fg"],
            highlightthickness=0,
            bd=0,
        )

    def _mk_tool(self, parent: tk.Misc, text: str, command: Callable) -> tk.Button:
        c = self.colors
        return tk.Button(
            parent,
            text=text,
            command=command,
            relief=tk.FLAT,
            padx=8,
            pady=3,
            cursor="hand2",
            font=(FONT_UI[0], 8),
            bg=c["elevated"],
            fg=c["text"],
            activebackground=c["btn_active"],
            activeforeground=c["text"],
            highlightthickness=0,
            bd=0,
        )

    def _build_tree(self, parent: tk.Frame, columns: tuple[str, ...], widths: tuple[int, ...]) -> ttk.Treeview:
        wrap = tk.Frame(parent, bg=self.colors["surface"])
        wrap.pack(fill=tk.BOTH, expand=True, padx=1, pady=1)

        tree = ttk.Treeview(wrap, columns=columns, show="headings", selectmode="extended")
        for c, w in zip(columns, widths):
            tree.heading(c, text=c.upper())
            tree.column(c, width=w, anchor=tk.W, minwidth=48)

        ysb = ttk.Scrollbar(wrap, orient=tk.VERTICAL, command=tree.yview)
        xsb = ttk.Scrollbar(wrap, orient=tk.HORIZONTAL, command=tree.xview)
        tree.configure(yscrollcommand=ysb.set, xscrollcommand=xsb.set)
        tree.grid(row=0, column=0, sticky="nsew")
        ysb.grid(row=0, column=1, sticky="ns")
        xsb.grid(row=1, column=0, sticky="ew")
        wrap.rowconfigure(0, weight=1)
        wrap.columnconfigure(0, weight=1)
        return tree

    def _pane_chrome(
        self,
        parent: tk.Frame,
        title: str,
        path_var: tk.StringVar,
        go_cmd,
        up_cmd,
        extra_btns=None,
        title_attr: str = "",
    ) -> tk.Frame:
        c = self.colors
        head = tk.Frame(parent, bg=c["surface_2"], padx=12, pady=10)
        head.pack(fill=tk.X)
        title_lbl = tk.Label(
            head,
            text=title,
            bg=c["surface_2"],
            fg=c["pane_title"],
            font=(FONT_UI[0], 10, "bold"),
        )
        title_lbl.pack(anchor=tk.W)
        if title_attr:
            setattr(self, title_attr, title_lbl)
        path_row = tk.Frame(head, bg=c["surface_2"])
        path_row.pack(fill=tk.X, pady=(8, 0))
        ent = tk.Entry(
            path_row,
            textvariable=path_var,
            bg=c["input_bg"],
            fg=c["input_fg"],
            insertbackground=c["accent"],
            relief=tk.FLAT,
            font=(FONT_MONO[0], 9),
            highlightthickness=1,
            highlightbackground=c["border"],
            highlightcolor=c["accent"],
        )
        ent.pack(side=tk.LEFT, fill=tk.X, expand=True, ipady=4)
        self._mk_tool(path_row, "Go", go_cmd).pack(side=tk.LEFT, padx=(6, 0))
        self._mk_tool(path_row, "Up", up_cmd).pack(side=tk.LEFT, padx=(4, 0))
        if extra_btns:
            for label, cmd in extra_btns:
                self._mk_tool(path_row, label, cmd).pack(side=tk.LEFT, padx=(4, 0))

        tools = tk.Frame(parent, bg=c["surface"], padx=8, pady=6)
        tools.pack(fill=tk.X)
        return tools

    def _build_local_pane(self, parent: tk.Frame) -> None:
        tools = self._pane_chrome(
            parent,
            "LOCAL",
            self.local_path,
            self.refresh_local,
            self.local_up,
            extra_btns=[("…", self.browse_local)],
            title_attr="lbl_local_title",
        )
        for text, cmd in [
            ("Refresh", self.refresh_local),
            ("View", self.local_view),
            ("Edit", self.local_edit),
            ("New folder", self.local_mkdir),
            ("New file", self.local_new_file),
            ("Delete", self.local_delete),
            ("Rename", self.local_rename),
            ("Permissions", self.local_chmod),
        ]:
            self._mk_tool(tools, text, cmd).pack(side=tk.LEFT, padx=2)

        self.local_tree = self._build_tree(
            parent, ("name", "size", "mtime", "perms", "type"), (200, 80, 120, 90, 56)
        )
        self.local_tree.bind("<Double-1>", self.on_local_double)
        self.local_tree.bind("<Button-3>", self.on_local_context)

    def _build_remote_pane(self, parent: tk.Frame) -> None:
        tools = self._pane_chrome(
            parent,
            "DEVICE  ·  WebDAV",
            self.remote_path,
            self.refresh_remote,
            self.remote_up,
            title_attr="lbl_remote_title",
        )
        for text, cmd in [
            ("Refresh", self.refresh_remote),
            ("View", self.remote_view),
            ("Edit", self.remote_edit),
            ("New folder", self.remote_mkdir),
            ("New file", self.remote_touch),
            ("Delete", self.remote_delete),
            ("Rename", self.remote_rename),
            ("Move", self.remote_move),
            ("Format SD", self.remote_format),
            ("Properties", self.remote_properties),
        ]:
            self._mk_tool(tools, text, cmd).pack(side=tk.LEFT, padx=2)

        self.remote_tree = self._build_tree(
            parent, ("name", "size", "perms", "type"), (220, 90, 70, 56)
        )
        self.remote_tree.bind("<Double-1>", self.on_remote_double)
        self.remote_tree.bind("<Button-3>", self.on_remote_context)

    # --- theme ---

    def toggle_theme(self) -> None:
        self.dark_mode.set(not self.dark_mode.get())
        self.cfg["dark_mode"] = bool(self.dark_mode.get())
        save_config(self.cfg)
        self.colors = THEMES["dark" if self.dark_mode.get() else "light"]
        self.btn_theme.configure(text="Dark" if self.dark_mode.get() else "Light")
        self.apply_theme()

    def apply_theme(self) -> None:
        c = self.colors
        self.configure(bg=c["app_bg"])
        try:
            self.style.theme_use("clam")
        except tk.TclError:
            pass

        self.style.configure("TFrame", background=c["app_bg"])
        self.style.configure("TLabel", background=c["app_bg"], foreground=c["text"])
        self.style.configure(
            "Treeview",
            background=c["tree_bg"],
            foreground=c["tree_fg"],
            fieldbackground=c["tree_bg"],
            bordercolor=c["border"],
            lightcolor=c["border"],
            darkcolor=c["border"],
            rowheight=26,
            font=(FONT_UI[0], 9),
        )
        self.style.configure(
            "Treeview.Heading",
            background=c["tree_head_bg"],
            foreground=c["tree_head_fg"],
            relief=tk.FLAT,
            font=(FONT_UI[0], 8, "bold"),
        )
        self.style.map(
            "Treeview",
            background=[("selected", c["select_bg"])],
            foreground=[("selected", c["select_fg"])],
        )
        self.style.map("Treeview.Heading", background=[("active", c["elevated"])])
        self.style.configure(
            "TCombobox",
            fieldbackground=c["input_bg"],
            background=c["input_bg"],
            foreground=c["input_fg"],
            arrowcolor=c["text_muted"],
            bordercolor=c["border"],
            lightcolor=c["border"],
            darkcolor=c["border"],
            padding=4,
        )
        self.style.map(
            "TCombobox",
            fieldbackground=[("readonly", c["input_bg"])],
            foreground=[("readonly", c["input_fg"])],
            selectbackground=[("readonly", c["select_bg"])],
            selectforeground=[("readonly", c["select_fg"])],
        )
        self.style.configure(
            "Horizontal.TProgressbar",
            troughcolor=c["elevated"],
            background=c["accent"],
            bordercolor=c["border"],
            lightcolor=c["accent"],
            darkcolor=c["accent_dim"],
            thickness=6,
        )
        self.style.configure("TScrollbar", background=c["elevated"], troughcolor=c["surface"], arrowcolor=c["text_muted"])

        self.header.configure(bg=c["header_bg"])
        self.header_line.configure(bg=c["header_line"])
        self.strip.configure(bg=c["surface"])
        self.body.configure(bg=c["app_bg"])
        self.mid.configure(bg=c["app_bg"])
        self.footer.configure(bg=c["surface"])
        self.left_pane.configure(bg=c["surface"])
        self.right_pane.configure(bg=c["surface"])

        self._paint_descendants(self.header, c["header_bg"])
        self._paint_descendants(self.strip, c["surface"])
        self._paint_pane(self.left_pane)
        self._paint_pane(self.right_pane)
        self._paint_descendants(self.mid, c["app_bg"])
        self._paint_descendants(self.footer, c["surface"])

        self.lbl_brand.configure(bg=c["header_bg"], fg=c["text"])
        self.lbl_tag.configure(bg=c["header_bg"], fg=c["text_muted"])
        online = bool(self.client and self.client.connected)
        self.lbl_conn_pill.configure(
            bg=c["elevated"], fg=c["ok"] if online else c["text_muted"]
        )
        self.lbl_storage.configure(bg=c["surface"], fg=c["text_muted"])
        self.lbl_status.configure(bg=c["surface"], fg=c["text_muted"])
        self.btn_theme.configure(
            bg=c["btn_bg"], fg=c["btn_fg"], activebackground=c["btn_active"], activeforeground=c["btn_fg"]
        )
        self._style_primary(self.btn_connect, connected=online)
        self._style_primary(self.btn_upload, always_primary=True)
        self._style_primary(self.btn_download, always_primary=True)
        self.btn_refresh_urls.configure(
            bg=c["btn_bg"],
            fg=c["btn_fg"],
            activebackground=c["btn_active"],
            activeforeground=c["btn_fg"],
        )

        self.log.configure(
            state=tk.NORMAL,
            bg=c["log_bg"],
            fg=c["log_fg"],
            insertbackground=c["accent"],
        )
        self.log.configure(state=tk.DISABLED)
        self.local_tree.tag_configure("dir", foreground=c["accent"])
        self.local_tree.tag_configure("file", foreground=c["tree_fg"])
        self.remote_tree.tag_configure("dir", foreground=c["accent"])
        self.remote_tree.tag_configure("file", foreground=c["tree_fg"])
        if hasattr(self, "lbl_local_title"):
            self.lbl_local_title.configure(bg=c["surface_2"], fg=c["pane_title"])
        if hasattr(self, "lbl_remote_title"):
            self.lbl_remote_title.configure(bg=c["surface_2"], fg=c["pane_title"])
        self.lbl_tag.configure(bg=c["header_bg"], fg=c["text_muted"])

    def _paint_pane(self, pane: tk.Frame) -> None:
        c = self.colors
        pane.configure(bg=c["surface"])
        for child in pane.winfo_children():
            cls = child.winfo_class()
            if cls == "Frame":
                # First packed frame in pane is typically the header chrome
                try:
                    # Heuristic: header rows use surface_2
                    child.configure(bg=c["surface_2"])
                    self._paint_descendants(child, c["surface_2"])
                except tk.TclError:
                    pass
            else:
                self._paint_descendants(child, c["surface"])

        # Re-walk: toolbars and tree wrappers should be surface, not surface_2
        kids = pane.winfo_children()
        if len(kids) >= 2:
            for fr in kids[1:]:
                if fr.winfo_class() == "Frame":
                    fr.configure(bg=c["surface"])
                    self._paint_descendants(fr, c["surface"])

    def _paint_descendants(self, widget: tk.Misc, bg: str) -> None:
        c = self.colors
        for child in widget.winfo_children():
            cls = child.winfo_class()
            try:
                if cls == "Frame":
                    child.configure(bg=bg)
                    self._paint_descendants(child, bg)
                elif cls == "Label":
                    fg = c["text"]
                    # keep muted labels muted if they look like captions
                    child.configure(bg=bg, fg=child.cget("fg") if False else c["text"])
                    # restore known muted roles via font size heuristic is fragile; set below selectively
                elif cls == "Button":
                    # Skip primary/connect; restyle small tools
                    txt = child.cget("text")
                    if txt in ("Connect", "Disconnect", "  Upload →  ", "  ← Download  ", "Format"):
                        continue
                    if txt in ("Dark", "Light"):
                        continue
                    child.configure(
                        bg=c["elevated"] if bg != c["header_bg"] else c["btn_bg"],
                        fg=c["text"],
                        activebackground=c["btn_active"],
                        activeforeground=c["text"],
                    )
                elif cls == "Entry":
                    child.configure(
                        bg=c["input_bg"],
                        fg=c["input_fg"],
                        insertbackground=c["accent"],
                        highlightbackground=c["border"],
                        highlightcolor=c["accent"],
                    )
                elif cls == "Text":
                    pass
                else:
                    self._paint_descendants(child, bg)
            except tk.TclError:
                continue

    def _style_primary(
        self, btn: tk.Button, connected: bool = False, always_primary: bool = False
    ) -> None:
        c = self.colors
        if always_primary:
            btn.configure(
                bg=c["accent"],
                fg=c["accent_text"],
                activebackground=c["accent_dim"],
                activeforeground=c["accent_text"],
            )
        elif connected:
            btn.configure(
                bg=c["danger"],
                fg="#ffffff",
                activebackground="#b85a4a",
                activeforeground="#ffffff",
                text="Disconnect",
            )
        else:
            btn.configure(
                bg=c["accent"],
                fg=c["accent_text"],
                activebackground=c["accent_dim"],
                activeforeground=c["accent_text"],
                text="Connect",
            )

    def _center_sash(self) -> None:
        try:
            self.update_idletasks()
            w = self.paned.winfo_width()
            if w > 50:
                self.paned.sashpos(0, w // 2)
                self._sash_centered = True
        except tk.TclError:
            pass

    def _on_root_configure(self, event: tk.Event) -> None:
        if event.widget is self and not self._sash_centered:
            self.after(50, self._center_sash)

    # --- helpers ---

    def log_msg(self, msg: str) -> None:
        self.log.configure(state=tk.NORMAL)
        self.log.insert(tk.END, msg + "\n")
        self.log.see(tk.END)
        self.log.configure(state=tk.DISABLED)

    def set_busy(self, busy: bool, text: Optional[str] = None) -> None:
        self.busy = busy
        if text:
            self.status_var.set(text)

    def run_bg(self, work: Callable[[], None], done: Optional[Callable[[], None]] = None) -> None:
        if self.busy:
            messagebox.showinfo("Busy", "Wait for the current operation to finish.")
            return

        def runner() -> None:
            err: Optional[BaseException] = None
            try:
                work()
            except BaseException as exc:  # noqa: BLE001
                err = exc

            def finish() -> None:
                self.set_busy(False)
                self.progress.configure(value=0)
                if err:
                    self.log_msg(f"ERROR: {err}")
                    messagebox.showerror("RNDISStorage", str(err))
                if done and not err:
                    done()

            self.after(0, finish)

        self.set_busy(True, "Working…")
        threading.Thread(target=runner, daemon=True).start()

    def require_client(self) -> Optional[RndisStorageClient]:
        if not self.client or not self.client.connected:
            messagebox.showwarning("Not connected", "Connect to the device first.")
            return None
        return self.client

    def _is_probably_text(self, data: bytes) -> bool:
        if not data:
            return True
        sample = data[:4096]
        if b"\x00" in sample:
            return False
        try:
            sample.decode("utf-8")
            return True
        except UnicodeDecodeError:
            return False

    def _open_external(self, path: Path) -> None:
        try:
            if sys.platform.startswith("win"):
                os.startfile(path)  # type: ignore[attr-defined]
            elif sys.platform == "darwin":
                subprocess.Popen(["open", str(path)])
            else:
                subprocess.Popen(["xdg-open", str(path)])
        except OSError as exc:
            messagebox.showerror("Open", str(exc))

    def _ctx_menu(self, event: tk.Event, items: list[tuple[str, Optional[Callable]]]) -> None:
        c = self.colors
        menu = tk.Menu(
            self,
            tearoff=0,
            bg=c["elevated"],
            fg=c["text"],
            activebackground=c["select_bg"],
            activeforeground=c["select_fg"],
            bd=0,
            font=(FONT_UI[0], 9),
        )
        for label, cmd in items:
            if label == "---":
                menu.add_separator()
            elif cmd:
                menu.add_command(label=label, command=cmd)
        menu.tk_popup(event.x_root, event.y_root)

    # --- connection ---

    def refresh_urls(self) -> None:
        hints = discover_default_urls()
        self._url_hints = hints
        self.url_combo["values"] = hints
        if not self.base_url_var.get():
            self.base_url_var.set(DEFAULT_BASE_URL)
        self.log_msg(
            "Plug the dongle — Windows should add a USB Ethernet adapter, "
            f"then Connect to {self.base_url_var.get()}"
        )

    def toggle_connect(self) -> None:
        if self.client and self.client.connected:
            self.client.close()
            self.client = None
            self.btn_connect.configure(text="Connect")
            self._style_primary(self.btn_connect, connected=False)
            self.status_var.set("Disconnected")
            self.conn_var.set("Offline")
            self.storage_var.set("No device")
            self.lbl_conn_pill.configure(fg=self.colors["text_muted"])
            self.remote_tree.delete(*self.remote_tree.get_children())
            self.log_msg("Disconnected")
            return

        url = normalize_base_url(self.base_url_var.get())
        if not url:
            messagebox.showwarning("URL", "Enter the device base URL (e.g. http://203.0.113.1).")
            return
        self.base_url_var.set(url)
        self.cfg["base_url"] = url
        save_config(self.cfg)

        def work() -> None:
            client = RndisStorageClient(url)
            client.connect()
            info = client.info()
            storage = client.storage()

            def applied() -> None:
                self.client = client
                self.btn_connect.configure(text="Disconnect")
                self._style_primary(self.btn_connect, connected=True)
                self.conn_var.set("Online")
                self.lbl_conn_pill.configure(fg=self.colors["ok"])
                transport = info.get("transport", "?")
                self.status_var.set(
                    f"Connected {url} — {info.get('device')} / {transport}"
                )
                self._update_storage_label(storage)
                self.log_msg(f"Connected to {url} ({transport})")
                self.refresh_remote()

            self.after(0, applied)

        self.run_bg(work)

    def _update_storage_label(self, storage: dict[str, Any]) -> None:
        total = int(storage.get("total_bytes", 0))
        used = int(storage.get("used_bytes", 0))
        free = int(storage.get("free_bytes", 0))
        backend = storage.get("backend", "?")
        self.storage_var.set(
            f"{backend}  ·  {fmt_size(used)} used / {fmt_size(total)}  ·  {fmt_size(free)} free"
        )

    # --- local ---

    def browse_local(self) -> None:
        path = filedialog.askdirectory(initialdir=self.local_path.get())
        if path:
            self.local_path.set(path)
            self.refresh_local()

    def local_up(self) -> None:
        self.local_path.set(str(Path(self.local_path.get()).resolve().parent))
        self.refresh_local()

    def refresh_local(self) -> None:
        path = Path(self.local_path.get())
        if not path.is_dir():
            messagebox.showerror("Local", f"Not a directory: {path}")
            return
        self.local_tree.delete(*self.local_tree.get_children())
        try:
            entries = sorted(path.iterdir(), key=lambda x: (not x.is_dir(), x.name.lower()))
        except PermissionError as exc:
            messagebox.showerror("Local", str(exc))
            return
        for entry in entries:
            try:
                st = entry.stat()
                kind = "dir" if entry.is_dir() else "file"
                size = "" if entry.is_dir() else fmt_size(st.st_size)
                self.local_tree.insert(
                    "",
                    tk.END,
                    iid=str(entry),
                    values=(entry.name, size, fmt_mtime(st.st_mtime), fmt_mode(st.st_mode), kind),
                    tags=("dir" if entry.is_dir() else "file",),
                )
            except OSError:
                continue
        self.local_tree.tag_configure("dir", foreground=self.colors["accent"])
        self.local_tree.tag_configure("file", foreground=self.colors["tree_fg"])

    def selected_local_paths(self) -> list[Path]:
        return [Path(i) for i in self.local_tree.selection()]

    def on_local_double(self, _event: tk.Event) -> None:
        sel = self.selected_local_paths()
        if len(sel) == 1 and sel[0].is_dir():
            self.local_path.set(str(sel[0]))
            self.refresh_local()
        elif len(sel) == 1 and sel[0].is_file():
            self.local_view()

    def on_local_context(self, event: tk.Event) -> None:
        self._ctx_menu(
            event,
            [
                ("Open / Enter", lambda: self.on_local_double(event)),
                ("View", self.local_view),
                ("Edit", self.local_edit),
                ("Open with system app", self.local_open_external),
                ("Upload →", self.upload_selected),
                ("---", None),
                ("New folder", self.local_mkdir),
                ("Delete", self.local_delete),
                ("Rename", self.local_rename),
                ("Permissions", self.local_chmod),
            ],
        )

    def local_view(self) -> None:
        paths = [p for p in self.selected_local_paths() if p.is_file()]
        if len(paths) != 1:
            messagebox.showinfo("View", "Select a single file.")
            return
        data = paths[0].read_bytes()
        if len(data) > MAX_EDIT_BYTES:
            if messagebox.askyesno("View", "File is large. Open with system app instead?"):
                self._open_external(paths[0])
            return
        if not self._is_probably_text(data):
            if messagebox.askyesno("View", "Binary file. Open with system app?"):
                self._open_external(paths[0])
            return
        TextEditorDialog(
            self,
            f"View — {paths[0].name}",
            data.decode("utf-8", errors="replace"),
            self.colors,
            readonly=True,
        )

    def local_edit(self) -> None:
        paths = [p for p in self.selected_local_paths() if p.is_file()]
        if len(paths) != 1:
            messagebox.showinfo("Edit", "Select a single file.")
            return
        path = paths[0]
        data = path.read_bytes()
        if len(data) > MAX_EDIT_BYTES or not self._is_probably_text(data):
            messagebox.showwarning("Edit", "Use an external editor for this file.")
            return
        dlg = TextEditorDialog(
            self,
            f"Edit — {path.name}",
            data.decode("utf-8", errors="replace"),
            self.colors,
            readonly=False,
        )
        if dlg.result is not None:
            path.write_text(dlg.result, encoding="utf-8", newline="\n")
            self.log_msg(f"Saved {path}")
            self.refresh_local()

    def local_open_external(self) -> None:
        paths = [p for p in self.selected_local_paths() if p.is_file()]
        if len(paths) == 1:
            self._open_external(paths[0])

    def local_mkdir(self) -> None:
        name = simpledialog.askstring("New folder", "Folder name:")
        if not name:
            return
        (Path(self.local_path.get()) / name).mkdir(parents=False, exist_ok=False)
        self.refresh_local()

    def local_new_file(self) -> None:
        name = simpledialog.askstring("New file", "File name:")
        if not name:
            return
        (Path(self.local_path.get()) / name).touch(exist_ok=False)
        self.refresh_local()

    def local_delete(self) -> None:
        paths = self.selected_local_paths()
        if not paths or not messagebox.askyesno("Delete", f"Delete {len(paths)} item(s)?"):
            return
        for p in paths:
            if p.is_dir():
                shutil.rmtree(p)
            else:
                p.unlink()
        self.refresh_local()

    def local_rename(self) -> None:
        paths = self.selected_local_paths()
        if len(paths) != 1:
            messagebox.showinfo("Rename", "Select a single item.")
            return
        new_name = simpledialog.askstring("Rename", "New name:", initialvalue=paths[0].name)
        if not new_name:
            return
        paths[0].rename(paths[0].with_name(new_name))
        self.refresh_local()

    def local_chmod(self) -> None:
        paths = self.selected_local_paths()
        if len(paths) != 1:
            messagebox.showinfo("Permissions", "Select a single item.")
            return
        mode_str = simpledialog.askstring("Permissions", "Octal mode:", initialvalue="644")
        if not mode_str:
            return
        try:
            os.chmod(paths[0], int(mode_str, 8))
            self.refresh_local()
        except OSError as exc:
            messagebox.showerror("Permissions", str(exc))

    # --- remote ---

    def remote_up(self) -> None:
        self.remote_path.set(parent_remote(self.remote_path.get()))
        self.refresh_remote()

    def refresh_remote(self) -> None:
        c = self.require_client()
        if not c:
            return
        path = self.remote_path.get() or "/"

        def work() -> None:
            resp = c.ls(path)
            storage = c.storage()

            def apply() -> None:
                self.remote_tree.delete(*self.remote_tree.get_children())
                entries = sorted(
                    resp.get("entries") or [],
                    key=lambda e: (e.get("type") != "dir", str(e.get("name", "")).lower()),
                )
                for e in entries:
                    name = e.get("name", "")
                    kind = e.get("type", "file")
                    size = "" if kind == "dir" else fmt_size(int(e.get("size", 0)))
                    self.remote_tree.insert(
                        "",
                        tk.END,
                        iid=name,
                        values=(name, size, "N/A", kind),
                        tags=("dir" if kind == "dir" else "file",),
                    )
                self.remote_tree.tag_configure("dir", foreground=self.colors["accent"])
                self.remote_tree.tag_configure("file", foreground=self.colors["tree_fg"])
                self._update_storage_label(storage)
                self.status_var.set(f"Remote: {path}")

            self.after(0, apply)

        self.run_bg(work)

    def selected_remote_names(self) -> list[str]:
        return list(self.remote_tree.selection())

    def _remote_kind(self, name: str) -> str:
        vals = self.remote_tree.item(name)["values"]
        return str(vals[3]) if vals and len(vals) > 3 else "file"

    def on_remote_double(self, _event: tk.Event) -> None:
        names = self.selected_remote_names()
        if len(names) != 1:
            return
        if self._remote_kind(names[0]) == "dir":
            self.remote_path.set(join_remote(self.remote_path.get(), names[0]))
            self.refresh_remote()
        else:
            self.remote_view()

    def on_remote_context(self, event: tk.Event) -> None:
        self._ctx_menu(
            event,
            [
                ("Open / Enter", lambda: self.on_remote_double(event)),
                ("View", self.remote_view),
                ("Edit", self.remote_edit),
                ("← Download", self.download_selected),
                ("---", None),
                ("New folder", self.remote_mkdir),
                ("New file", self.remote_touch),
                ("Delete", self.remote_delete),
                ("Rename", self.remote_rename),
                ("Move", self.remote_move),
                ("Format SD…", self.remote_format),
                ("Properties", self.remote_properties),
            ],
        )

    def remote_view(self) -> None:
        c = self.require_client()
        if not c:
            return
        names = [n for n in self.selected_remote_names() if self._remote_kind(n) == "file"]
        if len(names) != 1:
            messagebox.showinfo("View", "Select a single remote file.")
            return
        remote = join_remote(self.remote_path.get(), names[0])

        def work() -> None:
            with tempfile.NamedTemporaryFile(delete=False, suffix="_" + names[0]) as tmp:
                local = tmp.name
            try:
                c.get(remote, local)
                data = Path(local).read_bytes()
            finally:
                try:
                    os.unlink(local)
                except OSError:
                    pass

            def show() -> None:
                if len(data) > MAX_EDIT_BYTES or not self._is_probably_text(data):
                    messagebox.showwarning("View", "Download the file to open it externally.")
                    return
                TextEditorDialog(
                    self,
                    f"View — {names[0]}",
                    data.decode("utf-8", errors="replace"),
                    self.colors,
                    readonly=True,
                )

            self.after(0, show)

        self.run_bg(work)

    def remote_edit(self) -> None:
        c = self.require_client()
        if not c:
            return
        names = [n for n in self.selected_remote_names() if self._remote_kind(n) == "file"]
        if len(names) != 1:
            messagebox.showinfo("Edit", "Select a single remote file.")
            return
        remote = join_remote(self.remote_path.get(), names[0])

        def work() -> None:
            with tempfile.NamedTemporaryFile(delete=False, suffix="_" + names[0]) as tmp:
                local = tmp.name
            try:
                c.get(remote, local)
                data = Path(local).read_bytes()
            finally:
                try:
                    os.unlink(local)
                except OSError:
                    pass

            def edit() -> None:
                if len(data) > MAX_EDIT_BYTES or not self._is_probably_text(data):
                    messagebox.showwarning("Edit", "Download and edit locally.")
                    return
                dlg = TextEditorDialog(
                    self,
                    f"Edit — {names[0]}",
                    data.decode("utf-8", errors="replace"),
                    self.colors,
                    readonly=False,
                )
                if dlg.result is None:
                    return

                def upload() -> None:
                    with tempfile.NamedTemporaryFile(
                        delete=False, suffix="_" + names[0], mode="w", encoding="utf-8", newline="\n"
                    ) as tmp:
                        tmp.write(dlg.result or "")
                        up_path = tmp.name
                    try:
                        c.put(up_path, remote)
                    finally:
                        try:
                            os.unlink(up_path)
                        except OSError:
                            pass

                self.run_bg(upload, done=lambda: self.log_msg(f"Saved remote {remote}"))

            self.after(0, edit)

        self.run_bg(work)

    def remote_mkdir(self) -> None:
        c = self.require_client()
        if not c:
            return
        name = simpledialog.askstring("New folder", "Folder name:")
        if not name:
            return
        path = join_remote(self.remote_path.get(), name)
        self.run_bg(lambda: c.mkdir(path), done=self.refresh_remote)

    def remote_touch(self) -> None:
        c = self.require_client()
        if not c:
            return
        name = simpledialog.askstring("New file", "File name:")
        if not name:
            return
        path = join_remote(self.remote_path.get(), name)
        self.run_bg(lambda: c.touch(path), done=self.refresh_remote)

    def remote_delete(self) -> None:
        c = self.require_client()
        if not c:
            return
        names = self.selected_remote_names()
        if not names or not messagebox.askyesno("Delete", f"Delete {len(names)} remote item(s)?"):
            return

        def work() -> None:
            for name in names:
                c.delete(join_remote(self.remote_path.get(), name))

        self.run_bg(work, done=self.refresh_remote)

    def remote_rename(self) -> None:
        c = self.require_client()
        if not c:
            return
        names = self.selected_remote_names()
        if len(names) != 1:
            messagebox.showinfo("Rename", "Select a single item.")
            return
        new_name = simpledialog.askstring("Rename", "New name:", initialvalue=names[0])
        if not new_name:
            return
        src = join_remote(self.remote_path.get(), names[0])
        dst = join_remote(self.remote_path.get(), new_name)
        self.run_bg(lambda: c.rename(src, dst), done=self.refresh_remote)

    def remote_move(self) -> None:
        c = self.require_client()
        if not c:
            return
        names = self.selected_remote_names()
        if len(names) != 1:
            messagebox.showinfo("Move", "Select a single item.")
            return
        dest = simpledialog.askstring(
            "Move",
            "Destination path:",
            initialvalue=join_remote(self.remote_path.get(), names[0]),
        )
        if not dest:
            return
        src = join_remote(self.remote_path.get(), names[0])
        self.run_bg(lambda: c.move(src, dest), done=self.refresh_remote)

    def remote_format(self) -> None:
        c = self.require_client()
        if not c:
            return
        colors = self.colors
        dlg = tk.Toplevel(self)
        dlg.title("Format SD card")
        dlg.transient(self)
        dlg.grab_set()
        dlg.geometry("460x340")
        dlg.configure(bg=colors["app_bg"])

        tk.Label(
            dlg,
            text="Format SD",
            bg=colors["app_bg"],
            fg=colors["text"],
            font=(FONT_UI[0], 14, "bold"),
        ).pack(anchor=tk.W, padx=18, pady=(16, 4))
        tk.Label(
            dlg,
            text="Erases all data. Creates MBR + FAT32 for PC + dongle use.\n"
            "If a previous format broke PC detection, recover the card\n"
            "with Windows FAT32 / SD Formatter first.\n"
            "NTFS is not supported by the ESP32.",
            justify=tk.LEFT,
            bg=colors["app_bg"],
            fg=colors["text_muted"],
            font=(FONT_UI[0], 9),
        ).pack(anchor=tk.W, padx=18, pady=(0, 12))

        fs_var = tk.StringVar(value="fat32")
        for label, val in [("FAT32 (supported)", "fat32"), ("NTFS (rejected by device)", "ntfs")]:
            tk.Radiobutton(
                dlg,
                text=label,
                variable=fs_var,
                value=val,
                bg=colors["app_bg"],
                fg=colors["text"],
                selectcolor=colors["elevated"],
                activebackground=colors["app_bg"],
                activeforeground=colors["text"],
                font=(FONT_UI[0], 9),
            ).pack(anchor=tk.W, padx=28)

        tk.Label(
            dlg,
            text='Type yes to confirm',
            bg=colors["app_bg"],
            fg=colors["text_dim"],
            font=(FONT_UI[0], 8, "bold"),
        ).pack(anchor=tk.W, padx=18, pady=(14, 4))
        confirm_var = tk.StringVar()
        tk.Entry(
            dlg,
            textvariable=confirm_var,
            bg=colors["input_bg"],
            fg=colors["input_fg"],
            insertbackground=colors["accent"],
            relief=tk.FLAT,
            highlightthickness=1,
            highlightbackground=colors["border"],
            highlightcolor=colors["accent"],
            font=(FONT_MONO[0], 10),
        ).pack(fill=tk.X, padx=18, ipady=6)

        def do_format() -> None:
            if confirm_var.get().strip() != "yes":
                messagebox.showerror("Format", 'Confirmation must be exactly: yes', parent=dlg)
                return
            filesystem = fs_var.get()
            dlg.destroy()

            def work() -> None:
                result = c.format_storage(filesystem, confirm="yes")
                self.after(0, lambda: self.log_msg(f"Format result: {result}"))

            self.run_bg(work, done=self.refresh_remote)

        btns = tk.Frame(dlg, bg=colors["app_bg"])
        btns.pack(fill=tk.X, padx=18, pady=18)
        tk.Button(
            btns,
            text="Format",
            command=do_format,
            bg=colors["danger"],
            fg="#fff",
            relief=tk.FLAT,
            padx=16,
            pady=6,
            cursor="hand2",
        ).pack(side=tk.LEFT)
        tk.Button(
            btns,
            text="Cancel",
            command=dlg.destroy,
            bg=colors["btn_bg"],
            fg=colors["btn_fg"],
            relief=tk.FLAT,
            padx=16,
            pady=6,
            cursor="hand2",
        ).pack(side=tk.LEFT, padx=8)

    def remote_properties(self) -> None:
        c = self.require_client()
        if not c:
            return
        names = self.selected_remote_names()
        if len(names) != 1:
            messagebox.showinfo("Properties", "Select a single item.")
            return
        path = join_remote(self.remote_path.get(), names[0])

        def work() -> None:
            meta = c.stat(path)
            try:
                digest = c.hash(path) if meta.get("type") == "file" else None
            except RndisStorageError:
                digest = None

            def show() -> None:
                lines = [f"{k}: {v}" for k, v in meta.items()]
                lines.append("permissions: N/A")
                if digest:
                    lines.append(f"crc32: {digest.get('crc32')}")
                messagebox.showinfo("Properties", "\n".join(lines))

            self.after(0, show)

        self.run_bg(work)

    # --- transfers ---

    def upload_selected(self) -> None:
        c = self.require_client()
        if not c:
            return
        paths = [p for p in self.selected_local_paths() if p.is_file()]
        if not paths:
            messagebox.showinfo("Upload", "Select one or more local files.")
            return
        remote_dir = self.remote_path.get() or "/"

        def progress(done: int, total: int) -> None:
            pct = 0 if total <= 0 else int(done * 100 / total)
            self.after(0, lambda: self.progress.configure(value=pct))

        def work() -> None:
            for p in paths:
                remote = join_remote(remote_dir, p.name)
                self.after(0, lambda n=p.name: self.status_var.set(f"Uploading {n}…"))
                c.put(str(p), remote, progress=progress)
                self.after(0, lambda n=p.name: self.log_msg(f"Uploaded {n}"))

        self.run_bg(work, done=self.refresh_remote)

    def download_selected(self) -> None:
        c = self.require_client()
        if not c:
            return
        files = [n for n in self.selected_remote_names() if self._remote_kind(n) == "file"]
        if not files:
            messagebox.showinfo("Download", "Select one or more remote files.")
            return
        local_dir = Path(self.local_path.get())

        def progress(done: int, total: int) -> None:
            pct = 0 if total <= 0 else int(done * 100 / total)
            self.after(0, lambda: self.progress.configure(value=pct))

        def work() -> None:
            for name in files:
                remote = join_remote(self.remote_path.get(), name)
                local = local_dir / name
                self.after(0, lambda n=name: self.status_var.set(f"Downloading {n}…"))
                c.get(remote, str(local), progress=progress)
                self.after(0, lambda n=name: self.log_msg(f"Downloaded {n}"))

        self.run_bg(work, done=self.refresh_local)

    def on_close(self) -> None:
        self.cfg["dark_mode"] = bool(self.dark_mode.get())
        self.cfg["base_url"] = normalize_base_url(self.base_url_var.get())
        save_config(self.cfg)
        if self.client:
            try:
                self.client.close()
            except Exception:  # noqa: BLE001
                pass
        self.destroy()


def main() -> None:
    app = RndisStorageApp()
    app.mainloop()


if __name__ == "__main__":
    main()
