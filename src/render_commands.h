#ifndef FALLOUT_RENDER_COMMANDS_H_
#define FALLOUT_RENDER_COMMANDS_H_

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

struct RenderCommandHeader {
    uint32_t sequence = 0;
    uint16_t frameIndex = 0;
    uint16_t payloadSize = 0;
};

struct RenderCommandTileBlitPayload {
    RenderAssetHandle asset;
    Rect screenRect {};
    uint32_t fid = 0;
    uint8_t depthBucket = 0;
    uint16_t paletteId = 0;
    int16_t lighting = -1;
    uint16_t flags = RenderCommandFlag_None;
    int16_t isoTileX = 0;
    int16_t isoTileY = 0;
    uint8_t elevation = 0;
};

struct RenderCommandTileBlit {
    RenderCommandHeader header;
    RenderCommandOp op = RenderCommandOp::TileBlit;
    RenderCommandTileBlitPayload payload;
};

struct RenderCommandStats {
    uint32_t queued = 0;
    uint32_t dropped = 0;
};

bool renderCommandCaptureEnabled();
void renderCommandsInit();
void renderCommandsBeforePresent();
bool renderCommandCaptureEnabled();
const RenderCommandStats& renderCommandGetStats();
void renderCommandEmitTileBlit(RenderCommandOp op, const RenderCommandTileBlitPayload& payload);
bool renderCommandsDumpLastFrame(const char* reason);
bool renderCommandsHandleHotkey(int keyCode);

} // namespace fallout

#endif /* FALLOUT_RENDER_COMMANDS_H_ */
