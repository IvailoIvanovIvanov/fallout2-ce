# Download MinGW Runtime Dependencies for libplacebo
# This script downloads the required DLL files from MSYS2 repository

param(
    [string]$OutputDir = ".\build\Release",
    [switch]$ToLibplaceboDir = $false
)

$ErrorActionPreference = "Stop"

Write-Host "=== MinGW Dependency Downloader ===" -ForegroundColor Cyan
Write-Host ""

if ($ToLibplaceboDir) {
    $OutputDir = ".\third_party\libplacebo\bin"
}

# Ensure output directory exists
if (-not (Test-Path $OutputDir)) {
    Write-Host "Creating output directory: $OutputDir" -ForegroundColor Yellow
    New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null
}

$OutputDir = Resolve-Path $OutputDir

Write-Host "Output directory: $OutputDir" -ForegroundColor Green
Write-Host ""

# MSYS2 package information
# Note: These URLs point to the package repository. You'll need to extract DLLs from .tar.zst files.
$Packages = @(
    @{
        Name = "gcc-libs"
        Url = "https://mirror.msys2.org/mingw/mingw64/mingw-w64-x86_64-gcc-libs-14.2.0-1-any.pkg.tar.zst"
        Files = @("mingw64/bin/libgcc_s_seh-1.dll", "mingw64/bin/libstdc++-6.dll")
    },
    @{
        Name = "winpthread"
        Url = "https://mirror.msys2.org/mingw/mingw64/mingw-w64-x86_64-libwinpthread-13.0.0.r391.g848cce552-1-any.pkg.tar.zst"
        Files = @("mingw64/bin/libwinpthread-1.dll")
    },
    @{
        Name = "libdovi"
        Url = "https://mirror.msys2.org/mingw/mingw64/mingw-w64-x86_64-libdovi-3.3.1-1-any.pkg.tar.zst"
        Files = @("mingw64/bin/dovi.dll")
    },
    @{
        Name = "lcms2"
        Url = "https://mirror.msys2.org/mingw/mingw64/mingw-w64-x86_64-lcms2-2.16-1-any.pkg.tar.zst"
        Files = @("mingw64/bin/liblcms2-2.dll")
    },
    @{
        Name = "shaderc"
        Url = "https://mirror.msys2.org/mingw/mingw64/mingw-w64-x86_64-shaderc-2024.3-1-any.pkg.tar.zst"
        Files = @("mingw64/bin/libshaderc_shared.dll")
    },
    @{
        Name = "spirv-cross"
        Url = "https://mirror.msys2.org/mingw/mingw64/mingw-w64-x86_64-spirv-cross-1~1.4.335.0-1-any.pkg.tar.zst"
        Files = @("mingw64/bin/libspirv-cross-c-shared.dll")
    }
)

Write-Host "This script requires 7-Zip or tar/zstd to extract .tar.zst files." -ForegroundColor Yellow
Write-Host "Checking for extraction tools..." -ForegroundColor Yellow
Write-Host ""

# Check for 7-Zip
$7zPath = $null
$7zLocations = @(
    "C:\Program Files\7-Zip\7z.exe",
    "C:\Program Files (x86)\7-Zip\7z.exe",
    "$env:ProgramFiles\7-Zip\7z.exe"
)

foreach ($loc in $7zLocations) {
    if (Test-Path $loc) {
        $7zPath = $loc
        break
    }
}

if (-not $7zPath) {
    Write-Host "7-Zip not found. Please install 7-Zip from https://www.7-zip.org/" -ForegroundColor Red
    Write-Host ""
    Write-Host "Alternative: Download DLLs manually from:" -ForegroundColor Yellow
    Write-Host "  1. https://packages.msys2.org/package/mingw-w64-x86_64-gcc-libs"
    Write-Host "  2. https://packages.msys2.org/package/mingw-w64-x86_64-libdovi"
    Write-Host "  3. https://packages.msys2.org/package/mingw-w64-x86_64-lcms2"
    Write-Host ""
    Write-Host "Extract the bin/*.dll files to: $OutputDir" -ForegroundColor Cyan
    exit 1
}

Write-Host "Found 7-Zip: $7zPath" -ForegroundColor Green
Write-Host ""

$TempDir = Join-Path $env:TEMP "mingw_deps_download"
New-Item -ItemType Directory -Force -Path $TempDir | Out-Null

try {
    foreach ($pkg in $Packages) {
        Write-Host "Downloading $($pkg.Name)..." -ForegroundColor Cyan
        
        $fileName = Split-Path $pkg.Url -Leaf
        $downloadPath = Join-Path $TempDir $fileName
        
        # Download package
        try {
            Invoke-WebRequest -Uri $pkg.Url -OutFile $downloadPath -UseBasicParsing
            Write-Host "  Downloaded $fileName" -ForegroundColor Green
        } catch {
            Write-Warning "Failed to download $($pkg.Name): $_"
            Write-Host "  Please download manually from: $($pkg.Url)" -ForegroundColor Yellow
            continue
        }
        
        # Extract with 7-Zip (two-stage: .tar.zst -> .tar -> files)
        Write-Host "  Extracting..." -ForegroundColor Yellow
        
        $extractDir = Join-Path $TempDir ($pkg.Name + "_extracted")
        New-Item -ItemType Directory -Force -Path $extractDir | Out-Null
        
        # Stage 1: Extract .zst to .tar
        & $7zPath x "$downloadPath" -o"$extractDir" -y | Out-Null
        
        # Stage 2: Extract .tar
        $tarFile = Join-Path $extractDir ($fileName -replace '\.zst$', '')
        & $7zPath x "$tarFile" -o"$extractDir" -y | Out-Null
        
        # Copy DLL files
        foreach ($file in $pkg.Files) {
            $sourcePath = Join-Path $extractDir $file
            $dllName = Split-Path $file -Leaf
            $destPath = Join-Path $OutputDir $dllName
            
            if (Test-Path $sourcePath) {
                Copy-Item -Path $sourcePath -Destination $destPath -Force
                Write-Host "  Copied $dllName" -ForegroundColor Green
            } else {
                Write-Warning "  File not found in package: $file"
            }
        }
        
        # Clean up extracted files
        Remove-Item -Path $extractDir -Recurse -Force -ErrorAction SilentlyContinue
        Remove-Item -Path $downloadPath -Force -ErrorAction SilentlyContinue
    }
} finally {
    # Clean up temp directory
    Remove-Item -Path $TempDir -Recurse -Force -ErrorAction SilentlyContinue
}

Write-Host ""
Write-Host "=== Download Complete ===" -ForegroundColor Cyan
Write-Host ""

# Create libdovi.dll symlink/copy if dovi.dll exists
$doviPath = Join-Path $OutputDir "dovi.dll"
$libdoviPath = Join-Path $OutputDir "libdovi.dll"
if ((Test-Path $doviPath) -and (-not (Test-Path $libdoviPath))) {
    Copy-Item -Path $doviPath -Destination $libdoviPath -Force
    Write-Host "Created libdovi.dll copy (some tools expect this name)" -ForegroundColor Yellow
}

Write-Host ""
Write-Host "DLLs have been copied to: $OutputDir" -ForegroundColor Green
Write-Host ""
Write-Host "You should now be able to run fallout2-ce.exe without missing DLL errors." -ForegroundColor Green
Write-Host ""
