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
- [ ] **S4.2** Handle transparency/alpha blending so tile seams and critter outlines behave.
	- **Premult vs straight showdown:** decide whether to store premultiplied ARGB in the registry by comparing the math inside `applyTileLightingToArgb`/`applyIntensityToChannel`. Document which path wins and tag each HD registration with its alpha mode for later shader work.
	- **Critter cameos:** extend `_obj_render_pre_roof`, `_obj_render_post_roof`, and the UI helpers in `artRender` to ask `artGetTrueColorFrame` for overlays the same way floors already do. When a swap happens, emit a `RENDERTRACE` note with the fid so the replay hotkey shows where the HD critters landed.
	- **Lighting sanity:** ensure the compositor respects the per-pixel mask coming out of `tile.cc` so edge pixels don’t halo. Add assertions (or at least fatal logs) if the overlay dimensions ever drift from the indexed frame—they signal broken packs from the surface vaults.
- [ ] **S4.3** Introduce fallbacks when an HD asset is missing, logging hits/misses for modders.
	- **Cache scorecards:** count HD hits/misses per map load inside the registry and dump a `SCALER hd_cache map=VaultCity hits=42 misses=3` line right after `mapLoadByName` settles. The Neon Archivist can then tell modders exactly which sprites still need repainting.
	- **Graceful decay:** if a registration vanishes mid-frame (cache eviction, mod reload, feral molerat), auto-disable the overlay for that fid and note it in diagnostics so QA knows why a sprite snapped back to 8-bit.
	- **Fallback glyphs:** keep a tiny “missing HD” watermark (just a masked debug swatch) that can be toggled via config; scribes testing packs will see instantly when the compositor reverted to indexed art.

## Stage 5 – Stretch Goals & Polish
- [ ] **S5.1** Add optional shaders/post-effects (CRT, bloom, whatever the Overseer deems tasteful).
	- **TODO:** prototype shader toggles in `preferences.cc`, defaulting them off for potato-mode rigs.
- [ ] **S5.2** Benchmark CPU/GPU impact; add settings for throttling HD overlays on low-end hardware.
	- **TODO:** hook the existing diagnostics profiler so we can compare frame times with/without HD overlays.
- [ ] **S5.3** Document the modder-facing HD asset pipeline and expose toggles in the config UI.
	- **TODO:** extend this roadmap with a “Modder Addendum” once the asset loader stabilizes.

*Check off each task as we conquer it—leave witty notes if a deathclaw was involved.*
