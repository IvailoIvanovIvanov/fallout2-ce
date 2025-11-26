#ifndef FALLOUT_RENDER_ASSET_REGISTRY_H_
#define FALLOUT_RENDER_ASSET_REGISTRY_H_

#include <cstdint>

#include "art.h"
#include "render_commands.h"

namespace fallout {

struct RenderAssetState {
    RenderAssetHandle handle;
    uint64_t assetId = 0;
    uint16_t width = 0;
    uint16_t height = 0;
    bool hdAvailable = false;
    bool hdIsFallback = false;
};

void renderAssetRegistryReset();
void renderAssetRegistryTrackFrame(const RenderAssetHandle& handle,
    const void* owner,
    const unsigned char* indexed,
    uint16_t width,
    uint16_t height);
void renderAssetRegistryReleaseFramesForOwner(const void* owner);
bool renderAssetRegistryAttachHdView(const unsigned char* indexed,
    const HdTrueColorFrameView& view,
    bool isFallback);
void renderAssetRegistryDetachHdView(const unsigned char* indexed);
bool renderAssetRegistryGetHdView(const RenderAssetHandle& handle,
    HdTrueColorFrameView& outView,
    bool* outIsFallback = nullptr);
bool renderAssetRegistryGetState(const RenderAssetHandle& handle, RenderAssetState& outState);

} // namespace fallout

#endif /* FALLOUT_RENDER_ASSET_REGISTRY_H_ */
