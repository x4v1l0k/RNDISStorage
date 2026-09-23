"""RNDISStorage HTTP/WebDAV client library."""

from __future__ import annotations

import logging
import xml.etree.ElementTree as ET
from pathlib import Path
from typing import Any, Callable, Optional
from urllib.parse import quote, urljoin

try:
    import requests
except ImportError as exc:  # pragma: no cover
    raise SystemExit("Missing dependency: pip install -r requirements.txt") from exc

PROTOCOL_VERSION = "2.0"
DEFAULT_BASE_URL = "http://203.0.113.1"
DEFAULT_TIMEOUT = 60.0

LOG = logging.getLogger("rndisstorage")

ProgressCb = Optional[Callable[[int, int], None]]
DAV_NS = {"D": "DAV:"}


class RndisStorageError(RuntimeError):
    pass


def join_remote(base: str, name: str) -> str:
    base = base.rstrip("/") or ""
    if not base:
        return "/" + name.lstrip("/")
    return base + "/" + name.lstrip("/")


def parent_remote(path: str) -> str:
    path = path.rstrip("/") or "/"
    if path == "/":
        return "/"
    parent = path.rsplit("/", 1)[0]
    return parent if parent else "/"


def normalize_base_url(url: str) -> str:
    url = (url or "").strip().rstrip("/")
    if not url:
        return DEFAULT_BASE_URL
    if "://" not in url:
        url = "http://" + url
    return url.rstrip("/")


class RndisStorageClient:
    """Talks to the dongle over HTTP REST + WebDAV (no USB MSC, no COM protocol)."""

    def __init__(self, base_url: str = DEFAULT_BASE_URL, timeout: float = DEFAULT_TIMEOUT) -> None:
        self.base_url = normalize_base_url(base_url)
        self.timeout = timeout
        self._session: Optional[requests.Session] = None
        self._info_cache: dict[str, Any] = {}

    @property
    def connected(self) -> bool:
        return self._session is not None

    def connect(self) -> None:
        LOG.info("Connecting to %s", self.base_url)
        self._session = requests.Session()
        self._session.headers.update({"User-Agent": "RNDISStorage-Client/2.0"})
        info = self.info()
        self._info_cache = info
        LOG.info(
            "Connected: device=%s transport=%s",
            info.get("device"),
            info.get("transport"),
        )

    def close(self) -> None:
        if self._session:
            self._session.close()
            LOG.info("Disconnected")
        self._session = None

    def __enter__(self) -> "RndisStorageClient":
        self.connect()
        return self

    def __exit__(self, *args: Any) -> None:
        self.close()

    def _req(self) -> requests.Session:
        if not self._session:
            raise RndisStorageError("not connected")
        return self._session

    def _url(self, path: str) -> str:
        if path.startswith("http"):
            return path
        return urljoin(self.base_url + "/", path.lstrip("/"))

    def _raise_for_api(self, resp: requests.Response) -> dict[str, Any]:
        try:
            data = resp.json() if resp.content else {}
        except ValueError:
            data = {"message": resp.text[:300]}
        if resp.status_code >= 400:
            raise RndisStorageError(
                f"{data.get('error', 'http_error')}: {data.get('message', resp.reason)} "
                f"(HTTP {resp.status_code})"
            )
        return data if isinstance(data, dict) else {"status": "ok", "data": data}

    def info(self) -> dict[str, Any]:
        resp = self._req().get(self._url("/api/info"), timeout=self.timeout)
        return self._raise_for_api(resp)

    def storage(self) -> dict[str, Any]:
        info = self.info()
        st = info.get("storage") or {}
        return {"status": "ok", **st}

    def ls(self, path: str = "/") -> dict[str, Any]:
        resp = self._req().get(
            self._url("/api/list"),
            params={"path": path},
            timeout=self.timeout,
        )
        data = self._raise_for_api(resp)
        # Normalize for GUI (expects type=dir|file)
        for e in data.get("entries") or []:
            if "type" not in e:
                e["type"] = "dir" if e.get("is_dir") else "file"
        return data

    def stat(self, path: str) -> dict[str, Any]:
        parent = parent_remote(path)
        name = path.rstrip("/").rsplit("/", 1)[-1] if path not in ("", "/") else ""
        listing = self.ls(parent if name else path)
        if not name:
            return {
                "status": "ok",
                "path": path,
                "exists": True,
                "is_dir": True,
                "size": 0,
            }
        for e in listing.get("entries") or []:
            if e.get("name") == name:
                return {
                    "status": "ok",
                    "path": path,
                    "exists": True,
                    "is_dir": bool(e.get("is_dir")),
                    "size": int(e.get("size") or 0),
                }
        return {"status": "ok", "path": path, "exists": False}

    def delete(self, path: str) -> dict[str, Any]:
        resp = self._req().post(
            self._url("/api/delete"),
            json={"path": path},
            timeout=self.timeout,
        )
        return self._raise_for_api(resp)

    def mkdir(self, path: str) -> dict[str, Any]:
        resp = self._req().post(
            self._url("/api/mkdir"),
            json={"path": path},
            timeout=self.timeout,
        )
        return self._raise_for_api(resp)

    def rename(self, path: str, new_path: str) -> dict[str, Any]:
        resp = self._req().post(
            self._url("/api/rename"),
            json={"path": path, "new_path": new_path},
            timeout=self.timeout,
        )
        return self._raise_for_api(resp)

    def move(self, path: str, new_path: str) -> dict[str, Any]:
        return self.rename(path, new_path)

    def touch(self, path: str) -> dict[str, Any]:
        resp = self._req().post(
            self._url("/api/touch"),
            json={"path": path},
            timeout=self.timeout,
        )
        return self._raise_for_api(resp)

    def format_storage(self, filesystem: str, confirm: str = "yes") -> dict[str, Any]:
        resp = self._req().post(
            self._url("/api/format"),
            json={"filesystem": filesystem, "confirm": confirm},
            timeout=max(self.timeout, 180.0),
        )
        return self._raise_for_api(resp)

    def get(
        self,
        remote_path: str,
        local_path: str,
        progress: ProgressCb = None,
    ) -> dict[str, Any]:
        resp = self._req().get(
            self._url("/api/download"),
            params={"path": remote_path},
            timeout=self.timeout,
            stream=True,
        )
        if resp.status_code >= 400:
            self._raise_for_api(resp)
        total = int(resp.headers.get("Content-Length") or 0)
        out = Path(local_path)
        out.parent.mkdir(parents=True, exist_ok=True)
        received = 0
        with out.open("wb") as fh:
            for chunk in resp.iter_content(chunk_size=64 * 1024):
                if not chunk:
                    continue
                fh.write(chunk)
                received += len(chunk)
                if progress:
                    progress(received, total or received)
        return {"status": "ok", "bytes": received, "local_path": str(out)}

    def put(
        self,
        local_path: str,
        remote_path: str,
        progress: ProgressCb = None,
    ) -> dict[str, Any]:
        data_path = Path(local_path)
        if not data_path.is_file():
            raise RndisStorageError(f"local file not found: {local_path}")
        size = data_path.stat().st_size

        def gen() -> Any:
            sent = 0
            with data_path.open("rb") as fh:
                while True:
                    chunk = fh.read(64 * 1024)
                    if not chunk:
                        break
                    sent += len(chunk)
                    if progress:
                        progress(sent, size)
                    yield chunk

        resp = self._req().put(
            self._url("/api/upload"),
            params={"path": remote_path},
            data=gen(),
            headers={"Content-Type": "application/octet-stream", "Content-Length": str(size)},
            timeout=max(self.timeout, 300.0),
        )
        return self._raise_for_api(resp)

    def propfind(self, path: str = "/") -> list[dict[str, Any]]:
        """Raw WebDAV PROPFIND against /dav/... (for interoperability checks)."""
        dav = self.base_url + "/dav"
        if path and path != "/":
            dav += quote(path if path.startswith("/") else "/" + path, safe="/")
        if not dav.endswith("/"):
            dav += "/"
        resp = self._req().request(
            "PROPFIND",
            dav,
            headers={"Depth": "1", "Content-Type": "application/xml"},
            data=b'<?xml version="1.0"?><D:propfind xmlns:D="DAV:"><D:prop><D:resourcetype/>'
            b"<D:getcontentlength/></D:prop></D:propfind>",
            timeout=self.timeout,
        )
        if resp.status_code not in (207, 200):
            raise RndisStorageError(f"PROPFIND failed HTTP {resp.status_code}")
        root = ET.fromstring(resp.content)
        rows: list[dict[str, Any]] = []
        for resp_el in root.findall("D:response", DAV_NS):
            href = resp_el.findtext("D:href", default="", namespaces=DAV_NS)
            length = resp_el.findtext(".//D:getcontentlength", default="0", namespaces=DAV_NS)
            rtype = resp_el.find(".//D:resourcetype", DAV_NS)
            is_dir = rtype is not None and rtype.find("D:collection", DAV_NS) is not None
            rows.append({"href": href, "is_dir": is_dir, "size": int(length or 0)})
        return rows


def discover_default_urls() -> list[str]:
    """Suggested endpoints after Windows assigns DHCP on the USB Ethernet adapter."""
    return [
        DEFAULT_BASE_URL,
        "http://203.0.113.1",
    ]
