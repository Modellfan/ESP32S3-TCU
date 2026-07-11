param(
    [string]$Domain = $env:DUCKDNS_DOMAIN,
    [string]$Token = $env:DUCKDNS_TOKEN,
    [string]$Ip = "",
    [string]$EnvFile = ""
)

$ErrorActionPreference = "Stop"
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path

if (-not $EnvFile) {
    $candidate = Join-Path $scriptDir "duckdns.env"
    if (Test-Path -LiteralPath $candidate) {
        $EnvFile = $candidate
    }
}

if ($EnvFile) {
    Get-Content -LiteralPath $EnvFile | ForEach-Object {
        $line = $_.Trim()
        if (-not $line -or $line.StartsWith("#") -or $line -notmatch "=") {
            return
        }
        $key, $value = $line.Split("=", 2)
        $key = $key.Trim()
        $value = $value.Trim().Trim('"')
        if ($key -eq "DUCKDNS_DOMAIN" -and -not $Domain) {
            $Domain = $value
        }
        if ($key -eq "DUCKDNS_TOKEN" -and -not $Token) {
            $Token = $value
        }
    }
}

if (-not $Domain -or -not $Token) {
    throw "Set DUCKDNS_DOMAIN and DUCKDNS_TOKEN, or create tools\dyndns\duckdns.env from duckdns.env.example."
}

$query = @{
    domains = $Domain
    token = $Token
    verbose = "true"
}

if ($Ip) {
    $query.ip = $Ip
}

$uriBuilder = [System.UriBuilder]::new("https://www.duckdns.org/update")
$uriBuilder.Query = ($query.GetEnumerator() | ForEach-Object {
    "{0}={1}" -f [uri]::EscapeDataString($_.Key), [uri]::EscapeDataString($_.Value)
}) -join "&"

$response = Invoke-WebRequest -Uri $uriBuilder.Uri.AbsoluteUri -UseBasicParsing
if ($response.Content -is [byte[]]) {
    $body = [System.Text.Encoding]::UTF8.GetString($response.Content).Trim()
} else {
    $body = [string]$response.Content
    $body = $body.Trim()
}
Write-Host $body

if ($body -notmatch "^OK") {
    exit 1
}
