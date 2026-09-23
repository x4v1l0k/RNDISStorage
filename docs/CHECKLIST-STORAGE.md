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
| 7 | `python client.py --url http://203.0.113.1 info` OK | |
| 8 | Upload + download round-trip OK | |
| 9 | WebDAV `propfind /` returns 207 multistatus | |
| 10 | Conventional USB stick still blocked by GPO (Test A) | |

See [GPO-TESTING.md](GPO-TESTING.md).
