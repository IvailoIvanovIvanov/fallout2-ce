# Virtual Adapter Roadmap

> Because sometimes the wasteland deserves UHD without angering the ancient 640x480 spirits.

## Stage 1 – Establish the Vault (Virtual Surface) 
- [ ] **S1.1** Spin up an off-screen 640x480 back buffer that mirrors the existing pipeline (palette, blits, everything).
	- Reuse the existing `displayScalerInit(640, 480)` contract so all callers still query logical dimensions through `displayScalerGetLogicalBounds`.
	- Allocate the "virtual" surface in `window_manager_private.cc` right next to `gBackBuffer` so legacy code keeps writing 8-bit pixels.
- [ ] **S1.2** Wrap `renderPresent`/`window_manager_private` so the old buffer never hits the OS directly.
	- Hook the tail of `windowRefreshAll`/`renderPresent` so it blits the virtual surface into an intermediate texture instead of the physical window.
	- Keep the `display_scaler` letterboxing math so our composite honors integer scaling and logical bounds automatically.
- [ ] **S1.3** Mirror that buffer into a modern RGBA texture for the real window and prove we can scale it cleanly.
	- Use the SDL/DirectDraw presenter we already have: promote the copy step to 32-bit (`bufferToTexture` style) so later stages can mix HD overlays.
	- Instrument diagnostics (category `SCALER`) to log both virtual and physical dimensions for troubleshooting.

## Stage 2 – Tame the Rad-Input (Coordinate Translation)
- [ ] **S2.1** Capture actual window inputs at native resolution (mouse, wheel, touch).
- [ ] **S2.2** Normalize and remap to 640x480 coordinates before they reach `game_mouse` / `input`.
- [ ] **S2.3** Add diagnostics switches to visualize both coordinate spaces for sanity checks.

## Stage 3 – Scout the Draw Calls
- [ ] **S3.1** Instrument `artRender`/tile/UI paths to log which fid, frame, and layer rendered where in the virtual buffer.
- [ ] **S3.2** Define a lightweight render-command stream (fid, screen rect, depth bucket).
- [ ] **S3.3** Validate that the command stream matches the on-screen order in tricky scenes (combat, dialogue, UI overlays).

## Stage 4 – Deploy HD Overlays
- [ ] **S4.1** Teach the compositor to look up RGBA assets per fid/frame and draw them instead of the scaled buffer patch.
- [ ] **S4.2** Handle transparency/alpha blending so tile seams and critter outlines behave.
- [ ] **S4.3** Introduce fallbacks when an HD asset is missing, logging hits/misses for modders.

## Stage 5 – Stretch Goals & Polish
- [ ] **S5.1** Add optional shaders/post-effects (CRT, bloom, whatever the Overseer deems tasteful).
- [ ] **S5.2** Benchmark CPU/GPU impact; add settings for throttling HD overlays on low-end hardware.
- [ ] **S5.3** Document the modder-facing HD asset pipeline and expose toggles in the config UI.

*Check off each task as we conquer it—leave witty notes if a deathclaw was involved.*
