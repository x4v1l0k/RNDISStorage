# WebDAV / HTTP — Plug&Play

Primary host interface: **native WebDAV** (Windows Explorer, macOS Finder, Cyberduck, `davfs2`) and the **on-device web UI**. Nothing to install on the host.

Base URL (USB NCM): `http://203.0.113.1`  
SoftAP: `http://192.168.4.1`

## Plug&Play

### Browser

Open `http://203.0.113.1/` — onboard file explorer:

- Upload files / **Upload folder** (recursive)
- Drag-and-drop files or folders
- Download files or whole folders

### Windows Explorer

1. Plug the dongle (USB NCM) → host gets ~`203.0.113.2`
2. Map network location / open:

```text
http://203.0.113.1/dav/
```

Or in Explorer address bar / “Add a network location”.

3. Copy folders recursively with drag-and-drop — Explorer issues `MKCOL` + `PUT` (and `LOCK`/`UNLOCK`); the dongle handles that server-side.

### SoftAP

Join Wi‑Fi `RNDISStorage` / `rndis-lab`, then use `http://192.168.4.1/` or `http://192.168.4.1/dav/`.

---

## WebDAV root

Mount: `http://203.0.113.1/dav/`

| Method | Support |
| --- | --- |
| OPTIONS | Yes (`DAV: 1,2`, `MS-Author-Via: DAV`) |
| PROPFIND | Yes (Depth 0/1; infinity capped to 1 for RAM safety) |
| GET / HEAD | Yes (files) |
| PUT | Yes (32 KiB buffered I/O) |
| DELETE | Yes (recursive directory trees) |
| MKCOL | Yes |
| MOVE | Yes (rename/move) |
| COPY | Best-effort (files) |
| LOCK / UNLOCK | Fake tokens (required by Windows WebClient) |
| PROPPATCH | Acknowledged (no persistent dead props) |

Paths are sanitized (no `..`, no `\`). Max path length: 200.

Throughput (firmware 2.5): 32 KiB SD I/O chunks, TCP window/sendbuf 16 KiB, USB RX buffer pool — tuned for **speed without sacrificing reliability** on ESP32-S3 + USB NCM.

---

## REST (web UI backend)

Used only by the embedded browser UI at `/`.

| Method | Path | Body / params | Description |
| --- | --- | --- | --- |
| GET | `/api/info` | | Device + storage + transport |
| GET | `/api/list` | `?path=/` | Directory listing |
| GET | `/api/stat` | `?path=/file` | Exists / size (resume helpers) |
| GET | `/api/download` | `?path=/file` (+ optional `Range`) | Download file (206 partial) |
| PUT | `/api/upload` | `?path=/file&offset=N` + raw body | Upload / resume append |
| POST | `/api/mkdir` | `{"path":"/dir"}` | Create directory |
| POST | `/api/touch` | `{"path":"/file"}` | Create empty file |
| POST | `/api/delete` | `{"path":"/…"}` | Delete file/dir (recursive) |
| POST | `/api/rename` | `{"path":"…","new_path":"…"}` | Rename/move |
| POST | `/api/format` | `{"filesystem":"fat32","confirm":"yes"}` | Destructive format |
