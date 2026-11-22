#requires -Version 5
<#
One bench cycle: optional flash, then one tester command, then a compact
transcript.  Keeps the conversation to a couple of lines per measurement.

  tools/bench.ps1 -Flash                    # flash firmware, run sweep
  tools/bench.ps1 -Cmd "stress 20000"       # no flash, run stress
  tools/bench.ps1 -Cmd "repeat 0x00 1 200000"
#>
param(
    [switch]$Flash,
    [switch]$Tester,
    [string]$Cmd  = "sweep 20000",
    [string]$Port = "COM10",
    [int]$WaitSec = 90
)

$root = Split-Path -Parent $PSScriptRoot
$elf  = Join-Path $root "build\at32f421-1wire.elf"

if ($Tester) {
    $uf2 = Join-Path $root "tools\rp2040-tester\build\i2c_tester.uf2"
    $p = New-Object System.IO.Ports.SerialPort $Port,115200,None,8,one
    $p.DtrEnable = $true
    try { $p.Open(); $p.WriteLine("bootsel"); Start-Sleep -Milliseconds 600; $p.Close() } catch {}
    for ($i = 0; $i -lt 40; $i++) {
        Start-Sleep -Milliseconds 300
        if (Test-Path "E:\") { break }
    }
    Copy-Item $uf2 "E:\i2c_tester.uf2" -Force
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

# The tester runs its own suite on boot; abort it and wait for the prompt.
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

$p.WriteLine($Cmd)
$end = (Get-Date).AddSeconds($WaitSec)
$buf = New-Object System.Text.StringBuilder
while ((Get-Date) -lt $end) {
    Start-Sleep -Milliseconds 300
    $c = $p.ReadExisting()
    if ($c) {
        [void]$buf.Append($c)
        if ($buf.ToString() -match "RESULT:|max manager step|^sweep ") {
            # keep reading a little longer for the trailing lines
        }
    }
}
$p.Close()

$lines = ($buf.ToString() -split "`r?`n") |
         Where-Object { $_ -match "\S" } |
         ForEach-Object { ($_ -replace "`e\[[0-9;]*[A-Za-z]", "").Trim() } |
         Where-Object { $_ -notmatch "^> ?$" }

# Compact by default: drop the per-event transcript, keep the summary lines.
$lines | Where-Object {
    $_ -match "^(sweep|\s*\d+ kHz|stress \d+:|errors:|retry manager|max manager step|diag |slow |repeat |RESULT)"
} | ForEach-Object { $_ }
