#include "art_png_loader.h"

#include "art_texture.h"
#include "memory.h"
#include "svga.h"
#include "db.h"
#include "color.h"

#ifdef HAVE_SDL2_IMAGE
#include <SDL.h>
#include <SDL_image.h>
#endif

#include <string>
#include <unordered_map>

namespace fallout {

#ifdef HAVE_SDL2_IMAGE
// Simple cache for indexed PNG conversions keyed by fid. Lifetime: until artPngIndexedCacheClear.
struct IndexedCacheEntry {
    unsigned char* data;
    int w;
    int h;
};
static std::unordered_map<int, IndexedCacheEntry> g_indexedCache;
static void getCurrentPaletteRGB(unsigned char* rgbOut768)
{
    // Use the currently loaded game palette (_cmap). Values are 0-63; convert to 8-bit.
    unsigned char* pal6 = _cmap;
    for (int i = 0; i < 256; ++i) {
        rgbOut768[i * 3 + 0] = pal6[i * 3 + 0] << 2;
        rgbOut768[i * 3 + 1] = pal6[i * 3 + 1] << 2;
        rgbOut768[i * 3 + 2] = pal6[i * 3 + 2] << 2;
    }
}

static inline int colorDistanceSq(unsigned char r1, unsigned char g1, unsigned char b1,
                                  unsigned char r2, unsigned char g2, unsigned char b2)
{
    int dr = int(r1) - int(r2);
    int dg = int(g1) - int(g2);
    int db = int(b1) - int(b2);
    return dr * dr + dg * dg + db * db;
}

static unsigned char mapRgbaToPalette(Uint8 r, Uint8 g, Uint8 b, Uint8 a, const unsigned char* pal)
{
    if (a == 0) return 0; // transparent -> index 0 assumed transparent
    int bestIdx = 0;
    int bestDist = 0x7FFFFFFF;
    for (int i = 1; i < 256; ++i) { // skip 0 so transparent stays reserved
        int d = colorDistanceSq(r, g, b, pal[i * 3 + 0], pal[i * 3 + 1], pal[i * 3 + 2]);
        if (d < bestDist) {
            bestDist = d;
            bestIdx = i;
            if (d == 0) break;
        }
    }
    return (unsigned char)bestIdx;
}

static SDL_Surface* downscaleNearest(SDL_Surface* src, int dstW, int dstH)
{
    SDL_Surface* dst = SDL_CreateRGBSurfaceWithFormat(0, dstW, dstH, 32, SDL_PIXELFORMAT_RGBA32);
    if (!dst) return nullptr;

    int srcW = src->w;
    int srcH = src->h;
    Uint32* sp = (Uint32*)src->pixels;
    Uint32* dp = (Uint32*)dst->pixels;
    int spitch = src->pitch / 4;
    int dpitch = dst->pitch / 4;

    for (int y = 0; y < dstH; ++y) {
        int sy = (y * srcH) / dstH;
        const Uint32* srow = sp + sy * spitch;
        Uint32* drow = dp + y * dpitch;
        for (int x = 0; x < dstW; ++x) {
            int sx = (x * srcW) / dstW;
            drow[x] = srow[sx];
        }
    }

    return dst;
}

bool artPngLoadIndexed(int fid, unsigned char** outData, int* outWidth, int* outHeight)
{
    *outData = nullptr;
    *outWidth = 0;
    *outHeight = 0;

    const ArtTextureMeta* meta = artTextureGetMeta(fid);
    if (!meta || !meta->exists) return false;

    const char* frmPath = artBuildFilePath(fid);
    if (!frmPath || frmPath[0] == '\0') return false;
    std::string pngPath = std::string(frmPath);
    size_t dot = pngPath.find_last_of('.');
    if (dot == std::string::npos) pngPath += ".png"; else pngPath.replace(dot, std::string::npos, ".png");

    // Load PNG bytes via game VFS so files under patch dir (e.g. data\...) are found.
    File* f = fileOpen(pngPath.c_str(), "rb");
    if (!f) return false;
    int fsize = fileGetSize(f);
    if (fsize <= 0) {
        fileClose(f);
        return false;
    }
    void* bytes = internal_malloc(fsize);
    if (!bytes) {
        fileClose(f);
        return false;
    }
    size_t readCount = fileRead(bytes, 1, fsize, f);
    fileClose(f);
    if ((int)readCount != fsize) {
        internal_free(bytes);
        return false;
    }

    SDL_RWops* rw = SDL_RWFromMem(bytes, fsize);
    if (!rw) {
        internal_free(bytes);
        return false;
    }
    SDL_Surface* surface = IMG_Load_RW(rw, 1 /*freesrc*/);
    // IMG_Load_RW copies the data into a surface; we can free our buffer now.
    internal_free(bytes);
    if (!surface) return false;

    // Normalize to RGBA32
    SDL_Surface* rgba = SDL_ConvertSurfaceFormat(surface, SDL_PIXELFORMAT_RGBA32, 0);
    SDL_FreeSurface(surface);
    if (!rgba) return false;

    SDL_Surface* logical = rgba;
    if (rgba->w != meta->logicalWidth || rgba->h != meta->logicalHeight) {
        SDL_Surface* scaled = downscaleNearest(rgba, meta->logicalWidth, meta->logicalHeight);
        SDL_FreeSurface(rgba);
        logical = scaled;
        if (!logical) return false;
    }

    // Palette mapping
    unsigned char pal[768];
    getCurrentPaletteRGB(pal);

    int w = logical->w;
    int h = logical->h;
    unsigned char* dst = (unsigned char*)internal_malloc(w * h);
    if (!dst) {
        SDL_FreeSurface(logical);
        return false;
    }

    Uint8 r, g, b, a;
    for (int y = 0; y < h; ++y) {
        Uint32* row = (Uint32*)((Uint8*)logical->pixels + y * logical->pitch);
        for (int x = 0; x < w; ++x) {
            SDL_GetRGBA(row[x], logical->format, &r, &g, &b, &a);
            dst[y * w + x] = mapRgbaToPalette(r, g, b, a, pal);
        }
    }

    SDL_FreeSurface(logical);

    *outData = dst;
    *outWidth = w;
    *outHeight = h;
    return true;
}

SDL_Surface* artPngLoadSurface(int fid)
{
    const ArtTextureMeta* meta = artTextureGetMeta(fid);
    if (!meta || !meta->exists) return nullptr;

    const char* frmPath = artBuildFilePath(fid);
    if (!frmPath || frmPath[0] == '\0') return nullptr;
    std::string pngPath = std::string(frmPath);
    size_t dot = pngPath.find_last_of('.');
    if (dot == std::string::npos) pngPath += ".png"; else pngPath.replace(dot, std::string::npos, ".png");

    File* f = fileOpen(pngPath.c_str(), "rb");
    if (!f) return nullptr;
    int fsize = fileGetSize(f);
    if (fsize <= 0) {
        fileClose(f);
        return nullptr;
    }
    void* bytes = internal_malloc(fsize);
    if (!bytes) {
        fileClose(f);
        return nullptr;
    }
    size_t readCount = fileRead(bytes, 1, fsize, f);
    fileClose(f);
    if ((int)readCount != fsize) {
        internal_free(bytes);
        return nullptr;
    }
    SDL_RWops* rw = SDL_RWFromMem(bytes, fsize);
    if (!rw) {
        internal_free(bytes);
        return nullptr;
    }
    SDL_Surface* surface = IMG_Load_RW(rw, 1 /*freesrc*/);
    internal_free(bytes);
    if (!surface) return nullptr;
    // Convert to a standard truecolor format for predictable blits.
    SDL_Surface* rgba = SDL_ConvertSurfaceFormat(surface, SDL_PIXELFORMAT_RGBA32, 0);
    SDL_FreeSurface(surface);
    return rgba; // may be nullptr if conversion failed
}

bool artPngGetIndexedCached(int fid, unsigned char** outData, int* outWidth, int* outHeight)
{
    *outData = nullptr;
    *outWidth = 0;
    *outHeight = 0;

    auto it = g_indexedCache.find(fid);
    if (it != g_indexedCache.end()) {
        *outData = it->second.data;
        *outWidth = it->second.w;
        *outHeight = it->second.h;
        return it->second.data != nullptr;
    }

    unsigned char* data = nullptr;
    int w = 0, h = 0;
    if (!artPngLoadIndexed(fid, &data, &w, &h)) {
        // Cache negative result to avoid repeated attempts.
        g_indexedCache.emplace(fid, IndexedCacheEntry{ nullptr, 0, 0 });
        return false;
    }

    g_indexedCache.emplace(fid, IndexedCacheEntry{ data, w, h });
    *outData = data;
    *outWidth = w;
    *outHeight = h;
    return true;
}

void artPngIndexedCacheClear()
{
    for (auto& kv : g_indexedCache) {
        if (kv.second.data) {
            internal_free(kv.second.data);
        }
    }
    g_indexedCache.clear();
}

#else

bool artPngLoadIndexed(int fid, unsigned char** outData, int* outWidth, int* outHeight)
{
    (void)fid; (void)outData; (void)outWidth; (void)outHeight;
    return false;
}

SDL_Surface* artPngLoadSurface(int fid)
{
    (void)fid;
    return nullptr;
}

bool artPngGetIndexedCached(int fid, unsigned char** outData, int* outWidth, int* outHeight)
{
    (void)fid; (void)outData; (void)outWidth; (void)outHeight;
    return false;
}

void artPngIndexedCacheClear() {}

#endif

} // namespace fallout
