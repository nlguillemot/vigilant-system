#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef _WIN32
#ifdef RASTERIZER_EXPORTS
#define RASTERIZER_API __declspec(dllexport)
#else
#define RASTERIZER_API __declspec(dllimport)
#endif
#else
#define RASTERIZER_API
#endif

struct framebuffer_t;

typedef enum pixelformat_t
{
    pixelformat_r8g8b8a8_unorm,
    pixelformat_b8g8r8a8_unorm
} pixelformat_t;

RASTERIZER_API framebuffer_t* new_framebuffer(uint32_t width, uint32_t height);
RASTERIZER_API void delete_framebuffer(framebuffer_t* fb);
RASTERIZER_API void framebuffer_resolve(framebuffer_t* fb);
RASTERIZER_API void framebuffer_pack_row_major(framebuffer_t* fb, uint32_t x, uint32_t y, uint32_t width, uint32_t height, pixelformat_t format, void* data);
RASTERIZER_API void framebuffer_clear(framebuffer_t* fb, uint32_t color);

RASTERIZER_API void rasterizer_draw(
    framebuffer_t* fb,
    const int32_t* vertices,
    uint32_t num_vertices);

RASTERIZER_API void rasterizer_draw_indexed(
    framebuffer_t* fb,
    const int32_t* vertices,
    const uint32_t* indices,
    uint32_t num_indices);

// hack
RASTERIZER_API void rasterizer_set_color(uint32_t col);

#ifdef __cplusplus
} // end extern "C"
#endif