#include "render_commands.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <vector>

#include "art.h"
#include "display_scaler.h"
#include "diagnostics.h"
#include "kb.h"
#include "object.h"
#include "platform_compat.h"
#include "settings.h"
#include "window_manager.h"

namespace fallout {
namespace {

constexpr size_t kRenderCommandTileCapacity = 4096;
constexpr int kRenderCommandsDumpHotkey = KEY_CTRL_F9;
constexpr uint32_t kRenderCommandSerializedMagic = 'RCMD';
constexpr uint16_t kRenderCommandSerializedVersion = 1;
constexpr int kReplayLightingStep = 512;

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
std::vector<uint8_t> gLastFrameSerialized;
std::vector<uint8_t> gLastFrameReferenceBuffer;
int gLastFrameReferenceWidth = 0;
int gLastFrameReferenceHeight = 0;
std::vector<uint8_t> gReplayScratchBuffer;
std::vector<uint8_t> gReplayCoverageMask;
std::vector<RenderCommandTileBlit> gDecodedCommands;

struct RenderCommandReplayStats {
    bool ran = false;
    bool matched = false;
    uint32_t commandsReplayed = 0;
    uint32_t commandsFailed = 0;
    uint32_t comparedPixels = 0;
    uint32_t mismatchedPixels = 0;
    uint32_t ignoredPixels = 0;
};

RenderCommandReplayStats gReplayStats = {};

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

void renderCommandsResetFrameCaches();
void renderCommandsCaptureReferenceFrame();
void renderCommandsSerializeLastFrame();
void renderCommandsRunReplaySelfTest();
bool renderCommandsDecodeSerializedFrame(const std::vector<uint8_t>& buffer, std::vector<RenderCommandTileBlit>& out);
bool renderCommandsReplayFrame(const std::vector<RenderCommandTileBlit>& commands);
bool renderCommandsReplayTileCommand(const RenderCommandTileBlit& command, unsigned char* dest, int destPitch);
void renderCommandsReplayMarkCoverage(const Rect& rect);
bool renderCommandsClampRectToReference(Rect* rect);
void renderCommandsReplayLogStats();

void renderCommandsResetFrameCaches()
{
    gLastFrameSerialized.clear();
    gLastFrameReferenceBuffer.clear();
    gReplayScratchBuffer.clear();
    gReplayCoverageMask.clear();
    gDecodedCommands.clear();
    gLastFrameReferenceWidth = 0;
    gLastFrameReferenceHeight = 0;
    gReplayStats = {};
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
    renderCommandsResetFrameCaches();
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
        renderCommandsResetFrameCaches();
        return;
    }

    renderCommandsCaptureReferenceFrame();
    renderCommandsLogStatsIfNeeded();

    gLastFrameCommands.assign(gTileCommands.begin(), gTileCommands.begin() + gTileCommandCount);
    gLastFrameStats = gRenderCommandStats;
    gLastFrameIndex = gRenderCommandFrameIndex;

    renderCommandsSerializeLastFrame();
    renderCommandsRunReplaySelfTest();

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

void renderCommandsCaptureReferenceFrame()
{
    if (!windowIsVirtualScreenEnabled()) {
        gLastFrameReferenceBuffer.clear();
        gLastFrameReferenceWidth = 0;
        gLastFrameReferenceHeight = 0;
        return;
    }

    unsigned char* buffer = windowGetVirtualScreenBuffer();
    int pitch = windowGetVirtualScreenPitch();
    if (buffer == nullptr || pitch <= 0) {
        gLastFrameReferenceBuffer.clear();
        gLastFrameReferenceWidth = 0;
        gLastFrameReferenceHeight = 0;
        return;
    }

    const Rect& logicalBounds = displayScalerGetLogicalBounds();
    const int width = rectGetWidth(&logicalBounds);
    const int height = rectGetHeight(&logicalBounds);

    if (width <= 0 || height <= 0) {
        gLastFrameReferenceBuffer.clear();
        gLastFrameReferenceWidth = 0;
        gLastFrameReferenceHeight = 0;
        return;
    }

    gLastFrameReferenceBuffer.resize(static_cast<size_t>(width) * height);
    for (int y = 0; y < height; y++) {
        std::memcpy(&gLastFrameReferenceBuffer[static_cast<size_t>(y) * width], buffer + y * pitch, width);
    }

    gLastFrameReferenceWidth = width;
    gLastFrameReferenceHeight = height;
}

void renderCommandsSerializeLastFrame()
{
    gLastFrameSerialized.clear();

    if (gLastFrameCommands.empty()) {
        return;
    }

    const auto appendUint8 = [](std::vector<uint8_t>& buffer, uint8_t value) {
        buffer.push_back(value);
    };

    const auto appendUint16 = [&](uint16_t value) {
        appendUint8(gLastFrameSerialized, static_cast<uint8_t>(value & 0xFF));
        appendUint8(gLastFrameSerialized, static_cast<uint8_t>((value >> 8) & 0xFF));
    };

    const auto appendUint32 = [&](uint32_t value) {
        appendUint16(static_cast<uint16_t>(value & 0xFFFF));
        appendUint16(static_cast<uint16_t>((value >> 16) & 0xFFFF));
    };

    const auto appendInt32 = [&](int32_t value) {
        appendUint32(static_cast<uint32_t>(value));
    };

    const auto appendInt16 = [&](int16_t value) {
        appendUint16(static_cast<uint16_t>(value));
    };

    appendUint32(kRenderCommandSerializedMagic);
    appendUint16(kRenderCommandSerializedVersion);
    appendUint16(gLastFrameIndex);
    appendUint16(static_cast<uint16_t>(std::min<size_t>(gLastFrameCommands.size(), 0xFFFF)));
    appendUint32(gLastFrameStats.queued);
    appendUint32(gLastFrameStats.dropped);

    for (const RenderCommandTileBlit& cmd : gLastFrameCommands) {
        appendUint32(cmd.header.sequence);
        appendUint16(cmd.header.frameIndex);
        appendUint16(cmd.header.payloadSize);
        appendUint8(gLastFrameSerialized, static_cast<uint8_t>(cmd.op));
        appendUint8(gLastFrameSerialized, 0); // reserved for future flags

        const Rect& rect = cmd.payload.screenRect;
        appendInt32(rect.left);
        appendInt32(rect.top);
        appendInt32(rect.right);
        appendInt32(rect.bottom);

        appendUint32(cmd.payload.asset.fid);
        appendUint16(cmd.payload.asset.frame);
        appendUint8(gLastFrameSerialized, cmd.payload.asset.rotation);
        appendUint8(gLastFrameSerialized, cmd.payload.asset.variant);

        appendUint32(cmd.payload.fid);
        appendInt32(cmd.payload.tileIndex);
        appendUint8(gLastFrameSerialized, cmd.payload.depthBucket);
        appendUint8(gLastFrameSerialized, cmd.payload.elevation);
        appendUint16(cmd.payload.paletteId);
        appendUint16(cmd.payload.flags);
        appendInt16(cmd.payload.lighting);
        appendInt16(cmd.payload.isoTileX);
        appendInt16(cmd.payload.isoTileY);
        appendInt16(cmd.payload.sourceOffsetX);
        appendInt16(cmd.payload.sourceOffsetY);
        appendUint16(cmd.payload.sourceWidth);
        appendUint16(cmd.payload.sourceHeight);
    }
}

void renderCommandsRunReplaySelfTest()
{
    if (!settings.system.render_command_replay) {
        gReplayStats = {};
        return;
    }

    if (gLastFrameSerialized.empty() || gLastFrameReferenceBuffer.empty()) {
        gReplayStats = {};
        return;
    }

    if (!renderCommandsDecodeSerializedFrame(gLastFrameSerialized, gDecodedCommands)) {
        gReplayStats = {};
        return;
    }

    renderCommandsReplayFrame(gDecodedCommands);
}

bool renderCommandsDecodeSerializedFrame(const std::vector<uint8_t>& buffer, std::vector<RenderCommandTileBlit>& out)
{
    out.clear();

    if (buffer.size() < 14) {
        return false;
    }

    size_t offset = 0;

    const auto readUint8 = [&](uint8_t& value) {
        if (offset >= buffer.size()) {
            return false;
        }
        value = buffer[offset++];
        return true;
    };

    const auto readUint16 = [&](uint16_t& value) {
        uint8_t lo = 0;
        uint8_t hi = 0;
        if (!readUint8(lo) || !readUint8(hi)) {
            return false;
        }
        value = static_cast<uint16_t>(lo | (hi << 8));
        return true;
    };

    const auto readUint32 = [&](uint32_t& value) {
        uint16_t lo = 0;
        uint16_t hi = 0;
        if (!readUint16(lo) || !readUint16(hi)) {
            return false;
        }
        value = static_cast<uint32_t>(lo | (static_cast<uint32_t>(hi) << 16));
        return true;
    };

    const auto readInt32 = [&](int32_t& value) {
        uint32_t raw = 0;
        if (!readUint32(raw)) {
            return false;
        }
        value = static_cast<int32_t>(raw);
        return true;
    };

    const auto readInt16 = [&](int16_t& value) {
        uint16_t raw = 0;
        if (!readUint16(raw)) {
            return false;
        }
        value = static_cast<int16_t>(raw);
        return true;
    };

    uint32_t magic = 0;
    uint16_t version = 0;
    uint16_t frameIndex = 0;
    uint16_t commandCount = 0;
    uint32_t queued = 0;
    uint32_t dropped = 0;

    if (!readUint32(magic) || magic != kRenderCommandSerializedMagic) {
        return false;
    }

    if (!readUint16(version) || version != kRenderCommandSerializedVersion) {
        return false;
    }

    if (!readUint16(frameIndex) || !readUint16(commandCount) || !readUint32(queued) || !readUint32(dropped)) {
        return false;
    }

    (void)queued;
    (void)dropped;
    (void)frameIndex;

    out.reserve(commandCount);

    for (uint16_t i = 0; i < commandCount; i++) {
        RenderCommandTileBlit cmd;
        uint8_t opValue = 0;
        uint8_t reserved = 0;

        if (!readUint32(cmd.header.sequence) || !readUint16(cmd.header.frameIndex) || !readUint16(cmd.header.payloadSize)) {
            return false;
        }

        if (!readUint8(opValue) || !readUint8(reserved)) {
            return false;
        }

        cmd.op = static_cast<RenderCommandOp>(opValue);

        if (!readInt32(cmd.payload.screenRect.left) || !readInt32(cmd.payload.screenRect.top) || !readInt32(cmd.payload.screenRect.right) || !readInt32(cmd.payload.screenRect.bottom)) {
            return false;
        }

        if (!readUint32(cmd.payload.asset.fid) || !readUint16(cmd.payload.asset.frame)) {
            return false;
        }

        if (!readUint8(cmd.payload.asset.rotation) || !readUint8(cmd.payload.asset.variant)) {
            return false;
        }

        if (!readUint32(cmd.payload.fid) || !readInt32(cmd.payload.tileIndex)) {
            return false;
        }

        if (!readUint8(cmd.payload.depthBucket) || !readUint8(cmd.payload.elevation)) {
            return false;
        }

        if (!readUint16(cmd.payload.paletteId) || !readUint16(cmd.payload.flags) || !readInt16(cmd.payload.lighting) || !readInt16(cmd.payload.isoTileX) || !readInt16(cmd.payload.isoTileY) || !readInt16(cmd.payload.sourceOffsetX) || !readInt16(cmd.payload.sourceOffsetY)) {
            return false;
        }

        if (!readUint16(cmd.payload.sourceWidth) || !readUint16(cmd.payload.sourceHeight)) {
            return false;
        }

        out.push_back(cmd);
    }

    return true;
}

bool renderCommandsReplayFrame(const std::vector<RenderCommandTileBlit>& commands)
{
    gReplayStats = {};
    gReplayStats.ran = true;

    if (gLastFrameReferenceBuffer.empty() || gLastFrameReferenceWidth <= 0 || gLastFrameReferenceHeight <= 0) {
        return false;
    }

    const size_t pixelCount = gLastFrameReferenceBuffer.size();
    gReplayScratchBuffer.assign(pixelCount, 0);
    gReplayCoverageMask.assign(pixelCount, 0);

    unsigned char* replayBuffer = gReplayScratchBuffer.data();
    const int replayPitch = gLastFrameReferenceWidth;

    for (const RenderCommandTileBlit& cmd : commands) {
        if (cmd.op != RenderCommandOp::TileBlit && cmd.op != RenderCommandOp::RoofBlit) {
            gReplayStats.commandsFailed++;
            continue;
        }

        if (!renderCommandsReplayTileCommand(cmd, replayBuffer, replayPitch)) {
            gReplayStats.commandsFailed++;
            continue;
        }

        gReplayStats.commandsReplayed++;
        renderCommandsReplayMarkCoverage(cmd.payload.screenRect);
    }

    for (size_t i = 0; i < pixelCount; i++) {
        if (gReplayCoverageMask[i] == 0) {
            gReplayStats.ignoredPixels++;
            continue;
        }

        gReplayStats.comparedPixels++;
        if (gReplayScratchBuffer[i] != gLastFrameReferenceBuffer[i]) {
            gReplayStats.mismatchedPixels++;
        }
    }

    gReplayStats.matched = gReplayStats.mismatchedPixels == 0;
    renderCommandsReplayLogStats();

    return gReplayStats.matched;
}

bool renderCommandsReplayTileCommand(const RenderCommandTileBlit& command, unsigned char* dest, int destPitch)
{
    if (command.payload.sourceWidth == 0 || command.payload.sourceHeight == 0) {
        return false;
    }

    if ((command.payload.flags & RenderCommandFlag_LightingPerPixel) != 0) {
        return false;
    }

    Rect destRect = command.payload.screenRect;
    if (!renderCommandsClampRectToReference(&destRect)) {
        return false;
    }

    CacheEntry* cacheEntry = nullptr;
    Art* art = artLock(command.payload.fid, &cacheEntry);
    if (art == nullptr) {
        return false;
    }

    const int frameWidth = artGetWidth(art, 0, 0);
    const int frameHeight = artGetHeight(art, 0, 0);
    unsigned char* frameData = artGetFrameData(art, 0, 0);
    if (frameData == nullptr || frameWidth <= 0 || frameHeight <= 0) {
        artUnlock(cacheEntry);
        return false;
    }

    const int srcOffsetX = std::clamp(static_cast<int>(command.payload.sourceOffsetX), 0, frameWidth - 1);
    const int srcOffsetY = std::clamp(static_cast<int>(command.payload.sourceOffsetY), 0, frameHeight - 1);
    const int srcWidth = std::min(static_cast<int>(command.payload.sourceWidth), frameWidth - srcOffsetX);
    const int srcHeight = std::min(static_cast<int>(command.payload.sourceHeight), frameHeight - srcOffsetY);

    if (srcWidth <= 0 || srcHeight <= 0) {
        artUnlock(cacheEntry);
        return false;
    }

    unsigned char* src = frameData + srcOffsetY * frameWidth + srcOffsetX;

    int lightingIndex = command.payload.lighting >= 0 ? command.payload.lighting : 0;
    if (lightingIndex < 0) {
        lightingIndex = 0;
    }
    if (lightingIndex > 255) {
        lightingIndex = 255;
    }

    const int lightingValue = lightingIndex * kReplayLightingStep;

    _dark_trans_buf_to_buf(src,
        srcWidth,
        srcHeight,
        frameWidth,
        dest,
        destRect.left,
        destRect.top,
        destPitch,
        lightingValue);

    artUnlock(cacheEntry);
    return true;
}

void renderCommandsReplayMarkCoverage(const Rect& rect)
{
    Rect clipped = rect;
    if (!renderCommandsClampRectToReference(&clipped)) {
        return;
    }

    const int width = gLastFrameReferenceWidth;
    for (int y = clipped.top; y <= clipped.bottom; y++) {
        uint8_t* row = gReplayCoverageMask.data() + static_cast<size_t>(y) * width;
        std::fill_n(row + clipped.left, clipped.right - clipped.left + 1, 1);
    }
}

bool renderCommandsClampRectToReference(Rect* rect)
{
    if (rect == nullptr || gLastFrameReferenceWidth <= 0 || gLastFrameReferenceHeight <= 0) {
        return false;
    }

    Rect bounds;
    bounds.left = 0;
    bounds.top = 0;
    bounds.right = gLastFrameReferenceWidth - 1;
    bounds.bottom = gLastFrameReferenceHeight - 1;

    return rectIntersection(rect, &bounds, rect) == 0;
}

void renderCommandsReplayLogStats()
{
    if (!diagnosticsWouldLog(DiagnosticsLevel::Trace)) {
        if (!gReplayStats.matched && diagnosticsWouldLog(DiagnosticsLevel::Info)) {
            diagnosticsLog(DiagnosticsLevel::Info,
                "SCALER",
                "command_replay mismatch frame=%u compared=%u mismatched=%u failures=%u",
                gLastFrameIndex,
                gReplayStats.comparedPixels,
                gReplayStats.mismatchedPixels,
                gReplayStats.commandsFailed);
        }
        return;
    }

    const double coverage = gReplayStats.comparedPixels + gReplayStats.ignoredPixels > 0
        ? (static_cast<double>(gReplayStats.comparedPixels) / static_cast<double>(gReplayStats.comparedPixels + gReplayStats.ignoredPixels)) * 100.0
        : 0.0;

    diagnosticsLog(DiagnosticsLevel::Trace,
        "SCALER",
        "command_replay frame=%u commands=%u failed=%u compared=%u mismatched=%u coverage=%.2f%%",
        gLastFrameIndex,
        gReplayStats.commandsReplayed,
        gReplayStats.commandsFailed,
        gReplayStats.comparedPixels,
        gReplayStats.mismatchedPixels,
        coverage);
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
            "    {\"seq\":%u,\"op\":%d,\"fid\":%u,\"rect\":[%d,%d,%d,%d],\"flags\":%u,\"lighting\":%d,\"iso\":[%d,%d,%d],\"tile\":%d,\"palette\":%u,\"src\":[%d,%d,%u,%u]}%s\n",
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
            cmd.payload.tileIndex,
            cmd.payload.paletteId,
            cmd.payload.sourceOffsetX,
            cmd.payload.sourceOffsetY,
            cmd.payload.sourceWidth,
            cmd.payload.sourceHeight,
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
