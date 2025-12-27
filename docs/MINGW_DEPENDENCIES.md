# MinGW Runtime Dependencies for libplacebo

When using the pre-built MinGW version of libplacebo, you need several runtime DLL files from the MinGW toolchain.

## Required DLLs

The following DLLs must be present in the same directory as `fallout2-ce.exe`:

- **libgcc_s_seh-1.dll** - GCC runtime library
- **libstdc++-6.dll** - C++ standard library
- **dovi.dll** - Dolby Vision library (used by libplacebo)
- **liblcms2-2.dll** - Little CMS color management library

## Option 1: Download Pre-built DLLs (Quickest)

Download the required DLLs from the MSYS2 package repository:

1. **gcc-libs** (for libgcc_s_seh-1.dll and libstdc++-6.dll):
   - Visit: https://packages.msys2.org/package/mingw-w64-x86_64-gcc-libs
   - Click "Files" to see the package contents
   - Download the package and extract `bin/libgcc_s_seh-1.dll` and `bin/libstdc++-6.dll`

2. **libdovi** (for dovi.dll):
   - Visit: https://packages.msys2.org/package/mingw-w64-x86_64-libdovi
   - Download and extract `bin/dovi.dll`

3. **lcms2**:
   - Visit: https://packages.msys2.org/package/mingw-w64-x86_64-lcms2
   - Download and extract `bin/liblcms2-2.dll`

Copy all 4 DLLs to: `build/Release/` (or wherever `fallout2-ce.exe` is located)

## Option 2: Install MSYS2 (Recommended for Developers)

If you're doing active development, installing MSYS2 is recommended:

1. Download and install MSYS2 from: https://www.msys2.org/

2. Open "MSYS2 MinGW 64-bit" terminal

3. Install the required packages:
   ```bash
   pacman -S mingw-w64-x86_64-gcc-libs mingw-w64-x86_64-libdovi mingw-w64-x86_64-lcms2
   ```

4. Copy DLLs from `C:\msys64\mingw64\bin\` to your build folder:
   - libgcc_s_seh-1.dll
   - libstdc++-6.dll
   - dovi.dll
   - liblcms2-2.dll

5. Or re-run the setup script:
   ```powershell
   .\tools\setup_libplacebo.ps1
   ```

## Option 3: Use MSVC-built libplacebo (Alternative)

If you want to avoid MinGW dependencies entirely:

1. Install libplacebo via vcpkg:
   ```bash
   vcpkg install libplacebo[vulkan,shaderc,lcms]:x64-windows
   ```

2. Configure CMake to use vcpkg:
   ```bash
   cmake -B build -DCMAKE_TOOLCHAIN_FILE=[vcpkg root]/scripts/buildsystems/vcpkg.cmake
   ```

## Troubleshooting

### Missing DLL Error on Startup

If you see errors about missing DLLs when launching the game:
1. Check that all 4 DLLs are in the same folder as `fallout2-ce.exe`
2. Verify the DLLs are 64-bit (x86_64) versions, not 32-bit
3. Make sure you downloaded the "mingw-w64-x86_64" packages, not "mingw-w64-i686"

### libplacebo Fails to Initialize

Check the `renderer.log` file in the game directory for detailed error messages.

## Notes

- The MinGW runtime DLLs are stable across versions, so you only need to download them once
- These DLLs are distributed under the GPLv3+ with runtime exception, making them safe to redistribute with your application
- Total size of all 4 DLLs is approximately 3-4 MB
