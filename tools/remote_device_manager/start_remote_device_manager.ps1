param(
    [string]$MqttHost = "127.0.0.1",
    [int]$MqttPort = 1883,
    [string]$Device = "eboxster",
    [string]$TopicPrefix = "eboxster",
    [string]$HttpHost = "0.0.0.0",
    [int]$HttpPort = 8080,
    [string]$LocalHttpUrl = "",
    [string]$PublicBaseUrl = "",
    [string]$PublicHttpUrl = "",
    [string]$LocalMqttHost = "",
    [string]$PublicMqttHost = "",
    [int]$PublicMqttPort = 0,
    [string]$WebUser = "admin",
    [string]$WebPassword = $env:RDM_WEB_PASSWORD,
    [string]$SharedSecret = $env:RDM_SHARED_SECRET
)

$ErrorActionPreference = "Stop"
$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..\..")

if ([string]::IsNullOrWhiteSpace($LocalHttpUrl)) {
    $ip = Get-NetIPAddress -AddressFamily IPv4 |
        Where-Object { $_.IPAddress -notlike "127.*" -and $_.PrefixOrigin -ne "WellKnown" } |
        Select-Object -First 1 -ExpandProperty IPAddress
    if ([string]::IsNullOrWhiteSpace($ip)) {
        $ip = "127.0.0.1"
    }
    $LocalHttpUrl = "http://${ip}:$HttpPort"
}

if ([string]::IsNullOrWhiteSpace($PublicBaseUrl)) {
    $PublicBaseUrl = $LocalHttpUrl
}

if ($PublicMqttPort -eq 0) {
    $PublicMqttPort = $HttpPort
}

Set-Location $repoRoot
python tools\remote_device_manager\server.py `
    --mqtt-host $MqttHost `
    --mqtt-port $MqttPort `
    --device $Device `
    --topic-prefix $TopicPrefix `
    --http-host $HttpHost `
    --http-port $HttpPort `
    --local-http-url $LocalHttpUrl `
    --public-base-url $PublicBaseUrl `
    --public-http-url $PublicHttpUrl `
    --local-mqtt-host $LocalMqttHost `
    --public-mqtt-host $PublicMqttHost `
    --public-mqtt-port $PublicMqttPort `
    --web-user $WebUser `
    --web-password $WebPassword `
    --shared-secret $SharedSecret `
    --open
