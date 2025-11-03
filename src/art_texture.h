#ifndef ART_TEXTURE_H
#define ART_TEXTURE_H

#include "art.h"
#include "svga.h"

namespace fallout {

// Metadata for a potential PNG replacement for an FRM asset.
struct ArtTextureMeta {
    bool exists;        // true if a PNG override was found
    int pngWidth;       // physical PNG width in pixels
    int pngHeight;      // physical PNG height in pixels
    int logicalWidth;   // logical width after dividing by sourceScale
    int logicalHeight;  // logical height after dividing by sourceScale
    int sourceScale;    // 1 for 1x, 2 for @2x, 3 for @3x, 4 for @4x, etc.
};

// Probe for a PNG replacement for the given fid without decoding the image.
// Returns true if a PNG was found and fills out metadata (including logical size).
bool artTextureProbePng(int fid, ArtTextureMeta* outMeta);

// Convenience: returns true if a PNG override exists for fid.
bool artTextureExists(int fid);

// Cached metadata lookup: probes once and caches result (hit or miss).
// Returns pointer to cached entry if a PNG exists for fid, otherwise nullptr.
const ArtTextureMeta* artTextureGetMeta(int fid);

// Clear the metadata cache (e.g., on resource reload), safe to call anytime.
void artTextureCacheClear();

// Truecolor texture object (only valid when PNGs are enabled at build time).
struct ArtTexture {
    SDL_Texture* texture;
    int logicalWidth;
    int logicalHeight;
    int sourceScale;
};

// Load (or fetch cached) SDL texture for the PNG override. Returns nullptr if
// no PNG exists or if PNG support is not available.
const ArtTexture* artTextureGet(int fid);

// Release all cached textures.
void artTextureClear();

} // namespace fallout

#endif // ART_TEXTURE_H
