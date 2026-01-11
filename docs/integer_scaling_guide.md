# Integer Scaling Guide for Fallout 2 CE

## What is Integer Scaling?

Integer scaling is a **lossless pixel replication** technique perfect for preserving the original pixel art aesthetic of classic games like Fallout 2. Unlike ML-based upscalers (FSR2, DLSS, XeSS), integer scaling provides:

- ✅ **Zero blur** - Perfect pixel clarity
- ✅ **Zero artifacts** - No temporal ghosting, no aliasing
- ✅ **Perfect sharpness** - Original aesthetic preserved
- ✅ **Ultra-fast** - Simple pixel replication (CPU-based, negligible cost)
- ✅ **Deterministic** - Same output every time

## How It Works

Each source pixel (640x480) is replicated into an NxN block in the output:
- **2x scaling**: 640x480 → 1280x960 (each pixel becomes 2x2)
- **3x scaling**: 640x480 → 1920x1440 (each pixel becomes 3x3)  
- **4x scaling**: 640x480 → 2560x1920 (each pixel becomes 4x4)

This ensures **perfect pixel alignment** with no interpolation or smoothing.

## Configuration

Edit `fallout2.cfg` in your Fallout 2 installation directory:

```ini
[system]
; Upscaler mode selection:
;   0 = NONE (passthrough)
;   1 = FSR2 (temporal ML upscaling, any resolution)
;   2 = INTEGER_2X (640x480 -> 1280x960)
;   3 = INTEGER_3X (640x480 -> 1920x1440)
;   4 = INTEGER_4X (640x480 -> 2560x1920)
upscaler_mode=2

; Optional pre-scaling Kuwahara filter (edge-preserving color smoothing)
; Smooth colors BEFORE scaling while preserving sharp edges
; Great for making color gradients smoother and more natural
upscaler_kuwahara_enable=0            ; 0=off (default), 1=on
upscaler_kuwahara_radius=2            ; Smoothing strength (1-5, higher=smoother)

; Optional post-processing (applies to all modes):
upscaler_debanding=1              ; Reduce 8-bit palette color banding (0=off, 1=on)
upscaler_debanding_strength=0.5   ; Banding reduction strength (0.0-1.0)
upscaler_edge_smoothing=0         ; Optional pixel art anti-aliasing (0=off, 1=on)
upscaler_verbose_logging=0        ; Detailed diagnostics (0=off, 1=on)
```

## Choosing the Right Scale Factor

### For 1920x1080 Displays (Full HD):
**Recommended: INTEGER_2X (mode=2)**
- Output: 1280x960
- Scales to 1920x1080 with pillarboxing (black bars on sides)
- Perfect pixel sharpness

### For 2560x1440 Displays (QHD):
**Recommended: INTEGER_2X or INTEGER_3X**
- INTEGER_2X: 1280x960 → scales to 1920x1440 (pillarbox)
- INTEGER_3X: 1920x1440 → native resolution (no pillarbox!)

### For 3840x2160 Displays (4K):
**Recommended: INTEGER_3X (mode=3)**
- Output: 1920x1440
- Scales to 2880x2160 with pillarboxing
- Excellent pixel clarity

**Alternative: INTEGER_4X (mode=4)**
- Output: 2560x1920
- Very close to 4K native resolution
- Maximum pixel density

## Integer vs FSR2 Comparison

| Feature | Integer Scaling | FSR2 |
|---------|----------------|------|
| **Sharpness** | Perfect (1:1 pixels) | Good (RCAS sharpening) |
| **Artifacts** | Zero | Minimal (temporal ghosting) |
| **Blur** | Zero | Some (temporal accumulation) |
| **Performance** | Ultra-fast (CPU) | Fast (GPU compute) |
| **Resolution** | Fixed multiples only | Any resolution |
| **Best For** | Pixel art purists | Variable resolutions |

## Troubleshooting

### "Black bars on the sides of the screen"
This is **pillarboxing** and is intentional for integer scaling. It ensures perfect pixel alignment. Your monitor will automatically center the image.

### "Output looks blurry on my 4K monitor"
You may need to:
1. Increase scale factor (try INTEGER_3X or INTEGER_4X)
2. Disable monitor's built-in upscaling
3. Set Windows scaling to 100%

### "Game crashes on startup with integer scaling"
1. Check `upscale.log` in your Fallout 2 directory for errors
2. Try reducing scale factor (4X → 3X → 2X)
3. Ensure sufficient system RAM (integer scaling requires buffer allocation)

### "Want to disable integer scaling"
Set `upscaler_mode=1` for FSR2, or `upscaler_mode=0` to disable upscaling entirely.

## Advanced: Pre-Scaling Kuwahara Filter

The **Kuwahara filter** is an optional edge-preserving smoothing filter that runs **before** integer scaling to make colors smoother and more natural while keeping edges sharp.

### What It Does

```
Without Kuwahara:  Input (640x480 with distinct colors) → Integer Scale → Sharp edges, distinct colors
With Kuwahara:     Input → Smooth colors (preserve edges) → Integer Scale → Smooth gradients + sharp edges
```

The Kuwahara algorithm:
1. Examines a small window around each pixel
2. Divides the window into 4 quadrants
3. Computes the average color of each quadrant
4. Selects the average **closest to the original pixel** (preserves edges)
5. Uses that average as the output color

**Result**: Flat color regions become smoother, but edge boundaries stay crisp.

### When to Use

✅ Enable Kuwahara if:
- Colors look too harsh or artificial
- You want smoother color gradients (especially from palette transitions)
- You prefer a "softer" aesthetic while keeping pixel sharpness

❌ Disable Kuwahara if:
- You want 100% authentic original appearance
- Colors are already smooth enough
- Performance is critical (adds ~1-2ms per frame)

### Configuration

```ini
upscaler_kuwahara_enable=1        ; Enable pre-scaling smoothing
upscaler_kuwahara_radius=2        ; Smoothing strength (1-5)
```

**Radius guide:**
- `radius=1`: Minimal smoothing (subtle effect)
- `radius=2`: Default - good balance (recommended)
- `radius=3`: Moderate smoothing (noticeable)
- `radius=4-5`: Aggressive smoothing (may blur details)

### Performance

- **CPU cost**: ~1-2ms per frame (depends on radius)
- **Memory**: Temporary buffer (~1.2 MB for input size)
- **Quality**: No loss - smoothing is lossless

### Example Configurations

**Maximum Sharpness (No Kuwahara)**
```ini
upscaler_mode=3
upscaler_kuwahara_enable=0
upscaler_debanding=1
upscaler_edge_smoothing=0
```

**Balanced (Recommended)**
```ini
upscaler_mode=3
upscaler_kuwahara_enable=1
upscaler_kuwahara_radius=2
upscaler_debanding=1
upscaler_edge_smoothing=0
```

**Maximum Smoothness**
```ini
upscaler_mode=3
upscaler_kuwahara_enable=1
upscaler_kuwahara_radius=3
upscaler_debanding=1
upscaler_edge_smoothing=1
upscaler_edge_smoothing_strength=0.5
```

---



### Combining with Post-Processing

Integer scaling works great with optional post-processing:

```ini
upscaler_mode=2                   ; Integer 2x scaling
upscaler_debanding=1              ; Reduce color banding from 8-bit palette
upscaler_debanding_strength=0.7   ; Aggressive banding reduction
upscaler_edge_smoothing=0         ; Keep pure pixels (recommended)
```

**Note**: Edge smoothing is generally **not recommended** for integer scaling, as it defeats the purpose of perfect pixel replication. Leave it disabled unless you want a slight softening effect.

### Verbose Logging

Enable detailed diagnostics to verify integer scaling is working:

```ini
upscaler_verbose_logging=1
```

Check `upscale.log` in your Fallout 2 directory. You should see:
```
INTEGER SCALING INITIALIZATION COMPLETE ✓
Benefits: Zero blur, zero artifacts, perfect sharpness
INTEGER SCALE DISPATCH (Frame 0, Factor: 2x)
```

## Performance Notes

Integer scaling is **extremely lightweight**:
- **CPU cost**: <1ms per frame (simple memory copy + pixel replication)
- **GPU cost**: Zero (no GPU compute required)
- **Memory cost**: Input buffer (640x480) + output buffer (depends on scale factor)

For reference:
- INTEGER_2X: ~5 MB total
- INTEGER_3X: ~11 MB total
- INTEGER_4X: ~19 MB total

## Recommendations

**For the authentic Fallout 2 experience**, we recommend:
```ini
upscaler_mode=3                   ; Integer 3x (1920x1440)
upscaler_kuwahara_enable=1        ; Smooth colors naturally
upscaler_kuwahara_radius=2        ; Balanced smoothing
upscaler_debanding=1              ; Reduce palette banding
upscaler_edge_smoothing=0         ; Keep pure pixels
```

This configuration provides:
- Perfect pixel-art preservation with integer scaling
- Naturally smooth color gradients (Kuwahara filter)
- Smooth palette transitions (debanding)
- Zero blur or artifacts
- Maximum authenticity with modern visual refinement

## See Also

- [FSR2 Configuration Guide](../README.md#upscaling) - Alternative ML-based upscaling
- [Coordinate System](coordinate_system.md) - Understanding Fallout 2's rendering
- [Asset Architecture](asset_architecture.md) - How game assets are rendered
