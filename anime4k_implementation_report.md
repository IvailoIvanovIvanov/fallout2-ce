I have implemented the full Anime4K upscaler and fixed the initialization crash.

### Changes Implemented

1.  **Crash Fix (D3D12 Format Mismatch)**:
    *   The crash `DXGI_ERROR_INVALID_CALL` (0x887A0001) was caused by a mismatch between the texture resource format (`DXGI_FORMAT_B8G8R8A8_UNORM`) and the Shader Resource View / Unordered Access View format (`DXGI_FORMAT_R8G8B8A8_UNORM`).
    *   I updated `src/upscaler.cc` to use `DXGI_FORMAT_B8G8R8A8_UNORM` for both SRV and UAV creation, ensuring compatibility with the underlying resources.

2.  **Full Anime4K Implementation**:
    *   Replaced the placeholder "stub" shader (which was just a simple bilinear scaler) with the full Anime4K algorithm from `src/shaders/anime4k.hlsl`.
    *   The new shader includes:
        *   Luminance-based edge detection (Sobel operator).
        *   Edge-aware sharpening to preserve pixel art crispness.
        *   Unsharp masking kernel.
    *   Embedded the shader code directly into `src/upscaler.cc` to ensure it works reliably without depending on external file copying during the build process.

### Verification
When you run the game now:
1.  The Anime4K initialization should succeed (no more "Device has been removed" errors).
2.  The log will show `Step 3/3: Initializing Anime4K pipeline` (without "stub fallback").
3.  The visual output should show the Anime4K effect (sharpened edges) instead of the fallback integer scaling.

You can check `upscale.log` to confirm:
```
[UPSCALER] Step 5.8/6: Device still ready after GPU sync ✓
[UPSCALER] Step 6/6: Creating command allocator and list...
[UPSCALER] ANIME4K INITIALIZATION COMPLETE ✓
```
