# Fallout 2 CE - Upscaler Rendering Pipeline Analysis

## GOAL
Display 640x480 game content → Kuwahara filter → Integer 3x scaling (1920x1440) → Display on 2560x1440 monitor with 4:3 aspect ratio and black letterbox bars

## CURRENT RENDERING PIPELINE (Step-by-Step)

### STEP 1: Upscaler Initialization (createRenderer in svga.cc)
**Location:** `src/svga.cc` line ~850-880

**Current Code Path:**
```cpp
UpscalerMode configuredMode = upscalerGetConfiguredMode();  // Returns INTEGER_3X
int outputWidth = 640 * 3 = 1920;
int outputHeight = 480 * 3 = 1440;
upscalerInit(640, 480, 1920, 1440, UpscalerMode::FSR2);
```

**Expected Result:**
- Input buffer: 640x480 (RGBA)
- Output buffer: 1920x1440 (RGBA)

**PROBLEM:** The configuration is loaded INSIDE upscalerInit(), which might override these dimensions.

---

### STEP 2: Upscaler Configuration Loading (upscaler.cc init)
**Location:** `src/upscaler.cc` line ~997-1020

**Current Code Path:**
```cpp
int init(int inputW, int inputH, int outputW, int outputH, UpscalerMode mode) {
    loadConfiguration();  // Reads config, sets mConfiguredMode = INTEGER_3X
    mode = mConfiguredMode;  // Overrides passed mode
    
    // ISSUE: outputW and outputH are ALREADY SET to 1920x1440 from caller
    // But they should match what integer scaling needs
    mOutputWidth = outputW;   // 1920
    mOutputHeight = outputH;  // 1440
}
```

**Expected Result:**
- mOutputWidth = 1920
- mOutputHeight = 1440
- mMode = INTEGER_3X

---

### STEP 3: Game Frame Rendering (renderPresent in svga.cc)
**Location:** `src/svga.cc` line ~2265-2370

**Current Code Path:**
```cpp
// 1. Get game buffer from SDL surface
unsigned char* gameBuffer = gSdlSurface->pixels;  // 640x480 indexed color

// 2. Convert palette
uint32_t palette[256];  // SDL_Color → ARGB8888

// 3. Upload to upscaler
upscalerSetIndexedInput(gameBuffer, palette);

// 4. Run upscaler (Kuwahara + integer scale)
upscalerDispatch();

// 5. Get result
const uint32_t* upscaledBuffer = upscalerGetOutputBuffer();
int upscaledWidth, upscaledHeight;
upscalerGetOutputDimensions(upscaledWidth, upscaledHeight);  // Should be 1920x1440
```

**Expected Result:**
- upscaledBuffer contains 1920x1440 RGBA pixels
- upscaledWidth = 1920
- upscaledHeight = 1440

---

### STEP 4: Texture Upload (renderPresent in svga.cc)
**Location:** `src/svga.cc` line ~2360-2375

**Current Code Path:**
```cpp
// Upload to texture
SDL_Rect uploadRect;
uploadRect.x = 0;
uploadRect.y = 0;
uploadRect.w = upscaledWidth;   // 1920
uploadRect.h = upscaledHeight;  // 1440
SDL_UpdateTexture(gSdlTexture, &uploadRect, upscaledBuffer, upscaledWidth * 4);
```

**PROBLEM:** What is the actual size of gSdlTexture?
- Texture is created with presenterWidth x presenterHeight
- Need to verify: Is texture 2560x1440 or something else?

**Expected Result:**
- 1920x1440 data uploaded to top-left of texture
- Texture dimensions need investigation

---

### STEP 5: Source Rectangle Definition (renderPresent in svga.cc)
**Location:** `src/svga.cc` line ~2390-2405

**Current Code Path:**
```cpp
SDL_Rect srcRect;
srcRect.x = 0;
srcRect.y = 0;
srcRect.w = actualUpscaledWidth;   // 1920
srcRect.h = actualUpscaledHeight;  // 1440
```

**Expected Result:**
- SDL will read 1920x1440 pixels from texture starting at (0,0)

---

### STEP 6: Destination Rectangle Calculation (renderPresent in svga.cc)
**Location:** `src/svga.cc` line ~2490-2525

**Current Code Path:**
```cpp
const int physicalWidth = screenGetPhysicalWidth();    // 2560
const int physicalHeight = screenGetPhysicalHeight();  // 1440

destRect.w = actualUpscaledWidth;   // 1920
destRect.h = actualUpscaledHeight;  // 1440
destRect.x = (2560 - 1920) / 2 = 320;  // Black bars on sides
destRect.y = (1440 - 1440) / 2 = 0;
```

**Expected Result:**
- Display 1920x1440 content at 1:1 pixel scale
- Centered horizontally with 320px black bars on each side
- Perfect 4:3 aspect ratio

---

### STEP 7: SDL Rendering (after destRect calculation)
**Location:** `src/svga.cc` line ~2550+

**Current Code Path:**
```cpp
SDL_RenderCopy(gSdlRenderer, gSdlTexture, &srcRect, &destRect);
```

**Expected Behavior:**
- Copy srcRect (1920x1440) from texture
- Scale/stretch to destRect (1920x1440 at position 320,0)
- Should be 1:1, no scaling needed

---

## IDENTIFIED ISSUES

### Issue #1: Texture Dimensions Unknown
**Problem:** We don't know if gSdlTexture is 2560x1440 or some other size
**Impact:** If texture is wrong size, upload or rendering will fail
**Investigation Needed:** Check texture creation in createRenderer()

### Issue #2: Missing Logging
**Problem:** No [RENDER] logs appearing - dimension logging isn't executing
**Impact:** Can't verify actual values being used
**Fix:** Add logging that ALWAYS executes on first frame

### Issue #3: displayScalerGetPhysicalViewport() Interference
**Problem:** The viewport calculation might be overriding our destRect
**Impact:** Could be causing zoom/stretch
**Investigation Needed:** Check if viewport is applied AFTER our destRect

### Issue #4: Presenter Bounds Mismatch
**Problem:** getPresenterSurfaceBounds() might not match our expectations
**Impact:** srcRect could be wrong size
**Investigation Needed:** Log presenter bounds

---

## PROPOSED FIX PLAN

### Fix 1: Add Comprehensive Logging
Add logging at EVERY step to trace actual values:
- Texture dimensions at creation
- Upscaler output dimensions
- srcRect values
- destRect values
- Presenter bounds
- Physical display size

### Fix 2: Verify Texture Size
Ensure gSdlTexture matches what we expect (should be large enough for upscaled content)

### Fix 3: Trace Viewport Override
Check if displayScalerGetPhysicalViewport() or other code is overriding our destRect

### Fix 4: Test with Simpler Path
Temporarily bypass display scaler and render directly to verify upscaler works

---

## NEXT STEPS

1. Add comprehensive logging to trace all dimensions
2. Rebuild and run to capture actual values
3. Compare expected vs actual at each step
4. Identify where the mismatch occurs
5. Apply targeted fix
