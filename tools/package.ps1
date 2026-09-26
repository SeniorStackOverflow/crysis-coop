<#
Builds the release archive dist\Crysis-Coop-v<version>.zip:

  install.bat, install.ps1, install_linux.py, README.md, LICENSE.txt
  Mods\Coop\  Bin32\Coop.dll, CrysisCoop.exe (the launcher; the installer
              copies it into the game folder), CrysisCoop.png (its icon for
              Linux menus), Game\ (scripts, configs),
              info.xml, logo.jpg, README*.md, uninstall.ps1, LICENSE.txt

No game files: the installer builds the co-op levels from the player's own
copy of Crysis.

  -Dll       the built Coop.dll (default: build\Bin32\Coop.dll, or the
             Visual Studio generator's build\Bin32\Release\Coop.dll)
  -Version   default: the project version in CMakeLists.txt
#>
param(
    [string]$Dll,
    [string]$Version
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path

if (-not $Version) {
    $cmake = Get-Content -LiteralPath (Join-Path $root 'CMakeLists.txt') -Raw
    $Version = [regex]::Match($cmake, 'project\(Coop\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)').Groups[1].Value
}
if (-not $Dll) {
    $Dll = @('build\Bin32\Coop.dll', 'build\Bin32\Release\Coop.dll') | ForEach-Object { Join-Path $root $_ } |
        Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
}
if (-not $Dll -or -not (Test-Path -LiteralPath $Dll)) { throw "Coop.dll not found - build the mod first." }
# the launcher is built next to the DLL
$Launcher = Join-Path (Split-Path -Parent $Dll) 'CrysisCoop.exe'
if (-not (Test-Path -LiteralPath $Launcher)) { throw "CrysisCoop.exe not found next to $Dll - build the mod first." }

$name = "Crysis-Coop-v$Version"
$dist = Join-Path $root 'dist'
$stage = Join-Path $dist $name
if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
$mod = Join-Path $stage 'Mods\Coop'
New-Item -ItemType Directory -Force -Path (Join-Path $mod 'Bin32') | Out-Null

Copy-Item -LiteralPath $Dll -Destination (Join-Path $mod 'Bin32\Coop.dll')
Copy-Item -LiteralPath $Launcher -Destination (Join-Path $mod 'CrysisCoop.exe')
Copy-Item -LiteralPath (Join-Path $root 'Launcher\CrysisCoop.png') -Destination $mod
Copy-Item -LiteralPath (Join-Path $root 'Mod\Game') -Destination $mod -Recurse
Get-ChildItem -LiteralPath (Join-Path $root 'Mod') -File | Copy-Item -Destination $mod
Copy-Item -LiteralPath (Join-Path $root 'Installer\uninstall.ps1') -Destination $mod
Copy-Item -LiteralPath (Join-Path $root 'LICENSE.txt') -Destination $mod
Copy-Item -LiteralPath (Join-Path $root 'logo.jpg') -Destination $mod
(Get-Content -LiteralPath (Join-Path $root 'info.xml.in') -Raw).
    Replace('${CMAKE_PROJECT_NAME}', 'Coop').Replace('${CMAKE_PROJECT_VERSION}', $Version).
    Replace('${CMAKE_PROJECT_DESCRIPTION}', 'Crysis co-op campaign').
    Replace('${CMAKE_PROJECT_HOMEPAGE_URL}', 'https://github.com/SeniorStackOverflow/crysis-coop') |
    Set-Content -LiteralPath (Join-Path $mod 'info.xml') -Encoding UTF8

foreach ($f in 'install.bat', 'install.ps1', 'install_linux.py') { Copy-Item -LiteralPath (Join-Path $root "Installer\$f") -Destination $stage }
Copy-Item -LiteralPath (Join-Path $root 'LICENSE.txt') -Destination $stage
Copy-Item -LiteralPath (Join-Path $root 'Mod\README.md') -Destination (Join-Path $stage 'README.md')

$zip = Join-Path $dist "$name.zip"
if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip -Force }
# not Compress-Archive: Windows PowerShell 5.1 writes "\" into the entry names
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
$archive = [IO.Compression.ZipFile]::Open($zip, 'Create')
try {
    Get-ChildItem -LiteralPath $stage -Recurse -File | ForEach-Object {
        $entry = $_.FullName.Substring($stage.Length + 1).Replace('\', '/')
        [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $_.FullName, $entry, 'Optimal') | Out-Null
    }
} finally {
    $archive.Dispose()
}
Write-Host "$zip ($([math]::Round((Get-Item -LiteralPath $zip).Length / 1MB, 2)) MB)"
