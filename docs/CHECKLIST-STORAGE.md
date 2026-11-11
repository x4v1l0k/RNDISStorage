# Storage checklist (RNDISStorage)

Pass/fail for authorized lab runs.

| # | Check | Pass? |
| --- | --- | --- |
| 1 | microSD inserted, FAT/FAT32 | |
| 2 | Firmware flashed (`pio run -t upload`) | |
| 3 | Display shows Ready (or Failure/NO-SD if no card) | |
| 4 | Host does **not** get a new volume letter from the dongle | |
| 5 | SoftAP `RNDISStorage` visible (lab build) | |
| 6 | Browser opens `http://203.0.113.1/` explorer | |
| 7 | `/api/info` responds in browser (or Network tab) | |
| 8 | Upload + download round-trip OK (files and folders) | |
| 9 | WebDAV mount `http://203.0.113.1/dav/` works in Explorer | |
| 10 | Conventional USB stick still blocked by GPO (Test A) | |

See [GPO-TESTING.md](GPO-TESTING.md).
