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

typedef enum attachment_t
{
    attachment_color0,
    attachment_depth
} attachment_t;

typedef enum pixelformat_t
{
    pixelformat_r8g8b8a8_unorm,
    pixelformat_b8g8r8a8_unorm,
    pixelformat_r32_unorm
} pixelformat_t;

RASTERIZER_API framebuffer_t* new_framebuffer(int32_t width, int32_t height);
RASTERIZER_API void delete_framebuffer(framebuffer_t* fb);

RASTERIZER_API void framebuffer_clear(framebuffer_t* fb, uint32_t color);
RASTERIZER_API void framebuffer_resolve(framebuffer_t* fb);
RASTERIZER_API void framebuffer_pack_row_major(framebuffer_t* fb, attachment_t attachment, int32_t x, int32_t y, int32_t width, int32_t height, pixelformat_t format, void* data);

RASTERIZER_API void framebuffer_draw(
    framebuffer_t* fb,
    const int32_t* vertices,
    uint32_t num_vertices);

RASTERIZER_API void framebuffer_draw_indexed(
    framebuffer_t* fb,
    const int32_t* vertices,
    const uint32_t* indices,
    uint32_t num_indices);

RASTERIZER_API int32_t framebuffer_get_total_num_tiles(framebuffer_t* fb); // to know how big an array to pass to get_tile_perfcounters
RASTERIZER_API uint64_t framebuffer_get_perfcounter_frequency(framebuffer_t* fb);
RASTERIZER_API void framebuffer_reset_perfcounters(framebuffer_t* fb);
RASTERIZER_API int32_t framebuffer_get_num_perfcounters(framebuffer_t* fb);
RASTERIZER_API void framebuffer_get_perfcounters(framebuffer_t* fb, uint64_t* pcs);
RASTERIZER_API void framebuffer_get_perfcounter_names(framebuffer_t* fb, const char** names);
RASTERIZER_API int32_t framebuffer_get_num_tile_perfcounters(framebuffer_t* fb);
RASTERIZER_API void framebuffer_get_tile_perfcounter_names(framebuffer_t* fb, const char** names);
RASTERIZER_API void framebuffer_get_tile_perfcounters(framebuffer_t* fb, uint64_t* tile_pcs); // grabs perfcounters for ALL tiles: (total_num_tiles * num_tile_perfcounters) counters.

#ifdef __cplusplus
} // end extern "C"
#endif