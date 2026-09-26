<#
Crysis Coop - installer.

Installs the co-op campaign mod into an existing Crysis (2007) installation
(GOG, Steam, EA or DVD patched to 1.2.1):

  1. finds the game (or asks for its folder)
  2. installs C1-Launcher if the game does not use it yet (the original
     Bin32\Crysis.exe is kept as Crysis.exe.original)
  3. copies the mod to Mods\Coop
  4. builds the co-op versions of the 11 campaign levels from the game's own
     level files (hard links: almost no extra disk space; the original files
     are never modified)
  5. puts the launcher CrysisCoop.exe into the game folder (it starts the game
     with the mod) and creates the "Crysis Coop" shortcuts to it (game folder,
     desktop, Start menu)

Usage (from the extracted release archive):
  install.bat
  powershell -ExecutionPolicy Bypass -File install.ps1 [-GamePath "D:\Games\Crysis"] [-Force] [-Yes]

Or without downloading anything first (fetches the latest release):
  powershell -ExecutionPolicy Bypass -c "irm https://github.com/SeniorStackOverflow/crysis-coop/releases/latest/download/install.ps1 | iex"

  -GamePath   the Crysis folder (the one with Bin32 and Game in it)
  -Force      rebuild the co-op levels even if they exist
  -Yes        no questions (installs C1-Launcher when needed)
  -NoShortcut no desktop and Start menu shortcuts
#>
[CmdletBinding()]
param(
    [string]$GamePath,
    [switch]$Force,
    [switch]$Yes,
    [switch]$NoShortcut
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'   # Invoke-WebRequest is very slow with the progress bar
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

$Repo = 'SeniorStackOverflow/crysis-coop'
$LauncherUrl = 'https://github.com/ccomrade/c1-launcher/releases/download/v7/c1-launcher-v7-build.zip'
$Campaign = [ordered]@{
    island = 'Contact'; village = 'Recovery'; rescue = 'Relic'; harbor = 'Assault'; tank = 'Onslaught'
    mine = 'Awakening'; core = 'Core'; ice = 'Paradise Lost'; sphere = 'Exodus'; ascension = 'Ascension'; fleet = 'Reckoning'
}

function Say([string]$text, [string]$color = 'Gray') { Write-Host $text -ForegroundColor $color }
function Step([string]$text) { Write-Host ""; Write-Host "== $text" -ForegroundColor Cyan }
function Fail([string]$text) {
    Write-Host ""
    Write-Host "ERROR: $text" -ForegroundColor Red
    if (-not $Yes) { Read-Host "Press Enter to close" | Out-Null }
    exit 1
}
function Ask([string]$question) {
    if ($Yes) { return $true }
    $a = Read-Host "$question [Y/n]"
    return ($a -eq '' -or $a -match '^[Yy]')
}

function Test-CrysisFolder([string]$path) {
    return $path -and (Test-Path -LiteralPath (Join-Path $path 'Bin32\CrySystem.dll')) -and
        (Test-Path -LiteralPath (Join-Path $path 'Game\Levels\island\island.cry'))
}

function Find-Crysis {
    $candidates = @()
    if ($PSScriptRoot) {
        # the installer inside (or next to) the game folder
        $candidates += (Join-Path $PSScriptRoot '..')
        $candidates += (Join-Path $PSScriptRoot '..\..')
        $candidates += $PSScriptRoot
    }
    foreach ($key in 'HKLM:\SOFTWARE\WOW6432Node\Crytek\Crysis', 'HKLM:\SOFTWARE\Crytek\Crysis') {
        $p = (Get-ItemProperty -LiteralPath $key -ErrorAction SilentlyContinue).InstallDir
        if ($p) { $candidates += $p }
    }
    foreach ($key in 'HKLM:\SOFTWARE\WOW6432Node\GOG.com\Games', 'HKLM:\SOFTWARE\GOG.com\Games') {
        Get-ChildItem -LiteralPath $key -ErrorAction SilentlyContinue | ForEach-Object {
            $g = Get-ItemProperty -LiteralPath $_.PSPath -ErrorAction SilentlyContinue
            if ($g.gameName -eq 'Crysis' -and $g.path) { $candidates += $g.path }
        }
    }
    # Steam: every library folder
    $steam = (Get-ItemProperty -LiteralPath 'HKCU:\Software\Valve\Steam' -ErrorAction SilentlyContinue).SteamPath
    if (-not $steam) { $steam = (Get-ItemProperty -LiteralPath 'HKLM:\SOFTWARE\WOW6432Node\Valve\Steam' -ErrorAction SilentlyContinue).InstallPath }
    if ($steam) {
        $libraries = @($steam)
        $vdf = Join-Path $steam 'steamapps\libraryfolders.vdf'
        if (Test-Path -LiteralPath $vdf) {
            $libraries += [regex]::Matches((Get-Content -LiteralPath $vdf -Raw), '"path"\s+"([^"]+)"') | ForEach-Object { $_.Groups[1].Value -replace '\\\\', '\' }
        }
        foreach ($lib in $libraries) { $candidates += (Join-Path $lib 'steamapps\common\Crysis') }
    }
    foreach ($drive in (Get-PSDrive -PSProvider FileSystem -ErrorAction SilentlyContinue).Root) {
        $candidates += (Join-Path $drive 'GOG Games\Crysis')
        $candidates += (Join-Path $drive 'Program Files (x86)\Steam\steamapps\common\Crysis')
        $candidates += (Join-Path $drive 'Program Files (x86)\Electronic Arts\Crytek\Crysis')
        $candidates += (Join-Path $drive 'Program Files (x86)\Origin Games\Crysis')
        $candidates += (Join-Path $drive 'Games\Crysis')
    }
    foreach ($c in $candidates) {
        if (Test-CrysisFolder $c) { return (Resolve-Path -LiteralPath $c).Path }
    }
    return $null
}

function Test-Writable([string]$dir) {
    try {
        $probe = Join-Path $dir ".coop_write_test_$PID"
        [IO.File]::WriteAllText($probe, 'x')
        Remove-Item -LiteralPath $probe -Force
        return $true
    } catch { return $false }
}

function Get-Download([string]$url, [string]$file) {
    Say "   downloading $url"
    Invoke-WebRequest -Uri $url -OutFile $file -UseBasicParsing -Headers @{ 'User-Agent' = 'crysis-coop-installer' }
}

# ---------------------------------------------------------------------------
Write-Host ""
Write-Host "Crysis Coop installer" -ForegroundColor White
Write-Host "This mod is not endorsed by or affiliated with Crytek or Electronic Arts." -ForegroundColor DarkGray

Step "Looking for Crysis"
if ($GamePath) {
    if (-not (Test-CrysisFolder $GamePath)) { Fail "'$GamePath' does not look like a Crysis folder (Bin32\CrySystem.dll and Game\Levels\island are needed)." }
    $Game = (Resolve-Path -LiteralPath $GamePath).Path
} else {
    $Game = Find-Crysis
    if (-not $Game) {
        Say "Crysis was not found automatically." Yellow
        $GamePath = Read-Host "Enter the Crysis folder (the one with Bin32 and Game in it)"
        $GamePath = $GamePath.Trim('"', ' ')
        if (-not (Test-CrysisFolder $GamePath)) { Fail "'$GamePath' does not look like a Crysis folder." }
        $Game = (Resolve-Path -LiteralPath $GamePath).Path
    }
}
Say "   $Game" Green

$version = (Get-Item -LiteralPath (Join-Path $Game 'Bin32\CrySystem.dll')).VersionInfo.FileVersion -replace '\s', ''
if ($version -notmatch '6156') {
    Fail "Crysis $version found, the mod needs version 1.2.1 (build 6156). GOG, Steam and EA versions are 1.2.1 already; a DVD version needs the official patches 1.2 and 1.2.1."
}
Say "   version 1.2.1 (build 6156)" Green

if (-not (Test-Writable $Game)) {
    Say "The game folder needs administrator rights to write to. Restarting as administrator..." Yellow
    $self = $PSCommandPath
    if (-not $self) {
        $self = Join-Path $env:TEMP 'crysis-coop-install.ps1'
        [IO.File]::WriteAllText($self, $MyInvocation.MyCommand.ScriptBlock.ToString())
    }
    $elevated = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$self`"", '-GamePath', "`"$Game`"")
    if ($Force) { $elevated += '-Force' }
    if ($Yes) { $elevated += '-Yes' }
    if ($NoShortcut) { $elevated += '-NoShortcut' }
    Start-Process -FilePath 'powershell.exe' -ArgumentList $elevated -Verb RunAs | Out-Null
    exit 0
}

$temp = Join-Path $env:TEMP "crysis-coop-$PID"
New-Item -ItemType Directory -Force -Path $temp | Out-Null
try {
    # -----------------------------------------------------------------------
    Step "Mod files"
    $payload = $null
    if ($PSScriptRoot -and (Test-Path -LiteralPath (Join-Path $PSScriptRoot 'Mods\Coop\Bin32\Coop.dll'))) {
        $payload = Join-Path $PSScriptRoot 'Mods\Coop'
        Say "   from $payload"
    } else {
        $release = Invoke-RestMethod -Uri "https://api.github.com/repos/$Repo/releases/latest" -Headers @{ 'User-Agent' = 'crysis-coop-installer' }
        $asset = $release.assets | Where-Object { $_.name -like 'Crysis-Coop-*.zip' } | Select-Object -First 1
        if (-not $asset) { Fail "The latest release of $Repo has no Crysis-Coop-*.zip." }
        Say "   release $($release.tag_name)"
        $zip = Join-Path $temp $asset.name
        Get-Download $asset.browser_download_url $zip
        Expand-Archive -LiteralPath $zip -DestinationPath (Join-Path $temp 'release') -Force
        $payload = Get-ChildItem -LiteralPath (Join-Path $temp 'release') -Recurse -Directory -Filter 'Coop' |
            Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName 'Bin32\Coop.dll') } | Select-Object -First 1 -ExpandProperty FullName
        if (-not $payload) { Fail "The release archive has no Mods\Coop\Bin32\Coop.dll." }
    }
    $modDir = Join-Path $Game 'Mods\Coop'
    New-Item -ItemType Directory -Force -Path $modDir | Out-Null
    # the co-op levels are not in the release: they are built below from the game's files
    # (nothing to copy when the archive was extracted right into the game folder)
    if ((Resolve-Path -LiteralPath $payload).Path.TrimEnd('\') -ne (Resolve-Path -LiteralPath $modDir).Path.TrimEnd('\')) {
        robocopy $payload $modDir /E /NFL /NDL /NJH /NJS /NP | Out-Null
        if ($LASTEXITCODE -ge 8) { Fail "Copying the mod to $modDir failed (robocopy $LASTEXITCODE)." }
    }
    Say "   installed to $modDir" Green

    # -----------------------------------------------------------------------
    Step "C1-Launcher"
    $exe = Join-Path $Game 'Bin32\Crysis.exe'
    $product = if (Test-Path -LiteralPath $exe) { (Get-Item -LiteralPath $exe).VersionInfo.ProductName } else { '' }
    if ($product -eq 'C1-Launcher') {
        Say "   already installed ($((Get-Item -LiteralPath $exe).VersionInfo.FileVersion))" Green
    } else {
        Say "   The mod needs C1-Launcher (https://github.com/ccomrade/c1-launcher), an open-source"
        Say "   replacement of Bin32\Crysis.exe. The original is kept as Bin32\Crysis.exe.original."
        if (-not (Ask "   Install C1-Launcher?")) { Fail "The mod cannot run without C1-Launcher." }
        $zip = Join-Path $temp 'c1-launcher.zip'
        Get-Download $LauncherUrl $zip
        Expand-Archive -LiteralPath $zip -DestinationPath (Join-Path $temp 'c1-launcher') -Force
        $newExe = Get-ChildItem -LiteralPath (Join-Path $temp 'c1-launcher') -Recurse -File -Filter 'Crysis.exe' |
            Where-Object { $_.Directory.Name -eq 'Bin32' } | Select-Object -First 1
        if (-not $newExe) { Fail "The C1-Launcher archive has no Bin32\Crysis.exe." }
        $backup = "$exe.original"
        if ((Test-Path -LiteralPath $exe) -and -not (Test-Path -LiteralPath $backup)) {
            Copy-Item -LiteralPath $exe -Destination $backup
        }
        Copy-Item -LiteralPath $newExe.FullName -Destination $exe -Force
        Say "   installed (original: $backup)" Green
    }

    # -----------------------------------------------------------------------
    Step "Co-op levels"
    $levelsDir = Join-Path $modDir 'Game\Levels\Multiplayer\TIA'
    New-Item -ItemType Directory -Force -Path $levelsDir | Out-Null
    foreach ($L in $Campaign.Keys) {
        $src = Join-Path $Game "Game\Levels\$L"
        $dst = Join-Path $levelsDir "coop_$L"
        if (-not (Test-Path -LiteralPath (Join-Path $src "$L.cry"))) { Say "   $L : not in this game, skipped" Yellow; continue }
        if ((Test-Path -LiteralPath (Join-Path $dst "coop_$L.xml")) -and -not $Force) { Say "   $L : already there"; continue }
        if (Test-Path -LiteralPath $dst) { Remove-Item -LiteralPath $dst -Recurse -Force }
        New-Item -ItemType Directory -Force -Path $dst | Out-Null
        $links = 0; $copies = 0
        foreach ($f in (Get-ChildItem -LiteralPath $src -Recurse -File)) {
            $rel = $f.FullName.Substring($src.Length).TrimStart('\')
            $name = $rel
            if ($rel -ieq "$L.cry") { $name = "coop_$L.cry" }
            if ($rel -ieq "$L.xml") {
                # the only file that changes: a real copy, never a link to the original
                $xml = [IO.File]::ReadAllText($f.FullName)
                $pretty = "Coop: $($Campaign[$L])"
                $xml = [regex]::Replace($xml, '<Gamerules[^>]*/>', '<Gamerules MP1="TeamInstantAction"/>')
                if ($xml -match '<Display[^>]*/>') { $xml = [regex]::Replace($xml, '<Display[^>]*/>', "<Display Name=`"$pretty`"/>") }
                else { $xml = $xml -replace '</MetaData>', "`t<Display Name=`"$pretty`"/>`r`n</MetaData>" }
                $xml = [regex]::Replace($xml, '<HeaderText[^>]*/>', "<HeaderText text=`"$pretty`"/>")
                [IO.File]::WriteAllText((Join-Path $dst "coop_$L.xml"), $xml, (New-Object Text.UTF8Encoding($false)))
                $copies++
                continue
            }
            $target = Join-Path $dst $name
            $dir = Split-Path -Parent $target
            if (-not (Test-Path -LiteralPath $dir)) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }
            try {
                New-Item -ItemType HardLink -Path $target -Target $f.FullName -ErrorAction Stop | Out-Null
                $links++
            } catch {
                Copy-Item -LiteralPath $f.FullName -Destination $target -Force
                $copies++
            }
        }
        Say "   $L ($($Campaign[$L])): ready" Green
    }

    # -----------------------------------------------------------------------
    Step "Launcher"
    # CrysisCoop.exe starts Bin32\Crysis.exe -mod Coop -dx9 (and says what is
    # missing when something is)
    $launcher = Join-Path $Game 'CrysisCoop.exe'
    $modLauncher = Join-Path $modDir 'CrysisCoop.exe'
    if (Test-Path -LiteralPath $modLauncher) {
        Copy-Item -LiteralPath $modLauncher -Destination $launcher -Force
        $check = Start-Process -FilePath $launcher -ArgumentList '-coop_check' -Wait -PassThru
        $why = @{ 2 = 'Crysis not found'; 3 = 'Mods\Coop\Bin32\Coop.dll missing'; 4 = 'co-op levels missing'; 5 = 'Bin32\Crysis.exe is not C1-Launcher' }
        if ($check.ExitCode -ne 0) { Fail "The launcher's check failed: $($why[$check.ExitCode]) (code $($check.ExitCode))." }
        Say "   $launcher (checked: ready)" Green
    } else {
        # an archive from before the launcher: the shortcut starts the game itself
        $launcher = $null
        Say "   not in this release, the shortcut starts Bin32\Crysis.exe -mod Coop -dx9" Yellow
    }

    # -----------------------------------------------------------------------
    Step "Shortcut"
    $shell = New-Object -ComObject WScript.Shell
    $places = @(Join-Path $Game 'Crysis Coop.lnk')
    if (-not $NoShortcut) {
        $places += (Join-Path ([Environment]::GetFolderPath('Desktop')) 'Crysis Coop.lnk')
        $places += (Join-Path ([Environment]::GetFolderPath('Programs')) 'Crysis Coop.lnk')
    }
    foreach ($lnk in $places) {
        $s = $shell.CreateShortcut($lnk)
        if ($launcher) {
            $s.TargetPath = $launcher
            $s.Arguments = ''
            $s.IconLocation = "$launcher,0"
        } else {
            $s.TargetPath = $exe
            $s.Arguments = '-mod Coop -dx9'
            $s.IconLocation = "$exe,0"
        }
        $s.WorkingDirectory = $Game
        $s.Description = 'Crysis co-op campaign'
        $s.Save()
        Say "   $lnk" Green
    }
} finally {
    Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue
}

Write-Host ""
Write-Host "Done. How to play:" -ForegroundColor White
Write-Host "  Start 'Crysis Coop' (the shortcut, or CrysisCoop.exe in the game folder)."
Write-Host "  Host:    Multiplayer (the 2nd item of the main menu) > Co-op game > New campaign or Continue."
Write-Host "           A code appears on screen."
Write-Host "  Friend:  Multiplayer > Co-op game > Join a friend, the host's code, Join."
Write-Host "  Remove:  $modDir\uninstall.ps1"
Write-Host ""
if (-not $Yes) { Read-Host "Press Enter to close" | Out-Null }
