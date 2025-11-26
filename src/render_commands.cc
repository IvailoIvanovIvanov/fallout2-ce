#include "render_commands.h"

#include <array>

#include "diagnostics.h"
#include "settings.h"

namespace fallout {
namespace {

constexpr size_t kRenderCommandTileCapacity = 4096;

std::array<RenderCommandTileBlit, kRenderCommandTileCapacity> gTileCommands;
size_t gTileCommandCount = 0;
RenderCommandStats gRenderCommandStats = {};
uint32_t gRenderCommandSequence = 0;
uint16_t gRenderCommandFrameIndex = 0;
bool gRenderCommandsInitialized = false;

void renderCommandsEnsureInitialized()
{
    if (!gRenderCommandsInitialized) {
        renderCommandsInit();
    }
}

void renderCommandsLogStatsIfNeeded()
{
    if (!renderCommandCaptureEnabled()) {
        return;
    }

    if (diagnosticsWouldLog(DiagnosticsLevel::Trace)) {
        diagnosticsLog(DiagnosticsLevel::Trace,
            "SCALER",
            "command_queue stats frame=%u queued=%u dropped=%u",
            gRenderCommandFrameIndex,
            gRenderCommandStats.queued,
            gRenderCommandStats.dropped);
    }
}

} // namespace

void renderCommandsInit()
{
    gTileCommandCount = 0;
    gRenderCommandStats = {};
    gRenderCommandSequence = 0;
    gRenderCommandFrameIndex = 0;
    gRenderCommandsInitialized = true;
}

bool renderCommandCaptureEnabled()
{
    return settings.system.render_command_trace;
}

const RenderCommandStats& renderCommandGetStats()
{
    return gRenderCommandStats;
}

void renderCommandsBeforePresent()
{
    renderCommandsEnsureInitialized();

    if (!renderCommandCaptureEnabled()) {
        return;
    }

    renderCommandsLogStatsIfNeeded();

    gRenderCommandFrameIndex++;
    gTileCommandCount = 0;
    gRenderCommandStats = {};
}

void renderCommandEmitTileBlit(RenderCommandOp op, const RenderCommandTileBlitPayload& payload)
{
    renderCommandsEnsureInitialized();

    if (!renderCommandCaptureEnabled()) {
        return;
    }

    gRenderCommandStats.queued++;

    if (gTileCommandCount >= gTileCommands.size()) {
        gRenderCommandStats.dropped++;
        if (diagnosticsWouldLog(DiagnosticsLevel::Trace)) {
            diagnosticsLog(DiagnosticsLevel::Trace,
                "SCALER",
                "command_queue overflow capacity=%zu",
                gTileCommands.size());
        }
        return;
    }

    RenderCommandTileBlit& command = gTileCommands[gTileCommandCount++];
    command.header.sequence = ++gRenderCommandSequence;
    command.header.frameIndex = gRenderCommandFrameIndex;
    command.header.payloadSize = sizeof(RenderCommandTileBlitPayload);
    command.op = op;
    command.payload = payload;

    if (diagnosticsWouldLog(DiagnosticsLevel::Trace)) {
        const Rect& rect = command.payload.screenRect;
        diagnosticsLog(DiagnosticsLevel::Trace,
            "SCALER",
            "command_emit op=%d fid=%u rect=(%d,%d %dx%d) flags=0x%X lighting=%d",
            static_cast<int>(op),
            command.payload.fid,
            rect.left,
            rect.top,
            rectGetWidth(&rect),
            rectGetHeight(&rect),
            command.payload.flags,
            command.payload.lighting);
    }
}

} // namespace fallout
