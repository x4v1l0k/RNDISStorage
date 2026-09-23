# Enumerate RNDISStorage / non-MSC USB evidence on Windows.
# Read-only. Does not change policies or drivers.

param(
    [string]$VidMatch = "303A",
    [string]$ProbeUrl = "http://203.0.113.1/api/info"
)

Write-Host "=== PnP Net (network adapters) ===" -ForegroundColor Cyan
Get-PnpDevice -Class Net -ErrorAction SilentlyContinue |
    Format-Table Status, FriendlyName, InstanceId -AutoSize

Write-Host "=== PnP DiskDrive ===" -ForegroundColor Cyan
Get-PnpDevice -Class DiskDrive -ErrorAction SilentlyContinue |
    Format-Table Status, FriendlyName, InstanceId -AutoSize

Write-Host "=== Disks ===" -ForegroundColor Cyan
Get-Disk -ErrorAction SilentlyContinue |
    Format-Table Number, FriendlyName, Size, BusType, OperationalStatus -AutoSize

Write-Host "=== Volumes ===" -ForegroundColor Cyan
Get-Volume -ErrorAction SilentlyContinue |
    Format-Table DriveLetter, FileSystemLabel, FileSystem, Size, SizeRemaining -AutoSize

Write-Host "=== USB entities matching VID $VidMatch ===" -ForegroundColor Cyan
Get-CimInstance Win32_PnPEntity -ErrorAction SilentlyContinue |
    Where-Object {
        $_.PNPDeviceID -match $VidMatch -or
        $_.Name -match 'RNDIS|Remote NDIS|USB Ethernet|Network' -or
        $_.PNPClass -eq 'Net'
    } |
    Select-Object Name, Status, PNPDeviceID, Manufacturer, Service, PNPClass |
    Format-List

Write-Host "=== HTTP probe $ProbeUrl ===" -ForegroundColor Cyan
try {
    $r = Invoke-WebRequest -Uri $ProbeUrl -UseBasicParsing -TimeoutSec 5
    Write-Host "HTTP $($r.StatusCode)"
    Write-Host $r.Content
} catch {
    Write-Host "Probe failed: $($_.Exception.Message)" -ForegroundColor Yellow
    Write-Host "Join SoftAP SSID RNDISStorage first (lab build), or wait for USB RNDIS DHCP."
}

Write-Host "Done. Expect NO DiskDrive/Volume for the dongle; HTTP/WebDAV should work on 203.0.113.1." -ForegroundColor Green
