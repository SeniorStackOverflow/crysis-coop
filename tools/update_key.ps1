<#
Creates the key that signs the mod's automatic updates (once, on the PC that
publishes releases).

  private key: %USERPROFILE%\.crysis-coop\update_signing_key.bin (never in
               the repository; keep a backup: without it no update the
               launchers accept can be published any more)
  public key:  printed for UPDATE_PUBLIC_KEY in Launcher/Update.cpp

package.ps1 signs update.txt with it. An existing key is never replaced
(-Show prints its public key again).
#>
param([switch]$Show)

$ErrorActionPreference = 'Stop'
$dir = Join-Path $env:USERPROFILE '.crysis-coop'
$file = Join-Path $dir 'update_signing_key.bin'
Add-Type -AssemblyName System.Core

if (Test-Path -LiteralPath $file) {
    if (-not $Show) { Write-Host "The key exists already: $file (use -Show for its public key)" }
    $key = [Security.Cryptography.CngKey]::Import([IO.File]::ReadAllBytes($file), [Security.Cryptography.CngKeyBlobFormat]::EccPrivateBlob)
} else {
    $params = New-Object Security.Cryptography.CngKeyCreationParameters
    $params.ExportPolicy = [Security.Cryptography.CngExportPolicies]::AllowPlaintextExport
    $key = [Security.Cryptography.CngKey]::Create([Security.Cryptography.CngAlgorithm]::ECDsaP256, $null, $params)
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    [IO.File]::WriteAllBytes($file, $key.Export([Security.Cryptography.CngKeyBlobFormat]::EccPrivateBlob))
    Write-Host "New key: $file (back it up)"
}
# BCRYPT_ECCKEY_BLOB: magic, length (8 bytes), then X and Y
$public = $key.Export([Security.Cryptography.CngKeyBlobFormat]::EccPublicBlob)
$xy = $public[8..($public.Length - 1)]
$lines = for ($i = 0; $i -lt $xy.Length; $i += 16) {
    "`t" + (($xy[$i..($i + 15)] | ForEach-Object { '0x{0:x2},' -f $_ }) -join ' ')
}
Write-Host "UPDATE_PUBLIC_KEY (X then Y):"
$lines | ForEach-Object { Write-Host $_ }
