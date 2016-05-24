#include "rasterizer.h"

#include <stdlib.h>
#include <string.h>
#include <assert.h>

// Fine raster works on blocks of 2x2 pixels, since NEON can fine rasterize a block in parallel using SIMD-4.
// Coarse raster works on 2x2 blocks of fine blocks, since 4 fine blocks are processed in parallel. Therefore, 4x4 pixels per coarse block.
// Tile raster works on 2x2 blocks of coarse blocks, since 4 coarse blocks are processed in parallel. Therefore, 8x8 pixels per tile.
#define FRAMEBUFFER_TILE_SIZE_IN_PIXELS 8
#define FRAMEBUFFER_COARSE_BLOCK_SIZE_IN_PIXELS 4
#define FRAMEBUFFER_FINE_BLOCK_SIZE_IN_PIXELS 2

// The swizzle masks, using alternating yxyxyx bit pattern for morton-code swizzling pixels in a tile.
// This makes the pixels morton code swizzled within every rasterization level (fine/coarse/tile)
// The tiles themselevs are stored row major.
// For examples of this concept, see:
// https://software.intel.com/en-us/node/514045
// https://msdn.microsoft.com/en-us/library/windows/desktop/dn770442%28v=vs.85%29.aspx
#define FRAMEBUFFER_TILE_X_SWIZZLE_MASK 0b01'0101
#define FRAMEBUFFER_TILE_Y_SWIZZLE_MASK 0b10'1010

// Convenience
#define FRAMEBUFFER_PIXELS_PER_TILE (FRAMEBUFFER_TILE_SIZE_IN_PIXELS * FRAMEBUFFER_TILE_SIZE_IN_PIXELS)

typedef struct framebuffer_t
{
    pixel_t* backbuffer;
    
    uint32_t width_in_pixels;
    uint32_t height_in_pixels;
    
    // number of pixels in a row of tiles (num_tiles_per_row * num_pixels_per_tile)
    uint32_t pixels_per_tile_row;
    // number of pixels in the whole image (num_pixels_per_tile_row * num_tile_rows)
    uint32_t pixels_per_slice;

    // the swizzle masks used to organize the storage of pixels within an individual tile
    // tiles themselves are stored row-major order
    uint32_t tile_x_swizzle_mask;
    uint32_t tile_y_swizzle_mask;
} framebuffer_t;

framebuffer_t* new_framebuffer(uint32_t width, uint32_t height)
{
    // limits of the rasterizer's precision
    // this is based on an analysis of the range of results of the 2D cross product between two fixed16.8 numbers.
    assert(width < 16384);
    assert(height < 16384);

    framebuffer_t* fb = (framebuffer_t*)malloc(sizeof(framebuffer_t));
    assert(fb);

    fb->width_in_pixels = width;
    fb->height_in_pixels = height;

    // pad framebuffer up to size of next tile
    // that way the rasterization code doesn't have to handlep otential out of bounds access after tile binning
    int32_t padded_width_in_pixels = (width + (FRAMEBUFFER_TILE_SIZE_IN_PIXELS - 1)) & -FRAMEBUFFER_TILE_SIZE_IN_PIXELS;
    int32_t padded_height_in_pixels = (height + (FRAMEBUFFER_TILE_SIZE_IN_PIXELS - 1)) & -FRAMEBUFFER_TILE_SIZE_IN_PIXELS;

    fb->pixels_per_tile_row = padded_width_in_pixels * FRAMEBUFFER_TILE_SIZE_IN_PIXELS;
    fb->pixels_per_slice = padded_height_in_pixels / FRAMEBUFFER_TILE_SIZE_IN_PIXELS * fb->pixels_per_tile_row;

    fb->backbuffer = (pixel_t*)malloc(fb->pixels_per_slice * sizeof(pixel_t));
    assert(fb->backbuffer);
 
    // clear to black/transparent initially
    memset(fb->backbuffer, 0, fb->pixels_per_slice * sizeof(pixel_t));

    return fb;
}

void delete_framebuffer(framebuffer_t* fb)
{
    if (!fb)
        return;

    free(fb->backbuffer);
    free(fb);
}

void framebuffer_resolve(framebuffer_t* fb)
{

}

void framebuffer_pack_row_major(framebuffer_t* fb, uint32_t x, uint32_t y, uint32_t width, uint32_t height, pixelformat_t format, void* data)
{
    assert(fb);
    assert(x < fb->width_in_pixels);
    assert(y < fb->height_in_pixels);
    assert(width <= fb->width_in_pixels);
    assert(height <= fb->height_in_pixels);
    assert(x + width <= fb->width_in_pixels);
    assert(y + height <= fb->height_in_pixels);
    assert(data);

    uint32_t topleft_tile_y = y / FRAMEBUFFER_TILE_SIZE_IN_PIXELS;
    uint32_t topleft_tile_x = x / FRAMEBUFFER_TILE_SIZE_IN_PIXELS;
    uint32_t bottomright_tile_y = (y + height - 1) / FRAMEBUFFER_TILE_SIZE_IN_PIXELS;
    uint32_t bottomright_tile_x = (x + width - 1) / FRAMEBUFFER_TILE_SIZE_IN_PIXELS;
    
    uint32_t dst_i = 0;

    uint32_t curr_tile_row_start = topleft_tile_y * fb->pixels_per_tile_row + topleft_tile_x * FRAMEBUFFER_PIXELS_PER_TILE;
    for (uint32_t tile_y = topleft_tile_y; tile_y <= bottomright_tile_y; tile_y++)
    {
        uint32_t curr_tile_start = curr_tile_row_start;

        for (uint32_t tile_x = topleft_tile_x; tile_x <= bottomright_tile_x; tile_x++)
        {
            for (uint32_t pixel_y = 0, pixel_y_bits = 0;
                pixel_y < FRAMEBUFFER_TILE_SIZE_IN_PIXELS;
                pixel_y++, pixel_y_bits = (pixel_y_bits - FRAMEBUFFER_TILE_Y_SWIZZLE_MASK) & FRAMEBUFFER_TILE_Y_SWIZZLE_MASK)
            {
                for (uint32_t pixel_x = 0, pixel_x_bits = 0;
                    pixel_x < FRAMEBUFFER_TILE_SIZE_IN_PIXELS;
                    pixel_x++, pixel_x_bits = (pixel_x_bits - FRAMEBUFFER_TILE_X_SWIZZLE_MASK) & FRAMEBUFFER_TILE_X_SWIZZLE_MASK)
                {
                    uint32_t src_y = tile_y * FRAMEBUFFER_TILE_SIZE_IN_PIXELS + pixel_y;
                    uint32_t src_x = tile_x * FRAMEBUFFER_TILE_SIZE_IN_PIXELS + pixel_x;

                    // don't copy pixels outside src rectangle region
                    if (src_y < y || src_y >= y + height)
                        continue;
                    if (src_x < x || src_x >= x + width)
                        continue;

                    uint32_t src_i = curr_tile_start + (pixel_y_bits | pixel_x_bits);
                    pixel_t src = fb->backbuffer[src_i];
                    if (format == pixelformat_r8g8b8a8_unorm)
                    {
                        uint8_t* dst = (uint8_t*)data + dst_i * 4;
                        dst[0] = (uint8_t)((src & 0x00FF0000) >> 16);
                        dst[1] = (uint8_t)((src & 0x0000FF00) >> 8);
                        dst[2] = (uint8_t)((src & 0x000000FF) >> 0);
                        dst[3] = (uint8_t)((src & 0xFF000000) >> 24);
                    }
                    else if (format == pixelformat_b8g8r8a8_unorm)
                    {
                        uint8_t* dst = (uint8_t*)data + dst_i * 4;
                        dst[0] = (uint8_t)((src & 0x000000FF) >> 0);
                        dst[1] = (uint8_t)((src & 0x0000FF00) >> 8);
                        dst[2] = (uint8_t)((src & 0x00FF0000) >> 16);
                        dst[3] = (uint8_t)((src & 0xFF000000) >> 24);
                    }
                    else
                    {
                        assert(false);
                    }

                    dst_i++;
                }
            }

            curr_tile_start += FRAMEBUFFER_PIXELS_PER_TILE;
        }

        curr_tile_row_start += fb->pixels_per_tile_row;
    }
}

// hack
uint32_t g_Color;

void rasterize_triangle_fixed16_8(
    framebuffer_t* fb,
    uint32_t window_x0, uint32_t window_y0, uint32_t window_z0,
    uint32_t window_x1, uint32_t window_y1, uint32_t window_z1,
    uint32_t window_x2, uint32_t window_y2, uint32_t window_z2)
{
}