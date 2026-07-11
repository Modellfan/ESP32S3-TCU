param(
    [int]$Port = 1883,
    [string]$Config = ""
)

$ErrorActionPreference = "Stop"

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..\..")
$nanomqExe = Join-Path $PSScriptRoot "nanomq\bin\nanomq.exe"
$baseConfig = if ($Config) { Resolve-Path $Config } else { Join-Path $PSScriptRoot "nanomq.conf" }
$runtimeDir = Join-Path $repoRoot "broker-data"
$runtimeConfig = Join-Path $runtimeDir "nanomq.runtime.conf"

if (-not (Test-Path $nanomqExe)) {
    Write-Error "NanoMQ is not installed locally. Download nanomq-*-windows-x86_64.zip from https://github.com/nanomq/nanomq/releases and extract it to tools\mqtt_broker\nanomq."
}

New-Item -ItemType Directory -Force -Path $runtimeDir | Out-Null

$content = Get-Content -LiteralPath $baseConfig -Raw
$content = $content -replace 'bind = "0\.0\.0\.0:\d+"', "bind = `"0.0.0.0:$Port`""
$content = $content -replace 'dir = "broker-data"', "dir = `"$($runtimeDir -replace '\\','/')`""
Set-Content -LiteralPath $runtimeConfig -Value $content -Encoding UTF8

Write-Host "NanoMQ broker listening on 0.0.0.0:$Port"
Write-Host "Use one of these IPv4 addresses as TCALL_MQTT_HOST in src\LocalConfig.h:"
Get-NetIPAddress -AddressFamily IPv4 |
    Where-Object { $_.IPAddress -notlike "127.*" -and $_.PrefixOrigin -ne "WellKnown" } |
    Select-Object -ExpandProperty IPAddress |
    ForEach-Object { Write-Host "  $_" }
Write-Host ""
Write-Host "Starting NanoMQ. Press Ctrl+C to stop."

& $nanomqExe start --conf $runtimeConfig --log_stdout true

