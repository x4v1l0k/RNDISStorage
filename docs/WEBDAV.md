# WebDAV / HTTP API

Base URL (lab SoftAP): `http://203.0.113.1`

## Browser UI

| Method | Path | Description |
| --- | --- | --- |
| GET | `/` | Embedded file-explorer panel |

## REST (used by CLI/GUI)

| Method | Path | Body / params | Description |
| --- | --- | --- | --- |
| GET | `/api/info` | | Device + storage + transport |
| GET | `/api/list` | `?path=/` | Directory listing |
| GET | `/api/download` | `?path=/file` | Download file |
| PUT | `/api/upload` | `?path=/file` + raw body | Upload file |
| POST | `/api/mkdir` | `{"path":"/dir"}` | Create directory |
| POST | `/api/touch` | `{"path":"/file"}` | Create empty file |
| POST | `/api/delete` | `{"path":"/…"}` | Delete file/dir |
| POST | `/api/rename` | `{"path":"…","new_path":"…"}` | Rename/move |
| POST | `/api/format` | `{"filesystem":"fat32","confirm":"yes"}` | Destructive format |

Paths are sanitized (no `..`, no `\`, no drive letters). Max path length: 200.

## WebDAV root

Mount / probe: `http://203.0.113.1/dav/`

| Method | Support |
| --- | --- |
| OPTIONS | Yes (`DAV: 1, 2`) |
| PROPFIND | Yes (Depth 0/1) |
| GET / HEAD | Yes (files) |
| PUT | Yes |
| DELETE | Yes |
| MKCOL | Best-effort via not-found handler |
| MOVE / COPY / LOCK | Not fully implemented |

Example (Windows, after SoftAP join):

```powershell
# Explorer "Add a network location" or:
python client\client.py --url http://203.0.113.1 propfind /
```
