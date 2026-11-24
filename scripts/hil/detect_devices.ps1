# Detect the Flipper Zero and the M5StickS3 on Windows. READ-ONLY.
#
# Windows counterpart of detect_devices.sh (which needs udevadm and /dev/tty*).
# It only queries Windows Plug and Play: it never opens a COM port, so it does
# not reset the ESP32-S3 (opening its USB-Serial/JTAG port can toggle DTR/RTS
# and reboot the chip) and does not disturb a running Flipper app.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File scripts/hil/detect_devices.ps1
#
# Output: every USB device with a known VID, its COM port if it has one, and
# the PnP status; Bluetooth radios; and FLIPPER_PORT / STICKS3_PORT lines for
# the other HIL scripts. Exit code 0 always (absence is reported, not an error).
#
# USB IDs (same as common.sh):
#   Flipper Zero CDC                VID 0483 PID 5740
#   ESP32-S3 native USB-Serial/JTAG VID 303A PID 1001 (also in download mode)
#   ESP32-S3 TinyUSB CDC (if Bruce enables it) VID 303A, other PID

$ErrorActionPreference = 'Stop'

$known = @{
    '0483:5740' = 'Flipper Zero (CDC)'
    '303A:1001' = 'ESP32-S3 USB-Serial/JTAG (M5StickS3)'
}
$vendors = @{ '0483' = 'STMicro (Flipper?)'; '303A' = 'Espressif (ESP32-S3?)' }

$rows = @()
$usb = Get-CimInstance Win32_PnPEntity |
    Where-Object { $_.PNPDeviceID -match '^USB\\VID_([0-9A-F]{4})&PID_([0-9A-F]{4})' }
foreach ($d in $usb) {
    $null = $d.PNPDeviceID -match '^USB\\VID_([0-9A-F]{4})&PID_([0-9A-F]{4})'
    $vid = $Matches[1].ToUpper(); $usbPid = $Matches[2].ToUpper()
    if (-not $vendors.ContainsKey($vid)) { continue }
    $com = $null
    if ($d.Name -match '\((COM\d+)\)') { $com = $Matches[1] }
    $label = $known["${vid}:${usbPid}"]
    if (-not $label) { $label = $vendors[$vid] }
    $rows += [pscustomobject]@{
        Device = $label; VidPid = "${vid}:${usbPid}"; Port = $com
        Status = $d.Status; Name = $d.Name; InstanceId = $d.PNPDeviceID
    }
}

Write-Output '== USB devices with a Flipper/ESP32-S3 vendor ID =='
if ($rows.Count -eq 0) {
    Write-Output '  none (check the cable carries data, the device is on, and its driver)'
} else {
    $rows | Sort-Object Device, Port | Format-Table Device, VidPid, Port, Status, Name -AutoSize | Out-String -Width 200 | Write-Output
}

Write-Output '== COM port names known to Windows (listed, not opened) =='
$names = [System.IO.Ports.SerialPort]::GetPortNames() | Sort-Object
if ($names) { Write-Output ('  ' + ($names -join ', ')) } else { Write-Output '  none' }

Write-Output ''
Write-Output '== Bluetooth radios (needed only for the phone-scanner check, HARDWARE_TESTING.md 3b) =='
try {
    $bt = Get-PnpDevice -Class Bluetooth -PresentOnly -ErrorAction Stop |
        Where-Object { $_.FriendlyName -notmatch 'Enumerator|RFCOMM|LE Generic|Device Identification' }
    if ($bt) {
        $bt | Format-Table FriendlyName, Status, InstanceId -AutoSize | Out-String -Width 200 | Write-Output
    } else { Write-Output '  none present' }
} catch { Write-Output "  could not query: $($_.Exception.Message)" }

$fl = $rows | Where-Object { $_.VidPid -eq '0483:5740' -and $_.Port } | Select-Object -First 1
$es = $rows | Where-Object { $_.VidPid -like '303A:*' -and $_.Port } | Select-Object -First 1
Write-Output '== For the other HIL scripts =='
if ($fl) { Write-Output "FLIPPER_PORT=$($fl.Port)" } else { Write-Output 'FLIPPER_PORT=<not found>' }
if ($es) { Write-Output "STICKS3_PORT=$($es.Port)" } else { Write-Output 'STICKS3_PORT=<not found>' }
exit 0
