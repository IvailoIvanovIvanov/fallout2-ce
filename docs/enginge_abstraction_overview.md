1. Run the game natively on 640x480 pixels resolution running on a "phantom display" with its own cache that contains only the original assets, the game run as it is intended.
2. The "phantom display" don't render on the screen directly, rather it is using command send to the real display, which can run on high resolution and it is drowning HD assets on the screen. The "phantom display" can receive commands with the user interactions, like mouse movements and clicks, key presses, so the game can run.
3. To ensure correct position of the assets and the user interactions, the physical resolution of the "phantom" and real displays are converted to logical units so this guarantees that the relevant position to the screen is the same.
4. The real display have its own separate cache, with the HD assets in a proper format and draws the assets on the screen in HD format, and ensuring the proper position on the screen with the logical units used, than it catches the user interaction and convert the to logical units and send them back to the "phantom display"


#Upscale Architecture Plan

## Anchor the Phantom Display
Keep _screen_buffer + _screen_buffer_pitch in window_manager.cc as the only surface the classic 8‑bit logic ever touches; treat windowPresentVirtualScreen, virtualScreenInvalidateRect, and the dirty queue as the contract that mirrors the Engine Abstraction Overview (EAO) step 1.
Ensure system.virtual_adapter=1 initializes displayScalerInit(640,480) and allocates virtual surfaces next to the legacy window buffers; block any direct SDL/DirectDraw writes by routing _refresh_all, _GNW_win_refresh, and renderPresent through windowPresentVirtualScreen.
Guard this mode with diagnostics: keep VA_TRACE/SCALER present_stats enabled to confirm the phantom display’s cache stays 640×480 and never leaks to the OS backbuffer.

## Feed the Command Bus (Phantom → Real)
Use render_commands.* as the “commands” mentioned in EAO step 2: every tile/roof/object/UI blit must emit a RenderCommandTileBlitPayload before touching _screen_buffer. Audit _obj_render_object, artRender, particle paths, automap, and any direct memmove scrolls; when legacy paths can’t be migrated yet, log SCALER command_miss so we know what still bypasses the bus.
Keep the recorder hotkeys (system.render_command_trace, Ctrl+F9) available so each frame’s opcode/rect metadata can be replayed; this guarantees the phantom display remains the authoritative simulation while the real display replays commands deterministically.

## Drive the Real Display Orchestrator
Flip system.render_display_orchestrator=1 to let render_display_orchestrator.cc consume the command queue and paint the high‑res “real display” overlays, matching EAO step 2/4 verbatim.
windowCompositeTrueColorOverlays should prefer the orchestrator’s viewport-sized ARGB buffers (via windowGetPhysicalTrueColorOverlay) and only fall back to the 640×480 logical overlays when virtual_adapter_fullres=0 or orchestrator ownership drops (movies, splash).
Maintain the “no logic change” guarantee by keeping _screen_buffer as a QA reference: every present scrubs the SDL texture (SCALER presenter_clear reason=orchestrator) before copying orchestrated pixels so the physical monitor only shows HD content sourced from commands.

## Normalize Spaces & Input
Rely on display_scaler.cc to provide the logical↔physical bridge from EAO step 3: displayScalerGetScaleTable, displayScalerLogicalToPhysical, and displayScalerMapPointToVirtual already encode the letterbox-aware span tables; keep them up to date whenever the window resizes or integer scaling toggles.
virtual_input.cc + mouseDeviceGetData must stay the sole path for SDL input → logical coordinates. Preserve VirtualMouseMappingSample telemetry so diagnostics can visualize red (physical) vs. green (virtual) crosshairs, ensuring the real display’s hit-tests match what the phantom display expects.

## Separate Asset Caches
render_asset_registry.* and the HD registry in art.* embody EAO step 4. Continue registering every (fid, frame, rotation) that the phantom display loads via renderAssetRegistryTrackFrame, attach HD textures (straight RGBA) through renderAssetRegistryAttachHdView, and mirror them into logical + physical overlays when available.
Keep fallback generation enabled: if an HD asset is missing or evicted mid-frame, the registry auto-promotes the indexed frame into a cached RGBA buffer, logs SCALER hd_overlay disabled fid=…, and the orchestrator paints that fallback without touching the original 8‑bit logic.

## Lifecycle, Events, and QA Guardrails
Use renderCommandEmitViewportEvent to mirror scrolls, fades, and zero-fill operations so the orchestrator always clears/pauses exactly when the phantom display does; this prevents ghost pixels when the dirty queue advances.
Keep system.render_command_direct_blit_fallback available: if the command queue overflows or replay mismatches appear, immediately drop back to legacy blits (logged via SCALER command_fallback) so gameplay stays stable while debugging.
Enforce heavy diagnostics: SCALER orchestrator hd=… fallback=… hd_scale_max=…, render_commands stats, and the HD cache scorecards prove that the upscale path stays deterministic and reveal any regressions before they hit release.