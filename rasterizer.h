#pragma once

#include <stdint.h>

struct framebuffer_t;
typedef uint32_t pixel_t;

typedef enum pixelformat_t
{
    pixelformat_r8g8b8a8_unorm,
    pixelformat_b8g8r8a8_unorm
} pixelformat_t;

framebuffer_t* new_framebuffer(uint32_t width, uint32_t height);
void delete_framebuffer(framebuffer_t* fb);
void framebuffer_resolve(framebuffer_t* fb);
void framebuffer_pack_row_major(framebuffer_t* fb, uint32_t x, uint32_t y, uint32_t width, uint32_t height, pixelformat_t format, void* data);

// hack
extern uint32_t g_Color;

// Rasterizes a triangle with its vertices represented as 16.8 fixed point values
// Arguments:
// * fb: The framebuffer the triangle is written to. Pixels are assumed in BGRA format
// * fb_width: The width in pixels of the framebuffer
// * window_xi, window_yi (i in [0..2]): coordinate of vertex i of the triangle, encoded as 16.8 fixed point.
// * window_zi (i in [0..2]): depth of the vertex i of the triangle.
// Preconditions:
// * The triangle vertices are stored clockwise (relative to their position on the display)
void rasterize_triangle_fixed16_8(
    framebuffer_t* fb,
    uint32_t window_x0, uint32_t window_y0, uint32_t window_z0,
    uint32_t window_x1, uint32_t window_y1, uint32_t window_z1,
    uint32_t window_x2, uint32_t window_y2, uint32_t window_z2);