# Black Level Enhancement Guide

## Overview
The HDR filter now includes advanced black level controls to provide **vivid bright lights with deep black levels** optimized for OLED and HDR displays.

## Problem Solved
After upscaling and filtering, some colors that should be black can appear "lifted" (grayish) due to:
1. Upscaling interpolation introducing intermediate values
2. Filter processing that brightens dark pixels
3. Contrast adjustments that lift the black floor

## New Features

### 1. Configurable Black Crush Threshold
**Config Key:** `upscaler_black_crush_threshold`  
**Range:** 0.0 - 0.15  
**Default:** 0.03  

Controls the luminance level below which colors are converted to pure black (RGB 0,0,0).
- Lower values (0.01-0.02): Only crush very dark pixels, preserves more shadow detail
- Default (0.03): Balanced - converts near-blacks while preserving intentional dark grays
- Higher values (0.04-0.06): Aggressive - ensures deep OLED blacks but may lose shadow detail

**Luminance Scale Reference:**
- 0.00 = Pure black (RGB 0,0,0)
- 0.03 = Roughly RGB(8,8,8) - barely visible on most displays
- 0.05 = Roughly RGB(13,13,13) - slightly visible gray
- 0.10 = Clearly visible dark gray

### 2. Black Crush Strength
**Config Key:** `upscaler_black_crush_strength`  
**Range:** 0.0 - 2.0  
**Default:** 1.0  

Controls how aggressively colors just above the black threshold are darkened.
- 0.0: Disabled - no darkening of near-blacks
- 1.0: Natural - smooth power curve darkening
- 1.5-2.0: Aggressive - strongly pushes near-blacks toward black

This creates a smooth transition zone where colors between `threshold` and `threshold × 3` are progressively darkened, preventing a harsh cutoff.

### 3. Improved Contrast Handling
The contrast adjustment now uses a **dual-mode system**:
- **Above near-black range:** Full contrast applied (creates vivid bright lights)
- **Within near-black range:** Reduced contrast (prevents lifting blacks)

This ensures bright colors pop while maintaining deep blacks.

## Configuration Examples

### Balanced (Default - Recommended)
```ini
[system]
upscaler_hdr_enable=1
upscaler_hdr_strength=0.5
upscaler_hdr_saturation=1.2
upscaler_hdr_contrast=1.1
upscaler_black_crush_threshold=0.03
upscaler_black_crush_strength=1.0
```

### OLED Optimized (Deep Blacks)
Perfect for OLED displays where you want absolute blacks:
```ini
[system]
upscaler_hdr_enable=1
upscaler_hdr_strength=0.6
upscaler_hdr_saturation=1.3
upscaler_hdr_contrast=1.2
upscaler_black_crush_threshold=0.04
upscaler_black_crush_strength=1.5
```

### Vivid Colors with Moderate Blacks
For LCD displays or if you prefer more shadow detail:
```ini
[system]
upscaler_hdr_enable=1
upscaler_hdr_strength=0.7
upscaler_hdr_saturation=1.4
upscaler_hdr_contrast=1.3
upscaler_black_crush_threshold=0.02
upscaler_black_crush_strength=0.8
```

### Cinema Mode (Preserve Shadow Detail)
For atmospheric scenes where shadow detail is important:
```ini
[system]
upscaler_hdr_enable=1
upscaler_hdr_strength=0.4
upscaler_hdr_saturation=1.1
upscaler_hdr_contrast=1.0
upscaler_black_crush_threshold=0.015
upscaler_black_crush_strength=0.5
```

## Technical Details

### Processing Pipeline
1. **Saturation Boost** - Applied first to enhance color vividness
2. **Luminance Check** - Calculate brightness of each pixel
3. **Absolute Black Crush** - Convert colors below threshold to pure black
4. **Black Point Adjustment** - Progressively darken near-blacks using power curve
5. **Dual-Mode Contrast** - Apply full contrast to bright colors, reduced contrast to near-blacks
6. **Clamping** - Ensure valid RGB range (0-255)

### Black Point Adjustment Algorithm
```
For pixels with luminance between threshold and threshold×3:
  blackProximity = (luminance - threshold) / (threshold × 2)
  darkeningFactor = 1 - pow(1 - blackProximity, 2 / blackCrushStrength)
  RGB *= darkeningFactor
```

This creates a smooth darkening curve that:
- Strongly darkens pixels just above threshold
- Gradually reduces effect as luminance increases
- Prevents visible banding or harsh transitions

## Testing Your Settings

1. **Enable HDR in fallout2-ce.cfg:**
   ```ini
   [system]
   upscaler_hdr_enable=1
   ```

2. **Start with defaults and adjust incrementally**

3. **Test in dark areas** - Look at shadows and dark UI elements

4. **Test in bright areas** - Ensure highlights are vivid without clipping

5. **Check for banding** - Look for visible steps in dark gradients

## Troubleshooting

### Blacks look gray/lifted
- Increase `upscaler_black_crush_threshold` to 0.04-0.05
- Increase `upscaler_black_crush_strength` to 1.3-1.5

### Lost shadow detail
- Decrease `upscaler_black_crush_threshold` to 0.02 or lower
- Decrease `upscaler_black_crush_strength` to 0.7-0.8

### Colors not vivid enough
- Increase `upscaler_hdr_saturation` to 1.3-1.5
- Increase `upscaler_hdr_contrast` to 1.2-1.4
- Increase `upscaler_hdr_strength` to 0.6-0.8

### Visible banding in dark areas
- Ensure debanding is enabled
- Adjust `upscaler_black_crush_threshold` in smaller increments (0.005)

## See Also
- [FSR Integration Guide](fsr_integration_guide.md) - Upscaling configuration
- [Rendering Pipeline Documentation](RENDERING_PIPELINE_DOCUMENTATION.md) - Full pipeline overview
