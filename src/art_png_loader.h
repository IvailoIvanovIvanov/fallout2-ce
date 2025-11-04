#ifndef ART_PNG_LOADER_H
#define ART_PNG_LOADER_H

struct SDL_Surface; // forward decl

namespace fallout {

// Load a PNG override for fid and convert to 8-bit indexed using current palette.
// Applies @Nx downscaling to match logical size. Returns true on success.
// Caller must free(*outData) with internal_free.
bool artPngLoadIndexed(int fid, unsigned char** outData, int* outWidth, int* outHeight);

// Load a PNG override for fid as a truecolor SDL_Surface at native resolution (no downscale).
// Returns nullptr if SDL2_image is not available or file is missing/invalid.
// Caller takes ownership and must SDL_FreeSurface on success.
SDL_Surface* artPngLoadSurface(int fid);

// Cached accessors for indexed PNG data used in frequently rendered elements (e.g., scenery).
// Returns true and sets out pointers if a PNG exists and is cached/loaded; the returned pointer
// is owned by the cache and must NOT be freed by the caller.
bool artPngGetIndexedCached(int fid, unsigned char** outData, int* outWidth, int* outHeight);

// Clears the indexed PNG cache and frees all cached buffers.
void artPngIndexedCacheClear();

}

#endif // ART_PNG_LOADER_H
