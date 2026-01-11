# Asset Architecture

## Overview

The Fallout 2 CE engine maintains two separate asset caches to support the dual-display abstraction:

- **Phantom Cache (8-bit):** `gArtCache` in `art.cc` stores indexed FRM data from disk (640×480 logical space)
- **Real Cache (HD):** `RenderAssetRegistry` owned by `RealDisplay` stores high-resolution RGBA assets

This separation ensures the phantom display (game logic) operates on original 8-bit data while the real display (HD rendering) uses upscaled/replaced assets.

## Cache Ownership

### Phantom Cache (gArtCache)

**File:** `src/art.cc`  
**Type:** `Cache` (generic LRU cache from `cache.h`)  
**Contents:** 8-bit indexed FRM frames loaded from `master.dat/critter.dat`

**Lifetime:**
- Initialized in `artInit()` with configurable size (default 8MB)
- Entries cached via `cacheLock()` and released via `cacheUnlock()`
- Flushed on map transitions via `artCacheFlush()`
- Destroyed in `artReset()`

**Access Pattern:**
- `artLock(fid)` → returns `Art*` header
- `artLockFrameData(fid, frame, rotation)` → returns `unsigned char*` to indexed pixels
- All returned pointers point to 8-bit palette indices (0-255)

**Guarantee:** `gArtCache` NEVER contains RGBA or HD data.

### Real Cache (RenderAssetRegistry)

**File:** `src/render_asset_registry.h/.cc`  
**Owner:** `RealDisplay::assetRegistry_` (instance member)  
**Type:** `RenderAssetRegistry` (class with internal `Impl`)

**Contents:**
- `RenderAssetMetadata`: maps `RenderAssetKey` (fid, frame, rotation, variant) to HD views
- `HdTrueColorFrameView`: RGBA pixel data + scale/alignment metadata
- Fallback storage: auto-upscaled RGBA from indexed frames when HD missing

**Lifetime:**
- Constructed with `RealDisplay` instance
- Entries tracked via `trackFrame()` when render commands reference assets
- HD views attached via `attachHdView()` (called by `artRegisterTrueColorFrameData`)
- Detached via `detachHdView()` (called by `artUnregisterTrueColorFrameData`)
- Cleared on reset via `reset()`

**Access Patterns:**
1. **Handle-based (primary):** `getHdView(RenderAssetHandle)` - used by orchestrator
2. **Pointer-based (legacy):** `getHdViewByPointer(indexed)` - used by `artLookupRegisteredTrueColorFrame`

**Guarantee:** `RenderAssetRegistry` ONLY contains RGBA/HD data, never indexed.

## Data Flow

### Loading an Asset

```
1. Game requests FRM: artLock(fid) 
   → gArtCache loads from disk (8-bit indexed)
   
2. Game blits frame: blitBufferToBuffer(frameData, ...)
   → Phantom display receives indexed pixels
   → Render command emitted: RenderCommandObjectBlit{handle, ...}
   
3. Orchestrator processes command:
   → Queries RenderAssetRegistry::getHdView(handle)
   → If found: composite HD RGBA
   → If missing: fallback to upscaled indexed
```

### Registering HD Assets

```
1. HD loader detects PNG: artLoadHdFrame(fid, frame, rotation)
   → Loads PNG from data/art/hd/
   → Decodes to RGBA buffer
   
2. Registration: artRegisterTrueColorFrameData(indexed, rgba, w, h)
   → Stores in gHdTrueColorFrameStorage (managed by art.cc)
   → Delegates to renderAssetRegistryAttachHdView(indexed, view)
   → RenderAssetRegistry validates dimensions, stores view
   
3. Tracking: renderAssetRegistryTrackFrame(handle, owner, indexed, w, h)
   → Called when orchestrator sees new asset reference
   → Creates RenderAssetMetadata entry
   → Associates indexed pointer → handle mapping
```

## Fallback Strategy

When an HD asset is missing or fails validation:

1. **Validation:** `conformHdView()` checks dimension parity with indexed frame
2. **Fallback Generation:** `ensureFallback()` auto-upscales indexed → RGBA
   - Iterates indexed pixels, converts via `paletteIndexToArgb()`
   - Stores in `metadata.fallbackStorage` (managed by registry)
   - Marks as fallback: `metadata.hdIsFallback = true`
3. **Usage:** Orchestrator treats fallback identically to native HD assets
4. **Replacement:** If HD PNG loads later, fallback is discarded

**Diagnostics:** Fallback events logged at `Trace` level:
```
"SCALER" "asset_registry fallback fid=%u frame=%u rot=%u variant=%u"
```

## Pointer vs Handle Lookups

### Handle-Based (Preferred)

**Method:** `RenderAssetRegistry::getHdView(RenderAssetHandle)`  
**Used By:** `render_display_orchestrator.cc`  
**Advantages:**
- Direct O(1) lookup via `(fid, frame, rotation, variant)` key
- No dependency on indexed pointer validity
- Clean separation: handles are logical identifiers

### Pointer-Based (Legacy)

**Method:** `RenderAssetRegistry::getHdViewByPointer(unsigned char*)`  
**Used By:** `artLookupRegisteredTrueColorFrame()`, `artGetTrueColorFrame()`  
**Mechanism:**
- Looks up `PointerMap` (indexed → key)
- Then queries `AssetMap` (key → metadata)
**Limitations:**
- Requires indexed pointer still valid (cache not flushed)
- Two-step indirection
- Exists for backward compatibility with pre-orchestrator code

**Migration Path:** Phase 5 will deprecate pointer-based lookups when orchestrator becomes mandatory.

## Lifetime Management

### Asset Tracking by Owner

```cpp
renderAssetRegistryTrackFrame(handle, owner, indexed, w, h);
```

**Owner:** Typically cache entry or art object owning the indexed data.

**Release:**
```cpp
renderAssetRegistryReleaseFramesForOwner(owner);
```
- Invalidates `indexed` pointer associations
- Clears non-fallback HD views (fallback persists until explicit detach)
- Allows cache to reuse memory without dangling references

### HD View Lifecycle

```
Attach:  artRegisterTrueColorFrameData(indexed, rgba, ...)
         → renderAssetRegistryAttachHdView(indexed, view, false)
         → Registry stores view, validates dimensions
         
Detach:  artUnregisterTrueColorFrameData(indexed)
         → renderAssetRegistryDetachHdView(indexed)
         → Registry clears view (unless fallback)
```

## Dual Registry Consolidation (Phase 4)

**Before Phase 4:**
- `gHdTrueColorFrameRegistry` (art.cc) - simple `unordered_map<indexed, view>`
- `gAssets/gPointerToAsset/gOwnerToPointers` (render_asset_registry.cc) - rich metadata

**After Phase 4:**
- `gHdTrueColorFrameRegistry` **removed**
- `RenderAssetRegistry` is **single source of truth**
- `artRegisterTrueColorFrameData` delegates to `renderAssetRegistryAttachHdView`
- `artLookupRegisteredTrueColorFrame` delegates to `renderAssetRegistryGetHdViewByPointer`

**Benefits:**
- No synchronization bugs between parallel registries
- Fallback logic centralized in registry
- Clear ownership: `RealDisplay` owns HD cache

## Configuration Flags

```ini
[system]
virtual_adapter=1              ; Enable dual-display architecture
virtual_adapter_fullres=1      ; Use physical resolution for real display
render_display_orchestrator=1  ; Enable command bus + HD compositor
```

When orchestrator disabled, pointer-based lookups still function for legacy direct-blit paths.

## Future Work (Phase 5+)

- **Lazy Loading:** Load HD PNGs on-demand when first referenced in render command
- **Async Queue:** Background thread for HD asset decoding
- **Preload Hints:** Load commonly-used HD assets (UI, HUD) at map start
- **Deprecate Pointer Lookups:** Remove `getHdViewByPointer` when orchestrator mandatory
