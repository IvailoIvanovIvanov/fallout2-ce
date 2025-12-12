# Fallout 2 CE - GPU Upscaler Testing Setup Guide

## Problem: Game Not Starting

The game requires more than just the exe and 2 DLLs to run. You need:

1. **Game executable and DLLs** ✓ (you have these)
2. **Game data files** ✗ (missing)
3. **Working directory** ✗ (might be wrong)
4. **Log file location** ⚠️ (need to verify)

---

## Solution: Proper Game Setup

### Step 1: Create Game Folder

Create a dedicated folder for the game (not in the source directory):

```powershell
# Create folder (example: C:\Games\Fallout2CE)
New-Item -ItemType Directory -Path "C:\Games\Fallout2CE" -Force

# Or use any location you prefer:
# D:\Games\Fallout2CE
# C:\Users\YourName\Documents\Games\Fallout2CE
```

### Step 2: Copy Game Files

Copy the minimum required files:

```powershell
# Navigate to build directory
cd 'c:\Users\User\Documents\Source\fallout2-ce\build\Release'

# Copy exe and DLLs
Copy-Item -Path 'fallout2-ce.exe' -Destination 'C:\Games\Fallout2CE\'
Copy-Item -Path 'amd_fidelityfx_loader_dx12.dll' -Destination 'C:\Games\Fallout2CE\'
Copy-Item -Path 'amd_fidelityfx_upscaler_dx12.dll' -Destination 'C:\Games\Fallout2CE\'

# Copy game data (CRITICAL!)
Copy-Item -Path 'c:\Users\User\Documents\Source\fallout2-ce\data' -Destination 'C:\Games\Fallout2CE\' -Recurse -Force
Copy-Item -Path 'c:\Users\User\Documents\Source\fallout2-ce\master.dat' -Destination 'C:\Games\Fallout2CE\'

# Optional: copy config file if it exists
Copy-Item -Path 'c:\Users\User\Documents\Source\fallout2-ce\fallout2.cfg' -Destination 'C:\Games\Fallout2CE\' -ErrorAction SilentlyContinue
```

### Step 3: Verify Files Are Copied

```powershell
cd 'C:\Games\Fallout2CE'
Get-ChildItem -Path '.' | Select-Object Name, Length

# Expected output:
# fallout2-ce.exe
# amd_fidelityfx_loader_dx12.dll
# amd_fidelityfx_upscaler_dx12.dll
# master.dat
# data (folder)
```

### Step 4: Run the Game

```powershell
cd 'C:\Games\Fallout2CE'

# Run with output to console
.\fallout2-ce.exe

# Or run and capture log
.\fallout2-ce.exe | Tee-Object -FilePath 'game_output.txt'

# Or run and redirect output to file
.\fallout2-ce.exe > game.log 2>&1
```

### Step 5: Find and Review Logs

```powershell
# Log file location depends on where the game looks
# Check current directory
Test-Path '.\game.log'
Test-Path '.\game_output.txt'

# Check temp folder
Test-Path "$env:TEMP\fallout2-ce.log"

# Check user documents
Test-Path "$env:USERPROFILE\Documents\fallout2-ce.log"

# Check application data
Test-Path "$env:APPDATA\fallout2-ce.log"
```

---

## Complete Setup Script

Here's a complete PowerShell script to do everything:

```powershell
# Configuration
$SourceDir = 'c:\Users\User\Documents\Source\fallout2-ce'
$GameDir = 'C:\Games\Fallout2CE'  # Change this to your preferred location
$BuildDir = "$SourceDir\build\Release"

# Step 1: Create game directory
Write-Host "Creating game directory: $GameDir" -ForegroundColor Green
New-Item -ItemType Directory -Path $GameDir -Force | Out-Null

# Step 2: Copy executable and DLLs
Write-Host "Copying executable and DLLs..." -ForegroundColor Green
Copy-Item -Path "$BuildDir\fallout2-ce.exe" -Destination $GameDir -Force
Copy-Item -Path "$BuildDir\amd_fidelityfx_loader_dx12.dll" -Destination $GameDir -Force
Copy-Item -Path "$BuildDir\amd_fidelityfx_upscaler_dx12.dll" -Destination $GameDir -Force

# Step 3: Copy game data
Write-Host "Copying game data files..." -ForegroundColor Green
Copy-Item -Path "$SourceDir\data" -Destination $GameDir -Recurse -Force
Copy-Item -Path "$SourceDir\master.dat" -Destination $GameDir -Force

# Step 4: Copy config (optional)
if (Test-Path "$SourceDir\fallout2.cfg") {
    Write-Host "Copying config file..." -ForegroundColor Green
    Copy-Item -Path "$SourceDir\fallout2.cfg" -Destination $GameDir -Force
}

# Step 5: Verify
Write-Host "`nVerifying installation..." -ForegroundColor Green
$files = @('fallout2-ce.exe', 'amd_fidelityfx_loader_dx12.dll', 'amd_fidelityfx_upscaler_dx12.dll', 'master.dat')
foreach ($file in $files) {
    if (Test-Path "$GameDir\$file") {
        Write-Host "  ✓ $file" -ForegroundColor Green
    } else {
        Write-Host "  ✗ MISSING: $file" -ForegroundColor Red
    }
}

if (Test-Path "$GameDir\data") {
    Write-Host "  ✓ data folder" -ForegroundColor Green
} else {
    Write-Host "  ✗ MISSING: data folder" -ForegroundColor Red
}

Write-Host "`nSetup complete! Game folder: $GameDir" -ForegroundColor Green
Write-Host "`nTo run the game:" -ForegroundColor Yellow
Write-Host "  cd '$GameDir'" -ForegroundColor Cyan
Write-Host "  .\fallout2-ce.exe" -ForegroundColor Cyan
```

---

## Troubleshooting: Game Still Won't Start

### Issue: "Missing DLL" or "DLL not found"

```powershell
# Verify DLLs are in game folder
Get-ChildItem 'C:\Games\Fallout2CE\*.dll' -Force

# Expected:
# amd_fidelityfx_loader_dx12.dll
# amd_fidelityfx_upscaler_dx12.dll

# Check if they're 64-bit (should be)
[System.Diagnostics.FileVersionInfo]::GetVersionInfo('C:\Games\Fallout2CE\amd_fidelityfx_loader_dx12.dll')
```

**Fix:** Make sure you copied the DLLs from `build/Release/` (64-bit) not `build/Debug/` (32-bit)

### Issue: "Game data files not found"

```powershell
# Verify data folder exists and has files
Get-ChildItem 'C:\Games\Fallout2CE\data' -Recurse | Measure-Object

# Should show more than 0 files
# If count is 0, data folder is empty - recopy it
```

**Fix:** Copy the `data` folder and `master.dat` from source directory again

### Issue: "Cannot create/write log file"

```powershell
# Check folder permissions
Test-Path 'C:\Games\Fallout2CE\'
Get-ItemProperty -Path 'C:\Games\Fallout2CE\' | Select-Object FullName

# Try running as administrator if permissions issue
```

**Fix:** Run command prompt as administrator, then run game from there

### Issue: "Application crashed on startup"

```powershell
# Check Event Viewer for crash details
Get-EventLog -LogName Application -Newest 5 -Source 'fallout2-ce' -ErrorAction SilentlyContinue

# Or check for crash dumps
Get-ChildItem -Path "$env:TEMP\*fallout2-ce*" -ErrorAction SilentlyContinue
```

**Fix:** 
- Ensure DirectX 12 is installed (Windows 10+)
- Update GPU drivers
- Try running from different location
- Check for antivirus blocking

---

## Finding Log Files

The game can output logs in several locations. Check in this order:

```powershell
# 1. Current working directory
Test-Path "C:\Games\Fallout2CE\game.log"
Test-Path "C:\Games\Fallout2CE\fallout2-ce.log"

# 2. Windows temp folder
Test-Path "$env:TEMP\fallout2-ce.log"
Get-ChildItem -Path $env:TEMP -Filter '*fallout2*' -ErrorAction SilentlyContinue

# 3. User documents
Test-Path "$env:USERPROFILE\Documents\fallout2-ce.log"
Get-ChildItem -Path "$env:USERPROFILE\Documents\" -Filter '*fallout2*' -ErrorAction SilentlyContinue

# 4. Roaming app data
Test-Path "$env:APPDATA\fallout2-ce.log"
Get-ChildItem -Path $env:APPDATA -Filter '*fallout2*' -Recurse -ErrorAction SilentlyContinue

# 5. Run with console output (redirect to file)
cd "C:\Games\Fallout2CE"
.\fallout2-ce.exe > game_output.log 2>&1
# This creates game_output.log in the current directory
```

---

## Expected Log Output

When the game runs successfully with FSR2 debug logging, you should see:

```
========================================
UPSCALER INITIALIZATION
========================================
Input: 640x480, Output: 1920x1080, Mode: 1
Step 1/5: Checking GPU device readiness...
Step 1/5: GPU device is ready ✓
Step 2/5: Acquiring GPU device context...
Step 2/5: GPU device context acquired ✓ (Device=0x..., Queue=0x...)
Step 3/5: Creating GPU command allocator...
Step 3/5: GPU command allocator created ✓ (Allocator=0x...)
Step 4/5: Creating GPU input/output textures...
Step 4/5: Input texture created ✓ (640x480, resource=0x...)
Step 4/5: Output texture created ✓ (1920x1080, resource=0x...)
Step 5/5: Creating FSR2 context...
  Context descriptor prepared: maxRender=640x480, maxUpscale=1920x1080
Step 5/5: FSR2 context created successfully ✓ (context=0x...)
================== FSR2 INITIALIZATION COMPLETE ✓ ==================

========================================
UPSCALER INITIALIZATION COMPLETE ✓
========================================
```

If you see this, GPU upscaling is working!

---

## Quick Debug Checklist

Before running the game, verify:

- [ ] Game folder location: `C:\Games\Fallout2CE` (or your chosen location)
- [ ] `fallout2-ce.exe` is in game folder
- [ ] `amd_fidelityfx_loader_dx12.dll` is in game folder  
- [ ] `amd_fidelityfx_upscaler_dx12.dll` is in game folder
- [ ] `data` folder exists in game folder
- [ ] `master.dat` exists in game folder
- [ ] You can navigate to game folder: `cd 'C:\Games\Fallout2CE'`
- [ ] Running PowerShell as Administrator (if permission issues)

---

## Running the Game

```powershell
# Navigate to game folder
cd 'C:\Games\Fallout2CE'

# Option 1: Run normally
.\fallout2-ce.exe

# Option 2: Run and capture console output
.\fallout2-ce.exe | Out-Host

# Option 3: Run and save to file
.\fallout2-ce.exe > console_output.log 2>&1

# Option 4: Run with detailed PowerShell logging
Start-Transcript -Path "game_transcript.log"
.\fallout2-ce.exe
Stop-Transcript

# After running, review logs:
# - console_output.log (if created with > redirection)
# - game_transcript.log (if using Start-Transcript)
# - Any .log files in current directory or temp folder
```

---

## Still Having Issues?

1. **Check the FSR2_DEBUG_LOGGING_GUIDE.md** for what each log message means
2. **Run from command line with output capture** to see any error messages
3. **Verify game data files are intact** - copy fresh from source if needed
4. **Check GPU driver version** - update to latest
5. **Try from different folder** - some locations have permission issues

Good luck! 🚀
