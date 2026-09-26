<#
Crysis Coop - uninstaller (lives in Mods\Coop after installation).

Removes Mods\Coop and the "Crysis Coop" shortcuts. The co-op levels in
Mods\Coop are hard links to the game's own level files: removing them never
changes or deletes the originals under Game\Levels.

  -RestoreLauncher  also put back the original Bin32\Crysis.exe
                    (the installer kept it as Bin32\Crysis.exe.original)
  -Force            no question
#>
[CmdletBinding()]
param(
    [switch]$RestoreLauncher,
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
$modDir = $PSScriptRoot
$game = (Resolve-Path -LiteralPath (Join-Path $modDir '..\..')).Path
if (-not (Test-Path -LiteralPath (Join-Path $game 'Bin32\CrySystem.dll'))) {
    Write-Host "This script must stay in <Crysis>\Mods\Coop." -ForegroundColor Red
    exit 1
}

Write-Host "This removes:"
Write-Host "  $modDir"
Write-Host "  the 'Crysis Coop' shortcuts"
if ($RestoreLauncher) { Write-Host "  C1-Launcher (Bin32\Crysis.exe.original is put back)" }
if (-not $Force) {
    $a = Read-Host "Continue? [y/N]"
    if ($a -notmatch '^[Yy]') { Write-Host "Nothing removed."; exit 0 }
}

foreach ($lnk in @((Join-Path $game 'Crysis Coop.lnk'), (Join-Path ([Environment]::GetFolderPath('Desktop')) 'Crysis Coop.lnk'))) {
    if (Test-Path -LiteralPath $lnk) { Remove-Item -LiteralPath $lnk -Force; Write-Host "Removed $lnk" }
}

if ($RestoreLauncher) {
    $exe = Join-Path $game 'Bin32\Crysis.exe'
    $backup = "$exe.original"
    if (Test-Path -LiteralPath $backup) {
        Copy-Item -LiteralPath $backup -Destination $exe -Force
        Remove-Item -LiteralPath $backup -Force
        Write-Host "Restored the original Bin32\Crysis.exe"
    } else {
        Write-Host "No Bin32\Crysis.exe.original found, the launcher is left as it is."
    }
}

# leave the folder first: this script runs from inside it
Set-Location -LiteralPath $game
Remove-Item -LiteralPath $modDir -Recurse -Force
Write-Host "Removed $modDir"
Write-Host "Crysis Coop is uninstalled."
