// This library implements a Pineda-style software rasterizer inspired from Larrabee's rasterizer.
// See "A Parallel Algorithm for Polygon Rasterization", by Juan Pineda, SIGGRAPH '88:
// http://citeseerx.ist.psu.edu/viewdoc/download?doi=10.1.1.157.4621&rep=rep1&type=pdf
// Also see Michael Abrash's article "Rasterization on Larrabee":
// https://software.intel.com/en-us/articles/rasterization-on-larrabee
// For a modern take on this algorithm, see Fabian Giesen's GPU pipeline and Software Occlusion Culling blog series:
// https://fgiesen.wordpress.com/2011/07/09/a-trip-through-the-graphics-pipeline-2011-index/
// https://fgiesen.wordpress.com/2013/02/17/optimizing-sw-occlusion-culling-index/

#include <rasterizer.h>

#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>

#ifdef _MSC_VER
#include <intrin.h>
#endif

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#endif

// Sized according to the Larrabee rasterizer's description
// The tile size must be up to 128x128
//    this is because any edge that isn't trivially accepted or rejected
//    can be rasterized with 32 bits inside a 128x128 tile
#define TILE_WIDTH_IN_PIXELS 128
#define COARSE_BLOCK_WIDTH_IN_PIXELS 16
#define FINE_BLOCK_WIDTH_IN_PIXELS 4

// Convenience
#define PIXELS_PER_TILE (TILE_WIDTH_IN_PIXELS * TILE_WIDTH_IN_PIXELS)
#define PIXELS_PER_COARSE_BLOCK (COARSE_BLOCK_WIDTH_IN_PIXELS * COARSE_BLOCK_WIDTH_IN_PIXELS)
#define PIXELS_PER_FINE_BLOCK (FINE_BLOCK_WIDTH_IN_PIXELS * FINE_BLOCK_WIDTH_IN_PIXELS)

#define TILE_WIDTH_IN_COARSE_BLOCKS (TILE_WIDTH_IN_PIXELS / COARSE_BLOCK_WIDTH_IN_PIXELS)
#define COARSE_BLOCK_WIDTH_IN_FINE_BLOCKS (COARSE_BLOCK_WIDTH_IN_PIXELS / FINE_BLOCK_WIDTH_IN_PIXELS)
#define COARSE_BLOCKS_PER_TILE (PIXELS_PER_TILE / PIXELS_PER_COARSE_BLOCK)
 
// The swizzle masks, using alternating yxyxyx bit pattern for morton-code swizzling pixels in a tile.
// This makes the pixels morton code swizzled within every rasterization level (fine/coarse/tile)
// The tiles themselves are stored row major.
// For examples of this concept, see:
// https://software.intel.com/en-us/node/514045
// https://msdn.microsoft.com/en-us/library/windows/desktop/dn770442%28v=vs.85%29.aspx
#define TILE_X_SWIZZLE_MASK (0x55555555 & (PIXELS_PER_TILE - 1))
#define TILE_Y_SWIZZLE_MASK (0xAAAAAAAA & (PIXELS_PER_TILE - 1))

// If there are too many commands and this buffer gets filled up,
// then the command buffer for that tile must be flushed.
#define TILE_COMMAND_BUFFER_SIZE_IN_DWORDS 128

// parallel bit deposit low-order source bits according to mask bits
#ifdef __AVX2__
__forceinline uint32_t pdep_u32(uint32_t source, uint32_t mask)
{
    // AVX2 implementation
    return _pdep_u32(source, mask);
}
#else
__forceinline uint32_t pdep_u32(uint32_t source, uint32_t mask)
{
    // generic implementation
    uint32_t temp = source;
    uint32_t dest = 0;
    uint32_t m = 0, k = 0;
    while (m < 32)
    {
        if (mask & (1 << m))
        {
            dest = (dest & ~(1 << m)) | (((temp & (1 << k)) >> k) << m);
            k = k + 1;
        }
        m = m+ 1;
    }
    return dest;
}
#endif

// count leading zeros (32 bits)
#ifdef __AVX2__
__forceinline uint32_t lzcnt(uint32_t value)
{
    // AVX2 implementation
    return __lzcnt(value);
}
#elif defined(_MSC_VER)
__forceinline uint32_t lzcnt(uint32_t value)
{
    // MSVC implementation
    unsigned long index;
    if (_BitScanReverse(&index, value))
    {
        return (32 - 1) - index;
    }
    else
    {
        return 32;
    }
}
#else
__forceinline uint32_t lzcnt(uint32_t value)
{
    // generic implementation
    uint32_t i;
    for (i = 0; i < 32; i++)
    {
        if (value & 0x80000000)
            break;
        
        value = value << 1;
    }
    return i;
}
#endif

// count leading zeros (64 bits)
#ifdef __AVX2__
__forceinline uint64_t lzcnt64(uint64_t value)
{
    return __lzcnt64(value);
}
#elif defined(_MSC_VER)
__forceinline uint64_t lzcnt64(uint64_t value)
{
    // MSVC implementation
    unsigned long index;
    if (_BitScanReverse64(&index, value))
    {
        return (64 - 1) - index;
    }
    else
    {
        return 64;
    }
}
#else
__forceinline uint64_t lzcnt64(uint64_t value)
{
    // generic implementation
    uint64_t i;
    for (i = 0; i < 64; i++)
    {
        if (value & 0x8000000000000000ULL)
            break;

        value = value << 1ULL;
    }
    return i;
}
#endif

#ifdef _WIN32
uint64_t qpc()
{
    LARGE_INTEGER pc;
    QueryPerformanceCounter(&pc);
    return pc.QuadPart;
}
#else
"Missing QPC implementation for this platform!";
#endif

#ifdef _WIN32
uint64_t qpf()
{
    LARGE_INTEGER pf;
    QueryPerformanceFrequency(&pf);
    return pf.QuadPart;
}
#else
"Missing QPF implementation for this platform!";
#endif

static int32_t s1516_add(int32_t a, int32_t b)
{
    int32_t result;
    result = a + b;
    return result;
}

static int32_t s1516_add_sat(int32_t a, int32_t b)
{
    int32_t result;
    int64_t tmp;

    tmp = (int64_t)a + (int64_t)b;
    if (tmp > (int64_t)0x7FFFFFFF)
        tmp = (int64_t)0x7FFFFFFF;
    if (tmp < -(int64_t)0x80000000)
        tmp = -(int64_t)0x80000000;
    result = (int32_t)tmp;

    return result;
}

// saturate to range of int32_t
static int32_t s1516_sat(int64_t x)
{
    if (x >(int64_t)0x7FFFFFFF) return (int64_t)0x7FFFFFFF;
    else if (x < -(int64_t)0x80000000) return -(int64_t)0x80000000;
    else return (int32_t)x;
}

static int32_t s1516_mul(int32_t a, int32_t b)
{
    int32_t result;
    int64_t temp;

    temp = (int64_t)a * (int64_t)b;
    // Rounding: mid values are rounded up
    temp += 1 << 15;
    // Correct by dividing by base and saturate result
    result = s1516_sat(temp >> 16);

    return result;
}

static int32_t s1516_div(int32_t a, int32_t b)
{
    int32_t result;
    int64_t temp;

    // pre-multiply by the base
    temp = (int64_t)a << 16;
    // Rounding: mid values are rounded up (down for negative values)
    if ((temp >= 0 && b >= 0) || (temp < 0 && b < 0))
        temp += b / 2;
    else
        temp -= b / 2;
    result = (int32_t)(temp/ b);

    return result;
}

static int32_t s1516_fma(int32_t a, int32_t b, int32_t c)
{
    int32_t result;
    int64_t temp;

    temp = (int64_t)a * (int64_t)b + ((int64_t)c << 16);

    // Rounding: mid values are rounded up
    temp += 1 << 15;

    // Correct by dividing by base and saturate result
    result = s1516_sat(temp >> 16);

    return result;
}

static int32_t s1516_int(int32_t i)
{
    return i << 16;
}

static int32_t s1516_flt(float f)
{
    return (int32_t)(f * 0xffff);
}

static int32_t s168_s1516(int32_t s1516)
{
    return s1516_div(s1516, s1516_int(256));
}

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
    tilecmd_id_resetbuf, // when there's not enough space in the command ring buffer and the ring loops
    tilecmd_id_drawsmalltri,
    tilecmd_id_drawtile_0edge,
    tilecmd_id_drawtile_1edge,
    tilecmd_id_drawtile_2edge,
    tilecmd_id_drawtile_3edge,
    tilecmd_id_cleartile
} tilecmd_id_t;

typedef struct xyzw_i32_t
{
    int32_t x, y, z, w;
} xyzw_i32_t;

typedef struct tilecmd_drawsmalltri_t
{
    uint32_t tilecmd_id;
    int32_t edges[3];
    int32_t edge_dxs[3];
    int32_t edge_dys[3];
    int32_t vert_Zs[3];
    uint32_t max_Z, min_Z;
    uint32_t rcp_triarea2;
    int32_t first_coarse_x, last_coarse_x;
    int32_t first_coarse_y, last_coarse_y;
} tilecmd_drawsmalltri_t;

typedef struct tilecmd_drawtile_t
{
    uint32_t tilecmd_id;
    int32_t edges[3];
    int32_t edge_dxs[3];
    int32_t edge_dys[3];
    int32_t vert_Zs[3];
    uint32_t max_Z, min_Z;
    uint32_t rcp_triarea2;
} tilecmd_drawtile_t;

typedef struct tilecmd_cleartile_t
{
    uint32_t tilecmd_id;
    uint32_t color;
} tilecmd_cleartile_t;

typedef struct framebuffer_t
{
    uint32_t* backbuffer;
    uint32_t* depthbuffer;
    
    uint32_t* tile_cmdpool;
    tile_cmdbuf_t* tile_cmdbufs;
    
    int32_t width_in_pixels;
    int32_t height_in_pixels;

    int32_t width_in_tiles;
    int32_t height_in_tiles;
    int32_t total_num_tiles;
    
    // num_tiles_per_row * num_pixels_per_tile
    int32_t pixels_per_row_of_tiles;

    // pixels_per_row_of_tiles * num_tile_rows
    int32_t pixels_per_slice;

    // performance counters
    uint64_t pc_frequency;
    framebuffer_perfcounters_t perfcounters;
    tile_perfcounters_t* tile_perfcounters;

} framebuffer_t;

framebuffer_t* new_framebuffer(int32_t width, int32_t height)
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
    int32_t padded_width_in_pixels = (width + (TILE_WIDTH_IN_PIXELS - 1)) & -TILE_WIDTH_IN_PIXELS;
    int32_t padded_height_in_pixels = (height + (TILE_WIDTH_IN_PIXELS - 1)) & -TILE_WIDTH_IN_PIXELS;
    
    fb->width_in_tiles = padded_width_in_pixels / TILE_WIDTH_IN_PIXELS;
    fb->height_in_tiles = padded_height_in_pixels / TILE_WIDTH_IN_PIXELS;
    fb->total_num_tiles = fb->width_in_tiles * fb->height_in_tiles;

    fb->pixels_per_row_of_tiles = padded_width_in_pixels * TILE_WIDTH_IN_PIXELS;
    fb->pixels_per_slice = padded_height_in_pixels / TILE_WIDTH_IN_PIXELS * fb->pixels_per_row_of_tiles;

    fb->backbuffer = (uint32_t*)malloc(fb->pixels_per_slice * sizeof(uint32_t));
    assert(fb->backbuffer);

    // clear to black/transparent initially
    memset(fb->backbuffer, 0, fb->pixels_per_slice * sizeof(uint32_t));

    fb->depthbuffer = (uint32_t*)malloc(fb->pixels_per_slice * sizeof(uint32_t));
    assert(fb->depthbuffer);
    
    // clear to infinity initially
    memset(fb->depthbuffer, 0xFF, fb->pixels_per_slice * sizeof(uint32_t));

    // allocate command lists for each tile
    fb->tile_cmdpool = (uint32_t*)malloc(fb->total_num_tiles * TILE_COMMAND_BUFFER_SIZE_IN_DWORDS * sizeof(uint32_t));
    assert(fb->tile_cmdpool);

    fb->tile_cmdbufs = (tile_cmdbuf_t*)malloc(fb->total_num_tiles * sizeof(tile_cmdbuf_t));
    assert(fb->tile_cmdbufs);

    // command lists are circular queues that are initially empty
    for (int32_t i = 0; i < fb->total_num_tiles; i++)
    {
        fb->tile_cmdbufs[i].cmdbuf_start = &fb->tile_cmdpool[i * TILE_COMMAND_BUFFER_SIZE_IN_DWORDS];
        fb->tile_cmdbufs[i].cmdbuf_end = fb->tile_cmdbufs[i].cmdbuf_start + TILE_COMMAND_BUFFER_SIZE_IN_DWORDS;
        fb->tile_cmdbufs[i].cmdbuf_read = fb->tile_cmdbufs[i].cmdbuf_start;
        fb->tile_cmdbufs[i].cmdbuf_write = fb->tile_cmdbufs[i].cmdbuf_start;
    }

    fb->pc_frequency = qpf();

    memset(&fb->perfcounters, 0, sizeof(framebuffer_perfcounters_t));

    fb->tile_perfcounters = (tile_perfcounters_t*)malloc(fb->total_num_tiles * sizeof(tile_perfcounters_t));
    memset(fb->tile_perfcounters, 0, fb->total_num_tiles * sizeof(tile_perfcounters_t));
    
    return fb;
}

void delete_framebuffer(framebuffer_t* fb)
{
    if (!fb)
        return;

    free(fb->tile_perfcounters);
    free(fb->tile_cmdbufs);
    free(fb->tile_cmdpool);
    free(fb->backbuffer);
    free(fb);
}

static void draw_coarse_block_smalltri(framebuffer_t* fb, int32_t tile_id, int32_t coarse_topleft_x, int32_t coarse_topleft_y, const tilecmd_drawsmalltri_t* drawcmd)
{
    uint64_t coarse_start_pc = qpc();

    int32_t edges[3];
    for (int32_t v = 0; v < 3; v++)
    {
        edges[v] = drawcmd->edges[v];
    }

    int32_t tile_start_i = PIXELS_PER_TILE * tile_id;

    for (
        int32_t fineblock_y = coarse_topleft_y, fineblock_ybits = pdep_u32(coarse_topleft_y, TILE_Y_SWIZZLE_MASK);
        fineblock_y < coarse_topleft_y + COARSE_BLOCK_WIDTH_IN_PIXELS;
        fineblock_y++, fineblock_ybits = (fineblock_ybits - TILE_Y_SWIZZLE_MASK) & TILE_Y_SWIZZLE_MASK)
    {
        int32_t edges_row[3];
        for (int32_t v = 0; v < 3; v++)
        {
            edges_row[v] = edges[v];
        }

        for (
            int32_t fineblock_x = coarse_topleft_x, fineblock_xbits = pdep_u32(coarse_topleft_x, TILE_X_SWIZZLE_MASK);
            fineblock_x < coarse_topleft_x + COARSE_BLOCK_WIDTH_IN_PIXELS;
            fineblock_x++, fineblock_xbits = (fineblock_xbits - TILE_X_SWIZZLE_MASK) & TILE_X_SWIZZLE_MASK)
        {
            int32_t dst_i = tile_start_i + (fineblock_ybits | fineblock_xbits);

            // TODO: rasterize whole fine blocks at a time rather than pixels at a time
            int32_t pixel_discarded = 0;
            for (int32_t v = 0; v < 3; v++)
            {
                if (edges_row[v] >= 0)
                {
                    pixel_discarded = 1;
                    break;
                }
            }

            if (!pixel_discarded)
            {
                int32_t rcp_triarea2_mantissa = (drawcmd->rcp_triarea2 & 0xFF);
                int32_t rcp_triarea2_exponent = (drawcmd->rcp_triarea2 & 0xFF00) >> 8;
                int32_t rcp_triarea2_rshift = rcp_triarea2_exponent - 127;

                int32_t shifted_e2 = -edges_row[2];
                int32_t shifted_e0 = -edges_row[0];
                if (rcp_triarea2_rshift < 0)
                {
                    shifted_e2 = shifted_e2 << -rcp_triarea2_rshift;
                    shifted_e0 = shifted_e0 << -rcp_triarea2_rshift;
                }
                else
                {
                    shifted_e2 = shifted_e2 >> rcp_triarea2_rshift;
                    shifted_e0 = shifted_e0 >> rcp_triarea2_rshift;
                }

                // compute non-perspective-correct barycentrics for vertices 1 and 2
                int32_t u = (shifted_e2 * rcp_triarea2_mantissa) >> 1;
                int32_t v = (shifted_e0 * rcp_triarea2_mantissa) >> 1;
                assert(u < 0x8000);
                assert(v < 0x8000);
                
                // not related to vertex w. Just third barycentric. Bad naming.
                int32_t w = 0x7FFF - u - v;

                // compute interpolated depth
                // TODO? Might want to saturate here.
                // Can probably get 1 more bit of precision if I handle a overflow bit properly? FMA magic?
                uint32_t pixel_Z = (drawcmd->vert_Zs[0] << 15)
                    + u * (drawcmd->vert_Zs[1] - drawcmd->vert_Zs[0])
                    + v * (drawcmd->vert_Zs[2] - drawcmd->vert_Zs[0]);

                if (pixel_Z < (drawcmd->min_Z << 15))
                    pixel_Z = (drawcmd->min_Z << 15);
                
                if (pixel_Z > (drawcmd->max_Z << 15))
                    pixel_Z = (drawcmd->max_Z << 15);

                if (pixel_Z < fb->depthbuffer[dst_i])
                {
                    fb->depthbuffer[dst_i] = pixel_Z;
                    fb->backbuffer[dst_i] = (0xFF << 24) | ((w/0x80) << 16) | ((u/ 0x80) << 8) | (v/ 0x80);
                }
            }

            for (int32_t v = 0; v < 3; v++)
            {
                edges_row[v] += drawcmd->edge_dxs[v];
            }
        }

        for (int32_t v = 0; v < 3; v++)
        {
            edges[v] += drawcmd->edge_dys[v];
        }
    }

    // Add the drawing time to the small triangle rasterization timer
    fb->tile_perfcounters[tile_id].smalltri_coarse_raster += qpc() - coarse_start_pc;
}

static void draw_tile_smalltri(framebuffer_t* fb, int32_t tile_id, const tilecmd_drawsmalltri_t* drawcmd)
{
    uint64_t tile_start_pc = qpc();

    int32_t coarse_edge_dxs[3];
    int32_t coarse_edge_dys[3];
    for (int32_t v = 0; v < 3; v++)
    {
        coarse_edge_dxs[v] = drawcmd->edge_dxs[v] * COARSE_BLOCK_WIDTH_IN_PIXELS;
        coarse_edge_dys[v] = drawcmd->edge_dys[v] * COARSE_BLOCK_WIDTH_IN_PIXELS;
    }

    int32_t edges[3];
    for (int32_t v = 0; v < 3; v++)
    {
        edges[v] = drawcmd->edges[v]
            + drawcmd->first_coarse_x * coarse_edge_dxs[v]
            + drawcmd->first_coarse_y * coarse_edge_dys[v];
    }

    int32_t tile_y = tile_id / fb->width_in_tiles;
    int32_t tile_x = tile_id - tile_y * fb->width_in_tiles;

    // figure out which coarse blocks pass the reject and accept tests
    for (int32_t cb_y = drawcmd->first_coarse_y; cb_y <= drawcmd->last_coarse_y; cb_y++)
    {
        int32_t row_edges[3];
        for (int32_t v = 0; v < 3; v++)
        {
            row_edges[v] = edges[v];
        }

        for (int32_t cb_x = drawcmd->first_coarse_x; cb_x <= drawcmd->last_coarse_x; cb_x++)
        {
            tilecmd_drawsmalltri_t cbargs;

            cbargs = *drawcmd;
            
            for (int32_t v = 0; v < 3; v++)
            {
                cbargs.edges[v] = row_edges[v];
            }

            int32_t coarse_topleft_x = tile_x * TILE_WIDTH_IN_PIXELS + cb_x * COARSE_BLOCK_WIDTH_IN_PIXELS;
            int32_t coarse_topleft_y = tile_y * TILE_WIDTH_IN_PIXELS + cb_y * COARSE_BLOCK_WIDTH_IN_PIXELS;
            
            fb->tile_perfcounters[tile_id].smalltri_tile_raster += qpc() - tile_start_pc;
            draw_coarse_block_smalltri(fb, tile_id, coarse_topleft_x, coarse_topleft_y, &cbargs);
            tile_start_pc = qpc();

            for (int32_t v = 0; v < 3; v++)
            {
                row_edges[v] += coarse_edge_dxs[v];
            }
        }

        for (int32_t v = 0; v < 3; v++)
        {
            edges[v] += coarse_edge_dys[v];
        }
    }

    fb->tile_perfcounters[tile_id].smalltri_tile_raster = qpc() - tile_start_pc;
}

static void draw_coarse_block_largetri(framebuffer_t* fb, int32_t tile_id, int32_t coarse_topleft_x, int32_t coarse_topleft_y, const tilecmd_drawtile_t* drawcmd)
{
    uint64_t coarse_start_pc = qpc();

    int32_t num_test_edges = drawcmd->tilecmd_id - tilecmd_id_drawtile_0edge;

    int32_t edges[3];
    for (int32_t v = 0; v < num_test_edges; v++)
    {
        edges[v] = drawcmd->edges[v];
    }

    int32_t tile_start_i = PIXELS_PER_TILE * tile_id;

    for (
        int32_t fineblock_y = coarse_topleft_y, fineblock_ybits = pdep_u32(coarse_topleft_y, TILE_Y_SWIZZLE_MASK);
        fineblock_y < coarse_topleft_y + COARSE_BLOCK_WIDTH_IN_PIXELS;
        fineblock_y++, fineblock_ybits = (fineblock_ybits - TILE_Y_SWIZZLE_MASK) & TILE_Y_SWIZZLE_MASK)
    {
        int32_t edges_row[3];
        for (int32_t v = 0; v < num_test_edges; v++)
        {
            edges_row[v] = edges[v];
        }

        for (
            int32_t fineblock_x = coarse_topleft_x, fineblock_xbits = pdep_u32(coarse_topleft_x, TILE_X_SWIZZLE_MASK);
            fineblock_x < coarse_topleft_x + COARSE_BLOCK_WIDTH_IN_PIXELS;
            fineblock_x++, fineblock_xbits = (fineblock_xbits - TILE_X_SWIZZLE_MASK) & TILE_X_SWIZZLE_MASK)
        {
            int32_t dst_i = tile_start_i + (fineblock_ybits | fineblock_xbits);

            // TODO: rasterize whole fine blocks at a time rather than pixels at a time
            int32_t pixel_discarded = 0;
            for (int32_t v = 0; v < num_test_edges; v++)
            {
                if (edges_row[v] >= 0)
                {
                    pixel_discarded = 1;
                    break;
                }
            }

            if (!pixel_discarded)
            {
                int32_t rcp_triarea2_mantissa = (drawcmd->rcp_triarea2 & 0xFFFF);
                int32_t rcp_triarea2_exponent = (drawcmd->rcp_triarea2 & 0xFF0000) >> 16;
                int32_t rcp_triarea2_rshift = rcp_triarea2_exponent - 127;

                int32_t shifted_e2 = -edges_row[2];
                int32_t shifted_e0 = -edges_row[0];
                if (rcp_triarea2_rshift < 0)
                {
                    shifted_e2 = shifted_e2 << -rcp_triarea2_rshift;
                    shifted_e0 = shifted_e0 << -rcp_triarea2_rshift;
                }
                else
                {
                    shifted_e2 = shifted_e2 >> rcp_triarea2_rshift;
                    shifted_e0 = shifted_e0 >> rcp_triarea2_rshift;
                }

                // compute non-perspective-correct barycentrics for vertices 1 and 2
                 int32_t u = (shifted_e2 * rcp_triarea2_mantissa) >> 16 >> 1;
                 if (num_test_edges < 3)
                     u = 0x0;
                 int32_t v = (shifted_e0 * rcp_triarea2_mantissa) >> 16 >> 1;
                 if (num_test_edges < 1)
                     v = 0x0;
                 assert(u < 0x8000);
                 assert(v < 0x8000);

                 // not related to vertex w. Just third barycentric. Bad naming.
                 int32_t w = 0x7FFF - u - v;

                 // compute interpolated depth
                 // TODO? Might want to saturate here.
                 // Can probably get 1 more bit of precision if I handle a overflow bit properly? FMA magic?
                 uint32_t pixel_Z = (drawcmd->vert_Zs[0] << 15)
                     + u * (drawcmd->vert_Zs[1] - drawcmd->vert_Zs[0])
                     + v * (drawcmd->vert_Zs[2] - drawcmd->vert_Zs[0]);

                 if (pixel_Z < (drawcmd->min_Z << 15))
                     pixel_Z = (drawcmd->min_Z << 15);

                 if (pixel_Z >(drawcmd->max_Z << 15))
                     pixel_Z = (drawcmd->max_Z << 15);
                
                 if (pixel_Z < fb->depthbuffer[dst_i])
                {
                    fb->depthbuffer[dst_i] = pixel_Z;
                    fb->backbuffer[dst_i] = (0xFF << 24) | ((w / 0x80) << 16) | ((u / 0x80) << 8) | (v / 0x80);
                }
            }

            for (int32_t v = 0; v < num_test_edges; v++)
            {
                edges_row[v] += drawcmd->edge_dxs[v];
            }
        }

        for (int32_t v = 0; v < num_test_edges; v++)
        {
            edges[v] += drawcmd->edge_dys[v];
        }
    }

    fb->tile_perfcounters[tile_id].largetri_coarse_raster += qpc() - coarse_start_pc;
}

static void draw_tile_largetri(framebuffer_t* fb, int32_t tile_id, const tilecmd_drawtile_t* drawcmd)
{
    uint64_t tile_start_pc = qpc();
    
    int32_t num_test_edges = drawcmd->tilecmd_id - tilecmd_id_drawtile_0edge;

    int32_t coarse_edge_dxs[3];
    int32_t coarse_edge_dys[3];
    for (int32_t v = 0; v < num_test_edges; v++)
    {
        coarse_edge_dxs[v] = drawcmd->edge_dxs[v] * COARSE_BLOCK_WIDTH_IN_PIXELS;
        coarse_edge_dys[v] = drawcmd->edge_dys[v] * COARSE_BLOCK_WIDTH_IN_PIXELS;
    }

    int32_t edges[3];
    for (int32_t v = 0; v < num_test_edges; v++)
    {
        edges[v] = drawcmd->edges[v];
    }

    int32_t edge_trivRejs[3];
    int32_t edge_trivAccs[3];
    for (int32_t v = 0; v < num_test_edges; v++)
    {
        edge_trivRejs[v] = drawcmd->edges[v];
        edge_trivAccs[v] = drawcmd->edges[v];
        if (coarse_edge_dxs[v] < 0) edge_trivRejs[v] += coarse_edge_dxs[v];
        if (coarse_edge_dxs[v] > 0) edge_trivAccs[v] += coarse_edge_dxs[v];
        if (coarse_edge_dys[v] < 0) edge_trivRejs[v] += coarse_edge_dys[v];
        if (coarse_edge_dys[v] > 0) edge_trivAccs[v] += coarse_edge_dys[v];
    }

    int32_t tile_y = tile_id / fb->width_in_tiles;
    int32_t tile_x = tile_id - tile_y * fb->width_in_tiles;

    // figure out which coarse blocks pass the reject and accept tests
    for (int32_t cb_y = 0; cb_y < TILE_WIDTH_IN_COARSE_BLOCKS; cb_y++)
    {
        int32_t row_edges[3];
        for (int32_t v = 0; v < num_test_edges; v++)
        {
            row_edges[v] = edges[v];
        }

        int32_t edge_row_trivRejs[3];
        int32_t edge_row_trivAccs[3];
        for (int32_t v = 0; v < num_test_edges; v++)
        {
            edge_row_trivRejs[v] = edge_trivRejs[v];
            edge_row_trivAccs[v] = edge_trivAccs[v];
        }

        for (int32_t cb_x = 0; cb_x < TILE_WIDTH_IN_COARSE_BLOCKS; cb_x++)
        {
            // trivial reject if at least one edge doesn't cover the coarse block at all
            int32_t trivially_rejected = 0;
            for (int32_t v = 0; v < num_test_edges; v++)
            {
                if (edge_row_trivRejs[v] >= 0)
                {
                    trivially_rejected = 1;
                    break;
                }
            }

            if (!trivially_rejected)
            {
                tilecmd_drawtile_t drawtilecmd = *drawcmd;

                int32_t edge_needs_test[3] = { 0, 0, 0 };
                int32_t num_tests_necessary = 0;
                for (int32_t v = 0; v < num_test_edges; v++)
                {
                    if ((edge_needs_test[v] = edge_row_trivAccs[v] >= 0))
                    {
                        num_tests_necessary++;
                    }
                }

                drawtilecmd.tilecmd_id = tilecmd_id_drawtile_0edge + num_tests_necessary;

                int32_t vertex_rotation = 0;

                if (num_tests_necessary == 1) {
                    if (edge_needs_test[1]) vertex_rotation = 1;
                    else if (edge_needs_test[2]) vertex_rotation = 2;
                }
                else if (num_tests_necessary == 2) {
                    if (!edge_needs_test[0]) vertex_rotation = 1;
                    else if (!edge_needs_test[1]) vertex_rotation = 2;
                }

                for (int32_t v = 0; v < num_tests_necessary; v++)
                {
                    int32_t rotated_v = (v + vertex_rotation) % 3;

                    drawtilecmd.edges[v] = (int32_t)row_edges[rotated_v];
                    drawtilecmd.edge_dxs[v] = (int32_t)drawcmd->edge_dxs[rotated_v];
                    drawtilecmd.edge_dys[v] = (int32_t)drawcmd->edge_dys[rotated_v];
                }

                int32_t coarse_topleft_x = tile_x * TILE_WIDTH_IN_PIXELS + cb_x * COARSE_BLOCK_WIDTH_IN_PIXELS;
                int32_t coarse_topleft_y = tile_y * TILE_WIDTH_IN_PIXELS + cb_y * COARSE_BLOCK_WIDTH_IN_PIXELS;

                fb->tile_perfcounters[tile_id].largetri_tile_raster += qpc() - tile_start_pc;
                draw_coarse_block_largetri(fb, tile_id, coarse_topleft_x, coarse_topleft_y, &drawtilecmd);
                tile_start_pc = qpc();
            }

            for (int32_t v = 0; v < num_test_edges; v++)
            {
                row_edges[v] += coarse_edge_dxs[v];
            }

            for (int32_t v = 0; v < num_test_edges; v++)
            {
                edge_row_trivRejs[v] += coarse_edge_dxs[v];
                edge_row_trivAccs[v] += coarse_edge_dxs[v];
            }
        }

        for (int32_t v = 0; v < num_test_edges; v++)
        {
            edges[v] += coarse_edge_dys[v];
        }

        for (int32_t v = 0; v < num_test_edges; v++)
        {
            edge_trivRejs[v] += coarse_edge_dys[v];
            edge_trivAccs[v] += coarse_edge_dys[v];
        }
    }
    
    fb->tile_perfcounters[tile_id].largetri_tile_raster += qpc() - tile_start_pc;
}

static void clear_tile(framebuffer_t* fb, int32_t tile_id, tilecmd_cleartile_t* cmd)
{
    uint64_t clear_start_pc = qpc();

    int32_t tile_start_i = PIXELS_PER_TILE * tile_id;
    int32_t tile_end_i = tile_start_i + PIXELS_PER_TILE;
    uint32_t color = cmd->color;
    for (int32_t px = tile_start_i; px < tile_end_i; px++)
    {
        fb->backbuffer[px] = color;
        fb->depthbuffer[px] = 0xFFFFFFFF;
    }

    fb->tile_perfcounters[tile_id].clear += qpc() - clear_start_pc;
}

static void debugprint_cmdbuf(tile_cmdbuf_t* cmdbuf)
{
    int32_t read_i = (int32_t)(cmdbuf->cmdbuf_read - cmdbuf->cmdbuf_start);
    int32_t write_i = (int32_t)(cmdbuf->cmdbuf_write - cmdbuf->cmdbuf_start);
    int32_t sz = (int32_t)(cmdbuf->cmdbuf_end - cmdbuf->cmdbuf_start);
    for (int32_t i = 0; i < sz; i++)
    {
        if (i == write_i)
            printf(" W");
        else
            printf("--");
    }
    printf("\n");
    for (int32_t i = 0; i < sz; i++)
    {
        printf("| ");
    }
    printf("|\n");
    for (int32_t i = 0; i < sz; i++)
    {
        if (i == read_i)
            printf(" R");
        else
            printf("--");
    }
    printf("\n");
}

static void framebuffer_resolve_tile(framebuffer_t* fb, int32_t tile_id)
{
    uint64_t resolve_start_pc = qpc();

    tile_cmdbuf_t* cmdbuf = &fb->tile_cmdbufs[tile_id];
    
    uint32_t* cmd;
    for (cmd = cmdbuf->cmdbuf_read; cmd != cmdbuf->cmdbuf_write; )
    {
        uint32_t tilecmd_id = *cmd;
        
        // debugging code for logging commands
        // printf("Reading command [id: %d]\n", tilecmd_id);
        // debugprint_cmdbuf(cmdbuf);

        if (tilecmd_id == tilecmd_id_resetbuf)
        {
            cmd = cmdbuf->cmdbuf_start;
        }
        else if (tilecmd_id == tilecmd_id_drawsmalltri)
        {
            fb->tile_perfcounters[tile_id].cmdbuf_resolve += qpc() - resolve_start_pc;
            draw_tile_smalltri(fb, tile_id, (tilecmd_drawsmalltri_t*)cmd);
            resolve_start_pc = qpc();

            cmd += sizeof(tilecmd_drawsmalltri_t) / sizeof(uint32_t);
        }
        else if (tilecmd_id >= tilecmd_id_drawtile_0edge && tilecmd_id <= tilecmd_id_drawtile_3edge)
        {   
            fb->tile_perfcounters[tile_id].cmdbuf_resolve += qpc() - resolve_start_pc;
            draw_tile_largetri(fb, tile_id, (tilecmd_drawtile_t*)cmd);
            resolve_start_pc = qpc();

            cmd += sizeof(tilecmd_drawtile_t) / sizeof(uint32_t);
        }
        else if (tilecmd_id == tilecmd_id_cleartile)
        {
            fb->tile_perfcounters[tile_id].cmdbuf_resolve += qpc() - resolve_start_pc;
            clear_tile(fb, tile_id, (tilecmd_cleartile_t*)cmd);
            resolve_start_pc = qpc();

            cmd += sizeof(tilecmd_cleartile_t) / sizeof(uint32_t);
        }
        else
        {
            assert(!"Unknown tile command");
        }

        if (cmd == cmdbuf->cmdbuf_end)
        {
            cmd = cmdbuf->cmdbuf_start;
            
            if (cmdbuf->cmdbuf_write == cmdbuf->cmdbuf_end)
            {
                break;
            }
        }
    }

    // read ptr should never be at the end ptr after interpreting
    assert(cmd != cmdbuf->cmdbuf_end);

    cmdbuf->cmdbuf_read = cmd;

    fb->tile_perfcounters[tile_id].cmdbuf_resolve += qpc() - resolve_start_pc;
}

static void framebuffer_push_tilecmd(framebuffer_t* fb, int32_t tile_id, const uint32_t* cmd_dwords, int32_t num_dwords)
{
    assert(tile_id < fb->total_num_tiles);

    uint64_t pushcmd_start_pc = qpc();

    tile_cmdbuf_t* cmdbuf = &fb->tile_cmdbufs[tile_id];

    // read should never be at the end.
    assert(cmdbuf->cmdbuf_read != cmdbuf->cmdbuf_end);

    // debugging code for logging commands
    // printf("Writing command [id: %d, sz: %d]\n", cmd_dwords[0], num_dwords);
    // debugprint_cmdbuf(cmdbuf);

    if (cmdbuf->cmdbuf_read - cmdbuf->cmdbuf_write > 0 && cmdbuf->cmdbuf_read - cmdbuf->cmdbuf_write < num_dwords + 1)
    {
        // read ptr is after write ptr and there's not enough room in between
        // therefore, need to flush
        // note: write is not allowed to "catch up" to read from behind, hence why a +1 is added to keep them separate.
        fb->tile_perfcounters[tile_id].cmdbuf_pushcmd += qpc() - pushcmd_start_pc;
        framebuffer_resolve_tile(fb, tile_id);
        pushcmd_start_pc = qpc();

        // after resolve, read should now have "caught up" to write from behind
        assert(cmdbuf->cmdbuf_read == cmdbuf->cmdbuf_write);
    }

    // At this point, the read head can't be a problem. However, it's possible there isn't enough memory at the end.
    if (cmdbuf->cmdbuf_end - cmdbuf->cmdbuf_write < num_dwords)
    {
        // not enough room in the buffer to write the commands, need to loop around

        // should never be at the end of the buffer since it always loops at the end of this function
        assert(cmdbuf->cmdbuf_write != cmdbuf->cmdbuf_end);

        // abandon the rest of the slop at the end of the buffer
        *cmdbuf->cmdbuf_write = tilecmd_id_resetbuf;

        if (cmdbuf->cmdbuf_start == cmdbuf->cmdbuf_read)
        {
            // write is not allowed to catch up to read,
            // so make sure read catches up to write instead.
            fb->tile_perfcounters[tile_id].cmdbuf_pushcmd += qpc() - pushcmd_start_pc;
            framebuffer_resolve_tile(fb, tile_id);
            pushcmd_start_pc = qpc();

            // reset read back to start since we'll set write back to start also
            cmdbuf->cmdbuf_read = cmdbuf->cmdbuf_start;
        }

        cmdbuf->cmdbuf_write = cmdbuf->cmdbuf_start;

        // After loping around the buffer, it's possible that the read head is in the way again.
        if (cmdbuf->cmdbuf_read - cmdbuf->cmdbuf_write > 0 && cmdbuf->cmdbuf_read - cmdbuf->cmdbuf_write < num_dwords + 1)
        {
            // read ptr is after write ptr and there's not enough room in between
            // therefore, need to flush
            // note: write is not allowed to "catch up" to read from behind, hence why a +1 is added to keep them separate.
            fb->tile_perfcounters[tile_id].cmdbuf_pushcmd += qpc() - pushcmd_start_pc;
            framebuffer_resolve_tile(fb, tile_id);
            pushcmd_start_pc = qpc();

            // after resolve, read should now have "caught up" to write from behind
            assert(cmdbuf->cmdbuf_read == cmdbuf->cmdbuf_write);
        }
    }

    // assert enough room in the buffer after the write
    assert(cmdbuf->cmdbuf_end - cmdbuf->cmdbuf_write >= num_dwords);
    // assert that the read head isn't in the way
    assert((cmdbuf->cmdbuf_read <= cmdbuf->cmdbuf_write) || (cmdbuf->cmdbuf_read > cmdbuf->cmdbuf_write && cmdbuf->cmdbuf_read - cmdbuf->cmdbuf_write >= num_dwords + 1));

    // finally actually write the command
    for (int32_t i = 0; i < num_dwords; i++)
    {
        cmdbuf->cmdbuf_write[i] = cmd_dwords[i];
    }
    cmdbuf->cmdbuf_write += num_dwords;

    // write is not allowed to catch up to read
    assert(cmdbuf->cmdbuf_write != cmdbuf->cmdbuf_read);

    // loop around the buffer if necessary
    if (cmdbuf->cmdbuf_write == cmdbuf->cmdbuf_end)
    {
        if (cmdbuf->cmdbuf_start == cmdbuf->cmdbuf_read)
        {
            // write is not allowed to catch up to read,
            // so make sure read catches up to write instead.
            fb->tile_perfcounters[tile_id].cmdbuf_pushcmd += qpc() - pushcmd_start_pc;
            framebuffer_resolve_tile(fb, tile_id);
            pushcmd_start_pc = qpc();

            // since the resolve made read ptr catch up to write ptr, that means read reached the end
            // that also means it currently looped back to the start, so the write ptr can be put there too
            // this is the case where the whole buffer gets consumed in one go
        }

        cmdbuf->cmdbuf_write = cmdbuf->cmdbuf_start;
    }

    fb->tile_perfcounters[tile_id].cmdbuf_pushcmd += qpc() - pushcmd_start_pc;
}

void framebuffer_resolve(framebuffer_t* fb)
{
    assert(fb);

    int32_t tile_i = 0;
    for (int32_t tile_y = 0; tile_y < fb->height_in_tiles; tile_y++)
    {
        for (int32_t tile_x = 0; tile_x < fb->width_in_tiles; tile_x++)
        {
            framebuffer_resolve_tile(fb, tile_i);
            tile_i++;
        }
    }
}

void framebuffer_pack_row_major(framebuffer_t* fb, attachment_t attachment, int32_t x, int32_t y, int32_t width, int32_t height, pixelformat_t format, void* data)
{
    assert(fb);
    assert(x >= 0 && x < fb->width_in_pixels);
    assert(y >= 0 && y < fb->height_in_pixels);
    assert(width >= 0 && width <= fb->width_in_pixels);
    assert(height >= 0 && height <= fb->height_in_pixels);
    assert(x + width <= fb->width_in_pixels);
    assert(y + height <= fb->height_in_pixels);
    assert(data);

    int32_t topleft_tile_y = y / TILE_WIDTH_IN_PIXELS;
    int32_t topleft_tile_x = x / TILE_WIDTH_IN_PIXELS;
    int32_t bottomright_tile_y = (y + (height - 1)) / TILE_WIDTH_IN_PIXELS;
    int32_t bottomright_tile_x = (x + (width - 1)) / TILE_WIDTH_IN_PIXELS;

    int32_t curr_tile_row_start = topleft_tile_y * fb->pixels_per_row_of_tiles + topleft_tile_x * PIXELS_PER_TILE;
    for (int32_t tile_y = topleft_tile_y; tile_y <= bottomright_tile_y; tile_y++)
    {
        int32_t curr_tile_start = curr_tile_row_start;

        for (int32_t tile_x = topleft_tile_x; tile_x <= bottomright_tile_x; tile_x++)
        {
            int32_t topleft_y = tile_y * TILE_WIDTH_IN_PIXELS;
            int32_t topleft_x = tile_x * TILE_WIDTH_IN_PIXELS;
            int32_t bottomright_y = topleft_y + TILE_WIDTH_IN_PIXELS;
            int32_t bottomright_x = topleft_x + TILE_WIDTH_IN_PIXELS;
            int32_t pixel_y_min = topleft_y < y ? y : topleft_y;
            int32_t pixel_x_min = topleft_x < x ? x : topleft_x;
            int32_t pixel_y_max = bottomright_y > y + height ? y + height : bottomright_y;
            int32_t pixel_x_max = bottomright_x > x + width ? x + width : bottomright_x;

            for (int32_t pixel_y = pixel_y_min, pixel_y_bits = pdep_u32(topleft_y, TILE_Y_SWIZZLE_MASK);
                pixel_y < pixel_y_max;
                pixel_y++, pixel_y_bits = (pixel_y_bits - TILE_Y_SWIZZLE_MASK) & TILE_Y_SWIZZLE_MASK)
            {
                for (int32_t pixel_x = pixel_x_min, pixel_x_bits = pdep_u32(topleft_x, TILE_X_SWIZZLE_MASK);
                    pixel_x < pixel_x_max;
                    pixel_x++, pixel_x_bits = (pixel_x_bits - TILE_X_SWIZZLE_MASK) & TILE_X_SWIZZLE_MASK)
                {
                    int32_t rel_pixel_y = pixel_y - y;
                    int32_t rel_pixel_x = pixel_x - x;
                    int32_t dst_i = rel_pixel_y * width + rel_pixel_x;

                    int32_t src_i = curr_tile_start + (pixel_y_bits | pixel_x_bits);
                    if (attachment == attachment_color0)
                    {
                        uint32_t src = fb->backbuffer[src_i];
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
                            assert(!"Unknown color pixel format");
                        }
                    }
                    else if (attachment == attachment_depth)
                    {
                        uint32_t src = fb->depthbuffer[src_i];
                        if (format == pixelformat_r32_unorm)
                        {
                            uint32_t* dst = (uint32_t*)data + dst_i;
                            *dst = src;
                        }
                        else
                        {
                            assert(!"Unknown depth pixel format");
                        }
                    }
                }
            }

            curr_tile_start += PIXELS_PER_TILE;
        }

        curr_tile_row_start += fb->pixels_per_row_of_tiles;
    }
}

void framebuffer_clear(framebuffer_t* fb, uint32_t color)
{
    tilecmd_cleartile_t tilecmd;
    tilecmd.tilecmd_id = tilecmd_id_cleartile;
    tilecmd.color = color;

    for (int32_t tile_id = 0; tile_id < fb->total_num_tiles; tile_id++)
    {
        framebuffer_push_tilecmd(fb, tile_id, &tilecmd.tilecmd_id, sizeof(tilecmd) / sizeof(uint32_t));
    }
}

static void rasterize_triangle(
    framebuffer_t* fb,
    xyzw_i32_t clipVerts[3])
{
    uint64_t clipping_start_pc = qpc();

    int32_t fully_clipped = 0;

    // perform near plane clipping
    {
        // check which vertices are behind the near plane
        int32_t vert_near_clipped[3];
        vert_near_clipped[0] = clipVerts[0].z < 0;
        vert_near_clipped[1] = clipVerts[1].z < 0;
        vert_near_clipped[2] = clipVerts[2].z < 0;

        int32_t num_near_clipped = vert_near_clipped[0] + vert_near_clipped[1] + vert_near_clipped[2];

        if (num_near_clipped == 3)
        {
            // clip whole triangles with 3 vertices behind the near plane
            fully_clipped = 1;
            goto clipping_end;
        }

        if (num_near_clipped == 2)
        {
            // Two vertices behind the near plane. In this case, cut the associated edges short.
            int32_t unclipped_vert = 0;
            if (!vert_near_clipped[1]) unclipped_vert = 1;
            else if (!vert_near_clipped[2]) unclipped_vert = 2;

            int32_t v1 = (unclipped_vert + 1) % 3;
            int32_t v2 = (unclipped_vert + 2) % 3;

            // clip the first edge
            int32_t a1 = s1516_div(clipVerts[unclipped_vert].z, clipVerts[unclipped_vert].z - clipVerts[v1].z);
            int32_t one_minus_a1 = s1516_int(1) - a1;
            clipVerts[v1].x = s1516_mul(one_minus_a1, clipVerts[unclipped_vert].x) + s1516_mul(a1, clipVerts[v1].x);
            clipVerts[v1].y = s1516_mul(one_minus_a1, clipVerts[unclipped_vert].y) + s1516_mul(a1, clipVerts[v1].y);
            clipVerts[v1].z = 0;
            clipVerts[v1].w = s1516_mul(one_minus_a1, clipVerts[unclipped_vert].w) + s1516_mul(a1, clipVerts[v1].w);
            assert(clipVerts[v1].w != 0);

            // clip the second edge
            int32_t a2 = s1516_div(clipVerts[unclipped_vert].z, clipVerts[unclipped_vert].z - clipVerts[v2].z);
            int32_t one_minus_a2 = s1516_int(1) - a2;
            clipVerts[v2].x = s1516_mul(one_minus_a2, clipVerts[unclipped_vert].x) + s1516_mul(a2, clipVerts[v2].x);
            clipVerts[v2].y = s1516_mul(one_minus_a2, clipVerts[unclipped_vert].y) + s1516_mul(a2, clipVerts[v2].y);
            clipVerts[v2].z = 0;
            clipVerts[v2].w = s1516_mul(one_minus_a2, clipVerts[unclipped_vert].w) + s1516_mul(a2, clipVerts[v2].w);
            assert(clipVerts[v2].w != 0);
        }

        if (num_near_clipped == 1)
        {
            // One vertex behind the near plane. In this case, triangulate the triangle into two triangles.
            int32_t clipped_vert = 0;
            if (vert_near_clipped[1]) clipped_vert = 1;
            else if (vert_near_clipped[2]) clipped_vert = 2;

            int32_t v1 = (clipped_vert + 1) % 3;
            int32_t v2 = (clipped_vert + 2) % 3;

            // clip the first edge
            xyzw_i32_t clipped1;
            int32_t a1 = s1516_div(clipVerts[clipped_vert].z, clipVerts[clipped_vert].z - clipVerts[v1].z);
            int32_t one_minus_a1 = s1516_int(1) - a1;
            clipped1.x = s1516_mul(one_minus_a1, clipVerts[clipped_vert].x) + s1516_mul(a1, clipVerts[v1].x);
            clipped1.y = s1516_mul(one_minus_a1, clipVerts[clipped_vert].y) + s1516_mul(a1, clipVerts[v1].y);
            clipped1.z = 0;
            clipped1.w = s1516_mul(one_minus_a1, clipVerts[clipped_vert].w) + s1516_mul(a1, clipVerts[v1].w);
            assert(clipped1.w != 0);

            // clip the second edge
            xyzw_i32_t clipped2;
            int32_t a2 = s1516_div(clipVerts[clipped_vert].z, clipVerts[clipped_vert].z - clipVerts[v2].z);
            int32_t one_minus_a2 = s1516_int(1) - a2;
            clipped2.x = s1516_mul(one_minus_a2, clipVerts[clipped_vert].x) + s1516_mul(a2, clipVerts[v2].x);
            clipped2.y = s1516_mul(one_minus_a2, clipVerts[clipped_vert].y) + s1516_mul(a2, clipVerts[v2].y);
            clipped2.z = 0;
            clipped2.w = s1516_mul(one_minus_a2, clipVerts[clipped_vert].w) + s1516_mul(a2, clipVerts[v2].w);
            assert(clipped2.w != 0);

            // output the first clipped triangle (note: recursive call)
            xyzw_i32_t clipVerts1[3] = { clipVerts[0], clipVerts[1], clipVerts[2] };
            clipVerts1[clipped_vert] = clipped1;

            fb->perfcounters.clipping += qpc() - clipping_start_pc;
            rasterize_triangle(fb, clipVerts1);
            clipping_start_pc = qpc();

            // set self up to output the second clipped triangle
            clipVerts[clipped_vert] = clipped2;
            clipVerts[v1] = clipped1;
        }
    }

    // perform far plane clipping
    {
        // check which vertices are behind (or on) the far plane
        int32_t vert_far_clipped[3];
        vert_far_clipped[0] = clipVerts[0].z >= clipVerts[0].w;
        vert_far_clipped[1] = clipVerts[1].z >= clipVerts[1].w;
        vert_far_clipped[2] = clipVerts[2].z >= clipVerts[2].w;

        int32_t num_far_clipped = vert_far_clipped[0] + vert_far_clipped[1] + vert_far_clipped[2];

        if (num_far_clipped == 3)
        {
            // clip whole triangles with 3 vertices behind the far plane
            fully_clipped = 1;
            goto clipping_end;
        }

        if (num_far_clipped == 2)
        {
            // Two vertices behind the far plane. In this case, cut the associated edges short.
            int32_t unclipped_vert = 0;
            if (!vert_far_clipped[1]) unclipped_vert = 1;
            else if (!vert_far_clipped[2]) unclipped_vert = 2;

            int32_t v1 = (unclipped_vert + 1) % 3;
            int32_t v2 = (unclipped_vert + 2) % 3;

            // clip the first edge
            int32_t a1 = s1516_div(clipVerts[unclipped_vert].z - clipVerts[unclipped_vert].w, (clipVerts[unclipped_vert].z - clipVerts[unclipped_vert].w) - (clipVerts[v1].z - clipVerts[v1].w));
            int32_t one_minus_a1 = s1516_int(1) - a1;
            clipVerts[v1].x = s1516_mul(one_minus_a1, clipVerts[unclipped_vert].x) + s1516_mul(a1, clipVerts[v1].x);
            clipVerts[v1].y = s1516_mul(one_minus_a1, clipVerts[unclipped_vert].y) + s1516_mul(a1, clipVerts[v1].y);
            clipVerts[v1].w = s1516_mul(one_minus_a1, clipVerts[unclipped_vert].w) + s1516_mul(a1, clipVerts[v1].w);
            clipVerts[v1].z = clipVerts[v1].w - 1;
            assert(clipVerts[v1].w != 0);

            // clip the second edge
            int32_t a2 = s1516_div(clipVerts[unclipped_vert].z - clipVerts[unclipped_vert].w, (clipVerts[unclipped_vert].z - clipVerts[unclipped_vert].w) - (clipVerts[v2].z - clipVerts[v2].w));
            int32_t one_minus_a2 = s1516_int(1) - a2;
            clipVerts[v2].x = s1516_mul(one_minus_a2, clipVerts[unclipped_vert].x) + s1516_mul(a2, clipVerts[v2].x);
            clipVerts[v2].y = s1516_mul(one_minus_a2, clipVerts[unclipped_vert].y) + s1516_mul(a2, clipVerts[v2].y);
            clipVerts[v2].w = s1516_mul(one_minus_a2, clipVerts[unclipped_vert].w) + s1516_mul(a2, clipVerts[v2].w);
            clipVerts[v2].z = clipVerts[v2].w - 1;
            assert(clipVerts[v2].w != 0);
        }

        if (num_far_clipped == 1)
        {
            // One vertex behind the near plane. In this case, triangulate the triangle into two triangles.
            int32_t clipped_vert = 0;
            if (vert_far_clipped[1]) clipped_vert = 1;
            else if (vert_far_clipped[2]) clipped_vert = 2;

            int32_t v1 = (clipped_vert + 1) % 3;
            int32_t v2 = (clipped_vert + 2) % 3;

            // clip the first edge
            xyzw_i32_t clipped1;
            int32_t a1 = s1516_div(clipVerts[clipped_vert].z - clipVerts[clipped_vert].w, (clipVerts[clipped_vert].z - clipVerts[clipped_vert].w) - (clipVerts[v1].z - clipVerts[v1].w));
            int32_t one_minus_a1 = s1516_int(1) - a1;
            clipped1.x = s1516_mul(one_minus_a1, clipVerts[clipped_vert].x) + s1516_mul(a1, clipVerts[v1].x);
            clipped1.y = s1516_mul(one_minus_a1, clipVerts[clipped_vert].y) + s1516_mul(a1, clipVerts[v1].y);
            clipped1.w = s1516_mul(one_minus_a1, clipVerts[clipped_vert].w) + s1516_mul(a1, clipVerts[v1].w);
            clipped1.z = clipped1.w - 1;
            assert(clipped1.w != 0);

            // clip the second edge
            xyzw_i32_t clipped2;
            int32_t a2 = s1516_div(clipVerts[clipped_vert].z - clipVerts[clipped_vert].w, (clipVerts[clipped_vert].z - clipVerts[clipped_vert].w) - (clipVerts[v2].z - clipVerts[v2].w));
            int32_t one_minus_a2 = s1516_int(1) - a2;
            clipped2.x = s1516_mul(one_minus_a2, clipVerts[clipped_vert].x) + s1516_mul(a2, clipVerts[v2].x);
            clipped2.y = s1516_mul(one_minus_a2, clipVerts[clipped_vert].y) + s1516_mul(a2, clipVerts[v2].y);
            clipped2.w = s1516_mul(one_minus_a2, clipVerts[clipped_vert].w) + s1516_mul(a2, clipVerts[v2].w);
            clipped2.z = clipped2.w - 1;
            assert(clipped2.w != 0);

            // output the first clipped triangle (note: recursive call)
            xyzw_i32_t clipVerts1[3] = { clipVerts[0], clipVerts[1], clipVerts[2] };
            clipVerts1[clipped_vert] = clipped1;

            fb->perfcounters.clipping += qpc() - clipping_start_pc;
            rasterize_triangle(fb, clipVerts1);
            clipping_start_pc = qpc();

            // set self up to output the second clipped triangle
            clipVerts[clipped_vert] = clipped2;
            clipVerts[v1] = clipped1;
        }
    }

clipping_end:
    fb->perfcounters.clipping += qpc() - clipping_start_pc;
    if (fully_clipped)
    {
        return;
    }

    uint64_t commonsetup_start_pc = qpc();

    // transform vertices from clip space to window coordinates
    xyzw_i32_t verts[3];
    int32_t rcp_ws[3];
    for (int32_t v = 0; v < 3; v++)
    {
        int32_t one_over_w = s1516_div(s1516_int(1), clipVerts[v].w);

        // convert s15.16 (in clip space) to s16.8 window coordinates
        // note to self: should probably avoid round-to-zero here? otherwise geometry warps inwards to the center of the screen
        verts[v].x = s168_s1516(s1516_mul(s1516_div(s1516_add(s1516_mul(+clipVerts[v].x, one_over_w), s1516_int(1)), s1516_int(2)), s1516_int(fb->width_in_pixels)));
        verts[v].y = s168_s1516(s1516_mul(s1516_div(s1516_add(s1516_mul(-clipVerts[v].y, one_over_w), s1516_int(1)), s1516_int(2)), s1516_int(fb->height_in_pixels)));

        // TODO: clip things that all outside the guard band

        verts[v].z = s1516_mul(clipVerts[v].z, one_over_w);

        verts[v].w = clipVerts[v].w;
        rcp_ws[v] = one_over_w;
    }

    uint32_t min_Z = verts[0].z;
    uint32_t max_Z = verts[0].z;
    for (int32_t v = 1; v < 3; v++)
    {
        if ((uint32_t)verts[v].z < min_Z)
            min_Z = (uint32_t)verts[v].z;
        
        if ((uint32_t)verts[v].z > max_Z)
            max_Z = (uint32_t)verts[v].z;
    }
    
    // get window coordinates bounding box
    int32_t bbox_min_x = verts[0].x;
    if (verts[1].x < bbox_min_x) bbox_min_x = verts[1].x;
    if (verts[2].x < bbox_min_x) bbox_min_x = verts[2].x;
    int32_t bbox_max_x = verts[0].x;
    if (verts[1].x > bbox_max_x) bbox_max_x = verts[1].x;
    if (verts[2].x > bbox_max_x) bbox_max_x = verts[2].x;
    int32_t bbox_min_y = verts[0].y;
    if (verts[1].y < bbox_min_y) bbox_min_y = verts[1].y;
    if (verts[2].y < bbox_min_y) bbox_min_y = verts[2].y;
    int32_t bbox_max_y = verts[0].y;
    if (verts[1].y > bbox_max_y) bbox_max_y = verts[1].y;
    if (verts[2].y > bbox_max_y) bbox_max_y = verts[2].y;

    // clip triangles that are fully outside the scissor rect (scissor rect = whole window)
    if (bbox_max_x < 0 ||
        bbox_max_y < 0 ||
        bbox_min_x >= (int32_t)(fb->width_in_pixels << 8) ||
        bbox_min_y >= (int32_t)(fb->height_in_pixels << 8))
    {
        fully_clipped = 1;
        goto commonsetup_end;
    }

    int32_t clamped_bbox_min_x = bbox_min_x, clamped_bbox_max_x = bbox_max_x;
    int32_t clamped_bbox_min_y = bbox_min_y, clamped_bbox_max_y = bbox_max_y;

    // clamp bbox to scissor rect
    if (clamped_bbox_min_x < 0) clamped_bbox_min_x = 0;
    if (clamped_bbox_min_y < 0) clamped_bbox_min_y = 0;
    if (clamped_bbox_max_x >= (int32_t)(fb->width_in_pixels << 8)) clamped_bbox_max_x = ((int32_t)fb->width_in_pixels << 8) - 1;
    if (clamped_bbox_max_y >= (int32_t)(fb->height_in_pixels << 8)) clamped_bbox_max_y = ((int32_t)fb->height_in_pixels << 8) - 1;

    // "small" triangles are no wider than a tile.
    int32_t is_large =
        (bbox_max_x - bbox_min_x) >= (TILE_WIDTH_IN_PIXELS << 8) ||
        (bbox_max_y - bbox_min_y) >= (TILE_WIDTH_IN_PIXELS << 8);

commonsetup_end:
    fb->perfcounters.common_setup += qpc() - commonsetup_start_pc;
    if (fully_clipped)
    {
        return;
    }

    uint64_t setup_start_pc = qpc();

    if (!is_large)
    {
        // since this is a small triangle, that means the triangle is smaller than a tile.
        // that means it can overlap at most 2x2 adjacent tiles if it's in the middle of all of them.
        // just need to figure out which boxes are overlapping the triangle's bbox
        int32_t first_tile_x = (bbox_min_x >> 8) / TILE_WIDTH_IN_PIXELS;
        int32_t first_tile_y = (bbox_min_y >> 8) / TILE_WIDTH_IN_PIXELS;
        int32_t last_tile_x = (bbox_max_x >> 8) / TILE_WIDTH_IN_PIXELS;
        int32_t last_tile_y = (bbox_max_y >> 8) / TILE_WIDTH_IN_PIXELS;
        
        // pixel coordinates of the first and last tile of the (up to) 2x2 block of tiles
        int32_t first_tile_px_x = (first_tile_x << 8) * TILE_WIDTH_IN_PIXELS;
        int32_t first_tile_px_y = (first_tile_y << 8) * TILE_WIDTH_IN_PIXELS;
        int32_t last_tile_px_x = (last_tile_x << 8) * TILE_WIDTH_IN_PIXELS;
        int32_t last_tile_px_y = (last_tile_y << 8) * TILE_WIDTH_IN_PIXELS;

        // range of coarse blocks affected (relative to top left of 2x2 tile block)
        int32_t first_rel_cb_x = ((bbox_min_x - first_tile_px_x) >> 8) / COARSE_BLOCK_WIDTH_IN_PIXELS;
        int32_t first_rel_cb_y = ((bbox_min_y - first_tile_px_y) >> 8) / COARSE_BLOCK_WIDTH_IN_PIXELS;
        int32_t last_rel_cb_x = ((bbox_max_x - first_tile_px_x) >> 8) / COARSE_BLOCK_WIDTH_IN_PIXELS;
        int32_t last_rel_cb_y = ((bbox_max_y - first_tile_px_y) >> 8) / COARSE_BLOCK_WIDTH_IN_PIXELS;

        tilecmd_drawsmalltri_t drawsmalltricmd;
        drawsmalltricmd.tilecmd_id = tilecmd_id_drawsmalltri;

        // make vertices relative to the last tile they're in
        for (int32_t v = 0; v < 3; v++)
        {
            // the point of making them relative is to lower the required precision to 4 hex digits
            assert((verts[v].x - last_tile_px_x) >= (-128 << 8) && (verts[v].x - last_tile_px_x) <= ((128 << 8) - 1));
            assert((verts[v].y - last_tile_px_y) >= (-128 << 8) && (verts[v].y - last_tile_px_y) <= ((128 << 8) - 1));

            verts[v].x -= last_tile_px_x;
            verts[v].y -= last_tile_px_y;
        }

        int32_t triarea2 = ((verts[1].x - verts[0].x) * (verts[2].y - verts[0].y) - (verts[1].y - verts[0].y) * (verts[2].x - verts[0].x)) >> 8;
        
        if (triarea2 == 0)
        {
            goto setup_end;
        }

        if (triarea2 < 0)
        {
            xyzw_i32_t tmp = verts[1];
            verts[1] = verts[2];
            verts[2] = tmp;
            triarea2 = -triarea2;

            int32_t tmp_rcp_w = rcp_ws[1];
            rcp_ws[1] = rcp_ws[2];
            rcp_ws[2] = tmp_rcp_w;
        }

        // compute 1/(2triarea) and convert to a pseudo 8.8 floating point value
        int32_t triarea2_lzcnt = lzcnt(triarea2);
        int32_t triarea2_mantissa_rshift = (31 - 8) - triarea2_lzcnt;
        int32_t triarea2_mantissa;
        if (triarea2_mantissa_rshift < 0)
            triarea2_mantissa = triarea2 << -triarea2_mantissa_rshift;
        else
            triarea2_mantissa = triarea2 >> triarea2_mantissa_rshift;
        
        // perform the reciprocal
        // note: triarea2_mantissa is currently normalized as 1.8, and so is the numerator of the division (before being adjusted for rounding)
        int32_t rcp_triarea2_mantissa = 0xFFFF / triarea2_mantissa;
        assert(rcp_triarea2_mantissa != 0);
        
        // ensure the mantissa is denormalized so it fits in 8 bits
        int32_t rcp_triarea2_mantissa_rshift = (31 - 7) - lzcnt(rcp_triarea2_mantissa);
        if (rcp_triarea2_mantissa_rshift < 0)
            rcp_triarea2_mantissa = rcp_triarea2_mantissa << -rcp_triarea2_mantissa_rshift;
        else
            rcp_triarea2_mantissa = rcp_triarea2_mantissa >> rcp_triarea2_mantissa_rshift;

        assert(rcp_triarea2_mantissa < 0x100);
        rcp_triarea2_mantissa = rcp_triarea2_mantissa & 0xFF;
        uint32_t rcp_triarea2_exponent = 127 + triarea2_mantissa_rshift - rcp_triarea2_mantissa_rshift;
        uint32_t rcp_triarea2 = (rcp_triarea2_exponent << 8) | rcp_triarea2_mantissa;

        drawsmalltricmd.rcp_triarea2 = rcp_triarea2;

        // compute edge equations with reduced precision thanks to being localized to the tiles

        int32_t edges[3];
        int32_t edge_dxs[3], edge_dys[3];
        for (int32_t v = 0; v < 3; v++)
        {
            int32_t v1 = (v + 1) % 3;
            int32_t v2 = (v + 2) % 3;

            // find how the edge equation varies along x and y
            edge_dxs[v] = verts[v1].y - verts[v].y;
            edge_dys[v] = verts[v].x - verts[v1].x;

            // compute edge equation
            // |  x  y  z |
            // | ax ay  0 |
            // | bx by  0 |
            // = ax*by - ay*bx
            // eg: a = (px-v0), b = (v1-v0)
            // note: evaluated at px = (0.5,0.5) because the vertices are relative to the last tile
            const int32_t s168_zero_pt_five = 0x80;
            edges[v] = ((s168_zero_pt_five - verts[v].x) * edge_dxs[v]) - ((s168_zero_pt_five - verts[v].y) * -edge_dys[v]);
            
            // Top-left rule: shift top-left edges ever so slightly outward to make the top-left edges be the tie-breakers when rasterizing adjacent triangles
            if ((verts[v].y == verts[v1].y && verts[v].x < verts[v1].x) || verts[v].y > verts[v1].y) edges[v]--;

            // truncate (don't worry, this works out because the top-left rule works as a rounding mode)
            edges[v] = edges[v] >> 8;
        }

        drawsmalltricmd.min_Z = min_Z;
        drawsmalltricmd.max_Z = max_Z;

        // rotate vertices so the one with maximum edge equation slope doesn't get used in interpolation
        int32_t max_slope_vertex = -1;
        int32_t max_slope = 0;
        for (int32_t i = 0; i < 3; i++)
        {
            int32_t v1 = (i + 1) % 3;
            int32_t slope = edge_dxs[v1] * edge_dxs[1] + edge_dys[v1] * edge_dys[v1];
            assert((int64_t)slope == ((int64_t)edge_dxs[v1] * edge_dxs[1] + (int64_t)edge_dys[v1] * edge_dys[v1]));
            if (slope > max_slope)
            {
                max_slope_vertex = i;
                max_slope = slope;
            }
        }

        if (max_slope_vertex == 1)
        {
            int32_t e = edges[0];
            int32_t edx = edge_dxs[0];
            int32_t edy = edge_dys[0];
            xyzw_i32_t v = verts[0];
            int32_t rcpw = rcp_ws[0];

            edges[0] = edges[1];
            edge_dxs[0] = edge_dxs[1];
            edge_dys[0] = edge_dys[1];
            verts[0] = verts[1];
            rcp_ws[0] = rcp_ws[1];

            edges[1] = edges[2];
            edge_dxs[1] = edge_dxs[2];
            edge_dys[1] = edge_dys[2];
            verts[1] = verts[2];
            rcp_ws[1] = rcp_ws[2];

            edges[2] = e;
            edge_dxs[2] = edx;
            edge_dys[2] = edy;
            verts[2] = v;
            rcp_ws[2] = rcpw;
        }
        else if (max_slope_vertex == 2)
        {
            int32_t e = edges[0];
            int32_t edx = edge_dxs[0];
            int32_t edy = edge_dys[0];
            xyzw_i32_t v = verts[0];
            int32_t rcpw = rcp_ws[0];

            edges[0] = edges[2];
            edge_dxs[0] = edge_dxs[2];
            edge_dys[0] = edge_dys[2];
            verts[0] = verts[2];
            rcp_ws[0] = rcp_ws[2];

            edges[2] = edges[1];
            edge_dxs[2] = edge_dxs[1];
            edge_dys[2] = edge_dys[1];
            verts[2] = verts[1];
            rcp_ws[2] = rcp_ws[1];

            edges[1] = e;
            edge_dxs[1] = edx;
            edge_dys[1] = edy;
            verts[1] = v;
            rcp_ws[1] = rcpw;
        }

        for (int32_t v = 0; v < 3; v++)
        {
            drawsmalltricmd.edge_dxs[v] = edge_dxs[v];
            drawsmalltricmd.edge_dys[v] = edge_dys[v];
            drawsmalltricmd.vert_Zs[v] = verts[v].z;
        }

        // draw top left tile
        int32_t first_tile_id = first_tile_y * fb->width_in_tiles + first_tile_x;
        if (first_tile_x >= 0 && first_tile_y >= 0)
        {
            for (int32_t v = 0; v < 3; v++)
            {
                drawsmalltricmd.edges[v] = edges[v] + (
                    edge_dxs[v] * (first_tile_x - last_tile_x) +
                    edge_dys[v] * (first_tile_y - last_tile_y)) * TILE_WIDTH_IN_PIXELS;
            }

            drawsmalltricmd.first_coarse_x = first_rel_cb_x;
            if (drawsmalltricmd.first_coarse_x < 0)
            {
                drawsmalltricmd.first_coarse_x = 0;
            }
            
            drawsmalltricmd.last_coarse_x = last_rel_cb_x;
            if (drawsmalltricmd.last_coarse_x >= TILE_WIDTH_IN_COARSE_BLOCKS)
            {
                drawsmalltricmd.last_coarse_x = TILE_WIDTH_IN_COARSE_BLOCKS - 1;
            }
            
            drawsmalltricmd.first_coarse_y = first_rel_cb_y;
            if (drawsmalltricmd.first_coarse_y < 0)
            {
                drawsmalltricmd.first_coarse_y = 0;
            }

            drawsmalltricmd.last_coarse_y = last_rel_cb_y;
            if (drawsmalltricmd.last_coarse_y >= TILE_WIDTH_IN_COARSE_BLOCKS)
            {
                drawsmalltricmd.last_coarse_y = TILE_WIDTH_IN_COARSE_BLOCKS - 1;
            }

            fb->perfcounters.smalltri_setup += qpc() - setup_start_pc;
            framebuffer_push_tilecmd(fb, first_tile_id, &drawsmalltricmd.tilecmd_id, sizeof(drawsmalltricmd) / sizeof(uint32_t));
            setup_start_pc = qpc();
        }

        // draw top right tile
        if (last_tile_x > first_tile_x &&
            last_tile_x < fb->width_in_tiles && first_tile_y >= 0)
        {
            for (int32_t v = 0; v < 3; v++)
            {
                drawsmalltricmd.edges[v] = edges[v] + edge_dys[v] * (first_tile_y - last_tile_y) * TILE_WIDTH_IN_PIXELS;
            }

            drawsmalltricmd.first_coarse_x = 0;

            drawsmalltricmd.last_coarse_x = last_rel_cb_x - TILE_WIDTH_IN_COARSE_BLOCKS;
            if (drawsmalltricmd.last_coarse_x >= TILE_WIDTH_IN_COARSE_BLOCKS)
            {
                drawsmalltricmd.last_coarse_x = TILE_WIDTH_IN_COARSE_BLOCKS - 1;
            }

            drawsmalltricmd.first_coarse_y = first_rel_cb_y;
            if (drawsmalltricmd.first_coarse_y < 0)
            {
                drawsmalltricmd.first_coarse_y = 0;
            }

            drawsmalltricmd.last_coarse_y = last_rel_cb_y;
            if (drawsmalltricmd.last_coarse_y >= TILE_WIDTH_IN_COARSE_BLOCKS)
            {
                drawsmalltricmd.last_coarse_y = TILE_WIDTH_IN_COARSE_BLOCKS - 1;
            }

            int32_t tile_id_right = first_tile_id + 1;
            fb->perfcounters.smalltri_setup += qpc() - setup_start_pc;
            framebuffer_push_tilecmd(fb, tile_id_right, &drawsmalltricmd.tilecmd_id, sizeof(drawsmalltricmd) / sizeof(uint32_t));
            setup_start_pc = qpc();
        }

        // draw bottom left tile
        if (last_tile_y > first_tile_y &&
            first_tile_x >= 0 && last_tile_y < fb->height_in_tiles)
        {
            for (int32_t v = 0; v < 3; v++)
            {
                drawsmalltricmd.edges[v] = edges[v] + edge_dxs[v] * (first_tile_x - last_tile_x) * TILE_WIDTH_IN_PIXELS;
            }

            drawsmalltricmd.first_coarse_x = first_rel_cb_x;
            if (drawsmalltricmd.first_coarse_x < 0)
            {
                drawsmalltricmd.first_coarse_x = 0;
            }

            drawsmalltricmd.last_coarse_x = last_rel_cb_x;
            if (drawsmalltricmd.last_coarse_x >= TILE_WIDTH_IN_COARSE_BLOCKS)
            {
                drawsmalltricmd.last_coarse_x = TILE_WIDTH_IN_COARSE_BLOCKS - 1;
            }

            drawsmalltricmd.first_coarse_y = 0;

            drawsmalltricmd.last_coarse_y = last_rel_cb_y - TILE_WIDTH_IN_COARSE_BLOCKS;
            if (drawsmalltricmd.last_coarse_y >= TILE_WIDTH_IN_COARSE_BLOCKS)
            {
                drawsmalltricmd.last_coarse_y = TILE_WIDTH_IN_COARSE_BLOCKS - 1;
            }

            int32_t tile_id_down = first_tile_id + fb->width_in_tiles;
            fb->perfcounters.smalltri_setup += qpc() - setup_start_pc;
            framebuffer_push_tilecmd(fb, tile_id_down, &drawsmalltricmd.tilecmd_id, sizeof(drawsmalltricmd) / sizeof(uint32_t));
            setup_start_pc = qpc();
        }

        // draw bottom right tile
        if (last_tile_x > first_tile_x && last_tile_y > first_tile_y &&
            last_tile_x < fb->width_in_tiles && last_tile_y < fb->height_in_tiles)
        {
            for (int32_t v = 0; v < 3; v++)
            {
                drawsmalltricmd.edges[v] = edges[v];
            }

            drawsmalltricmd.first_coarse_x = 0;
            
            drawsmalltricmd.last_coarse_x = last_rel_cb_x - TILE_WIDTH_IN_COARSE_BLOCKS;
            if (drawsmalltricmd.last_coarse_x >= TILE_WIDTH_IN_COARSE_BLOCKS)
            {
                drawsmalltricmd.last_coarse_x = TILE_WIDTH_IN_COARSE_BLOCKS - 1;
            }
            
            drawsmalltricmd.first_coarse_y = 0;
            
            drawsmalltricmd.last_coarse_y = last_rel_cb_y - TILE_WIDTH_IN_COARSE_BLOCKS;
            if (drawsmalltricmd.last_coarse_y >= TILE_WIDTH_IN_COARSE_BLOCKS)
            {
                drawsmalltricmd.last_coarse_y = TILE_WIDTH_IN_COARSE_BLOCKS - 1;
            }

            int32_t tile_id_downright = first_tile_id + 1 + fb->width_in_tiles;
            fb->perfcounters.smalltri_setup += qpc() - setup_start_pc;
            framebuffer_push_tilecmd(fb, tile_id_downright, &drawsmalltricmd.tilecmd_id, sizeof(drawsmalltricmd) / sizeof(uint32_t));
            setup_start_pc = qpc();
        }
    }
    else // large triangle
    {
        // for large triangles, test each tile in their bbox for overlap
        // done using scalar code for simplicity, since rasterization dominates large triangle performance anyways.
        int32_t first_tile_x = (clamped_bbox_min_x >> 8) / TILE_WIDTH_IN_PIXELS;
        int32_t first_tile_y = (clamped_bbox_min_y >> 8) / TILE_WIDTH_IN_PIXELS;
        int32_t last_tile_x = (clamped_bbox_max_x >> 8) / TILE_WIDTH_IN_PIXELS;
        int32_t last_tile_y = (clamped_bbox_max_y >> 8) / TILE_WIDTH_IN_PIXELS;

        // evaluate edge equation at the top left tile
        int32_t first_tile_px_x = (first_tile_x << 8) * TILE_WIDTH_IN_PIXELS;
        int32_t first_tile_px_y = (first_tile_y << 8) * TILE_WIDTH_IN_PIXELS;

        // 64 bit integers are used for the edge equations here because multiplying two 16.8 numbers requires up to 48 bits
        // this results in some extra overhead, but it's not a big deal when you consider that this happens only for large triangles.
        // The tens of thousands of pixels that large triangles generate outweigh the cost of slightly more expensive setup.

        int64_t triarea2 = (((int64_t)verts[1].x - verts[0].x) * ((int64_t)verts[2].y - verts[0].y) - ((int64_t)verts[1].y - verts[0].y) * ((int64_t)verts[2].x - verts[0].x)) >> 8;
        
        if (triarea2 == 0)
        {
            goto setup_end;
        }

        if (triarea2 < 0)
        {
            xyzw_i32_t tmp = verts[1];
            verts[1] = verts[2];
            verts[2] = tmp;
            triarea2 = -triarea2;

            int32_t tmp_rcp_w = rcp_ws[1];
            rcp_ws[1] = rcp_ws[2];
            rcp_ws[2] = tmp_rcp_w;
        }

        // compute 1/(2triarea) and convert to a pseudo 8.16 floating point value
        int32_t triarea2_lzcnt = (int32_t)lzcnt64(triarea2);
        int32_t triarea2_mantissa_rshift = (63 - 16) - triarea2_lzcnt;
        int32_t triarea2_mantissa;
        if (triarea2_mantissa_rshift < 0)
            triarea2_mantissa = (int32_t)(triarea2 << -triarea2_mantissa_rshift);
        else
            triarea2_mantissa = (int32_t)(triarea2 >> triarea2_mantissa_rshift);

        // perform the reciprocal
        // note: triarea2_mantissa is currently normalized as 1.16, and so is the numerator of the division (before being adjusted for rounding)
        int32_t rcp_triarea2_mantissa = 0xFFFFFFFF / triarea2_mantissa;
        assert(rcp_triarea2_mantissa != 0);

        // ensure the mantissa is denormalized so it fits in 16 bits
        int32_t rcp_triarea2_mantissa_rshift = (31 - 15) - lzcnt(rcp_triarea2_mantissa);
        if (rcp_triarea2_mantissa_rshift < 0)
            rcp_triarea2_mantissa = rcp_triarea2_mantissa << -rcp_triarea2_mantissa_rshift;
        else
            rcp_triarea2_mantissa = rcp_triarea2_mantissa >> rcp_triarea2_mantissa_rshift;

        assert(rcp_triarea2_mantissa < 0x10000);
        rcp_triarea2_mantissa = rcp_triarea2_mantissa & 0xFFFF;
        uint32_t rcp_triarea2_exponent = 127 + triarea2_mantissa_rshift - rcp_triarea2_mantissa_rshift;
        uint32_t rcp_triarea2 = (rcp_triarea2_exponent << 16) | rcp_triarea2_mantissa;

        int64_t edges[3];
        int64_t edge_dxs[3], edge_dys[3];
        for (int32_t v = 0; v < 3; v++)
        {
            int32_t v1 = (v + 1) % 3;
            int32_t v2 = (v + 2) % 3;

            // find how the edge equation varies along x and y
            edge_dxs[v] = verts[v1].y - verts[v].y;
            edge_dys[v] = verts[v].x - verts[v1].x;

            // compute edge equation
            // |  x  y  z |
            // | ax ay  0 |
            // | bx by  0 |
            // = ax*by - ay*bx
            // eg: a = (px-v0), b = (v1-v0)
            // note: evaluated at px + (0.5,0.5)
            const int32_t s168_zero_pt_five = 0x80;
            edges[v] = ((int64_t)first_tile_px_x + s168_zero_pt_five - verts[v].x) * edge_dxs[v] - ((int64_t)first_tile_px_y + s168_zero_pt_five - verts[v].y) * -edge_dys[v];

            // Top-left rule: shift top-left edges ever so slightly outward to make the top-left edges be the tie-breakers when rasterizing adjacent triangles
            if ((verts[v].y == verts[v1].y && verts[v].x < verts[v1].x) || verts[v].y > verts[v1].y) edges[v]--;

            // truncate (don't worry, this works out because the top-left rule works as a rounding mode)
            edges[v] = edges[v] >> 8;
        }

        int64_t tile_edge_dxs[3];
        int64_t tile_edge_dys[3];
        for (int32_t v = 0; v < 3; v++)
        {
            tile_edge_dxs[v] = edge_dxs[v] * TILE_WIDTH_IN_PIXELS;
            tile_edge_dys[v] = edge_dys[v] * TILE_WIDTH_IN_PIXELS;
        }

        int64_t edge_trivRejs[3];
        int64_t edge_trivAccs[3];

        for (int32_t v = 0; v < 3; v++)
        {
            edge_trivRejs[v] = edges[v];
            edge_trivAccs[v] = edges[v];
            if (tile_edge_dxs[v] < 0) edge_trivRejs[v] += tile_edge_dxs[v];
            if (tile_edge_dxs[v] > 0) edge_trivAccs[v] += tile_edge_dxs[v];
            if (tile_edge_dys[v] < 0) edge_trivRejs[v] += tile_edge_dys[v];
            if (tile_edge_dys[v] > 0) edge_trivAccs[v] += tile_edge_dys[v];
        }

        int32_t tile_row_start = first_tile_y * fb->width_in_tiles + first_tile_x;
        for (int32_t tile_y = first_tile_y; tile_y <= last_tile_y; tile_y++)
        {
            int64_t tile_i_edges[3];
            int64_t tile_i_edge_trivRejs[3];
            int64_t tile_i_edge_trivAccs[3];
            for (int32_t v = 0; v < 3; v++)
            {
                tile_i_edges[v] = edges[v];
                tile_i_edge_trivRejs[v] = edge_trivRejs[v];
                tile_i_edge_trivAccs[v] = edge_trivAccs[v];
            }

            int32_t tile_i = tile_row_start;

            for (int32_t tile_x = first_tile_x; tile_x <= last_tile_x; tile_x++)
            {
                // trivial reject if at least one edge doesn't cover the tile at all
                int32_t trivially_rejected = tile_i_edge_trivRejs[0] >= 0 || tile_i_edge_trivRejs[1] >= 0 || tile_i_edge_trivRejs[2] >= 0;

                if (!trivially_rejected)
                {
                    tilecmd_drawtile_t drawtilecmd;

                    int32_t edge_needs_test[3];
                    edge_needs_test[0] = tile_i_edge_trivAccs[0] >= 0;
                    edge_needs_test[1] = tile_i_edge_trivAccs[1] >= 0;
                    edge_needs_test[2] = tile_i_edge_trivAccs[2] >= 0;
                    int32_t num_tests_necessary = edge_needs_test[0] + edge_needs_test[1] + edge_needs_test[2];

                    drawtilecmd.tilecmd_id = tilecmd_id_drawtile_0edge + num_tests_necessary;

                    // the N edges to test are the first N in the tilecmd.
                    // To do this, the triangle's vertices and edges are rotated.

                    int32_t vertex_rotation = 0;

                    if (num_tests_necessary == 1) {
                        if (edge_needs_test[1]) vertex_rotation = 1;
                        else if (edge_needs_test[2]) vertex_rotation = 2;
                    }
                    else if (num_tests_necessary == 2) {
                        if (!edge_needs_test[0]) vertex_rotation = 1;
                        else if (!edge_needs_test[1]) vertex_rotation = 2;
                    }

                    for (int32_t v = 0; v < 3; v++)
                    {
                        int32_t rotated_v = (v + vertex_rotation) % 3;

                        // ensure edges to test are within range of 32 bits (they should be, since trivial accept/reject only keeps nearby edges)
                        if (v < num_tests_necessary)
                        {
                            assert(tile_i_edges[rotated_v] >= INT32_MIN && tile_i_edges[rotated_v] <= INT32_MAX);
                            assert(edge_dxs[rotated_v] >= INT32_MIN && edge_dxs[rotated_v] <= INT32_MAX);
                            assert(edge_dys[rotated_v] >= INT32_MIN && edge_dys[rotated_v] <= INT32_MAX);
                        }

                        drawtilecmd.edges[v] = (int32_t)tile_i_edges[rotated_v];
                        drawtilecmd.edge_dxs[v] = (int32_t)edge_dxs[rotated_v];
                        drawtilecmd.edge_dys[v] = (int32_t)edge_dys[rotated_v];
                    }

                    drawtilecmd.min_Z = min_Z;
                    drawtilecmd.max_Z = max_Z;

                    drawtilecmd.rcp_triarea2 = rcp_triarea2;

                    fb->perfcounters.largetri_setup += qpc() - setup_start_pc;
                    framebuffer_push_tilecmd(fb, tile_i, &drawtilecmd.tilecmd_id, sizeof(drawtilecmd) / sizeof(uint32_t));
                    setup_start_pc = qpc();
                }

                tile_i++;
                for (int32_t v = 0; v < 3; v++)
                {
                    tile_i_edges[v] += tile_edge_dxs[v];
                    tile_i_edge_trivRejs[v] += tile_edge_dxs[v];
                    tile_i_edge_trivAccs[v] += tile_edge_dxs[v];
                }
            }

            tile_row_start += fb->width_in_tiles;
            for (int32_t v = 0; v < 3; v++)
            {
                edges[v] += tile_edge_dys[v];
                edge_trivRejs[v] += tile_edge_dys[v];
                edge_trivAccs[v] += tile_edge_dys[v];
            }
        }
    }

setup_end:;
    if (is_large)
    {
        fb->perfcounters.largetri_setup += qpc() - setup_start_pc;
    }
    else
    {
        fb->perfcounters.smalltri_setup += qpc() - setup_start_pc;
    }
} 

void framebuffer_draw(
    framebuffer_t* fb,
    const int32_t* vertices,
    uint32_t num_vertices)
{
    assert(fb);
    assert(vertices);
    assert(num_vertices % 3 == 0);

    for (uint32_t vertex_id = 0, cmpt_id = 0; vertex_id < num_vertices; vertex_id += 3, cmpt_id += 12)
    {
        xyzw_i32_t verts[3];

        verts[0].x = vertices[cmpt_id + 0];
        verts[0].y = vertices[cmpt_id + 1];
        verts[0].z = vertices[cmpt_id + 2];
        verts[0].w = vertices[cmpt_id + 3];
        verts[1].x = vertices[cmpt_id + 4];
        verts[1].y = vertices[cmpt_id + 5];
        verts[1].z = vertices[cmpt_id + 6];
        verts[1].w = vertices[cmpt_id + 7];
        verts[2].x = vertices[cmpt_id + 8];
        verts[2].y = vertices[cmpt_id + 9];
        verts[2].z = vertices[cmpt_id + 10];
        verts[2].w = vertices[cmpt_id + 11];

        rasterize_triangle(fb, verts);
    }
}

void framebuffer_draw_indexed(
    framebuffer_t* fb,
    const int32_t* vertices,
    const uint32_t* indices,
    uint32_t num_indices)
{
    assert(fb);
    assert(vertices);
    assert(indices);
    assert(num_indices % 3 == 0);

    for (uint32_t index_id = 0; index_id < num_indices; index_id += 3)
    {
        xyzw_i32_t verts[3];

        uint32_t cmpt_i0 = indices[index_id + 0] * 4;
        uint32_t cmpt_i1 = indices[index_id + 1] * 4;
        uint32_t cmpt_i2 = indices[index_id + 2] * 4;

        verts[0].x = vertices[cmpt_i0 + 0];
        verts[0].y = vertices[cmpt_i0 + 1];
        verts[0].z = vertices[cmpt_i0 + 2];
        verts[0].w = vertices[cmpt_i0 + 3];
        verts[1].x = vertices[cmpt_i1 + 0];
        verts[1].y = vertices[cmpt_i1 + 1];
        verts[1].z = vertices[cmpt_i1 + 2];
        verts[1].w = vertices[cmpt_i1 + 3];
        verts[2].x = vertices[cmpt_i2 + 0];
        verts[2].y = vertices[cmpt_i2 + 1];
        verts[2].z = vertices[cmpt_i2 + 2];
        verts[2].w = vertices[cmpt_i2 + 3];

        rasterize_triangle(fb, verts);
    }
}

int32_t framebuffer_get_total_num_tiles(framebuffer_t* fb)
{
    assert(fb);
    return fb->total_num_tiles;
}

uint64_t framebuffer_get_perfcounter_frequency(framebuffer_t* fb)
{
    assert(fb);
    return fb->pc_frequency;
}

void framebuffer_reset_perfcounters(framebuffer_t* fb)
{
    memset(&fb->perfcounters, 0, sizeof(framebuffer_perfcounters_t));
    memset(fb->tile_perfcounters, 0, sizeof(tile_perfcounters_t) * fb->total_num_tiles);
}

void framebuffer_get_perfcounters(framebuffer_t* fb, framebuffer_perfcounters_t* pcs)
{
    assert(fb);
    assert(pcs);

    *pcs = fb->perfcounters;
}

void framebuffer_get_tile_perfcounters(framebuffer_t* fb, tile_perfcounters_t tile_pcs[])
{
    assert(fb);
    assert(tile_pcs);

    for (int32_t i = 0; i < fb->total_num_tiles; i++)
    {
        tile_pcs[i] = fb->tile_perfcounters[i];
    }
}