#requires -Version 5
<#
One bench cycle, capped output.

  tools/bench.ps1 -Profile sweep            # 3 speeds x 3 block lengths
  tools/bench.ps1 -Profile stress           # stress 20000
  tools/bench.ps1 -Profile full             # run full
  tools/bench.ps1 -Cmd "repeat 0x00 1 50000"
  tools/bench.ps1 -Profile stress -Flash    # flash AT32 first
  tools/bench.ps1 -Profile sweep -Tester    # flash the Pico tester first

Prints at most -MaxLines lines; the full transcript goes to bench.log.
Exits as soon as the run reports RESULT (stress/full) or all sweep points.
#>
param(
    [ValidateSet("", "sweep", "stress", "full", "repeat")]
    [string]$Profile = "",
    [string]$Cmd     = "",
    [int]$Speed      = 400,
    [switch]$Flash,
    [switch]$Tester,
    [string]$Port    = "COM10",
    [int]$WaitSec    = 300,
    [int]$MaxLines   = 12,
    [string]$Log     = "bench.log"
)

$root = Split-Path -Parent $PSScriptRoot
$elf  = Join-Path $root "build\at32f421-1wire.elf"

switch ($Profile) {
    "sweep"  { if (-not $Cmd) { $Cmd = "sweep 20000" } }
    "stress" { if (-not $Cmd) { $Cmd = "stress 20000" } }
    "full"   { if (-not $Cmd) { $Cmd = "run full" } }
    "repeat" { if (-not $Cmd) { $Cmd = "repeat 0x00 1 50000" } }
}
if (-not $Cmd) { throw "give -Profile or -Cmd" }

if ($Tester) {
    $uf2 = Join-Path $root "tools\rp2040-tester\build\i2c_tester.uf2"
    $t = New-Object System.IO.Ports.SerialPort $Port,115200,None,8,one
    $t.DtrEnable = $true
    try { $t.Open(); $t.WriteLine("bootsel"); Start-Sleep -Milliseconds 600; $t.Close() } catch {}
    # The Pico bootloader mounts as the RPI-RP2 volume; its letter is assigned
    # by Windows and must not be hardcoded (E: may be another drive).
    $rp = $null
    for ($i = 0; $i -lt 40; $i++) {
        Start-Sleep -Milliseconds 300
        $v = Get-Volume -FileSystemLabel "RPI-RP2" -ErrorAction SilentlyContinue
        if ($v -and $v.DriveLetter) { $rp = "$($v.DriveLetter):\"; break }
    }
    if (-not $rp) { throw "RPI-RP2 volume not found" }
    Copy-Item $uf2 (Join-Path $rp "i2c_tester.uf2") -Force
    Start-Sleep -Seconds 9
}

if ($Flash) {
    $env:OPENOCD_SCRIPTS = Join-Path $root "tools\openocd-at32\scripts"
    $ocd = Join-Path $root "tools\openocd-at32\openocd.exe"
    & $ocd -f interface/stlink.cfg -c "transport select hla_swd" `
        -f target/at32f421xx.cfg -c "adapter speed 1000" -c "init" `
        -c "reset halt" -c "program $($elf -replace '\\','/') verify reset exit" 2>&1 |
        Select-String -Pattern "Verified|Failed" | Select-Object -Last 1
    Start-Sleep -Seconds 3
}

$p = New-Object System.IO.Ports.SerialPort $Port,115200,None,8,one
$p.DtrEnable = $true
$p.Open()
Start-Sleep -Seconds 2

# Abort the boot suite and wait for the prompt.
$p.Write([char]3); Start-Sleep -Seconds 1
$p.Write([char]3); Start-Sleep -Seconds 1
$p.ReadExisting() | Out-Null
$deadline = (Get-Date).AddSeconds(20)
while ((Get-Date) -lt $deadline) {
    Start-Sleep -Milliseconds 200
    if ($p.ReadExisting() -match "ready: \? for commands") { break }
}
$p.Write([char]3); Start-Sleep -Milliseconds 300
$p.ReadExisting() | Out-Null

if ($Speed -gt 0) {
    $p.WriteLine("speed $Speed")
    Start-Sleep -Milliseconds 900
    $p.ReadExisting() | Out-Null
}

$p.WriteLine($Cmd)
$end = (Get-Date).AddSeconds($WaitSec)
$buf = New-Object System.Text.StringBuilder
$sweepLines = 0
while ((Get-Date) -lt $end) {
    Start-Sleep -Milliseconds 300
    $c = $p.ReadExisting()
    if (-not $c) { continue }
    [void]$buf.Append($c)
    $text = $buf.ToString()
    if ($text -match "RESULT:") { break }
    if ($text -match "sweep " -and $c -match "kHz") {
        # a full sweep is 9 points; leave after the last one
        if ($text -notmatch "\d+ kHz 0x80 x8") {
            # keep waiting for the final point
        } else {
            Start-Sleep -Milliseconds 400
            [void]$buf.Append($p.ReadExisting())
            break
        }
    }
}
$p.Close()

$clean = ($buf.ToString() -split "`r?`n") |
         ForEach-Object { ($_ -replace "`e\[[0-9;]*[A-Za-z]", "").Trim() } |
         Where-Object { $_ -notmatch "^> ?$" -and $_ -match "\S" }

Set-Content -Path (Join-Path $root $Log) -Value ($clean -join "`r`n")

$want = '^(sweep |\s*\d+ kHz|stress \d+:|errors:|retry manager|max manager step|diag |slow |repeat |RESULT|SUMMARY|T\d+ .*FAIL)'
$pick = $clean | Where-Object { $_ -match $want }
if (-not $pick) { $pick = $clean | Select-Object -Last $MaxLines }
$pick | Select-Object -First $MaxLines

