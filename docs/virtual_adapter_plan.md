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
- [ ] **S1.2** Wrap `renderPresent`/`window_manager_private` so the old buffer never hits the OS directly.
	- Hook the tail of `windowRefreshAll`/`renderPresent` so it blits the virtual surface into an intermediate texture instead of the physical window.
	- Keep the `display_scaler` letterboxing math so our composite honors integer scaling and logical bounds automatically.
	- **TODO:** introduce a `virtual_present()` helper that copies `_screen_buffer` into the scaler path and logs the blit rectangle for diagnostics.
- [ ] **S1.3** Mirror that buffer into a modern RGBA texture for the real window and prove we can scale it cleanly.
	- Use the SDL/DirectDraw presenter we already have: promote the copy step to 32-bit (`bufferToTexture` style) so later stages can mix HD overlays.
	- Instrument diagnostics (category `SCALER`) to log both virtual and physical dimensions for troubleshooting.
	- **TODO:** allocate a cached `SDL_Texture` (or DirectDraw surface) sized to the physical window, then add a palette-to-RGBA conversion step before present.

## Stage 2 – Tame the Rad-Input (Coordinate Translation)
- [ ] **S2.1** Capture actual window inputs at native resolution (mouse, wheel, touch).
	- **TODO:** wire SDL raw mouse events into a new `virtualInputCapture` shim before `game_mouse` touches them.
- [ ] **S2.2** Normalize and remap to 640x480 coordinates before they reach `game_mouse` / `input`.
	- **TODO:** expose `displayScalerMapPointToVirtual()` that handles integer scaling, aspect padding, and hands back vault-space coords.
- [ ] **S2.3** Add diagnostics switches to visualize both coordinate spaces for sanity checks.
	- **TODO:** draw a translucent overlay highlighting the cursor’s raw vs virtual positions when `debug_input_overlay=1`.

## Stage 3 – Scout the Draw Calls
- [ ] **S3.1** Instrument `artRender`/tile/UI paths to log which fid, frame, and layer rendered where in the virtual buffer.
	- **TODO:** emit `RENDERTRACE` events containing `fid`, screen rect, and z-order for later playback.
- [ ] **S3.2** Define a lightweight render-command stream (fid, screen rect, depth bucket).
	- **TODO:** prototype a ring buffer of `RenderOp` structs so Stage 4 can remap them to HD assets.
- [ ] **S3.3** Validate that the command stream matches the on-screen order in tricky scenes (combat, dialogue, UI overlays).
	- **TODO:** add a developer hotkey that replays the command stream to a debug window for eyeballing.

## Stage 4 – Deploy HD Overlays
- [ ] **S4.1** Teach the compositor to look up RGBA assets per fid/frame and draw them instead of the scaled buffer patch.
	- **TODO:** replace the current stubs in `art.cc` with real lookups backed by the HD asset registry and fall back to indexed data when missing.
- [ ] **S4.2** Handle transparency/alpha blending so tile seams and critter outlines behave.
	- **TODO:** evaluate premultiplied alpha vs straight alpha paths; log decisions per asset for modders.
- [ ] **S4.3** Introduce fallbacks when an HD asset is missing, logging hits/misses for modders.
	- **TODO:** surface a `SCALER` log line summarizing HD cache hit rate every time we load a new map.

## Stage 5 – Stretch Goals & Polish
- [ ] **S5.1** Add optional shaders/post-effects (CRT, bloom, whatever the Overseer deems tasteful).
	- **TODO:** prototype shader toggles in `preferences.cc`, defaulting them off for potato-mode rigs.
- [ ] **S5.2** Benchmark CPU/GPU impact; add settings for throttling HD overlays on low-end hardware.
	- **TODO:** hook the existing diagnostics profiler so we can compare frame times with/without HD overlays.
- [ ] **S5.3** Document the modder-facing HD asset pipeline and expose toggles in the config UI.
	- **TODO:** extend this roadmap with a “Modder Addendum” once the asset loader stabilizes.

*Check off each task as we conquer it—leave witty notes if a deathclaw was involved.*
