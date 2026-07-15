$ErrorActionPreference = "Stop"

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$gh = Join-Path $repoRoot ".pio\tools\bin\gh.exe"

if (-not (Test-Path $gh)) {
    throw "GitHub CLI not found at $gh. Run the Codex setup again or install gh first."
}

& $gh auth login --hostname github.com --git-protocol https --web --scopes repo
