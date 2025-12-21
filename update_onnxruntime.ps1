# Script to update ONNX Runtime with DirectML to latest version via NuGet
# This fixes DirectML compatibility issues with Resize operations

$ErrorActionPreference = "Stop"

$ONNX_VERSION = "1.20.1"
$DML_VERSION = "1.15.2"
$TARGET_DIR = "$PSScriptRoot\third_party\onnxruntime-dml"

$ORT_PACKAGE_URL = "https://www.nuget.org/api/v2/package/Microsoft.ML.OnnxRuntime.DirectML/$ONNX_VERSION"
$DML_PACKAGE_URL = "https://www.nuget.org/api/v2/package/Microsoft.AI.DirectML/$DML_VERSION"

$ORT_ZIP = "$env:TEMP\ort.zip"
$DML_ZIP = "$env:TEMP\dml.zip"
$EXTRACT_DIR = "$env:TEMP\onnxruntime-extract"

Write-Host "Downloading ONNX Runtime $ONNX_VERSION and DirectML $DML_VERSION..." -ForegroundColor Cyan

try {
    # Download ORT
    Invoke-WebRequest -Uri $ORT_PACKAGE_URL -OutFile $ORT_ZIP -UseBasicParsing
    # Download DML
    Invoke-WebRequest -Uri $DML_PACKAGE_URL -OutFile $DML_ZIP -UseBasicParsing
    
    Write-Host "Downloaded successfully" -ForegroundColor Green
    
    # Extract
    if (Test-Path $EXTRACT_DIR) { Remove-Item $EXTRACT_DIR -Recurse -Force }
    New-Item -ItemType Directory -Path $EXTRACT_DIR -Force | Out-Null
    
    Write-Host "Extracting ORT..." -ForegroundColor Cyan
    Expand-Archive -Path $ORT_ZIP -DestinationPath "$EXTRACT_DIR\ort" -Force
    
    Write-Host "Extracting DML..." -ForegroundColor Cyan
    Expand-Archive -Path $DML_ZIP -DestinationPath "$EXTRACT_DIR\dml" -Force
    
    # Ensure target directories exist
    if (!(Test-Path "$TARGET_DIR\bin")) { New-Item -ItemType Directory -Path "$TARGET_DIR\bin" -Force }
    if (!(Test-Path "$TARGET_DIR\include")) { New-Item -ItemType Directory -Path "$TARGET_DIR\include" -Force }
    if (!(Test-Path "$TARGET_DIR\lib")) { New-Item -ItemType Directory -Path "$TARGET_DIR\lib" -Force }

    # Copy DLLs
    Write-Host "Updating DLLs..." -ForegroundColor Cyan
    Copy-Item -Path "$EXTRACT_DIR\ort\runtimes\win-x64\native\onnxruntime.dll" -Destination "$TARGET_DIR\bin\onnxruntime.dll" -Force
    Copy-Item -Path "$EXTRACT_DIR\dml\bin\x64-win\DirectML.dll" -Destination "$TARGET_DIR\bin\DirectML.dll" -Force

    # Copy Headers
    Write-Host "Updating Headers..." -ForegroundColor Cyan
    Copy-Item -Path "$EXTRACT_DIR\ort\build\native\include\*" -Destination "$TARGET_DIR\include" -Recurse -Force
    Copy-Item -Path "$EXTRACT_DIR\dml\include\*" -Destination "$TARGET_DIR\include" -Recurse -Force

    # Copy Libs
    Write-Host "Updating Libs..." -ForegroundColor Cyan
    Copy-Item -Path "$EXTRACT_DIR\ort\runtimes\win-x64\native\onnxruntime.lib" -Destination "$TARGET_DIR\lib\onnxruntime.lib" -Force
    Copy-Item -Path "$EXTRACT_DIR\dml\bin\x64-win\DirectML.lib" -Destination "$TARGET_DIR\lib\DirectML.lib" -Force

    # Cleanup
    Remove-Item $ORT_ZIP -Force
    Remove-Item $DML_ZIP -Force
    Remove-Item $EXTRACT_DIR -Recurse -Force
    
    Write-Host "`nONNX Runtime and DirectML updated successfully!" -ForegroundColor Green
}
catch {
    Write-Error "Failed to update: $($_.Exception.Message)"
    exit 1
}
