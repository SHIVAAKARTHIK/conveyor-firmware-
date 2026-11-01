<#
.SYNOPSIS
  Builds the conveyor firmware and flashes it to the ESP32 in one go.

.DESCRIPTION
  Does everything that would otherwise be typed by hand:
    1. checks that arduino-cli is installed
    2. creates config.h from config.h.example if it does not exist yet
    3. installs the ESP32 board support and the two libraries if they are missing (first run only)
    4. finds the board's COM port by itself (or uses -Port)
    5. compiles and uploads

  Run it from PowerShell, in this folder:
      powershell -ExecutionPolicy Bypass -File .\flash.ps1
  Compile only (no board needed):   ... -File .\flash.ps1 -CompileOnly
  Choose the port yourself:         ... -File .\flash.ps1 -Port COM5

  The WiFi, backend computer and login are NOT set here. After flashing, the board opens its own
  WiFi "MediDispense-Setup" (password medidispense); join it and open http://192.168.4.1.
#>
param(
  [string]$Port = "",
  [switch]$CompileOnly
)

$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot

$Fqbn      = "esp32:esp32:esp32"
$CoreName  = "esp32:esp32"
$CoreVer   = "3.3.11"
$BoardUrl  = "https://espressif.github.io/arduino-esp32/package_esp32_index.json"
$Libs      = @(@{ Name = "ArduinoJson"; Version = "7.4.3" }, @{ Name = "ESP32Servo"; Version = "3.2.1" })

function Step($text) { Write-Host ""; Write-Host "==> $text" -ForegroundColor Cyan }
function Fail($text) { Write-Host ""; Write-Host "ERROR: $text" -ForegroundColor Red; exit 1 }
function Run($what, [scriptblock]$cmd) {
  & $cmd
  if ($LASTEXITCODE -ne 0) { Fail "$what failed (exit code $LASTEXITCODE). See the messages above." }
}

# 1. arduino-cli
Step "Checking arduino-cli"
if (-not (Get-Command arduino-cli -ErrorAction SilentlyContinue)) {
  Fail ("arduino-cli is not installed (or not on PATH). Install it from https://arduino.github.io/arduino-cli/ " +
        "then open a NEW PowerShell window and run this script again.")
}
Write-Host (arduino-cli version)

# 2. config.h
Step "Checking config.h"
if (-not (Test-Path "config.h")) {
  Copy-Item "config.h.example" "config.h"
  Write-Host "Created config.h from config.h.example. You do not need to edit it."
} else {
  Write-Host "config.h already exists."
}

# 3. ESP32 board support
Step "Checking the ESP32 board support ($CoreName $CoreVer)"
$coreList = (arduino-cli core list) -join "`n"
if ($coreList -notmatch [regex]::Escape($CoreName) + "\s+" + [regex]::Escape($CoreVer)) {
  Write-Host "Not installed yet. This is a big download and happens only once."
  Run "Updating the board index" { arduino-cli core update-index --additional-urls $BoardUrl }
  Run "Installing the ESP32 board support" { arduino-cli core install "${CoreName}@${CoreVer}" --additional-urls $BoardUrl }
} else {
  Write-Host "Already installed."
}

# 3b. libraries
Step "Checking the libraries"
$libList = (arduino-cli lib list) -join "`n"
foreach ($lib in $Libs) {
  if ($libList -match [regex]::Escape($lib.Name)) {
    Write-Host "$($lib.Name): already installed."
  } else {
    Write-Host "$($lib.Name): installing $($lib.Version)..."
    Run "Installing $($lib.Name)" { arduino-cli lib install "$($lib.Name)@$($lib.Version)" }
  }
}

# 4. compile
# Arduino insists that a sketch folder is named like its .ino file (conveyor-firmware), but this repository's
# folder has a different name. So build from a correctly named copy; the real folder is never touched.
Step "Preparing a build copy"
$Stage = Join-Path ([System.IO.Path]::GetTempPath()) "medidispense-firmware\conveyor-firmware"
if (Test-Path $Stage) { Remove-Item $Stage -Recurse -Force }
New-Item -ItemType Directory -Force $Stage | Out-Null
Copy-Item "conveyor-firmware.ino", "setup_portal.h", "config.h" $Stage
Write-Host "Build copy: $Stage"

Step "Compiling the firmware"
Run "Compiling" { arduino-cli compile --fqbn $Fqbn $Stage }
if ($CompileOnly) {
  Write-Host ""
  Write-Host "Compiled OK (compile only, nothing was flashed)." -ForegroundColor Green
  exit 0
}

# 5. find the port
Step "Looking for the board"
if ($Port -eq "") {
  $json = arduino-cli board list --format json | Out-String | ConvertFrom-Json
  $detected = if ($json.detected_ports) { $json.detected_ports } else { $json }
  $ports = @($detected | Where-Object { $_.port -and $_.port.protocol -eq "serial" } | ForEach-Object { $_.port.address })
  if ($ports.Count -eq 0) {
    Fail ("No board found. Plug the ESP32 in with a USB cable that carries data, install the CP210x (or CH340) USB driver " +
          "if no COM port appears, then run this again. Or give the port yourself: -Port COM5")
  }
  if ($ports.Count -gt 1) {
    Write-Host "More than one serial port found: $($ports -join ', ')"
    $Port = Read-Host "Type the port of the ESP32 (for example $($ports[0]))"
  } else {
    $Port = $ports[0]
  }
}
Write-Host "Using port $Port"

# 6. upload
Step "Uploading to the board (if it hangs at 'Connecting...', hold the BOOT button on the board)"
Run "Uploading" { arduino-cli upload -p $Port --fqbn $Fqbn $Stage }

Write-Host ""
Write-Host "Done. The firmware is on the board." -ForegroundColor Green
Write-Host "Next: if the board has no WiFi saved yet, its LED blinks fast and it opens the WiFi 'MediDispense-Setup'"
Write-Host "      (password medidispense). Join it and open http://192.168.4.1 to set the WiFi, backend and login."
