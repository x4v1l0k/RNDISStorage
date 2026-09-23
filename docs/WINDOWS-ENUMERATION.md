# Expected Windows enumeration (RNDISStorage)

## SoftAP lab build (default)

| Class / view | Expected for dongle |
| --- | --- |
| DiskDrive / Removable Disk | **Absent** (no MSC) |
| Volume / drive letter | **Absent** |
| USB Mass Storage | **Absent** |
| Wi‑Fi SoftAP `RNDISStorage` | **Present** after plug-in |
| `http://203.0.113.1/api/info` | **200 OK** after SoftAP join |

## If using a USB RNDIS gadget build

| Class / view | Expected |
| --- | --- |
| Net adapter (RNDIS / USB Ethernet) | **Present** |
| DHCP client address near `203.0.113.2` | **Present** |
| DiskDrive for dongle | **Absent** |

## Helper

```powershell
.\scripts\Enumerate-RndisStorageDevice.ps1
```
