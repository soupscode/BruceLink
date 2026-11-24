# Read the Flipper's firmware identity over its USB CLI. READ-ONLY.
#
# Sends only read commands to the Flipper CLI (CDC serial): `device_info`,
# then `storage list` of the BruceLink app and data folders. It writes
# nothing, starts no app, and does not reset the Flipper. Close qFlipper and
# any serial monitor first (the port can only be opened once).
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File scripts/hil/flipper_info.ps1 [-Port COM12] [-All]
#
# Without -All, only the firmware/API/target lines of device_info are printed
# (the full output includes the device name, UID and BLE MAC).
# The FAP needs firmware_api 87.1 and Momentum mntm-012 (docs/HARDWARE_TESTING.md, section 1).

param(
    [string]$Port,
    [switch]$All
)
$ErrorActionPreference = 'Stop'

if (-not $Port) {
    $dev = Get-CimInstance Win32_PnPEntity |
        Where-Object { $_.PNPDeviceID -match '^USB\\VID_0483&PID_5740' -and $_.Name -match '\((COM\d+)\)' } |
        Select-Object -First 1
    if (-not $dev) { Write-Output 'Flipper (USB 0483:5740) not found.'; exit 1 }
    $null = $dev.Name -match '\((COM\d+)\)'; $Port = $Matches[1]
}

function Invoke-FlipperCli([System.IO.Ports.SerialPort]$sp, [string]$cmd, [int]$waitMs = 1500) {
    $null = $sp.ReadExisting()
    $sp.Write("$cmd`r")
    $deadline = (Get-Date).AddMilliseconds($waitMs)
    $buf = ''
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Milliseconds 100
        $buf += $sp.ReadExisting()
        if ($buf -match '>: $') { break }   # CLI prompt: reply complete
    }
    return $buf
}

$sp = New-Object System.IO.Ports.SerialPort $Port, 230400
$sp.DtrEnable = $true      # the Flipper starts its CLI session on DTR
$sp.ReadTimeout = 1000; $sp.WriteTimeout = 1000; $sp.NewLine = "`r`n"
try {
    $sp.Open()
    Start-Sleep -Milliseconds 600
    $null = $sp.ReadExisting()      # banner
    $sp.Write("`r"); Start-Sleep -Milliseconds 300; $null = $sp.ReadExisting()

    Write-Output "== Flipper on ${Port}: device_info =="
    $info = Invoke-FlipperCli $sp 'device_info' 2500
    $lines = $info -split "`r?`n" | Where-Object { $_ -match ':' -and $_ -notmatch '^>:' }
    if (-not $All) {
        $lines = $lines | Where-Object {
            $_ -match '^(firmware_(version|origin_fork|origin_git|api_major|api_minor|target|branch|commit|build_date)|hardware_(target|model|ver|region)|radio_stack_(major|minor|type|sub|branch|release)|protobuf_version)\s*:'
        }
    }
    $lines | ForEach-Object { Write-Output "  $_" }

    foreach ($dir in '/ext/apps/Bluetooth', '/ext/apps_data/brucelink') {
        Write-Output "== storage list $dir =="
        $out = Invoke-FlipperCli $sp "storage list $dir" 2000
        $out -split "`r?`n" | Where-Object { $_ -match '^\s*\[[FD]\]' -or $_ -match 'Storage error|not exist' } |
            ForEach-Object { Write-Output "  $($_.Trim())" }
    }
} catch {
    Write-Output "Could not query the Flipper on ${Port}: $($_.Exception.Message)"
    Write-Output '(Is qFlipper or a serial monitor holding the port?)'
    exit 1
} finally {
    if ($sp.IsOpen) { $sp.Close() }
}
exit 0
