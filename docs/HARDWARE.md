# Hardware — LILYGO TTGO T-Dongle-S3

Fuentes: wiki oficial LilyGO, repositorio [Xinyuan-LilyGO/T-Dongle-S3](https://github.com/Xinyuan-LilyGO/T-Dongle-S3).

## Microcontrolador

- **SoC:** ESP32-S3
- **Flash:** 16 MB Quad-SPI (QIO 80 MHz)
- **SRAM:** 512 KB
- **PSRAM:** no disponible (debe permanecer deshabilitado)
- **Radio:** Wi-Fi 802.11 b/g/n (usado como SoftAP en la build lab) + Bluetooth 5 LE (no usado)

## Revisiones

Existen variantes (T-Dongle-S3, Dual, Plus). Esta PoC apunta a **T-Dongle-S3** clásico (pantalla + TF + APA102).

## USB

- Conector: **USB Type-A macho** (formato dongle)
- Controlador USB: **PHY USB nativo del ESP32-S3**
- **USB Mass Storage:** no implementado / no habilitado
- CDC on boot: habilitado para flasheo/monitor (`ARDUINO_USB_MODE=1`)
- Canal de ficheros: **HTTP/WebDAV** en `203.0.113.1` (SoftAP lab; gadget RNDIS USB = siguiente transporte)

## SoftAP lab

| Parámetro | Valor |
| --- | --- |
| SSID | `RNDISStorage` |
| Password | `rndis-lab` |
| IP | `203.0.113.1` |
| HTTP | puerto 80 |
| WebDAV | `/dav/` |

## GPIO relevantes

| Función | GPIO |
| --- | --- |
| APA102 DIN | 40 |
| APA102 CLK | 39 |
| Botón BOOT | 0 |
| TFT CS | 4 |
| TFT MOSI | 3 |
| TFT SCLK | 5 |
| TFT DC | 2 |
| TFT RST | 1 |
| TFT backlight | 38 (activo-LOW) |
| SDMMC D0 | 14 |
| SDMMC D1 | 17 |
| SDMMC D2 | 21 |
| SDMMC D3 | 18 |
| SDMMC CLK | 12 |
| SDMMC CMD | 16 |

## Almacenamiento

- microSD vía **SDMMC**, FAT32
- No se usa LittleFS de flash como FS primario
- Format on-device: FatFs `f_mkfs` FAT32 (MBR)

## Pantalla

- ST7735 160×80 landscape, texto `RNDISStorage` + estado + capacidad
