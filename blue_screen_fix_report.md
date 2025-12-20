I have fixed the "blue screen" issue by ensuring the game texture matches the upscaler's output resolution.

### Root Cause
The "blue screen" (or black screen with missing content) was caused by a resolution mismatch:
1.  The game renderer created the main texture at **640x480** because the upscaler wasn't initialized yet.
2.  The Anime4K upscaler initialized successfully (after my previous fix) and started outputting frames at **2560x1440**.
3.  When the game tried to copy the 2560x1440 upscaled frame into the 640x480 texture, the operation failed or produced invalid results, leading to a blank screen.

### Fix Implementation
I modified `src/svga.cc` to add a texture recreation step immediately after upscaler initialization:
*   After `upscalerInit` returns success, the code now checks if the upscaler is active.
*   If active, it **destroys the old 640x480 texture** and **creates a new one at the full physical resolution** (e.g., 2560x1440).
*   This ensures the texture is large enough to hold the Anime4K output.

### Verification
When you run the game now:
1.  Anime4K will initialize (ignoring the spurious error).
2.  The log will show: `[SVGA] Upscaler active - recreating texture at physical resolution: 2560x1440`.
3.  The game should display the menu correctly with crisp Anime4K upscaling.
