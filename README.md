# RNDISStorage

**Proof of Concept** — filesystem over **HTTP / WebDAV** on a LILYGO TTGO T-Dongle-S3 (ESP32-S3), reached through a **network path** (SoftAP lab transport) instead of USB Mass Storage.

Lab tool to measure whether a Windows GPO that blocks **USB Mass Storage** still allows file transfer when the device exposes storage as a **WebDAV / web file explorer** on a local IP (`203.0.113.1`, RFC 5737 TEST-NET-3 — outside 10/172/192 LAN ranges), without enumerating a disk volume.

> **Scope:** authorized lab / Red Team control validation.  
> **Out of scope:** GPO/registry/EDR bypass, VID/PID spoofing, USB MSC, driver hiding.

---

## Why this exists

| Control surface | Typical USB stick | This PoC |
| --- | --- | --- |
| USB Mass Storage (MSC) | Yes | **No** |
| Disk Drive / Volume letter | Yes | **No** |
| Network path to files | No | **Yes** (HTTP/WebDAV) |
| File I/O path | Windows filesystem | WebDAV / REST → microSD |

The dongle does **not** present a removable disk. Storage lives on an inserted **microSD (FAT32)** and is served by an on-device HTTP stack with:

1. **Web UI** — file-explorer panel at `http://203.0.113.1/`
2. **WebDAV** — `http://203.0.113.1/dav/` (PROPFIND / GET / PUT / DELETE / MKCOL)
3. **REST API** — `/api/*` used by the Python GUI/CLI

### Transport note (current build)

The default PlatformIO env brings up:

1. **USB NCM** gadget (Windows: USB Ethernet / UsbNcm) → host DHCP near `203.0.113.2` → UI at `http://203.0.113.1/`
2. **Wi‑Fi SoftAP** `RNDISStorage` / `rndis-lab` → UI at `http://192.168.4.1/`
3. **Host pivot** from SoftAP → USB host (`203.0.113.2`) via on-dongle **SOCKS5 `:1080`** (recommended) or best-effort NAPT

Same HTTP / WebDAV / REST UI on USB and SoftAP. **No USB Mass Storage.** Full pivot cookbook: [docs/PIVOT.md](docs/PIVOT.md).

---

## Architecture

```
  Windows (USB NCM) ── HTTP ──────► 203.0.113.1 ─┐
                                                  ├─► ESP32-S3 ── SDMMC ── microSD
  Phone/laptop (Wi-Fi) ─ HTTP ────► 192.168.4.1 ─┤
                         └─ SOCKS5 :1080 ─────────┘──► host 203.0.113.2 (SSH/HTTP/…)
```

- **USB class:** Network (NCM) — appears as Ethernet adapter
- **SoftAP:** parallel control path (same Web UI / WebDAV)
- **SOCKS5:** reliable SoftAP→host pivot on `192.168.4.1:1080`
- **No USB MSC**
- **Storage:** SD card via SDMMC
- **Host:** browser explorer, Python CLI, dual-pane tkinter GUI

---

## Repository layout

```
firmware/     PlatformIO firmware (ESP32-S3, SoftAP, WebDAV, SD, display)
client/       Python library, CLI, and tkinter GUI (HTTP/WebDAV)
docs/         Hardware, WebDAV API, Windows enumeration, GPO tests
scripts/      PnP inventory helper + www embed helper
```

---

## Requirements

| Side | Need |
| --- | --- |
| Hardware | [LILYGO TTGO T-Dongle-S3](https://github.com/Xinyuan-LilyGO/T-Dongle-S3), microSD (FAT/FAT32), USB Type-A host |
| Firmware | [PlatformIO](https://platformio.org/) + ESP32 platform |
| Host | Windows 10/11, Python 3.10+, `requests`; Wi‑Fi to join SoftAP (lab build) |

---

## Quick start

### 1. Flash firmware

```bash
cd firmware
pio run -t upload
```

Insert a microSD before use. Without a card the display shows `Failure` / `NO-SD`.

### 2. Plug USB — Ethernet + SoftAP

1. Flash, then re-plug the dongle.
2. In Windows: **Settings → Network** or `Get-NetAdapter` — look for **USB Ethernet** / **UsbNcm** / similar.
3. Host should get an IP like `203.0.113.2` via DHCP.
4. Open [http://203.0.113.1/](http://203.0.113.1/) (USB) **or** join Wi-Fi `RNDISStorage` / `rndis-lab` and open [http://192.168.4.1/](http://192.168.4.1/).
5. SoftAP → host services (lab), prefer SOCKS:

```bash
curl --socks5 192.168.4.1:1080 http://203.0.113.2:1234/
ssh -o ProxyCommand="nc -x 192.168.4.1:1080 %h %p" user@203.0.113.2
```

See [docs/PIVOT.md](docs/PIVOT.md) for probe API, `ncat` on Windows, and firewall notes.

If flash fails because the port disappeared: hold **BOOT**, re-plug, release **BOOT**, then `pio run -t upload`.

### 3. Install client

```powershell
cd client
python -m pip install -r requirements.txt
```

### 4. CLI examples

```powershell
python client.py discover
python client.py --url http://203.0.113.1 info
python client.py --url http://203.0.113.1 ls /
python client.py --url http://203.0.113.1 put .\test.txt /test.txt
python client.py --url http://203.0.113.1 get /test.txt .\out.txt
python client.py --url http://203.0.113.1 propfind /
python client.py --url http://203.0.113.1 format --filesystem fat32 --confirm yes
```

### 5. GUI

```powershell
python gui.py
```

- Left pane: local filesystem  
- Right pane: remote SD over WebDAV/HTTP  
- URL defaults to `http://203.0.113.1`  
- Dark theme, dual explorer, transfer / rename / mkdir / format

---

## Expected Windows enumeration

| Interface | Expected (SoftAP lab build) |
| --- | --- |
| Disk Drive / Removable Disk for the dongle | **Absent** |
| Volume / drive letter for the dongle | **Absent** |
| USB Mass Storage class for the dongle | **Absent** |
| Reachability of `http://203.0.113.1` after SoftAP join | **Present** |

Read-only inventory:

```powershell
.\scripts\Enumerate-RndisStorageDevice.ps1
```

More detail:

- [docs/PIVOT.md](docs/PIVOT.md) — SoftAP → host (SOCKS5 / curl / SSH)  
- [docs/CHECKLIST-STORAGE.md](docs/CHECKLIST-STORAGE.md)  
- [docs/WINDOWS-ENUMERATION.md](docs/WINDOWS-ENUMERATION.md)  
- [docs/GPO-TESTING.md](docs/GPO-TESTING.md)  
- [docs/WEBDAV.md](docs/WEBDAV.md)  
- [docs/HARDWARE.md](docs/HARDWARE.md)  

---

## Security & authorization

This project is for **authorized** security assessments and controlled labs (e.g. measuring Removable Storage / Device Installation / DLP gaps around non-MSC file paths).

It does **not**:

- Alter Group Policy, registry, Defender, EDR, WDAC, or AppLocker  
- Spoof USB identity or hide the device  
- Implement USB Mass Storage  

Use only on systems and engagements where you are permitted to connect test hardware and transfer files.

---

## License / use

Internal / engagement lab use unless otherwise stated by the repository owner. Do not use to circumvent security policy outside an authorized scope.

## Contact

Blog: https://xavilok.es
