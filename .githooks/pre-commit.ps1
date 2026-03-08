#Requires -Version 5.1
$ErrorActionPreference = 'Stop'

# Auto-format staged C/C++ files using clang-format (PowerShell).

$clang = Get-Command clang-format -ErrorAction SilentlyContinue
if (-not $clang) {
  Write-Host 'clang-format not found; skipping formatting.'
  exit 0
}

$staged = git diff --cached --name-only --diff-filter=ACM | ForEach-Object { $_.Trim() }
if (-not $staged) { exit 0 }

$files = $staged |
  Where-Object { $_ -match '\.(h|hpp|hh|c|cc|cpp)$' -and $_ -notmatch '^[Bb]uild/' }

if (-not $files) { exit 0 }

foreach ($f in $files) {
  & $clang.Path '-i' $f
}

git add -- $files
exit 0
