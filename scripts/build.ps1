param(
    [switch]$SkipInstaller
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$dist = Join-Path $root 'dist'
$build = Join-Path $dist 'build'
$stage = Join-Path $dist 'staging'
$portable = Join-Path $dist 'portable'
$release = Join-Path $root 'release'

$cmakeText = Get-Content (Join-Path $root 'CMakeLists.txt') -Raw
if ($cmakeText -notmatch 'project\(HubVolume VERSION ([0-9.]+)') {
    throw 'Versi tidak ditemukan di CMakeLists.txt'
}
$version = $Matches[1]

$cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source
if (-not $cmake) { throw 'cmake tidak ditemukan di PATH' }

Write-Host "Hub Volume $version" -ForegroundColor Cyan

# Konfigurasi ulang hanya bila perlu.
if (-not (Test-Path (Join-Path $build 'CMakeCache.txt'))) {
    if ($env:CMAKE_GENERATOR) {
        & $cmake -S $root -B $build -G $env:CMAKE_GENERATOR
    } else {
        & $cmake -S $root -B $build -G 'Visual Studio 17 2022' -A x64
    }
    if ($LASTEXITCODE -ne 0) { throw 'Konfigurasi CMake gagal' }
}
& $cmake --build $build --config Release
if ($LASTEXITCODE -ne 0) { throw 'Build gagal' }

$exe = Join-Path $build 'Release\HubVolume.exe'
if (-not (Test-Path $exe)) { throw "Hasil build tidak ditemukan: $exe" }

foreach ($dir in @($stage, $portable, $release)) {
    if (Test-Path $dir) { Remove-Item $dir -Recurse -Force }
    New-Item -ItemType Directory -Path $dir -Force | Out-Null
}

Copy-Item $exe $stage
Copy-Item (Join-Path $root 'src\HubVolume.ico') $stage
Copy-Item (Join-Path $root 'res\welcome.bmp') $stage
Copy-Item (Join-Path $root 'res\header.bmp') $stage

# Paket portable: exe + lisensi + petunjuk singkat, tanpa installer.
Copy-Item $exe $portable
Copy-Item (Join-Path $root 'LICENSE') $portable
@"
Hub Volume $version - versi portable

Jalankan HubVolume.exe. Tidak perlu instalasi.
Ikon speaker muncul di dekat jam; klik untuk membuka mixer.
Pengaturan disimpan di %LOCALAPPDATA%\HubVolume.

Agar berjalan otomatis saat Windows menyala, aktifkan
"Mulai bersama Windows" di bagian bawah jendela mixer.
"@ | Set-Content (Join-Path $portable 'BACA-SAYA.txt') -Encoding utf8

$zip = Join-Path $release "HubVolume-$version-portable.zip"
Compress-Archive -Path (Join-Path $portable '*') -DestinationPath $zip -CompressionLevel Optimal
Write-Host "Portable : $zip" -ForegroundColor Green

if (-not $SkipInstaller) {
    $nsis = (Get-Command makensis -ErrorAction SilentlyContinue).Source
    if (-not $nsis) {
        $candidate = 'C:\Program Files (x86)\NSIS\makensis.exe'
        if (Test-Path $candidate) { $nsis = $candidate }
    }
    if ($nsis) {
        Push-Location (Join-Path $root 'installer')
        try {
            & $nsis 'installer.nsi'
            if ($LASTEXITCODE -ne 0) { throw 'Pembuatan installer gagal' }
        } finally {
            Pop-Location
        }
        Write-Host "Installer: $(Join-Path $release "HubVolume-Setup-$version.exe")" -ForegroundColor Green
    } else {
        Write-Warning 'makensis tidak ditemukan; installer dilewati.'
    }
}

$files = Get-ChildItem $release -File | Sort-Object Name
$hashLines = foreach ($f in $files) {
    $h = (Get-FileHash $f.FullName -Algorithm SHA256).Hash.ToLower()
    "$h  $($f.Name)"
}
$hashLines | Set-Content (Join-Path $release 'SHA256SUMS.txt') -Encoding ascii
Get-ChildItem $release -File | Select-Object Name, @{n = 'MB'; e = { [math]::Round($_.Length / 1MB, 2) } }
