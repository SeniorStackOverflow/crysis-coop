<#
Puts an automatic update (package.ps1's dist\update-v<version>\) on the mod's
update server: https://crysis.46-225-103-75.sslip.io/update/ (Caddy serves
/srv/crysis-coop-update on the VPS, see Relay/README.md). The launchers of
every player take it at their next start.

The package goes first, update.txt last (renamed into place, so a launcher
never sees a manifest without its package). The last 3 packages stay, so
launchers that read the previous manifest a moment ago still find theirs.
The same two files go into the GitHub release too (the fallback server).

  -Version   default: the project version in CMakeLists.txt
#>
param(
    [string]$Version,
    [string]$Server = 'andrei@46.225.103.75',
    [int]$Port = 39637,
    [string]$Dir = '/srv/crysis-coop-update'
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
if (-not $Version) {
    $cmake = Get-Content -LiteralPath (Join-Path $root 'CMakeLists.txt') -Raw
    $Version = [regex]::Match($cmake, 'project\(Coop\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)').Groups[1].Value
}
$update = Join-Path $root "dist\update-v$Version"
$manifest = Join-Path $update 'update.txt'
$package = Join-Path $update "Crysis-Coop-v$Version-update.bin"
if (-not (Test-Path -LiteralPath $manifest) -or -not (Test-Path -LiteralPath $package)) {
    throw "$update has no signed update.txt and package: run tools\package.ps1 (with the release key) first."
}
# Windows' own OpenSSH: Git's ssh cannot read the key
$ssh = Join-Path $env:SystemRoot 'System32\OpenSSH\ssh.exe'
$scp = Join-Path $env:SystemRoot 'System32\OpenSSH\scp.exe'

& $scp -o BatchMode=yes -P $Port $package "${Server}:$Dir/"
if ($LASTEXITCODE) { throw "uploading the package failed" }
& $scp -o BatchMode=yes -P $Port $manifest "${Server}:$Dir/.update.txt.new"
if ($LASTEXITCODE) { throw "uploading update.txt failed" }
& $ssh -o BatchMode=yes -p $Port $Server "cd $Dir && chmod 644 .update.txt.new Crysis-Coop-v$Version-update.bin && mv -f .update.txt.new update.txt && ls -1t Crysis-Coop-v*-update.bin | tail -n +4 | xargs -r rm -f && ls -l"
if ($LASTEXITCODE) { throw "installing update.txt failed" }
Write-Host "Published ${Version}: https://crysis.46-225-103-75.sslip.io/update/update.txt"
