#include "render_commands.h"

#include <array>
#include <cstdio>
#include <vector>

#include "diagnostics.h"
#include "kb.h"
#include "platform_compat.h"
#include "settings.h"
#include "window_manager.h"

namespace fallout {
namespace {

constexpr size_t kRenderCommandTileCapacity = 4096;
constexpr int kRenderCommandsDumpHotkey = KEY_CTRL_F9;

std::array<RenderCommandTileBlit, kRenderCommandTileCapacity> gTileCommands;
size_t gTileCommandCount = 0;
RenderCommandStats gRenderCommandStats = {};
uint32_t gRenderCommandSequence = 0;
uint16_t gRenderCommandFrameIndex = 0;
bool gRenderCommandsInitialized = false;
std::vector<RenderCommandTileBlit> gLastFrameCommands;
RenderCommandStats gLastFrameStats = {};
uint16_t gLastFrameIndex = 0;
uint32_t gRenderCommandDumpCounter = 0;

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
    gLastFrameCommands.clear();
    gLastFrameStats = {};
    gLastFrameIndex = 0;
    gRenderCommandDumpCounter = 0;
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
        if (!gLastFrameCommands.empty()) {
            gLastFrameCommands.clear();
            gLastFrameStats = {};
        }
        return;
    }

    renderCommandsLogStatsIfNeeded();

    gLastFrameCommands.assign(gTileCommands.begin(), gTileCommands.begin() + gTileCommandCount);
    gLastFrameStats = gRenderCommandStats;
    gLastFrameIndex = gRenderCommandFrameIndex;

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

bool renderCommandsDumpLastFrame(const char* reason)
{
    renderCommandsEnsureInitialized();

    if (!renderCommandCaptureEnabled()) {
        if (diagnosticsWouldLog(DiagnosticsLevel::Info)) {
            diagnosticsLog(DiagnosticsLevel::Info, "SCALER", "command_dump skipped (trace disabled)");
        }
        return false;
    }

    if (gLastFrameCommands.empty()) {
        if (diagnosticsWouldLog(DiagnosticsLevel::Info)) {
            diagnosticsLog(DiagnosticsLevel::Info, "SCALER", "command_dump skipped (no recorded frame)");
        }
        return false;
    }

    char filePath[COMPAT_MAX_PATH];
    std::snprintf(filePath, sizeof(filePath), "log/render_commands_frame_%06u.json", gRenderCommandDumpCounter++);

    FILE* stream = compat_fopen(filePath, "wb");
    if (stream == nullptr) {
        if (diagnosticsWouldLog(DiagnosticsLevel::Info)) {
            diagnosticsLog(DiagnosticsLevel::Info, "SCALER", "command_dump unable to open %s", filePath);
        }
        return false;
    }

    std::fprintf(stream, "{\n");
    std::fprintf(stream, "  \"reason\": \"%s\",\n", reason != nullptr ? reason : "manual");
    std::fprintf(stream, "  \"frame_index\": %u,\n", gLastFrameIndex);
    std::fprintf(stream, "  \"commands\": [\n");

    for (size_t i = 0; i < gLastFrameCommands.size(); i++) {
        const RenderCommandTileBlit& cmd = gLastFrameCommands[i];
        const Rect& rect = cmd.payload.screenRect;
        std::fprintf(stream,
            "    {\"seq\":%u,\"op\":%d,\"fid\":%u,\"rect\":[%d,%d,%d,%d],\"flags\":%u,\"lighting\":%d,\"iso\":[%d,%d,%d],\"palette\":%u}%s\n",
            cmd.header.sequence,
            static_cast<int>(cmd.op),
            cmd.payload.fid,
            rect.left,
            rect.top,
            rectGetWidth(&rect),
            rectGetHeight(&rect),
            cmd.payload.flags,
            cmd.payload.lighting,
            cmd.payload.isoTileX,
            cmd.payload.isoTileY,
            cmd.payload.elevation,
            cmd.payload.paletteId,
            i + 1 < gLastFrameCommands.size() ? "," : "");
    }

    std::fprintf(stream, "  ],\n");
    std::fprintf(stream, "  \"stats\": {\"queued\":%u,\"dropped\":%u}\n", gLastFrameStats.queued, gLastFrameStats.dropped);
    std::fprintf(stream, "}\n");
    std::fclose(stream);

    if (diagnosticsWouldLog(DiagnosticsLevel::Info)) {
        diagnosticsLog(DiagnosticsLevel::Info,
            "SCALER",
            "command_dump wrote %zu commands to %s",
            gLastFrameCommands.size(),
            filePath);
    }

    return true;
}

bool renderCommandsHandleHotkey(int keyCode)
{
    if (keyCode != kRenderCommandsDumpHotkey) {
        return false;
    }

    if (!windowIsVirtualScreenEnabled()) {
        if (diagnosticsWouldLog(DiagnosticsLevel::Info)) {
            diagnosticsLog(DiagnosticsLevel::Info, "SCALER", "command_dump hotkey ignored (virtual adapter disabled)");
        }
        return true;
    }

    if (!renderCommandsDumpLastFrame("hotkey")) {
        if (diagnosticsWouldLog(DiagnosticsLevel::Info)) {
            diagnosticsLog(DiagnosticsLevel::Info, "SCALER", "command_dump failed (see prior logs)");
        }
    }

    return true;
}

} // namespace fallout
