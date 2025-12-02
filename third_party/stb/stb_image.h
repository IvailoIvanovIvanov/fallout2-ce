// Minimal stb_image-compatible header providing only stbi_load_from_memory and stbi_image_free.
// This implementation decodes PNG -> RGBA8 on Windows using WIC (Windows Imaging Component),
// avoiding GDI+. On non-Windows platforms, provide the real stb_image.h or extend this file.

#ifndef STB_IMAGE_H
#define STB_IMAGE_H

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned char stbi_uc;

stbi_uc* stbi_load_from_memory(const stbi_uc* buffer, int len, int* x, int* y, int* channels_in_file, int req_comp);
void stbi_image_free(void* retval_from_stbi_load);

#ifdef __cplusplus
}
#endif

#ifdef STB_IMAGE_IMPLEMENTATION

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <wincodec.h>
#include <stdlib.h>

static stbi_uc* stbi__wic_load_rgba_from_memory(const stbi_uc* buffer, int len, int* out_w, int* out_h)
{
	if (!buffer || len <= 0) return NULL;
	HRESULT hr;
	BOOL didInit = FALSE;
	hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
	if (SUCCEEDED(hr)) didInit = TRUE;

	IWICImagingFactory* factory = NULL;
	hr = CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory, (void**)&factory);
	if (FAILED(hr)) { if (didInit) CoUninitialize(); return NULL; }

	IWICStream* stream = NULL;
	hr = factory->lpVtbl->CreateStream(factory, &stream);
	if (FAILED(hr)) { factory->lpVtbl->Release(factory); if (didInit) CoUninitialize(); return NULL; }

	hr = stream->lpVtbl->InitializeFromMemory(stream, (BYTE*)buffer, (DWORD)len);
	if (FAILED(hr)) { stream->lpVtbl->Release(stream); factory->lpVtbl->Release(factory); if (didInit) CoUninitialize(); return NULL; }

	IWICBitmapDecoder* decoder = NULL;
	hr = factory->lpVtbl->CreateDecoderFromStream(factory, (IStream*)stream, NULL, WICDecodeMetadataCacheOnDemand, &decoder);
	if (FAILED(hr)) { stream->lpVtbl->Release(stream); factory->lpVtbl->Release(factory); if (didInit) CoUninitialize(); return NULL; }

	IWICBitmapFrameDecode* frame = NULL;
	hr = decoder->lpVtbl->GetFrame(decoder, 0, &frame);
	if (FAILED(hr)) { decoder->lpVtbl->Release(decoder); stream->lpVtbl->Release(stream); factory->lpVtbl->Release(factory); if (didInit) CoUninitialize(); return NULL; }

	UINT w = 0, h = 0;
	frame->lpVtbl->GetSize(frame, &w, &h);
	if (w == 0 || h == 0) { frame->lpVtbl->Release(frame); decoder->lpVtbl->Release(decoder); stream->lpVtbl->Release(stream); factory->lpVtbl->Release(factory); if (didInit) CoUninitialize(); return NULL; }

	IWICFormatConverter* converter = NULL;
	hr = factory->lpVtbl->CreateFormatConverter(factory, &converter);
	if (FAILED(hr)) { frame->lpVtbl->Release(frame); decoder->lpVtbl->Release(decoder); stream->lpVtbl->Release(stream); factory->lpVtbl->Release(factory); if (didInit) CoUninitialize(); return NULL; }

	hr = converter->lpVtbl->Initialize(converter, (IWICBitmapSource*)frame, &GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, NULL, 0.0f, WICBitmapPaletteTypeCustom);
	if (FAILED(hr)) { converter->lpVtbl->Release(converter); frame->lpVtbl->Release(frame); decoder->lpVtbl->Release(decoder); stream->lpVtbl->Release(stream); factory->lpVtbl->Release(factory); if (didInit) CoUninitialize(); return NULL; }

	const size_t stride = (size_t)w * 4;
	const size_t total = stride * (size_t)h;
	stbi_uc* pixels = (stbi_uc*)malloc(total);
	if (!pixels) { converter->lpVtbl->Release(converter); frame->lpVtbl->Release(frame); decoder->lpVtbl->Release(decoder); stream->lpVtbl->Release(stream); factory->lpVtbl->Release(factory); if (didInit) CoUninitialize(); return NULL; }

	hr = converter->lpVtbl->CopyPixels(converter, NULL, (UINT)stride, (UINT)total, pixels);
	if (FAILED(hr)) { free(pixels); pixels = NULL; }

	converter->lpVtbl->Release(converter);
	frame->lpVtbl->Release(frame);
	decoder->lpVtbl->Release(decoder);
	stream->lpVtbl->Release(stream);
	factory->lpVtbl->Release(factory);
	if (didInit) CoUninitialize();

	if (pixels) {
		if (out_w) *out_w = (int)w;
		if (out_h) *out_h = (int)h;
	}
	return pixels;
}

stbi_uc* stbi_load_from_memory(const stbi_uc* buffer, int len, int* x, int* y, int* channels_in_file, int req_comp)
{
	(void)req_comp; // We always return RGBA
	if (channels_in_file) *channels_in_file = 4;
	return stbi__wic_load_rgba_from_memory(buffer, len, x, y);
}

void stbi_image_free(void* retval_from_stbi_load)
{
	if (retval_from_stbi_load) free(retval_from_stbi_load);
}

#else
#error "This minimal stb_image implementation currently supports only Windows via WIC. Provide the official stb_image.h for other platforms."
#endif // _WIN32

#endif // STB_IMAGE_IMPLEMENTATION

#endif // STB_IMAGE_H
