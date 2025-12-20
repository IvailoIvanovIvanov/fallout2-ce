The game crash has been resolved by implementing a robust renderer recovery system.

### Root Cause Analysis
The logs confirmed that the Anime4K upscaler correctly detected the GPU device removal (`0x887A0001`) and fell back to `INTEGER_3X` (CPU mode). However, the game continued to crash because the main SDL renderer (`gSdlRenderer`) was created *before* the upscaler initialization and shared the same underlying GPU adapter. When the GPU driver reset (TDR), the SDL renderer became invalid ("zombie state"), causing the game to crash when it tried to present the first frame.

### Fix Implementation
I have applied a multi-layer fix to handle this scenario gracefully:

1.  **Device Validation (`src/gpu_device.cc`)**:
    *   Added a "probe" test inside `gpuDeviceInit`. It now attempts to create a command allocator immediately. If this fails, it marks the adapter as broken and tries the next one, preventing the system from initializing with a dead device.

2.  **State Cleanup (`src/upscaler.cc`)**:
    *   Modified `initAnime4k` to call `gpuDeviceShutdown()` immediately upon detecting a device removal. This ensures the global `gpuDeviceIsReady()` flag correctly reflects the hardware state.

3.  **Renderer Recovery (`src/svga.cc`)**:
    *   Added a critical check in `_GNW95_init_window` after `upscalerInit`.
    *   If the GPU device is reported as "not ready" (indicating it died during init), the code now:
        1.  Logs the failure ("GPU device lost during upscaler init").
        2.  **Destroys the broken renderer.**
        3.  **Forces a fallback to Direct3D 11** (via `SDL_HINT_RENDER_DRIVER`) to ensure stability.
        4.  **Recreates the renderer** from scratch.

### Verification
When you start the game now:
1.  The upscaler will still fail and fallback (as seen in logs).
2.  The system will detect the dead GPU device.
3.  The game will automatically restart the renderer in safe mode.
4.  The game should launch successfully using the CPU upscaler.

Check `game_startup.log` or the console for the message:
`[SVGA] Renderer recreated successfully (fallback mode)`
