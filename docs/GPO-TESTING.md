# Pruebas GPO / políticas USB (entorno controlado)

Documento para **medir** el efecto de políticas corporativas sobre USB Mass Storage frente a un canal **HTTP/WebDAV** sin clase Mass Storage.

**Prohibido en esta PoC:** modificar GPO del dominio, registro para desactivar políticas, Defender/EDR, Device Installation Restrictions bypass, spoofing VID/PID, WDAC/AppLocker bypass, ocultación del dispositivo.

## Antes de empezar

1. Documenta el host: edición Windows, join a dominio, usuario de prueba.
2. Captura políticas relevantes (solo lectura), por ejemplo:

```powershell
gpresult /h gpresult.html
rsop.msc
```

3. Identifica restricciones del estilo:
   - Removable Storage Access (deny execute/read/write para Removable Disks)
   - Device Installation Restrictions
   - All Removable Storage classes: Deny all access
   - Controles DLP/EDR sobre USB storage vs red local / WebDAV

4. Conecta el dongle y completa [CHECKLIST-STORAGE.md](CHECKLIST-STORAGE.md) y [WINDOWS-ENUMERATION.md](WINDOWS-ENUMERATION.md).

---

## Test A — USB Mass Storage (control positivo)

**Dispositivo:** pendrive USB convencional (clase Mass Storage).

### Pasos

1. Conectar el pendrive.
2. Registrar enumeración (`Get-PnpDevice -Class DiskDrive`, `Get-Volume`).
3. Intentar lectura/escritura desde el Explorador.
4. Anotar bloqueos.

### Registro

| Pregunta | Resultado |
| --- | --- |
| ¿Se enumera como Disk Drive? | YES / NO |
| ¿Aparece volumen / letra? | YES / NO |
| ¿Se puede listar / escribir? | YES / NO |
| Evidencia | |

**Interpretación:** si el Mass Storage está bloqueado, el Test A debe fallar. Valida que el control “anti-pendrive” está activo.

---

## Test B — RNDISStorage WebDAV (dispositivo PoC)

**Dispositivo:** T-Dongle-S3 con firmware RNDISStorage (sin MSC).

### Pasos

1. Conectar el dongle (alimentación USB).
2. Verificar que **no** aparece como disco.
3. Unirse al SoftAP `RNDISStorage` / `rndis-lab` (build lab) **o** esperar IP RNDIS `203.0.113.x` si usas gadget USB-net.
4. Abrir `http://203.0.113.1/` (explorador web embebido) y/o mapear WebDAV `http://203.0.113.1/dav/` en el Explorador de Windows; subir/bajar ficheros y carpetas (recursivo).
5. Registrar si EDR/DLP genera alertas por WebDAV / HTTP local.

### Registro

| Pregunta | Resultado |
| --- | --- |
| ¿Aparece Disk Drive del dongle? | YES / NO (esperado NO) |
| ¿HTTP `/api/info` responde? | YES / NO |
| ¿Web UI lista la SD? | YES / NO |
| ¿Upload/download OK? | YES / NO |
| Alertas DLP/EDR | |

**Interpretación:** si A falla y B funciona, hay un **gap de control** entre políticas centradas en Mass Storage y canales de ficheros sobre red local (SoftAP / futuro RNDIS USB).

---

## Evidencia sugerida

- Capturas de `Enumerate-RndisStorageDevice.ps1`
- `gpresult` / RSOP (solo lectura)
- Capturas del explorador web / WebDAV en Explorer
- Nota de alcance / autorización del engagement
