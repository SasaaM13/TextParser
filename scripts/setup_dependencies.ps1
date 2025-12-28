# Bootstrap dependencies for TextParser project
# Usage: Open PowerShell as Administrator and run:
#   .\scripts\setup_dependencies.ps1

$root = Split-Path -Parent $MyInvocation.MyCommand.Definition
Set-Location $root

$extDir = Join-Path $root "external"
if(-not (Test-Path $extDir)) { New-Item -ItemType Directory -Path $extDir | Out-Null }

function Try-VcpkgInstall([string]$pkg) {
    $vcpkg = Join-Path $root "vcpkg\vcpkg.exe"
    if(-not (Test-Path $vcpkg)) { return $false }
    Write-Host "Installing $pkg via vcpkg..."
    & $vcpkg install $pkg 2>&1
    return $LASTEXITCODE -eq 0
}

# 1) Try to use vcpkg (clone & bootstrap if missing)
$vcpkgDir = Join-Path $root "vcpkg"
if(-not (Test-Path (Join-Path $vcpkgDir "vcpkg.exe"))) {
    Write-Host "vcpkg not found. Cloning vcpkg (this may take a while)..."
    if(-not (Test-Path $vcpkgDir)) { git clone https://github.com/microsoft/vcpkg.git $vcpkgDir }
    Push-Location $vcpkgDir
    if(Test-Path "bootstrap-vcpkg.bat") {
        Write-Host "Bootstrapping vcpkg..."
        & .\bootstrap-vcpkg.bat
    }
    Pop-Location
}

$useVcpkg = Test-Path (Join-Path $vcpkgDir "vcpkg.exe")

$packages = @("rapidjson", "tinyxml2", "csv-parser", "openxlsx")
$installed = @{}

if($useVcpkg) {
    foreach($p in $packages) {
        $ok = Try-VcpkgInstall("$p:x64-windows")
        $installed[$p] = $ok
        Write-Host "$p installed: $ok"
    }
} else {
    Write-Host "vcpkg not available or bootstrap failed. Will download headers/sources to external/ as fallback."
}

# Fallback download helper
function DownloadAndExtract($url, $destSubDir) {
    $zip = Join-Path $env:TEMP ([IO.Path]::GetFileName($url))
    try {
        Write-Host "Downloading $url..."
        Invoke-WebRequest -Uri $url -OutFile $zip -UseBasicParsing
        $target = Join-Path $extDir $destSubDir
        if(Test-Path $target) { Remove-Item -Recurse -Force $target }
        New-Item -ItemType Directory -Path $target | Out-Null
        Write-Host "Extracting to $target..."
        Expand-Archive -Path $zip -DestinationPath $extDir -Force
        # Some archives extract to folder like name-master; move contents
        $dirs = Get-ChildItem -Directory -Path $extDir | Where-Object { $_.Name -like "*" } | Select-Object -First 1
        if($dirs) {
            $first = $dirs[0]
            # if extraction created multiple top-level dirs we skip moving
            # Move extracted folder into desired subdir
            if(Test-Path $first.FullName -and (Get-ChildItem $first.FullName).Count -gt 0) {
                Move-Item -Path $first.FullName -Destination $target -Force
            }
        }
        Remove-Item $zip -Force
        return $true
    } catch {
        Write-Host "Failed to download or extract $url: $_"
        return $false
    }
}

# For each package not installed via vcpkg, download fallback
if(-not $useVcpkg) {
    $fallback = @(
        @{ url = 'https://github.com/Tencent/rapidjson/archive/refs/heads/master.zip'; dir = 'rapidjson' },
        @{ url = 'https://github.com/leethomason/tinyxml2/archive/refs/heads/master.zip'; dir = 'tinyxml2' },
        @{ url = 'https://github.com/vincentlaucsb/csv-parser/archive/refs/heads/master.zip'; dir = 'csv-parser' },
        @{ url = 'https://github.com/troldal/OpenXLSX/archive/refs/heads/master.zip'; dir = 'OpenXLSX' }
    )

    foreach($f in $fallback) {
        Write-Host "Fetching $($f.dir) ..."
        DownloadAndExtract $f.url $f.dir | Out-Null
    }
}

Write-Host "\n=== Summary ==="
if($useVcpkg) {
    foreach($p in $packages) { Write-Host "$p: $($installed[$p])" }
    Write-Host "If you use vcpkg, integrate it with Visual Studio by setting the Vcpkg Integration or using the toolchain file in your CMake project."
} else {
    Write-Host "Downloaded fallback sources into ./external. Add appropriate include/lib paths to your project settings."
}

Write-Host "Done."
