param(
    [string]$TaskName = "ESP32S3-TCU DuckDNS Update",
    [int]$IntervalMinutes = 5
)

$ErrorActionPreference = "Stop"
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$updateScript = Join-Path $scriptDir "update_duckdns.ps1"
$envFile = Join-Path $scriptDir "duckdns.env"

if (-not (Test-Path -LiteralPath $envFile)) {
    throw "Create tools\dyndns\duckdns.env from duckdns.env.example before installing the task."
}

$action = New-ScheduledTaskAction `
    -Execute "powershell.exe" `
    -Argument "-NoProfile -ExecutionPolicy Bypass -File `"$updateScript`" -EnvFile `"$envFile`""

$trigger = New-ScheduledTaskTrigger -Once -At (Get-Date).AddMinutes(1) `
    -RepetitionInterval (New-TimeSpan -Minutes $IntervalMinutes)

$settings = New-ScheduledTaskSettingsSet `
    -AllowStartIfOnBatteries `
    -DontStopIfGoingOnBatteries `
    -StartWhenAvailable

Register-ScheduledTask `
    -TaskName $TaskName `
    -Action $action `
    -Trigger $trigger `
    -Settings $settings `
    -Description "Updates DuckDNS for the ESP32S3-TCU MQTT LTE test broker." `
    -Force

Write-Host "Installed scheduled task: $TaskName"
