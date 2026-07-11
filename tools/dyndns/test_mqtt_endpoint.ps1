param(
    [Parameter(Mandatory = $true)]
    [string]$HostName,

    [int]$Port = 1883
)

$ErrorActionPreference = "Stop"

Write-Host "Resolving $HostName ..."
$addresses = [System.Net.Dns]::GetHostAddresses($HostName) |
    Where-Object { $_.AddressFamily -eq [System.Net.Sockets.AddressFamily]::InterNetwork }

if (-not $addresses) {
    throw "No IPv4 address found for $HostName."
}

$addresses | ForEach-Object { Write-Host "IPv4: $_" }

Write-Host "Testing TCP $HostName`:$Port ..."
$client = [System.Net.Sockets.TcpClient]::new()
$async = $client.BeginConnect($HostName, $Port, $null, $null)
if (-not $async.AsyncWaitHandle.WaitOne(5000, $false)) {
    $client.Close()
    throw "TCP connect timed out. Check router port forwarding and Windows firewall."
}
$client.EndConnect($async)
$client.Close()
Write-Host "TCP connect OK."
