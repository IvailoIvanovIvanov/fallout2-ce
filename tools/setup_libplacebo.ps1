# Setup libplacebo from mingw package
# This script copies the pre-built mingw libplacebo package to the project's third_party folder
# and downloads necessary MinGW runtime dependencies

param(
    [string]$PackagePath = "C:\Users\User\Downloads\mingw-w64-x86_64-libplacebo-7.351.0-1-any.pkg",
    [string]$ProjectRoot = $PSScriptRoot + "\..",
    [switch]$SkipDependencies = $false
)

$ErrorActionPreference = "Stop"

Write-Host "=== libplacebo Setup Script ===" -ForegroundColor Cyan
Write-Host ""

# MinGW runtime dependencies required by libplacebo
$RequiredDependencies = @(
    @{ Name = "libgcc_s_seh-1.dll"; Package = "gcc-libs" },
    @{ Name = "libstdc++-6.dll"; Package = "gcc-libs" },
    @{ Name = "dovi.dll"; Package = "libdovi" },
    @{ Name = "liblcms2-2.dll"; Package = "lcms2" },
    @{ Name = "libshaderc_shared.dll"; Package = "shaderc"; Version = "2025.2-1" },
    @{ Name = "libspirv-cross-c-shared.dll"; Package = "spirv-cross" }
)

# MSYS2 package repository base URL
$MsysRepoBase = "https://mirror.msys2.org/mingw/mingw64"

# Determine source path
if (Test-Path "$PackagePath\mingw64") {
    $SourcePath = "$PackagePath\mingw64"
} elseif (Test-Path $PackagePath) {
    $SourcePath = $PackagePath
} else {
    Write-Error "Package not found at: $PackagePath"
    Write-Host "Please extract the package first or provide the correct path."
    exit 1
}

Write-Host "Source: $SourcePath" -ForegroundColor Yellow
Write-Host "Project: $ProjectRoot" -ForegroundColor Yellow
Write-Host ""

# Create destination directories
$DestRoot = Join-Path $ProjectRoot "third_party\libplacebo"
$DestInclude = Join-Path $DestRoot "include"
$DestLib = Join-Path $DestRoot "lib"
$DestBin = Join-Path $DestRoot "bin"

Write-Host "Creating directories..." -ForegroundColor Green
New-Item -ItemType Directory -Force -Path $DestInclude | Out-Null
New-Item -ItemType Directory -Force -Path $DestLib | Out-Null
New-Item -ItemType Directory -Force -Path $DestBin | Out-Null

# Copy headers
Write-Host "Copying headers..." -ForegroundColor Green
$SourceHeaders = Join-Path $SourcePath "include\libplacebo"
if (Test-Path $SourceHeaders) {
    Copy-Item -Path $SourceHeaders -Destination $DestInclude -Recurse -Force
    Write-Host "  Copied libplacebo headers"
} else {
    Write-Error "Headers not found at: $SourceHeaders"
    exit 1
}

# Copy libraries
Write-Host "Copying libraries..." -ForegroundColor Green
$SourceLib = Join-Path $SourcePath "lib"
Get-ChildItem -Path $SourceLib -Filter "*.a" | ForEach-Object {
    Copy-Item -Path $_.FullName -Destination $DestLib -Force
    Write-Host "  Copied $($_.Name)"
}

# Copy DLLs
Write-Host "Copying DLLs..." -ForegroundColor Green
$SourceBin = Join-Path $SourcePath "bin"
Get-ChildItem -Path $SourceBin -Filter "*.dll" | ForEach-Object {
    Copy-Item -Path $_.FullName -Destination $DestBin -Force
    Write-Host "  Copied $($_.Name)"
}

# Copy pkgconfig
$SourcePkgConfig = Join-Path $SourcePath "lib\pkgconfig"
if (Test-Path $SourcePkgConfig) {
    $DestPkgConfig = Join-Path $DestLib "pkgconfig"
    New-Item -ItemType Directory -Force -Path $DestPkgConfig | Out-Null
    Copy-Item -Path "$SourcePkgConfig\*" -Destination $DestPkgConfig -Force
    Write-Host "  Copied pkgconfig files"
}

# Copy license
Write-Host "Copying license..." -ForegroundColor Green
$SourceLicense = Join-Path $SourcePath "share\licenses\libplacebo\LICENSE"
if (Test-Path $SourceLicense) {
    Copy-Item -Path $SourceLicense -Destination $DestRoot -Force
    Write-Host "  Copied LICENSE"
}

# Download and extract MinGW runtime dependencies
if (-not $SkipDependencies) {
    Write-Host ""
    Write-Host "Downloading MinGW runtime dependencies..." -ForegroundColor Green
    
    $TempDir = Join-Path $env:TEMP "libplacebo_deps"
    New-Item -ItemType Directory -Force -Path $TempDir | Out-Null
    
    # Function to extract specific DLLs from .tar.zst package
    function Get-DllsFromPackage {
        param(
            [string]$PackageName,
            [string[]]$DllNames,
            [string]$DestPath
        )
        
        try {
            Write-Host "  Checking for $PackageName..." -ForegroundColor Yellow
            
            # Try to find DLLs in common MinGW locations first
            $MinGWPaths = @(
                "C:\msys64\mingw64\bin",
                "C:\msys2\mingw64\bin",
                "C:\mingw64\bin"
            )
            
            $foundAll = $true
            foreach ($dllName in $DllNames) {
                $found = $false
                foreach ($mingwPath in $MinGWPaths) {
                    $dllPath = Join-Path $mingwPath $dllName
                    if (Test-Path $dllPath) {
                        Copy-Item -Path $dllPath -Destination $DestPath -Force
                        Write-Host "    Copied $dllName from $mingwPath" -ForegroundColor Green
                        $found = $true
                        break
                    }
                }
                if (-not $found) {
                    $foundAll = $false
                    Write-Host "    $dllName not found in local MinGW installations" -ForegroundColor Yellow
                }
            }
            
            if (-not $foundAll) {
                Write-Host "    Some DLLs missing - please install MSYS2 MinGW64 runtime packages" -ForegroundColor Yellow
                Write-Host "    Or download them from: https://packages.msys2.org/package/" -ForegroundColor Cyan
            }
            
        } catch {
            Write-Warning "Failed to process $PackageName : $_"
        }
    }
    
    # Process each required dependency
    Get-DllsFromPackage -PackageName "gcc-libs" `
        -DllNames @("libgcc_s_seh-1.dll", "libstdc++-6.dll") `
        -DestPath $DestBin
    
    Get-DllsFromPackage -PackageName "libdovi" `
        -DllNames @("dovi.dll") `
        -DestPath $DestBin
    
    Get-DllsFromPackage -PackageName "lcms2" `
        -DllNames @("liblcms2-2.dll") `
        -DestPath $DestBin
    
    # Clean up temp directory
    Remove-Item -Path $TempDir -Recurse -Force -ErrorAction SilentlyContinue
}

Write-Host ""
Write-Host "=== Setup Complete ===" -ForegroundColor Cyan
Write-Host ""
Write-Host "libplacebo has been installed to:" -ForegroundColor Green
Write-Host "  $DestRoot"
Write-Host ""

if ($SkipDependencies) {
    Write-Host "Warning: Dependency installation was skipped." -ForegroundColor Yellow
    Write-Host "You may need to manually copy these DLLs to your build folder:" -ForegroundColor Yellow
    Write-Host "  - libgcc_s_seh-1.dll"
    Write-Host "  - libstdc++-6.dll"
    Write-Host "  - dovi.dll"
    Write-Host "  - liblcms2-2.dll"
    Write-Host ""
    Write-Host "These can be obtained from MSYS2 MinGW64 packages." -ForegroundColor Cyan
    Write-Host ""
}

Write-Host "Next steps:" -ForegroundColor Yellow
Write-Host "  1. Rebuild the project (CMake will copy DLLs automatically)"
Write-Host "  2. If DLLs are missing, install MSYS2 and run:"
Write-Host "     pacman -S mingw-w64-x86_64-gcc-libs mingw-w64-x86_64-libdovi mingw-w64-x86_64-lcms2"
Write-Host "  3. Then re-run this script to copy the DLLs"
Write-Host ""

# Also copy DLL to common build locations
$BuildDirs = @(
    (Join-Path $ProjectRoot "build\Debug"),
    (Join-Path $ProjectRoot "build\Release"),
    (Join-Path $ProjectRoot "build-vcpkg\Debug"),
    (Join-Path $ProjectRoot "build-vcpkg\Release")
)

Write-Host "Copying DLL to build directories..." -ForegroundColor Green
foreach ($BuildDir in $BuildDirs) {
    if (Test-Path $BuildDir) {
        Get-ChildItem -Path $DestBin -Filter "*.dll" | ForEach-Object {
            Copy-Item -Path $_.FullName -Destination $BuildDir -Force
            Write-Host "  Copied to $BuildDir"
        }
    }
}

Write-Host ""
Write-Host "Done!" -ForegroundColor Green
