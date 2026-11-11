# SoftAP → host pivot (lab)

Alcance autorizado: desde un cliente Wi‑Fi del SoftAP del dongle, llegar a servicios del **host** donde está enchufado el USB (p. ej. SSH / HTTP), sin estar previamente en la LAN corporativa del host.

## Topología

```
Operador ──Wi‑Fi──► SoftAP 192.168.4.1 (ESP)
                       │
          ┌────────────┼────────────┐
          ▼                         ▼
   SOCKS5 :1080              NAPT (best-effort)
   (recomendado)             (transparente)
          │                         │
          └────────────┬────────────┘
                       ▼
                USB NCM 203.0.113.1 (ESP)
                       │
                       ▼
                Host 203.0.113.2 (DHCP)
                       │
              sshd / http.server / …
                       │
                       ▼
              (ya en el host → su LAN / servicios locales)
```

| Camino | Cómo | Fiabilidad |
| --- | --- | --- |
| **SOCKS5** `192.168.4.1:1080` | El cliente habla con el AP (on-link); el ESP abre la conexión al host por USB | **Recomendado** |
| **NAPT** transparente | El cliente enruta `203.0.113.0/24` vía `192.168.4.1` | Best-effort (falla si el móvil/PC no usa el AP como gateway por defecto) |

## Pasos de operador (recomendado)

1. Flashea firmware **≥ 2.4**, enchufa el dongle al host y espera USB Ethernet + SoftAP.
2. Conéctate al Wi‑Fi `RNDISStorage` / `rndis-lab`. Tras flashear, **reconecta** el Wi‑Fi para renovar DHCP.
3. Comprueba API:

```bash
curl http://192.168.4.1/api/info
# espera: "version":"2.4" y socks.up=true

curl "http://192.168.4.1/api/pivot/probe?port=1234"
# reachable:true ⇒ el ESP llega al host por USB
```

4. Acceso a servicios del host **vía SOCKS**:

### curl (HTTP)

En el host (lab):

```bash
python -m http.server 1234 --bind 0.0.0.0
```

Desde el cliente SoftAP:

```bash
curl --socks5 192.168.4.1:1080 http://203.0.113.2:1234/
```

Equivalente con URL de proxy:

```bash
curl -x socks5h://192.168.4.1:1080 http://203.0.113.2:1234/
```

### SSH (OpenSSH)

Linux / macOS / Git Bash (con `nc` que soporte `-x`):

```bash
ssh -o ProxyCommand="nc -x 192.168.4.1:1080 %h %p" usuario@203.0.113.2
```

Con `ncat` (Nmap), útil también en Windows:

```bash
ssh -o ProxyCommand="ncat --proxy-type socks5 --proxy 192.168.4.1:1080 %h %p" usuario@203.0.113.2
```

ProxyJump no aplica aquí (el salto no es otro SSH, es SOCKS en el dongle).

5. Con la sesión SSH (o el tráfico HTTP vía SOCKS) operas **como el host**.

## NAPT transparente (opcional)

Solo si el cliente SoftAP tiene gateway por defecto `192.168.4.1`:

```bash
ping 203.0.113.1
ping 203.0.113.2
curl http://203.0.113.2:1234/
ssh usuario@203.0.113.2
```

Si `192.168.4.1` responde pero `203.0.113.2` no, usa **SOCKS** (arriba). En móviles, desactiva datos o usa un portátil.

## Requisitos en el host

- Servicio escuchando en la NIC USB o en `0.0.0.0`
- Firewall permitiendo ese puerto en la interfaz USB
- IP USB ≈ `203.0.113.2`

```powershell
Get-NetIPAddress -AddressFamily IPv4 | Where-Object IPAddress -like '203.0.113.*'

New-NetFirewallRule -DisplayName "RNDIS-Lab-1234" -Direction Inbound -Protocol TCP `
  -LocalPort 1234 -Action Allow -Profile Any
```

Tras flashear ≥ 2.4 el DHCP USB **no** debe instalar una ruta por defecto `0.0.0.0/0` vía el dongle. Si queda una antigua:

```powershell
# Revisar rutas por la NIC USB (nombre típico: RNDISLab / USB Ethernet)
Get-NetRoute -AddressFamily IPv4 | Where-Object NextHop -eq '203.0.113.1'
```

## Qué no es esto

- El SoftAP **no** es la LAN corporativa del host.
- Sin sesión en el endpoint (SSH u otro servicio), no hay pivot automático al resto de la LAN.
