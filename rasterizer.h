#pragma once

#include <stdint.h>

struct framebuffer_t;

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

void draw(
    framebuffer_t* fb,
    const int32_t* vertices,
    uint32_t num_vertices);

void draw_indexed(
    framebuffer_t* fb,
    const int32_t* vertices,
    const uint32_t* indices,
    uint32_t num_indices);