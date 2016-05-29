// This library implements a Pineda-style software rasterizer inspired from Larrabee's rasterizer.
// See "A Parallel Algorithm for Polygon Rasterization", by Juan Pineda, SIGGRAPH '88:
// http://citeseerx.ist.psu.edu/viewdoc/download?doi=10.1.1.157.4621&rep=rep1&type=pdf
// Also see Michael Abrash's article "Rasterization on Larrabee":
// https://software.intel.com/en-us/articles/rasterization-on-larrabee
// For a modern take on this algorithm, see Fabian Giesen's GPU pipeline and Software Occlusion Culling blog series:
// https://fgiesen.wordpress.com/2011/07/09/a-trip-through-the-graphics-pipeline-2011-index/
// https://fgiesen.wordpress.com/2013/02/17/optimizing-sw-occlusion-culling-index/

#include "rasterizer.h"

#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>

#ifdef __AVX2__
#include <intrin.h>
#endif

// runs unit tests automatically when the library is used
#define RASTERIZER_UNIT_TESTS

#ifdef RASTERIZER_UNIT_TESTS
void run_rasterizer_unit_tests();
#endif

// Sized according to the Larrabee rasterizer's description
#define FRAMEBUFFER_TILE_WIDTH_IN_PIXELS 128
#define FRAMEBUFFER_COARSE_BLOCK_WIDTH_IN_PIXELS 16
#define FRAMEBUFFER_FINE_BLOCK_WIDTH_IN_PIXELS 4

// small sizes (for testing)
// #define FRAMEBUFFER_TILE_WIDTH_IN_PIXELS 4
// #define FRAMEBUFFER_COARSE_BLOCK_WIDTH_IN_PIXELS 2
// #define FRAMEBUFFER_FINE_BLOCK_WIDTH_IN_PIXELS 1

// Convenience
#define FRAMEBUFFER_PIXELS_PER_TILE (FRAMEBUFFER_TILE_WIDTH_IN_PIXELS * FRAMEBUFFER_TILE_WIDTH_IN_PIXELS)
 
// The swizzle masks, using alternating yxyxyx bit pattern for morton-code swizzling pixels in a tile.
// This makes the pixels morton code swizzled within every rasterization level (fine/coarse/tile)
// The tiles themselves are stored row major.
// For examples of this concept, see:
// https://software.intel.com/en-us/node/514045
// https://msdn.microsoft.com/en-us/library/windows/desktop/dn770442%28v=vs.85%29.aspx
#define FRAMEBUFFER_TILE_X_SWIZZLE_MASK (0x55555555 & (FRAMEBUFFER_PIXELS_PER_TILE - 1))
#define FRAMEBUFFER_TILE_Y_SWIZZLE_MASK (0xAAAAAAAA & (FRAMEBUFFER_PIXELS_PER_TILE - 1))

// If there are too many commands and this buffer gets filled up,
// then the command buffer for that tile must be flushed.
#define TILE_COMMAND_BUFFER_SIZE_IN_DWORDS 128

// width of the SIMD instruction set used
#define SIMD_PROGRAM_COUNT 4

// parallel bit deposit low-order source bits according to mask bits
#ifdef __AVX2__
__forceinline uint32_t pdep_u32(uint32_t source, uint32_t mask)
{
    return _pdep_u32(source, mask);
}
#else
__forceinline uint32_t pdep_u32(uint32_t source, uint32_t mask)
{
    // horribly inefficient, but that's life without AVX2.
    // however, typically not a problem since you only need to swizzle once up front.
    uint32_t dst = 0;
    for (uint32_t mask_i = 0, dst_i = 0; mask_i < 32; mask_i++)
    {
        if (mask & (1 << mask_i))
        {
            uint32_t src_bit = (source & (1 << dst_i)) >> dst_i;
            dst |= src_bit << mask_i;

            dst_i++;
        }
    }
    return dst;
}
#endif

typedef struct tile_cmdbuf_t
{
    // start and past-the-end of the allocation for the buffer
    uint32_t* cmdbuf_start;
    uint32_t* cmdbuf_end;
    // the next location where to read and write commands
    uint32_t* cmdbuf_read;
    uint32_t* cmdbuf_write;
} tile_cmdbuf_t;

typedef enum tilecmd_id_t
{
    tilecmd_id_drawsmalltri
} tilecmd_id_t;

typedef struct tilecmd_drawsmalltri_t
{
    uint32_t tilecmd_id;
    int32_t x0, y0, z0, w0;
    int32_t x1, y1, z1, w1;
    int32_t x2, y2, z2, w2;
} tilecmd_drawsmalltri_t;

typedef struct framebuffer_t
{
    pixel_t* backbuffer;
    
    uint32_t* tile_cmdpool;
    tile_cmdbuf_t* tile_cmdbufs;
    
    uint32_t width_in_pixels;
    uint32_t height_in_pixels;

    uint32_t width_in_tiles;
    uint32_t height_in_tiles;
    uint32_t total_num_tiles;
    
    // num_tiles_per_row * num_pixels_per_tile
    uint32_t pixels_per_row_of_tiles;

    // pixels_per_row_of_tiles * num_tile_rows
    uint32_t pixels_per_slice;
} framebuffer_t;

framebuffer_t* new_framebuffer(uint32_t width, uint32_t height)
{
#ifdef RASTERIZER_UNIT_TESTS
    static int ran_rasterizer_unit_tests_once = 0;
    if (!ran_rasterizer_unit_tests_once)
    {
        // set this before running the tests, so that unit tests can create framebuffers without causing infinite recursion
        ran_rasterizer_unit_tests_once = 1;
        run_rasterizer_unit_tests();
    }
#endif

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
    uint32_t padded_width_in_pixels = (width + (FRAMEBUFFER_TILE_WIDTH_IN_PIXELS - 1)) & -FRAMEBUFFER_TILE_WIDTH_IN_PIXELS;
    uint32_t padded_height_in_pixels = (height + (FRAMEBUFFER_TILE_WIDTH_IN_PIXELS - 1)) & -FRAMEBUFFER_TILE_WIDTH_IN_PIXELS;
    
    fb->width_in_tiles = padded_width_in_pixels / FRAMEBUFFER_TILE_WIDTH_IN_PIXELS;
    fb->height_in_tiles = padded_height_in_pixels / FRAMEBUFFER_TILE_WIDTH_IN_PIXELS;
    fb->total_num_tiles = fb->width_in_tiles * fb->height_in_tiles;

    fb->pixels_per_row_of_tiles = padded_width_in_pixels * FRAMEBUFFER_TILE_WIDTH_IN_PIXELS;
    fb->pixels_per_slice = padded_height_in_pixels / FRAMEBUFFER_TILE_WIDTH_IN_PIXELS * fb->pixels_per_row_of_tiles;

    fb->backbuffer = (pixel_t*)malloc(fb->pixels_per_slice * sizeof(pixel_t));
    assert(fb->backbuffer);
 
    // clear to black/transparent initially
    memset(fb->backbuffer, 0, fb->pixels_per_slice * sizeof(pixel_t));

    // allocate command lists for each tile
    fb->tile_cmdpool = (uint32_t*)malloc(fb->total_num_tiles * TILE_COMMAND_BUFFER_SIZE_IN_DWORDS * sizeof(uint32_t));
    assert(fb->tile_cmdpool);

    fb->tile_cmdbufs = (tile_cmdbuf_t*)malloc(fb->total_num_tiles * sizeof(tile_cmdbuf_t));
    assert(fb->tile_cmdbufs);

    // command lists are circular queues that are initially empty
    for (uint32_t i = 0; i < fb->total_num_tiles; i++)
    {
        fb->tile_cmdbufs[i].cmdbuf_start = &fb->tile_cmdpool[i * TILE_COMMAND_BUFFER_SIZE_IN_DWORDS];
        fb->tile_cmdbufs[i].cmdbuf_end = fb->tile_cmdbufs[i].cmdbuf_start + TILE_COMMAND_BUFFER_SIZE_IN_DWORDS;
        fb->tile_cmdbufs[i].cmdbuf_read = fb->tile_cmdbufs[i].cmdbuf_start;
        fb->tile_cmdbufs[i].cmdbuf_write = fb->tile_cmdbufs[i].cmdbuf_start;
    }

    return fb;
}

void delete_framebuffer(framebuffer_t* fb)
{
    if (!fb)
        return;

    free(fb->tile_cmdbufs);
    free(fb->tile_cmdpool);
    free(fb->backbuffer);
    free(fb);
}

static void framebuffer_push_tilecmd(framebuffer_t* fb, uint32_t tile_id, const uint32_t* cmd_dwords, uint32_t num_dwords)
{

}

void framebuffer_resolve(framebuffer_t* fb)
{
    assert(fb);

    // TODO: flush all tile command buffers
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

    uint32_t topleft_tile_y = y / FRAMEBUFFER_TILE_WIDTH_IN_PIXELS;
    uint32_t topleft_tile_x = x / FRAMEBUFFER_TILE_WIDTH_IN_PIXELS;
    uint32_t bottomright_tile_y = (y + (height - 1)) / FRAMEBUFFER_TILE_WIDTH_IN_PIXELS;
    uint32_t bottomright_tile_x = (x + (width - 1)) / FRAMEBUFFER_TILE_WIDTH_IN_PIXELS;
    
    uint32_t dst_i = 0;

    uint32_t curr_tile_row_start = topleft_tile_y * fb->pixels_per_row_of_tiles + topleft_tile_x * FRAMEBUFFER_PIXELS_PER_TILE;
    for (uint32_t tile_y = topleft_tile_y; tile_y <= bottomright_tile_y; tile_y++)
    {
        uint32_t curr_tile_start = curr_tile_row_start;

        for (uint32_t tile_x = topleft_tile_x; tile_x <= bottomright_tile_x; tile_x++)
        {
            uint32_t topleft_y = tile_y * FRAMEBUFFER_TILE_WIDTH_IN_PIXELS;
            uint32_t topleft_x = tile_x * FRAMEBUFFER_TILE_WIDTH_IN_PIXELS;
            uint32_t bottomright_y = topleft_y + FRAMEBUFFER_TILE_WIDTH_IN_PIXELS;
            uint32_t bottomright_x = topleft_x + FRAMEBUFFER_TILE_WIDTH_IN_PIXELS;
            uint32_t pixel_y_min = topleft_y < y ? y : topleft_y;
            uint32_t pixel_x_min = topleft_x < x ? x : topleft_x;
            uint32_t pixel_y_max = bottomright_y > y + height ? y + height : bottomright_y;
            uint32_t pixel_x_max = bottomright_x > x + width ? x + width : bottomright_x;

            for (uint32_t pixel_y = pixel_y_min, pixel_y_bits = pdep_u32(topleft_y, FRAMEBUFFER_TILE_Y_SWIZZLE_MASK);
                pixel_y < pixel_y_max;
                pixel_y++, pixel_y_bits = (pixel_y_bits - FRAMEBUFFER_TILE_Y_SWIZZLE_MASK) & FRAMEBUFFER_TILE_Y_SWIZZLE_MASK)
            {
                for (uint32_t pixel_x = pixel_x_min, pixel_x_bits = pdep_u32(topleft_x, FRAMEBUFFER_TILE_X_SWIZZLE_MASK);
                    pixel_x < pixel_x_max;
                    pixel_x++, pixel_x_bits = (pixel_x_bits - FRAMEBUFFER_TILE_X_SWIZZLE_MASK) & FRAMEBUFFER_TILE_X_SWIZZLE_MASK)
                {
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
                        assert(!"Unknown pixel format");
                    }

                    dst_i++;
                }
            }

            curr_tile_start += FRAMEBUFFER_PIXELS_PER_TILE;
        }

        curr_tile_row_start += fb->pixels_per_row_of_tiles;
    }
}

// hack
uint32_t g_Color;

void rasterize_triangle_fixed16_8_scalar(
    framebuffer_t* fb,
    int32_t x0, int32_t y0, int32_t z0, int32_t w0,
    int32_t x1, int32_t y1, int32_t z1, int32_t w1,
    int32_t x2, int32_t y2, int32_t z2, int32_t w2)
{
    // get window coordinates bounding box
    int32_t bbox_min_x = x0;
    if (x1 < bbox_min_x) bbox_min_x = x1;
    if (x2 < bbox_min_x) bbox_min_x = x2;
    int32_t bbox_max_x = x0;
    if (x1 > bbox_max_x) bbox_max_x = x1;
    if (x2 > bbox_max_x) bbox_max_x = x2;
    int32_t bbox_min_y = y0;
    if (y1 < bbox_min_y) bbox_min_y = y1;
    if (y2 < bbox_min_y) bbox_min_y = y2;
    int32_t bbox_max_y = y0;
    if (y1 > bbox_max_y) bbox_max_y = y1;
    if (y2 > bbox_max_y) bbox_max_y = y2;

    // clip triangles that are fully outside the scissor rect (scissor rect = whole window)
    if (bbox_max_x < 0 ||
        bbox_max_y < 0 ||
        bbox_min_x >= (fb->width_in_pixels << 8) ||
        bbox_min_y >= (fb->height_in_pixels << 8))
    {
        return;
    }

    // clip bbox to scissor rect
    if (bbox_min_x < 0) bbox_min_x = 0;
    if (bbox_min_y < 0) bbox_min_y = 0;
    if (bbox_max_x >= (fb->width_in_pixels << 8)) bbox_max_x = fb->width_in_pixels << 8;
    if (bbox_max_y >= (fb->height_in_pixels << 8)) bbox_max_y = fb->height_in_pixels << 8;

    // "small" triangles are no wider than a tile.
    int32_t is_large =
        (bbox_max_x - bbox_min_x) >= FRAMEBUFFER_TILE_WIDTH_IN_PIXELS ||
        (bbox_max_y - bbox_min_y) >= FRAMEBUFFER_TILE_WIDTH_IN_PIXELS;

    if (!is_large)
    {
        // since this is a small triangle, that means the triangle is smaller than a tile.
        // that means it can overlap at most 2x2 adjacent tiles if it's in the middle of all of them.
        // just need to figure out which boxes are overlapping the triangle's bbox
        uint32_t first_tile_x = (bbox_min_x >> 8) / FRAMEBUFFER_TILE_WIDTH_IN_PIXELS;
        uint32_t first_tile_y = (bbox_min_y >> 8) / FRAMEBUFFER_TILE_WIDTH_IN_PIXELS;
        uint32_t last_tile_x = (bbox_max_x >> 8) / FRAMEBUFFER_TILE_WIDTH_IN_PIXELS;
        uint32_t last_tile_y = (bbox_max_y >> 8) / FRAMEBUFFER_TILE_WIDTH_IN_PIXELS;

        tilecmd_drawsmalltri_t drawsmalltricmd;
        drawsmalltricmd.tilecmd_id = tilecmd_id_drawsmalltri;
        drawsmalltricmd.x0 = x0;
        drawsmalltricmd.x1 = x1;
        drawsmalltricmd.x2 = x2;
        drawsmalltricmd.y0 = y0;
        drawsmalltricmd.y1 = y1;
        drawsmalltricmd.y2 = y2;
        drawsmalltricmd.z0 = z0;
        drawsmalltricmd.z1 = z1;
        drawsmalltricmd.z2 = z2;
        drawsmalltricmd.w0 = w0;
        drawsmalltricmd.w1 = w1;
        drawsmalltricmd.w2 = w2;

        uint32_t first_tile_id = first_tile_y * fb->width_in_tiles + first_tile_x;
        framebuffer_push_tilecmd(fb, first_tile_id, &drawsmalltricmd.tilecmd_id, sizeof(drawsmalltricmd) / sizeof(uint32_t));

        if (last_tile_x > first_tile_x)
        {
            uint32_t tile_id_right = first_tile_id + 1;
            framebuffer_push_tilecmd(fb, tile_id_right, &drawsmalltricmd.tilecmd_id, sizeof(drawsmalltricmd) / sizeof(uint32_t));
        }

        if (last_tile_y > first_tile_y)
        {
            uint32_t tile_id_down = first_tile_id + fb->width_in_tiles;
            framebuffer_push_tilecmd(fb, tile_id_down, &drawsmalltricmd.tilecmd_id, sizeof(drawsmalltricmd) / sizeof(uint32_t));
        }

        if (last_tile_x > first_tile_x && last_tile_y > first_tile_y)
        {
            uint32_t tile_id_downright = first_tile_id + 1 + fb->width_in_tiles;
            framebuffer_push_tilecmd(fb, tile_id_downright, &drawsmalltricmd.tilecmd_id, sizeof(drawsmalltricmd) / sizeof(uint32_t));
        }
    }
    else
    {
        // for large triangles, test each tile in their bbox for overlap
        // done using scalar code for simplicity, since rasterization dominates large triangle performance anyways.
        uint32_t first_tile_x = (bbox_min_x >> 8) / FRAMEBUFFER_TILE_WIDTH_IN_PIXELS;
        uint32_t first_tile_y = (bbox_min_y >> 8) / FRAMEBUFFER_TILE_WIDTH_IN_PIXELS;
        uint32_t last_tile_x = (bbox_max_x >> 8) / FRAMEBUFFER_TILE_WIDTH_IN_PIXELS;
        uint32_t last_tile_y = (bbox_max_y >> 8) / FRAMEBUFFER_TILE_WIDTH_IN_PIXELS;

        uint32_t tile_row_start = first_tile_y * fb->width_in_tiles;
        for (uint32_t tile_y = first_tile_y; tile_y <= last_tile_y; tile_y++)
        {
            uint32_t tile_i = tile_row_start;

            for (uint32_t tile_x = first_tile_x; tile_x <= last_tile_x; tile_x++)
            {


                tile_i++;
            }

            tile_row_start += fb->width_in_tiles;
        }
    }
}

void rasterize_triangle_fixed16_8_simd4(
    framebuffer_t* fb,
    const int32_t* x0, const int32_t* y0, const int32_t* z0, const int32_t* w0,
    const int32_t* x1, const int32_t* y1, const int32_t* z1, const int32_t* w1,
    const int32_t* x2, const int32_t* y2, const int32_t* z2, const int32_t* w2)
{
    // shite implementation. Get scalar figured out first.
    for (uint32_t i = 0; i < 4; i++)
    {
        rasterize_triangle_fixed16_8_scalar(
            fb,
            x0[i], y0[i], z0[i], w0[i],
            x1[i], y1[i], z1[i], w1[i],
            x2[i], y2[i], z2[i], w2[i]);
    }
}

void draw(
    framebuffer_t* fb,
    const int32_t* vertices,
    uint32_t num_vertices)
{
    assert(fb);
    assert(vertices);

    // assuming triangles
    assert(num_vertices % 3 == 0);
    
    // number of loops that can be done with SIMD and that are in groups of 3 (3 vertices per triangle)
    uint32_t num_simd_vertices = num_vertices - (num_vertices % (SIMD_PROGRAM_COUNT * 3));

    for (uint32_t vertex_id = 0; vertex_id < num_simd_vertices; vertex_id += SIMD_PROGRAM_COUNT * 3)
    {
        int32_t x0[SIMD_PROGRAM_COUNT], y0[SIMD_PROGRAM_COUNT], z0[SIMD_PROGRAM_COUNT], w0[SIMD_PROGRAM_COUNT];
        int32_t x1[SIMD_PROGRAM_COUNT], y1[SIMD_PROGRAM_COUNT], z1[SIMD_PROGRAM_COUNT], w1[SIMD_PROGRAM_COUNT];
        int32_t x2[SIMD_PROGRAM_COUNT], y2[SIMD_PROGRAM_COUNT], z2[SIMD_PROGRAM_COUNT], w2[SIMD_PROGRAM_COUNT];
        
        for (uint32_t program_index = 0, cmpt_id = vertex_id * 4; program_index < SIMD_PROGRAM_COUNT; program_index++, cmpt_id += 12)
        {
            x0[program_index] = vertices[cmpt_id + 0];
            y0[program_index] = vertices[cmpt_id + 1];
            z0[program_index] = vertices[cmpt_id + 2];
            w0[program_index] = vertices[cmpt_id + 3];
            x1[program_index] = vertices[cmpt_id + 4];
            y1[program_index] = vertices[cmpt_id + 5];
            z1[program_index] = vertices[cmpt_id + 6];
            w1[program_index] = vertices[cmpt_id + 7];
            x2[program_index] = vertices[cmpt_id + 8];
            y2[program_index] = vertices[cmpt_id + 9];
            z2[program_index] = vertices[cmpt_id + 10];
            w2[program_index] = vertices[cmpt_id + 11];
        }

        rasterize_triangle_fixed16_8_simd4(
            fb,
            x0, y0, z0, w0,
            x1, y1, z1, w1,
            x2, y2, z2, w2);
    }

    for (uint32_t vertex_id = num_simd_vertices, cmpt_id = vertex_id * 4; vertex_id < num_vertices; vertex_id += 3, cmpt_id += 12)
    {
        int32_t x0, y0, z0, w0;
        int32_t x1, y1, z1, w1;
        int32_t x2, y2, z2, w2;

        x0 = vertices[cmpt_id + 0];
        y0 = vertices[cmpt_id + 1];
        z0 = vertices[cmpt_id + 2];
        w0 = vertices[cmpt_id + 3];
        x1 = vertices[cmpt_id + 4];
        y1 = vertices[cmpt_id + 5];
        z1 = vertices[cmpt_id + 6];
        w1 = vertices[cmpt_id + 7];
        x2 = vertices[cmpt_id + 8];
        y2 = vertices[cmpt_id + 9];
        z2 = vertices[cmpt_id + 10];
        w2 = vertices[cmpt_id + 11];

        rasterize_triangle_fixed16_8_scalar(
            fb,
            x0, y0, z0, w0,
            x1, y1, z1, w1,
            x2, y2, z2, w2);
    }
}

void draw_indexed(
    framebuffer_t* fb,
    const int32_t* vertices,
    const uint32_t* indices,
    uint32_t num_indices)
{
    assert(fb);
    assert(vertices);
    assert(indices);
    assert(num_indices % 3 == 0);

    // number of loops that can be done with SIMD and that are in groups of 3 (3 indices per triangle)
    uint32_t num_simd_indices = num_indices - (num_indices % (SIMD_PROGRAM_COUNT * 3));

    for (uint32_t index_id = 0; index_id < num_simd_indices; index_id += SIMD_PROGRAM_COUNT * 3)
    {
        int32_t x0[SIMD_PROGRAM_COUNT], y0[SIMD_PROGRAM_COUNT], z0[SIMD_PROGRAM_COUNT], w0[SIMD_PROGRAM_COUNT];
        int32_t x1[SIMD_PROGRAM_COUNT], y1[SIMD_PROGRAM_COUNT], z1[SIMD_PROGRAM_COUNT], w1[SIMD_PROGRAM_COUNT];
        int32_t x2[SIMD_PROGRAM_COUNT], y2[SIMD_PROGRAM_COUNT], z2[SIMD_PROGRAM_COUNT], w2[SIMD_PROGRAM_COUNT];

        for (uint32_t program_index = 0, cmpt_id = index_id; program_index < SIMD_PROGRAM_COUNT; program_index++, cmpt_id += 3)
        {
            uint32_t cmpt_i0 = indices[cmpt_id + 0] * 4;
            uint32_t cmpt_i1 = indices[cmpt_id + 1] * 4;
            uint32_t cmpt_i2 = indices[cmpt_id + 2] * 4;

            x0[program_index] = vertices[cmpt_i0 + 0];
            y0[program_index] = vertices[cmpt_i0 + 1];
            z0[program_index] = vertices[cmpt_i0 + 2];
            w0[program_index] = vertices[cmpt_i0 + 3];
            x1[program_index] = vertices[cmpt_i1 + 0];
            y1[program_index] = vertices[cmpt_i1 + 1];
            z1[program_index] = vertices[cmpt_i1 + 2];
            w1[program_index] = vertices[cmpt_i1 + 3];
            x2[program_index] = vertices[cmpt_i2 + 0];
            y2[program_index] = vertices[cmpt_i2 + 1];
            z2[program_index] = vertices[cmpt_i2 + 2];
            w2[program_index] = vertices[cmpt_i2 + 3];
        }

        rasterize_triangle_fixed16_8_simd4(
            fb,
            x0, y0, z0, w0,
            x1, y1, z1, w1,
            x2, y2, z2, w2);
    }

    for (uint32_t index_id = num_simd_indices; index_id < num_indices; index_id += 3)
    {
        int32_t x0, y0, z0, w0;
        int32_t x1, y1, z1, w1;
        int32_t x2, y2, z2, w2;

        uint32_t cmpt_i0 = indices[index_id + 0] * 4;
        uint32_t cmpt_i1 = indices[index_id + 1] * 4;
        uint32_t cmpt_i2 = indices[index_id + 2] * 4;

        x0 = vertices[cmpt_i0 + 0];
        y0 = vertices[cmpt_i0 + 1];
        z0 = vertices[cmpt_i0 + 2];
        w0 = vertices[cmpt_i0 + 3];
        x1 = vertices[cmpt_i1 + 0];
        y1 = vertices[cmpt_i1 + 1];
        z1 = vertices[cmpt_i1 + 2];
        w1 = vertices[cmpt_i1 + 3];
        x2 = vertices[cmpt_i2 + 0];
        y2 = vertices[cmpt_i2 + 1];
        z2 = vertices[cmpt_i2 + 2];
        w2 = vertices[cmpt_i2 + 3];

        rasterize_triangle_fixed16_8_scalar(
            fb,
            x0, y0, z0, w0,
            x1, y1, z1, w1,
            x2, y2, z2, w2);
    }
}

#ifdef RASTERIZER_UNIT_TESTS
void run_rasterizer_unit_tests()
{
    // pdep tests
    {
        //             source  mask
        assert(pdep_u32(0b000, 0b000000) == 0b000000);
        assert(pdep_u32(0b001, 0b000001) == 0b000001);
        assert(pdep_u32(0b001, 0b000010) == 0b000010);
        assert(pdep_u32(0b011, 0b001100) == 0b001100);
        assert(pdep_u32(0b101, 0b101010) == 0b100010);
        assert(pdep_u32(0b010, 0b010101) == 0b000100);
    }

    // swizzle test
    {
        uint32_t w = FRAMEBUFFER_TILE_WIDTH_IN_PIXELS * 2;
        uint32_t h = FRAMEBUFFER_TILE_WIDTH_IN_PIXELS * 2;
        
        framebuffer_t* fb = new_framebuffer(w, h);
        uint8_t* rowmajor_data = (uint8_t*)malloc(w * h * 4);

        // write indices of pixels linearly in memory (ignoring swizzling)
        // this will be read back and checked to verify the layout
        // For tiles of 4x4 pixels, a 8x8 row major image should look something like:
        //  0  1  4  5 | 16 17 20 21
        //  2  3  6  7 | 18 19 22 23
        //  8  9 12 13 | 24 25 28 29
        // 10 11 14 15 | 26 27 30 31
        // -------------------------
        // 32 33 36 37 | 48 49 52 53
        // 34 35 38 39 | 50 51 54 55
        // 40 41 44 45 | 56 57 60 61
        // 42 43 46 47 | 58 59 62 63
        // see: https://en.wikipedia.org/wiki/Z-order_curve
        for (uint32_t i = 0; i < fb->pixels_per_slice; i++)
        {
            fb->backbuffer[i] = i;
        }
        
        framebuffer_pack_row_major(fb, 0, 0, w, h, pixelformat_r8g8b8a8_unorm, rowmajor_data);
        
        for (uint32_t y = 0; y < h; y++)
        {
            uint32_t tile_y = y / FRAMEBUFFER_TILE_WIDTH_IN_PIXELS;

            for (uint32_t x = 0; x < w; x++)
            {
                uint32_t tile_x = x / FRAMEBUFFER_TILE_WIDTH_IN_PIXELS;
                uint32_t tile_i = tile_y * (fb->pixels_per_row_of_tiles / FRAMEBUFFER_PIXELS_PER_TILE) + tile_x;
                uint32_t topleft_pixel_i = tile_i * FRAMEBUFFER_PIXELS_PER_TILE;

                uint32_t tile_relative_x = x - tile_x * FRAMEBUFFER_TILE_WIDTH_IN_PIXELS;
                uint32_t tile_relative_y = y - tile_y * FRAMEBUFFER_TILE_WIDTH_IN_PIXELS;
                uint32_t rowmajor_i = topleft_pixel_i + (tile_relative_y * FRAMEBUFFER_TILE_WIDTH_IN_PIXELS + tile_relative_x);

                uint32_t xmask = FRAMEBUFFER_TILE_X_SWIZZLE_MASK;
                uint32_t ymask = FRAMEBUFFER_TILE_Y_SWIZZLE_MASK;
                uint32_t xbits = pdep_u32(x, xmask);
                uint32_t ybits = pdep_u32(y, ymask);
                uint32_t swizzled_i = topleft_pixel_i + xbits + ybits;

                assert(rowmajor_data[rowmajor_i * 4 + 0] == ((fb->backbuffer[swizzled_i] & 0x00FF0000) >> 16));
                assert(rowmajor_data[rowmajor_i * 4 + 1] == ((fb->backbuffer[swizzled_i] & 0x0000FF00) >> 8));
                assert(rowmajor_data[rowmajor_i * 4 + 2] == ((fb->backbuffer[swizzled_i] & 0x000000FF) >> 0));
                assert(rowmajor_data[rowmajor_i * 4 + 3] == ((fb->backbuffer[swizzled_i] & 0xFF000000) >> 24));
            }
        }
        
        free(rowmajor_data);
        delete_framebuffer(fb);
    }
}
#endif