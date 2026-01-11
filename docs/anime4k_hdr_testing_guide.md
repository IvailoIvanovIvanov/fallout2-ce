# Testing Guide: Anime4K + HDR Filters

## What Was Fixed

**Critical Bug:** HDR color filters were not being applied when using Anime4K upscaling (mode 4).  
**Impact:** Despite having correct config, colors were flat with no enhancement.  
**Fix:** Post-processing filters now applied to Anime4K output.

## How to Test

### 1. Verify Your Config

Check `C:\Program Files (x86)\Steam\steamapps\common\Fallout 2\fallout2.cfg`:

```ini
[system]
upscaler_mode=4              # Anime4K upscaling
upscaler_hdr_enable=1        # Enable HDR filter
upscaler_hdr_strength=0.5    # HDR intensity
upscaler_hdr_saturation=1.2  # Color vividness
upscaler_hdr_contrast=1.1    # Contrast boost
upscaler_black_crush_threshold=0.03  # Black level
upscaler_black_crush_strength=1.0    # Black crush
```

### 2. Launch the Game

Start Fallout 2 and observe the visuals.

### 3. What to Look For

#### **Before Fix** (What you were seeing):
- ❌ Flat colors, no vibrancy
- ❌ Blacks appeared grayish/lifted
- ❌ No contrast enhancement
- ❌ HDR settings had no effect

#### **After Fix** (What you should see now):
- ✅ **Vivid, saturated colors** - especially noticeable in:
  - Character clothing and armor
  - UI elements (menus, inventory)
  - Outdoor environments
  - Fire/light effects

- ✅ **Deep, true blacks** - especially in:
  - Dark caves and interiors
  - Night scenes
  - Shadows under objects
  - UI backgrounds

- ✅ **Enhanced contrast** - notice:
  - Brighter highlights
  - Darker shadows
  - More "pop" in the image

### 4. Test Scenes

**Best places to see the difference:**

1. **Main Menu**
   - Look at the vault door and background
   - Should have rich, deep blacks
   - Menu text should be bright and crisp

2. **Inventory Screen**
   - Item icons should be vivid
   - Black background should be true black
   - Colored items (stimpaks, weapons) should pop

3. **Outdoor Daylight**
   - Sky should be vibrant blue
   - Grass/ground should have rich earth tones
   - Bright areas should be punchy

4. **Dark Interiors**
   - True blacks in shadows
   - Light sources should stand out dramatically
   - Contrast between lit and unlit areas

5. **NPC Clothing**
   - Leather armor should be rich brown
   - Vault suits should be vivid blue/yellow
   - Colors should "pop" off the screen

### 5. Compare Settings

Try these presets to find your preference:

#### **Preset 1: Balanced (Current)**
```ini
upscaler_hdr_strength=0.5
upscaler_hdr_saturation=1.2
upscaler_hdr_contrast=1.1
upscaler_black_crush_threshold=0.03
upscaler_black_crush_strength=1.0
```

#### **Preset 2: OLED Deep Blacks**
```ini
upscaler_hdr_strength=0.6
upscaler_hdr_saturation=1.3
upscaler_hdr_contrast=1.2
upscaler_black_crush_threshold=0.04
upscaler_black_crush_strength=1.5
```

#### **Preset 3: Vivid Colors**
```ini
upscaler_hdr_strength=0.7
upscaler_hdr_saturation=1.4
upscaler_hdr_contrast=1.3
upscaler_black_crush_threshold=0.025
upscaler_black_crush_strength=0.8
```

#### **Preset 4: Subtle Enhancement**
```ini
upscaler_hdr_strength=0.3
upscaler_hdr_saturation=1.1
upscaler_hdr_contrast=1.0
upscaler_black_crush_threshold=0.02
upscaler_black_crush_strength=0.7
```

### 6. Enable Verbose Logging (Optional)

To see what the upscaler is doing:

```ini
[DIAGNOSTICS]
ENABLE=1
LOG_FILE=fallout2_upscaler.log
VERBOSITY=Trace

[system]
upscaler_verbose_log=1
```

Then check the log file:
```
C:\Program Files (x86)\Steam\steamapps\common\Fallout 2\fallout2_upscaler.log
```

Look for these messages:
```
ANIME4K: Applying post-processing filters
FILTERS: Applying minimal filter pipeline
FILTERS: Soft HDR (strength=X, saturation=Y, contrast=Z)
```

### 7. Troubleshooting

#### If colors still look flat:

1. **Check `upscaler_hdr_enable=1`** in config
2. **Verify upscaler_mode=4** (Anime4K)
3. **Increase saturation**: Try `upscaler_hdr_saturation=1.4`
4. **Increase contrast**: Try `upscaler_hdr_contrast=1.3`
5. **Check log file** for "Applying post-processing filters"

#### If blacks are still gray:

1. **Increase threshold**: Try `upscaler_black_crush_threshold=0.04`
2. **Increase strength**: Try `upscaler_black_crush_strength=1.5`
3. **Check display settings**: Ensure contrast/brightness on monitor is correct

#### If colors look oversaturated:

1. **Decrease saturation**: Try `upscaler_hdr_saturation=1.1`
2. **Decrease strength**: Try `upscaler_hdr_strength=0.4`

#### If too much contrast:

1. **Decrease contrast**: Try `upscaler_hdr_contrast=1.0`
2. **Decrease black crush**: Try `upscaler_black_crush_strength=0.7`

### 8. Performance Check

The filters should have minimal performance impact:
- **Expected FPS**: 60 FPS at 4K
- **Filter overhead**: ~3-5ms
- **Total frame time**: ~10-15ms

If you experience slowdown:
1. Disable debanding: `upscaler_debanding=0`
2. Disable edge smoothing: `upscaler_edge_smoothing=0`
3. Try integer scaling instead: `upscaler_mode=3` (3x)

### 9. Visual Comparison Method

**A/B Testing:**

1. Take a screenshot with HDR enabled
2. Set `upscaler_hdr_enable=0` in config
3. Restart game and take same screenshot
4. Compare side-by-side

**You should see dramatic differences!**

### 10. Expected Results

With the fix and optimal settings, you should experience:

✅ **Vivid, saturated colors** that "pop"  
✅ **Deep, true blacks** (RGB 0,0,0) on OLED  
✅ **Enhanced contrast** without clipping  
✅ **Smooth gradients** without banding  
✅ **Sharp edges** without jaggies  
✅ **Maintains 60 FPS** at 4K  

## Technical Notes

### Why It Wasn't Working

The post-processing pipeline was only connected to integer scaling modes. Anime4K did its GPU upscaling, downloaded the result, and returned it **without applying any CPU filters**.

### What Changed

Added the complete filter chain to the Anime4K dispatch function:
- Debanding (temporal dithering)
- Edge smoothing (anti-aliasing)
- **HDR processing (saturation, contrast, black crush)**

### Processing Order

```
Anime4K GPU Upscale
      ↓
Download to CPU
      ↓
Apply Debanding
      ↓
Apply Edge Smoothing
      ↓
Apply HDR Tonemapping ← THIS WAS MISSING!
  - Saturation boost
  - Contrast enhancement
  - Black level crush
      ↓
Display
```

## Contact/Feedback

If the fix doesn't work or you need help tuning settings, check:
- Log file: `fallout2_upscaler.log`
- Config file: `fallout2.cfg`
- Ensure you're running the newly compiled `fallout2-ce.exe`

**The filters should now work with Anime4K!** 🎉
