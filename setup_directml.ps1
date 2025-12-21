$ErrorActionPreference = "Stop"

$version = "1.20.0"
$url = "https://www.nuget.org/api/v2/package/Microsoft.ML.OnnxRuntime.DirectML/$version"
$destDir = "third_party/onnxruntime-dml"
$zipFile = "$destDir/package.zip"

# Create directory
if (!(Test-Path $destDir)) {
    New-Item -ItemType Directory -Path $destDir | Out-Null
}

# Download ONNX Runtime DirectML
Write-Host "Downloading ONNX Runtime DirectML v$version..."
Invoke-WebRequest -Uri $url -OutFile $zipFile

# Download DirectML Core (Dependency)
$dmlVersion = "1.13.1"
$dmlUrl = "https://www.nuget.org/api/v2/package/Microsoft.AI.DirectML/$dmlVersion"
$dmlZipFile = "$destDir/dml_package.zip"
Write-Host "Downloading Microsoft.AI.DirectML v$dmlVersion..."
Invoke-WebRequest -Uri $dmlUrl -OutFile $dmlZipFile

# Extract ONNX Runtime
Write-Host "Extracting ONNX Runtime..."
Expand-Archive -Path $zipFile -DestinationPath $destDir -Force

# Extract DirectML
Write-Host "Extracting DirectML..."
$dmlExtractDir = "$destDir/dml_temp"
Expand-Archive -Path $dmlZipFile -DestinationPath $dmlExtractDir -Force

# Organize files for CMake
$includeDir = "$destDir/include/onnxruntime"
$libDir = "$destDir/lib"
$binDir = "$destDir/bin"

New-Item -ItemType Directory -Path $includeDir -Force | Out-Null
New-Item -ItemType Directory -Path $libDir -Force | Out-Null
New-Item -ItemType Directory -Path $binDir -Force | Out-Null

# Move Headers (ONNX Runtime)
Copy-Item -Path "$destDir/build/native/include/*" -Destination $includeDir -Recurse -Force

# Move Libs (ONNX Runtime)
if (Test-Path "$destDir/build/native/lib/x64") {
    Copy-Item -Path "$destDir/build/native/lib/x64/*.lib" -Destination $libDir -Force
}

# Move DLLs (ONNX Runtime)
Copy-Item -Path "$destDir/runtimes/win-x64/native/*.dll" -Destination $binDir -Force
Copy-Item -Path "$destDir/runtimes/win-x64/native/*.lib" -Destination $libDir -Force -ErrorAction SilentlyContinue

# Move DirectML files
# bin/x64-win/DirectML.dll
# bin/x64-win/DirectML.lib (import lib?)
# No, usually lib is in bin/x64-win/DirectML.lib or similar
Copy-Item -Path "$dmlExtractDir/bin/x64-win/*.dll" -Destination $binDir -Force
Copy-Item -Path "$dmlExtractDir/bin/x64-win/*.lib" -Destination $libDir -Force
Copy-Item -Path "$dmlExtractDir/bin/x64-win/*.pdb" -Destination $binDir -Force -ErrorAction SilentlyContinue

# Cleanup
Remove-Item $zipFile -Force
Remove-Item $dmlZipFile -Force
Remove-Item $dmlExtractDir -Recurse -Force

Write-Host "ONNX Runtime DirectML setup complete!"
Write-Host "Headers: $includeDir"
Write-Host "Libs:    $libDir"
Write-Host "Binaries: $binDir"
