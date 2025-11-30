#ifndef FALLOUT_RENDER_COMMANDS_H_
#define FALLOUT_RENDER_COMMANDS_H_

#include <array>
#include <cstdint>

#include "geometry.h"

namespace fallout {

enum class RenderCommandOp : uint8_t {
    TileBlit = 0,
    RoofBlit,
    ObjectBlit,
    UiBlit,
    ScreenClear,
    ViewportEvent,
    DebugGlyph,
    // Phase 2 additions
    PaletteEffect,
    CursorBlit,
    VideoFrame,
};

enum RenderCommandFlags : uint16_t {
    RenderCommandFlag_None = 0,
    RenderCommandFlag_LightingPerPixel = 1 << 0,
    RenderCommandFlag_LightingFlat = 1 << 1,
    RenderCommandFlag_Translucent = 1 << 2,
    RenderCommandFlag_Masked = 1 << 3,
    RenderCommandFlag_ForceIndexedFallback = 1 << 4,
};

struct RenderAssetHandle {
    uint32_t fid = 0;
    uint16_t frame = 0;
    uint8_t rotation = 0;
    uint8_t variant = 0;
};

constexpr size_t kRenderCommandMaxLightingVertices = 10;
constexpr int kRenderCommandTileIntensityMapStride = 80;
constexpr int kRenderCommandTileIntensityMapRows = 41;
constexpr int kRenderCommandTileIntensityMapBaseOffset = 160;
constexpr int kRenderCommandTileIntensityMapSize = kRenderCommandTileIntensityMapStride * kRenderCommandTileIntensityMapRows;

struct RenderCommandHeader {
    uint32_t sequence = 0;
    uint16_t frameIndex = 0;
    uint16_t payloadSize = 0;
};

struct RenderCommandTileBlitPayload {
    RenderAssetHandle asset;
    Rect screenRect {};
    uint32_t fid = 0;
    int32_t tileIndex = -1;
    int16_t windowId = -1;
    int16_t reserved = 0;
    uint8_t depthBucket = 0;
    uint16_t paletteId = 0;
    int16_t lighting = -1;
    uint16_t flags = RenderCommandFlag_None;
    int16_t isoTileX = 0;
    int16_t isoTileY = 0;
    uint8_t elevation = 0;
    int16_t sourceOffsetX = 0;
    int16_t sourceOffsetY = 0;
    uint16_t sourceWidth = 0;
    uint16_t sourceHeight = 0;
    uint8_t perPixelLightingCount = 0;
    std::array<int32_t, kRenderCommandMaxLightingVertices> perPixelLighting = {};
};

struct RenderCommandTileBlit {
    RenderCommandHeader header;
    RenderCommandOp op = RenderCommandOp::TileBlit;
    RenderCommandTileBlitPayload payload;
};

struct RenderCommandStats {
    uint32_t queued = 0;
    uint32_t dropped = 0;
    // Phase 6: Extended metrics
    uint32_t directWrites = 0;      // Legacy path usage counter
    uint32_t orchestratorFrames = 0; // Frames handled by orchestrator
    uint32_t fallbackFrames = 0;     // Frames using indexed fallback
};

struct RenderCommandBufferView {
    const RenderCommandTileBlit* commands = nullptr;
    size_t count = 0;
    uint16_t frameIndex = 0;
};

enum class RenderViewportEventType : uint8_t {
    Unknown = 0,
    Scroll,
    FadeOut,
    FadeIn,
    Blackout,
    Resize,
};

struct RenderViewportEventPayload {
    RenderViewportEventType type = RenderViewportEventType::Unknown;
    Rect rect {};
    int16_t param0 = 0;
    int16_t param1 = 0;
    uint32_t dirtySequence = 0;
};

struct RenderViewportEvent {
    RenderCommandHeader header;
    RenderViewportEventPayload payload;
};

struct RenderViewportEventBufferView {
    const RenderViewportEvent* events = nullptr;
    size_t count = 0;
    uint16_t frameIndex = 0;
};

// -------- Phase 2: Additional command payloads --------

struct RenderCommandCursorBlitPayload {
    Rect screenRect {};
    uint16_t paletteId = 0; // for indexed cursor frames, optional
    uint16_t flags = RenderCommandFlag_None;
    int16_t windowId = -1;
};

struct RenderCommandCursorBlit {
    RenderCommandHeader header;
    RenderCommandOp op = RenderCommandOp::CursorBlit;
    RenderCommandCursorBlitPayload payload;
};

struct RenderCommandPaletteEffectPayload {
    uint8_t type = 0; // 0=fadeOut,1=fadeIn,2=gammaShift, etc.
    uint16_t param = 0; // strength/index
};

struct RenderCommandPaletteEffect {
    RenderCommandHeader header;
    RenderCommandOp op = RenderCommandOp::PaletteEffect;
    RenderCommandPaletteEffectPayload payload;
};

struct RenderCommandVideoFramePayload {
    Rect screenRect {};
    uint16_t width = 0;
    uint16_t height = 0;
    const unsigned char* indexed = nullptr; // optional for legacy frames
    const uint32_t* rgba = nullptr;         // optional for HD frames
};

struct RenderCommandVideoFrame {
    RenderCommandHeader header;
    RenderCommandOp op = RenderCommandOp::VideoFrame;
    RenderCommandVideoFramePayload payload;
};

struct RenderCommandCursorBufferView {
    const RenderCommandCursorBlit* commands = nullptr;
    size_t count = 0;
    uint16_t frameIndex = 0;
};

struct RenderCommandPaletteBufferView {
    const RenderCommandPaletteEffect* commands = nullptr;
    size_t count = 0;
    uint16_t frameIndex = 0;
};

struct RenderCommandVideoBufferView {
    const RenderCommandVideoFrame* commands = nullptr;
    size_t count = 0;
    uint16_t frameIndex = 0;
};

// Phase 6: Global stats instance
extern RenderCommandStats gRenderCommandStats;

bool renderCommandCaptureEnabled();
void renderCommandsInit();
void renderCommandsBeforePresent();
bool renderCommandCaptureEnabled();
const RenderCommandStats& renderCommandGetStats();
bool renderCommandsPeekTileCommands(RenderCommandBufferView& outView);
void renderCommandEmitTileBlit(RenderCommandOp op, const RenderCommandTileBlitPayload& payload);
bool renderCommandsPeekViewportEvents(RenderViewportEventBufferView& outView);
void renderCommandEmitViewportEvent(RenderViewportEventType type,
    const Rect& rect,
    int16_t param0,
    int16_t param1,
    uint32_t dirtySequence);
bool renderCommandsDumpLastFrame(const char* reason);
bool renderCommandsHandleHotkey(int keyCode);
bool renderCommandBuildPerPixelIntensityMap(const RenderCommandTileBlitPayload& payload,
    std::array<int, kRenderCommandTileIntensityMapSize>& out);
bool renderCommandDirectBlitFallbackActive();
const char* renderCommandDirectBlitFallbackReason();
void renderCommandTriggerDirectBlitFallback(const char* reason);
void renderCommandResetFallbackState();

// Phase 6: Diagnostics and metrics
void renderCommandLogFrameMetrics();

// Phase 2: emit/peek for new commands
void renderCommandEmitCursorBlit(const RenderCommandCursorBlitPayload& payload);
bool renderCommandsPeekCursorBlits(RenderCommandCursorBufferView& outView);
void renderCommandEmitPaletteEffect(const RenderCommandPaletteEffectPayload& payload);
bool renderCommandsPeekPaletteEffects(RenderCommandPaletteBufferView& outView);
void renderCommandEmitVideoFrame(const RenderCommandVideoFramePayload& payload);
bool renderCommandsPeekVideoFrames(RenderCommandVideoBufferView& outView);

} // namespace fallout

#endif /* FALLOUT_RENDER_COMMANDS_H_ */
