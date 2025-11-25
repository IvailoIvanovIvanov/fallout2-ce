# Virtual Adapter Roadmap

> Because sometimes the wasteland deserves UHD without angering the ancient 640x480 spirits.

## Mission Statement (Why This Exists)
We’re building a virtual 640x480 “vault” so the classic 8-bit engine can keep living in its comfy bunker while the outside world enjoys HD textures, modern input scaling, and future shaders. This roadmap tracks every step required to decouple original rendering/input from the real window, translate everything through the adapter, and eventually layer in crisp RGBA assets without breaking retro compatibility.

## Before You Dive In
1. **Read the whole scroll:** skim each stage below so you know what’s already complete (✅) and which TODO callouts still need love.
2. **Boot with the flag on:** set `system.virtual_adapter=1` and confirm `gVirtualScreenEnabled` lights up in diagnostics before touching code—most tasks assume the virtual surface is active.
3. **Trace the touchpoints:** familiarize yourself with `window_manager`, `display_scaler`, `renderPresent`, `input`, and `art` since every stage hooks one or more of these subsystems.
4. **Log obsessively:** enable the `SCALER` diagnostics channel while testing; every stage adds new log lines that make regression hunting survivable.
5. **Document the fallout:** whenever you land a step, update the corresponding notes/TODOs here so the next scavenger knows which traps were already disarmed.

## Using the Adapter Right Now
1. **Flip the Pip-Boy toggle:** set `system.virtual_adapter=1` inside `fallout2.cfg` (or use the in-game settings page once we wire it up). Leave it at `0` if you need stock behavior for regression testing.
2. **Rebuild once:** the flag is read during startup, so restart the game after editing the cfg. If you’re hacking on the engine, make sure `window_manager` picks up the new value during `windowManagerInit`.
3. **Verify the vault door opened:** enable `diagnostics=SCALER` in `fallout2-ce.ini` and check the log for lines mentioning `gVirtualScreenEnabled=1` and the expected `640x480` pitch. You can also drop a breakpoint on `windowGetVirtualScreenBuffer` and inspect `_screen_buffer_pitch` to be sure the off-screen surface is serving frames.
4. **Know the temporary limits:** true-color overlays are stubbed out in Stage 1.1, so any modules calling `windowHasTrueColorOverlay` or `artGetTrueColorFrame` will get safe defaults until the HD compositor lands in Stage 4.

## Stage 1 – Establish the Vault (Virtual Surface) 
- [x] **S1.1** Spin up an off-screen 640x480 back buffer that mirrors the existing pipeline (palette, blits, everything).
	- Reuse the existing `displayScalerInit(640, 480)` contract so all callers still query logical dimensions through `displayScalerGetLogicalBounds`.
	- Allocate the "virtual" surface directly inside `window_manager` next to the legacy window buffers so 8-bit drawing code keeps working.
	- Gate the behavior behind `system.virtual_adapter=1` (stored in `fallout2.cfg`) so we can flip the layer on without impacting default installs.
	- **Implementation notes:** `_screen_buffer` now doubles as the virtual surface, `_screen_buffer_pitch` records its stride, and `windowGetVirtualScreenBuffer/Pitch` expose it to the rest of the engine. Compatibility shims for the old true-color APIs live in `window_manager` and `art` so downstream systems compile while we re-route rendering.
	- **Usage reminder:** the feature is opt-in; double-check the cfg flag before reporting bugs.
- [x] **S1.2** Wrap `renderPresent`/`window_manager_private` so the old buffer never hits the OS directly.
	- Hook the tail of `windowRefreshAll`/`renderPresent` so it blits the virtual surface into an intermediate texture instead of the physical window.
	- Keep the `display_scaler` letterboxing math so our composite honors integer scaling and logical bounds automatically.
	- **Implementation notes:** `windowPresentVirtualScreen()` now drains the dirty-rect queue produced by `_GNW_win_refresh`, copies the affected slice of `_screen_buffer` into the SDL texture surface, and emits a `SCALER` trace line with the blitted bounds. `_refresh_all`, `windowRefresh*`, and `renderPresent` all call the helper, so nothing touches the OS backbuffer until the virtual present runs.
- [x] **S1.3** Mirror that buffer into a modern RGBA texture for the real window and prove we can scale it cleanly.
	- Use the SDL/DirectDraw presenter we already have: promote the copy step to 32-bit (`bufferToTexture` style) so later stages can mix HD overlays.
	- Instrument diagnostics (category `SCALER`) to log both virtual and physical dimensions for troubleshooting.
	- **Implementation notes:** `windowPresentVirtualScreen()` now copies the dirty rect from `_screen_buffer` straight into the ARGB8888 SDL texture surface via `blitIndexedRectToTexture`, using the shared palette cache that `directDrawSetPalette*` maintains. Every flush logs `virtual_present virtual=(...) viewport=(...)` so we can correlate 640×480 updates with the physical letterboxed viewport.

## Stage 2 – Tame the Rad-Input (Coordinate Translation)
- [x] **S2.1** Capture actual window inputs at native resolution (mouse, wheel, touch).
	- **Implementation notes:** `_GNW95_process_message` now funnels **every** SDL mouse/touch packet into `virtualInputCaptureEvent`, which caches native window coordinates plus wheel deltas the moment they arrive. The new `virtual_input.*` shim resets alongside `mouseDeviceAcquire`, so the Vault Dweller’s cursor always starts fresh after focus changes.
- [x] **S2.2** Normalize and remap to 640x480 coordinates before they reach `game_mouse` / `input`.
	- **Implementation notes:** `displayScalerMapPointToVirtual()` centralizes physical→vault math (letterbox padding, clamps, inverse scale). `mouseDeviceGetData` consumes it for delta math and publishes `VirtualMouseMappingSample`s so downstream systems see the same 640×480 truth as the diagnostics overlay.
- [x] **S2.3** Add diagnostics switches to visualize both coordinate spaces for sanity checks.
	- **Implementation notes:** Flip `[debug] debug_input_overlay=1` in `fallout2.cfg` to summon the Neon Overseer Overlay—red crosshairs track raw physical hits, green marks the remapped vault coords, and the viewport outline glows teal. Everything renders just before `SDL_RenderPresent`, so what you see is exactly what the wasteland gets.

## Stage 3 – Scout the Draw Calls
- [x] **S3.1** Instrument `artRender`/tile/UI paths to log which fid, frame, and layer rendered where in the virtual buffer.
	- **Implementation notes:** `render_trace.cc` now taps `artRender`, tile floors/roofs, and both `_obj_render_pre_roof` / `_obj_render_post_roof` so every blit reports a `RenderTraceOp` with fid, frame, rotation, elevation, and the resolved screen rect (thanks to the new `windowResolveBufferRect` helper). Each op lands in the `RENDERTRACE` diagnostics channel with its depth bucket for forensic spelunking.
- [x] **S3.2** Define a lightweight render-command stream (fid, screen rect, depth bucket).
	- **Implementation notes:** The adapter keeps a per-frame ring buffer (4K ops) of `RenderTraceOp` structs and commits it right before `renderPresent`. Overflow toggles a per-frame flag so we can spot stressed scenes before Stage 4 swaps in HD sprites.
- [x] **S3.3** Validate that the command stream matches the on-screen order in tricky scenes (combat, dialogue, UI overlays).
	- **Implementation notes:** Hitting `Ctrl+F8` spawns the Overseer Replay Window—a 320×240 overlay that replays the last frame’s command stream with color-coded rectangles per layer. Toggle it on/off to eyeball ordering without pausing the action; the hotkey is consumed inside `input.cc` so gameplay never sees the chord.

## Stage 4 – Deploy HD Overlays
- [x] **S4.1** Teach the compositor to look up RGBA assets per fid/frame and draw them instead of the scaled buffer patch.
	- **Registry rites:** done. `artRegisterTrueColorFrameData` now records per-frame RGBA views keyed by the indexed frame pointer, `artGetTrueColorFrame` enforces dimension parity before handing data out, and `artCacheFlush`/`artReset` clear the registry so no one hugs stale pixels. Diagnostics light up whenever a mismatch tries to sneak through.
	- **Overlay lifeline:** done. Every managed window allocates a mirrored RGBA overlay + coverage mask when the virtual adapter is flipped on, and tile/map refreshers can pull those pointers via `windowHasTrueColorOverlay/Mask`. Clearing a dirty rect scrubs both the mask and the pixels so seams behave.
	- **Presenter swap:** done. `windowPresentVirtualScreen()` now composites each window’s overlay through the new `blitTrueColorRectToTexture` path and logs `SCALER hd_overlay virtual=(...) pixels_overridden=...` so Vega can see exactly how many pixels escaped grayscale purgatory.
- [x] **S4.2** Handle transparency/alpha blending so tile seams and critter outlines behave.
	- **Premult vs straight showdown:** decide whether to store premultiplied ARGB in the registry by comparing the math inside `applyTileLightingToArgb`/`applyIntensityToChannel`. Document which path wins and tag each HD registration with its alpha mode for later shader work.
		- Verdict rendered: we’re keeping **straight (unpremult)** RGBA inside the registry. `colorApplyIntensityToChannel`/`colorApplyLightingToArgb` got promoted to shared helpers, `HdTrueColorFrameView` now records an `alphaMode`, and any rogue premult assets get logged + ignored until we teach the mixer to convert them.
	- **Critter cameos:** extend `_obj_render_pre_roof`, `_obj_render_post_roof`, and the UI helpers in `artRender` to ask `artGetTrueColorFrame` for overlays the same way floors already do. When a swap happens, emit a `RENDERTRACE` note with the fid so the replay hotkey shows where the HD critters landed.
		- Objects now tap the HD registry inside `_obj_render_object`, honor the same lighting curve as the 8-bit blits, and write into the iso-window overlay unless the frame is translucent, alpha-mismatched, or blocked by the infamous Egg occluder. SCALER logs rat out any frames that fail the width/alpha sniff test.
		- `artRender` gained a straight-through path for UI swaps whenever the widget isn’t being scaled—its buffer pointer is resolved back to the owning window, the overlay/mask are filled, and diagnostics call out any packs that ship premult or off-size art before they spook the Pip-Boy.
	- **Lighting sanity:** ensure the compositor respects the per-pixel mask coming out of `tile.cc` so edge pixels don’t halo. Add assertions (or at least fatal logs) if the overlay dimensions ever drift from the indexed frame—they signal broken packs from the surface vaults.
		- Window compositor now scrubs every overlay/mask pair before an SDL blit, zeroing stale RGBA where the mask went dark and shouting about any sanitized pixels so mask leaks can’t ghost onto the wasteland skyline.
		- Tile, critter, and UI swap paths all assert that HD sheets match their indexed ancestors the instant they’re accepted, so a malformed pack trips diagnostics immediately instead of smearing magma-colored seams across the Vault wall.
- [x] **S4.3** Introduce fallbacks when an HD asset is missing, logging hits/misses for modders.
	- [x] **Cache scorecards:** count HD hits/misses per map load inside the registry and dump a `SCALER hd_cache map=VaultCity hits=42 misses=3` line right after `mapLoadByName` settles. The Neon Archivist can then tell modders exactly which sprites still need repainting.
		- `artTrueColorStatsReset/Log` now bookmark every HD lookup and print the scorecard as soon as a map successfully loads (saved games included). The SCALER channel shows `hits`, `misses`, and the map tag so the Archivist can roast lazy repaint crews in release notes.
	- [x] **Graceful decay:** if a registration vanishes mid-frame (cache eviction, mod reload, feral molerat), auto-disable the overlay for that fid and note it in diagnostics so QA knows why a sprite snapped back to 8-bit.
		- The iso renderer now tracks which fids have ever drawn HD pixels; if the registry drops them, we scrub their overlay, stamp a `windowDebugStampMissingHdGlyph` ping, and emit `SCALER hd_overlay disabled fid=... reason=...` exactly once per dropout so QA can track regressions without log spam.
	- [x] **Fallback glyphs:** keep a tiny “missing HD” watermark (just a masked debug swatch) that can be toggled via config; scribes testing packs will see instantly when the compositor reverted to indexed art.
		- Flip `[debug] hd_missing_watermark=1` in `fallout2.cfg` to paint the neon chevron any time a tile/object falls back to 8-bit; the helper lives entirely in VRAM, so no 8-bit pixels are harmed.

## Stage 5 – Break the 640×480 Ceiling
- [x] **S5.1** Give the virtual adapter its own physical-resolution playground.
	- Flip `system.virtual_adapter_fullres=1` to hand the presenter a viewport-sized ARGB surface; `renderPresent` now resizes the SDL texture (and its staging surface) to match `displayScalerGetPhysicalViewport()` whenever the vault is running at 1× integer scale.
	- `blitIndexedRectToTexture`, `blitTrueColorRectToTexture`, and every diagnostic tap now route through `resolvePresenterRect`, so dirty rects and overlay composites land directly in physical space instead of trusting SDL to scale later.
	- `display_scaler` forges per-axis scale tables that map each logical column/row to its physical span; Stage 5.2 will chew on those tables for crisp pixel expansion and they already show up in the `SCALER` logs.
	- `copySurfaceRectToTexture`, `SDL_UpdateTexture` fallback logs, and texture-surface stats all clamp against the presenter bounds, keeping diagnostics honest even when the viewport drifts.
- [ ] **S5.2** Stretch classic palette assets deliberately instead of letting SDL smear them.
	- During the logical→physical blit, expand each indexed pixel into an integer `scale × scale` block (or nearest-neighbor if fractional), so the original art simply grows to fit the viewport while preserving crisp edges.
- [ ] **S5.3** Store and bind true-color overlays at physical resolution.
	- When `system.virtual_adapter=1`, have `windowCreate`/`objectsBindTrueColorOverlay`/`tileBindTrueColorOverlay` allocate overlay+mask grids big enough for the physical viewport and expose helper structs so renderers can convert logical coordinates (plus letterbox offsets) into physical offsets.
- [ ] **S5.4** Render HD sprites directly into that physical grid.
	- Update `objectsBlitTrueColorOverlay` and the tile HD path to march over the physical rect returned by `displayScalerLogicalToPhysical`, sampling `HdTrueColorFrameView` with true per-pixel math instead of averaging back to logical resolution.
- [ ] **S5.5** Composite everything in physical space and keep SDL honest.
	- Rewrite `windowCompositeTrueColorOverlays` to feed `blitTrueColorRectToTexture` the pre-scaled ARGB buffers, so SDL becomes a dumb present-only stage and never re-scales our handiwork.
- [ ] **S5.6** Handle letterboxing, fractional scales, and clean fallbacks.
	- Respect `displayScalerGetLetterboxOffset`, short-circuit when scale < 1.0, and ensure `isoDisable`/`tileDisable` clear the enlarged overlays so scene changes don’t leave ghosts.
- [ ] **S5.7** Instrument the new pipeline.
	- Emit `SCALER` logs for logical vs. physical rects, track HD pixels written per frame, and add a config toggle (`virtual_adapter_fullres=0/1`) so QA can revert instantly when debugging.
		- Toggle shipped during **S5.1**; the Overseer still wants per-frame counters before we call this stage done.

## Stage 6 – Stretch Goals & Polish
- [ ] **S6.1** Add optional shaders/post-effects (CRT, bloom, whatever the Overseer deems tasteful).
	- **TODO:** prototype shader toggles in `preferences.cc`, defaulting them off for potato-mode rigs.
- [ ] **S6.2** Benchmark CPU/GPU impact; add settings for throttling HD overlays on low-end hardware.
	- **TODO:** hook the existing diagnostics profiler so we can compare frame times with/without HD overlays.
- [ ] **S6.3** Document the modder-facing HD asset pipeline and expose toggles in the config UI.
	- **TODO:** extend this roadmap with a “Modder Addendum” once the asset loader stabilizes.

*Check off each task as we conquer it—leave witty notes if a deathclaw was involved.*
