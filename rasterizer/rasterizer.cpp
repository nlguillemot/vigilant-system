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

#ifdef __AVX2__
#include <intrin.h>
#endif

// runs unit tests automatically when the library is used
//#define RASTERIZER_UNIT_TESTS

#ifdef RASTERIZER_UNIT_TESTS
void run_rasterizer_unit_tests();
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
    return _pdep_u32(source, mask);
}
#else
__forceinline uint32_t pdep_u32(uint32_t source, uint32_t mask)
{
    // horribly inefficient, but that's life without AVX2.
    // however, typically not a problem since you only need to swizzle once up front.
	// Implementation based on the pseudocode in http://www.felixcloutier.com/x86/PDEP.html
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
    xyzw_i32_t verts[3];
	int32_t edges[3];
	int32_t edge_dxs[3];
	int32_t edge_dys[3];
	int32_t first_coarse_x, last_coarse_x;
	int32_t first_coarse_y, last_coarse_y;
} tilecmd_drawsmalltri_t;

typedef struct tilecmd_drawtile_t
{
    uint32_t tilecmd_id;
    xyzw_i32_t verts[3];
    int32_t edges[3];
    int32_t edge_dxs[3];
    int32_t edge_dys[3];
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
} framebuffer_t;

framebuffer_t* new_framebuffer(int32_t width, int32_t height)
{
#ifdef RASTERIZER_UNIT_TESTS
    static int32_t ran_rasterizer_unit_tests_once = 0;
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

// hack
static uint32_t g_Color = 0xFFFF00FF;

static void draw_coarse_block_smalltri(framebuffer_t* fb, int32_t tile_id, int32_t coarse_topleft_x, int32_t coarse_topleft_y, const tilecmd_drawsmalltri_t* drawcmd)
{
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
				fb->backbuffer[dst_i] = g_Color;
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
}

static void draw_tile_smalltri(framebuffer_t* fb, int32_t tile_id, const tilecmd_drawsmalltri_t* drawcmd)
{
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
			draw_coarse_block_smalltri(fb, tile_id, coarse_topleft_x, coarse_topleft_y, &cbargs);

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
}

static void draw_coarse_block_largetri(framebuffer_t* fb, int32_t tile_id, int32_t coarse_topleft_x, int32_t coarse_topleft_y, const tilecmd_drawtile_t* drawcmd)
{
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
				fb->backbuffer[dst_i] = g_Color;
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
}

static void draw_tile_largetri(framebuffer_t* fb, int32_t tile_id, const tilecmd_drawtile_t* drawcmd)
{
    int32_t num_test_edges = drawcmd->tilecmd_id - tilecmd_id_drawtile_0edge;

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
		for (int32_t v = 0; v < 3; v++)
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
				tilecmd_drawtile_t drawtilecmd;

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

				for (int32_t v = 0; v < 3; v++)
				{
					int32_t rotated_v = (v + vertex_rotation) % 3;

					drawtilecmd.verts[v] = drawcmd->verts[rotated_v];
					drawtilecmd.edges[v] = (int32_t)row_edges[rotated_v];
					drawtilecmd.edge_dxs[v] = (int32_t)drawcmd->edge_dxs[rotated_v];
					drawtilecmd.edge_dys[v] = (int32_t)drawcmd->edge_dys[rotated_v];
				}

				int32_t coarse_topleft_x = tile_x * TILE_WIDTH_IN_PIXELS + cb_x * COARSE_BLOCK_WIDTH_IN_PIXELS;
				int32_t coarse_topleft_y = tile_y * TILE_WIDTH_IN_PIXELS + cb_y * COARSE_BLOCK_WIDTH_IN_PIXELS;
                draw_coarse_block_largetri(fb, tile_id, coarse_topleft_x, coarse_topleft_y, &drawtilecmd);
            }

			for (int32_t v = 0; v < 3; v++)
			{
				row_edges[v] += coarse_edge_dxs[v];
			}

            for (int32_t v = 0; v < num_test_edges; v++)
            {
				edge_row_trivRejs[v] += coarse_edge_dxs[v];
				edge_row_trivAccs[v] += coarse_edge_dxs[v];
            }
        }

		for (int32_t v = 0; v < 3; v++)
		{
			edges[v] += coarse_edge_dys[v];
		}

        for (int32_t v = 0; v < num_test_edges; v++)
        {
            edge_trivRejs[v] += coarse_edge_dys[v];
            edge_trivAccs[v] += coarse_edge_dys[v];
        }
    }
}

static void clear_tile(framebuffer_t* fb, int32_t tile_id, tilecmd_cleartile_t* cmd)
{
    int32_t tile_start_i = PIXELS_PER_TILE * tile_id;
    int32_t tile_end_i = tile_start_i + PIXELS_PER_TILE;
    uint32_t color = cmd->color;
    for (int32_t px = tile_start_i; px < tile_end_i; px++)
    {
        fb->backbuffer[px] = color;
    }
}

static void framebuffer_resolve_tile(framebuffer_t* fb, int32_t tile_id)
{
    tile_cmdbuf_t* cmdbuf = &fb->tile_cmdbufs[tile_id];
    
    uint32_t* cmd;
    for (cmd = cmdbuf->cmdbuf_read; cmd != cmdbuf->cmdbuf_write; )
    {
        uint32_t tilecmd_id = *cmd;
        if (tilecmd_id == tilecmd_id_resetbuf)
        {
            cmd = cmdbuf->cmdbuf_start;
        }
        else if (tilecmd_id == tilecmd_id_drawsmalltri)
        {
            draw_tile_smalltri(fb, tile_id, (tilecmd_drawsmalltri_t*)cmd);
            cmd += sizeof(tilecmd_drawsmalltri_t) / sizeof(uint32_t);
        }
        else if (tilecmd_id >= tilecmd_id_drawtile_0edge && tilecmd_id <= tilecmd_id_drawtile_3edge)
        {           
            draw_tile_largetri(fb, tile_id, (tilecmd_drawtile_t*)cmd);
            cmd += sizeof(tilecmd_drawtile_t) / sizeof(uint32_t);
        }
        else if (tilecmd_id == tilecmd_id_cleartile)
        {
            clear_tile(fb, tile_id, (tilecmd_cleartile_t*)cmd);
            cmd += sizeof(tilecmd_cleartile_t) / sizeof(uint32_t);
        }
        else
        {
            assert(!"Unknown tile command");
        }

        if (cmd == cmdbuf->cmdbuf_end)
        {
            cmd = cmdbuf->cmdbuf_start;
        }
    }

    cmdbuf->cmdbuf_read = cmd;
}

static void framebuffer_push_tilecmd(framebuffer_t* fb, int32_t tile_id, const uint32_t* cmd_dwords, int32_t num_dwords)
{
    assert(tile_id < fb->total_num_tiles);

    tile_cmdbuf_t* cmdbuf = &fb->tile_cmdbufs[tile_id];
    
    if (cmdbuf->cmdbuf_read - cmdbuf->cmdbuf_write > 0 && cmdbuf->cmdbuf_read - cmdbuf->cmdbuf_write < num_dwords)
    {
        // read ptr is after write ptr and there's not enough room in between
        // therefore, need to flush
        framebuffer_resolve_tile(fb, tile_id);
    }

    if (cmdbuf->cmdbuf_end - cmdbuf->cmdbuf_write < num_dwords)
    {
        // not enough room in the buffer to write the commands, need to loop around

        // should never be at the end of the buffer since it always loops at the end of this function
        assert(cmdbuf->cmdbuf_write != cmdbuf->cmdbuf_end);

        // abandon the rest of the slop at the end of the buffer
        *cmdbuf->cmdbuf_write = tilecmd_id_resetbuf;
        cmdbuf->cmdbuf_write = cmdbuf->cmdbuf_start;

        if (cmdbuf->cmdbuf_read - cmdbuf->cmdbuf_write > 0 && cmdbuf->cmdbuf_read - cmdbuf->cmdbuf_write < num_dwords)
        {
            // read ptr is after write ptr and the write ptr can't pass the read ptr
            // therefore, need to flush
            framebuffer_resolve_tile(fb, tile_id);
        }
    }

    // finally actually write the command
    for (int32_t i = 0; i < num_dwords; i++)
    {
        cmdbuf->cmdbuf_write[i] = cmd_dwords[i];
    }
    cmdbuf->cmdbuf_write += num_dwords;

    // loop around the buffer if necessary
    if (cmdbuf->cmdbuf_write == cmdbuf->cmdbuf_end)
    {
        cmdbuf->cmdbuf_write = cmdbuf->cmdbuf_start;
    }
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

void framebuffer_pack_row_major(framebuffer_t* fb, int32_t x, int32_t y, int32_t width, int32_t height, pixelformat_t format, void* data)
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
                    int32_t src = fb->backbuffer[src_i];
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
    xyzw_i32_t verts[3];
    int32_t rcp_ws[3];
    for (int32_t v = 0; v < 3; v++)
    {
        // currently not handling near plane clipping
        assert(clipVerts[v].w > 0);

        int32_t one_over_w = s1516_div(1 << 16, clipVerts[v].w);

        // convert s15.16 (in clip space) to q16.8 window coordinates
        // TODO: review how rounding (or lack thereof) affects this operation
        verts[v].x = ((s1516_mul(clipVerts[v].x, one_over_w) + (1 << 16)) / 2 * fb->width_in_pixels) >> 8;
        verts[v].y = ((s1516_mul(-clipVerts[v].y, one_over_w) + (1 << 16)) / 2 * fb->height_in_pixels) >> 8;

        // TODO: clip things that all outside the guard band

        verts[v].z = s1516_mul(clipVerts[v].z, one_over_w);
        verts[v].w = clipVerts[v].w;
        rcp_ws[v] = one_over_w;
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
        return;
    }

    // clip bbox to scissor rect
    if (bbox_min_x < 0) bbox_min_x = 0;
    if (bbox_min_y < 0) bbox_min_y = 0;
    if (bbox_max_x >= (int32_t)(fb->width_in_pixels << 8)) bbox_max_x = ((int32_t)fb->width_in_pixels << 8) - 1;
    if (bbox_max_y >= (int32_t)(fb->height_in_pixels << 8)) bbox_max_y = ((int32_t)fb->height_in_pixels << 8) - 1;

    // "small" triangles are no wider than a tile.
    int32_t is_large =
        (bbox_max_x - bbox_min_x) >= (TILE_WIDTH_IN_PIXELS << 8) ||
        (bbox_max_y - bbox_min_y) >= (TILE_WIDTH_IN_PIXELS << 8);

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

        drawsmalltricmd.verts[0] = verts[0];
        drawsmalltricmd.verts[1] = verts[1];
        drawsmalltricmd.verts[2] = verts[2];

		// make vertices relative to the last tile they're in
		for (int32_t v = 0; v < 3; v++)
		{
			drawsmalltricmd.verts[v].x -= last_tile_px_x;
			drawsmalltricmd.verts[v].y -= last_tile_px_y;
		}

        int32_t triarea2 = (verts[1].x - verts[0].x) * (verts[2].y - verts[0].y) - (verts[1].y - verts[0].y) * (verts[2].x - verts[0].x);
        if (triarea2 <= 0)
        {
            goto skiptri;
        }

        int32_t rcp_triarea2 = s1516_div(1 << 16, triarea2);

        // TODO: Find a better way to interpolate barycentrics that doesn't result in 0 for 1/triarea2?
        if (rcp_triarea2 == 0)
            rcp_triarea2 = 1;

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

            // convert to s15.16 and divide by 2*TriArea
            edge_dxs[v] = s1516_mul(edge_dxs[v] << 8, rcp_triarea2);
            edge_dys[v] = s1516_mul(edge_dys[v] << 8, rcp_triarea2);

            // compute edge equation
            // |  x  y  z |
            // | ax ay  0 |
            // | bx by  0 |
            // = ax*by - ay*bx
            // eg: a = (px-v0), b = (v1-v0)
            // note: reusing edge_dxs and edge_dys to incorporate the 1/(2TriArea) term
            // note: evaluated at px = (0,0) because the vertices are relative to the last tile
            edges[v] = s1516_mul((0 - drawsmalltricmd.verts[v].x) << 8, edge_dxs[v]) - s1516_mul((0 - drawsmalltricmd.verts[v].y) << 8, -edge_dys[v]);

            // Top-left rule: shift top-left edges ever so slightly outward to make the top-left edges be the tie-breakers when rasterizing adjacent triangles
            if ((verts[v].y == verts[v1].y && verts[v].x < verts[v1].x) || verts[v].y > verts[v1].y) edges[v]--;
        }
		
		for (int32_t v = 0; v < 3; v++)
		{
			drawsmalltricmd.edge_dxs[v] = edge_dxs[v];
			drawsmalltricmd.edge_dys[v] = edge_dys[v];
		}

        // draw top left tile
        int32_t first_tile_id = first_tile_y * fb->width_in_tiles + first_tile_x;
        {
            for (int32_t v = 0; v < 3; v++)
            {
                drawsmalltricmd.edges[v] = edges[v] + (
                    edge_dxs[v] * (first_tile_x - last_tile_x) +
                    edge_dys[v] * (first_tile_y - last_tile_y)) * TILE_WIDTH_IN_PIXELS;
            }

            drawsmalltricmd.first_coarse_x = first_rel_cb_x;
            drawsmalltricmd.last_coarse_x = last_rel_cb_x;
            if (drawsmalltricmd.last_coarse_x >= TILE_WIDTH_IN_COARSE_BLOCKS)
            {
                drawsmalltricmd.last_coarse_x = TILE_WIDTH_IN_COARSE_BLOCKS - 1;
            }
            drawsmalltricmd.first_coarse_y = first_rel_cb_y;
            drawsmalltricmd.last_coarse_y = last_rel_cb_y;
            if (drawsmalltricmd.last_coarse_y >= TILE_WIDTH_IN_COARSE_BLOCKS)
            {
                drawsmalltricmd.last_coarse_y = TILE_WIDTH_IN_COARSE_BLOCKS - 1;
            }

            framebuffer_push_tilecmd(fb, first_tile_id, &drawsmalltricmd.tilecmd_id, sizeof(drawsmalltricmd) / sizeof(uint32_t));
        }

        // draw top right tile
        if (last_tile_x > first_tile_x)
        {
			for (int32_t v = 0; v < 3; v++)
			{
                drawsmalltricmd.edges[v] = edges[v] + edge_dys[v] * (first_tile_y - last_tile_y) * TILE_WIDTH_IN_PIXELS;
			}

            drawsmalltricmd.first_coarse_x = 0;
            drawsmalltricmd.last_coarse_x = last_rel_cb_x - TILE_WIDTH_IN_COARSE_BLOCKS;
            drawsmalltricmd.first_coarse_y = first_rel_cb_y;
            drawsmalltricmd.last_coarse_y = last_rel_cb_y;
            if (drawsmalltricmd.last_coarse_y >= TILE_WIDTH_IN_COARSE_BLOCKS)
            {
                drawsmalltricmd.last_coarse_y = TILE_WIDTH_IN_COARSE_BLOCKS - 1;
            }

			int32_t tile_id_right = first_tile_id + 1;
            framebuffer_push_tilecmd(fb, tile_id_right, &drawsmalltricmd.tilecmd_id, sizeof(drawsmalltricmd) / sizeof(uint32_t));
        }

        // draw bottom left tile
        if (last_tile_y > first_tile_y)
        {
			for (int32_t v = 0; v < 3; v++)
			{
                drawsmalltricmd.edges[v] = edges[v] + edge_dxs[v] * (first_tile_x - last_tile_x) * TILE_WIDTH_IN_PIXELS;
			}

            drawsmalltricmd.first_coarse_x = first_rel_cb_x;
            drawsmalltricmd.last_coarse_x = last_rel_cb_x;
            if (drawsmalltricmd.last_coarse_x >= TILE_WIDTH_IN_COARSE_BLOCKS)
            {
                drawsmalltricmd.last_coarse_x = TILE_WIDTH_IN_COARSE_BLOCKS - 1;
            }
            drawsmalltricmd.first_coarse_y = 0;
            drawsmalltricmd.last_coarse_y = last_rel_cb_y - TILE_WIDTH_IN_COARSE_BLOCKS;

			int32_t tile_id_down = first_tile_id + fb->width_in_tiles;
            framebuffer_push_tilecmd(fb, tile_id_down, &drawsmalltricmd.tilecmd_id, sizeof(drawsmalltricmd) / sizeof(uint32_t));
        }

        // draw bottom right tile
        if (last_tile_x > first_tile_x && last_tile_y > first_tile_y)
        {
			for (int32_t v = 0; v < 3; v++)
			{
                drawsmalltricmd.edges[v] = edges[v];
			}

            drawsmalltricmd.first_coarse_x = 0;
            drawsmalltricmd.last_coarse_y = last_rel_cb_x - TILE_WIDTH_IN_COARSE_BLOCKS;
            drawsmalltricmd.first_coarse_y = 0;
            drawsmalltricmd.last_coarse_y = last_rel_cb_y - TILE_WIDTH_IN_COARSE_BLOCKS;

			int32_t tile_id_downright = first_tile_id + 1 + fb->width_in_tiles;
            framebuffer_push_tilecmd(fb, tile_id_downright, &drawsmalltricmd.tilecmd_id, sizeof(drawsmalltricmd) / sizeof(uint32_t));
        }
    }
    else
    {
        // for large triangles, test each tile in their bbox for overlap
        // done using scalar code for simplicity, since rasterization dominates large triangle performance anyways.
        int32_t first_tile_x = (bbox_min_x >> 8) / TILE_WIDTH_IN_PIXELS;
        int32_t first_tile_y = (bbox_min_y >> 8) / TILE_WIDTH_IN_PIXELS;
        int32_t last_tile_x = (bbox_max_x >> 8) / TILE_WIDTH_IN_PIXELS;
        int32_t last_tile_y = (bbox_max_y >> 8) / TILE_WIDTH_IN_PIXELS;

        // evaluate edge equation at the top left tile
        int32_t first_tile_px_x = (first_tile_x << 8) * TILE_WIDTH_IN_PIXELS;
        int32_t first_tile_px_y = (first_tile_y << 8) * TILE_WIDTH_IN_PIXELS;

        // 64 bit integers are used for the edge equations here because multiplying two 16.8 numbers requires up to 48 bits
        // this results in some extra overhead, but it's not a big deal when you consider that this happens only for large triangles.
        // The tens of thousands of pixels that large triangles generate outweigh the cost of slightly more expensive setup.

        int64_t triarea2 = ((int64_t)verts[1].x - verts[0].x) * ((int64_t)verts[2].y - verts[0].y) - ((int64_t)verts[1].y - verts[0].y) * ((int64_t)verts[2].x - verts[0].x);
        if (triarea2 <= 0)
        {
            goto skiptri;
        }

        // pre-multiply by the base
        int64_t rcp_triarea2 = (int64_t)1 << 32;
        // Rounding: mid values are rounded up (down for negative values)
        if ((rcp_triarea2 >= 0 && triarea2 >= 0) || (rcp_triarea2 < 0 && triarea2 < 0))
            rcp_triarea2 += triarea2 / 2;
        else
            rcp_triarea2 -= triarea2 / 2;
        rcp_triarea2 = rcp_triarea2 / triarea2;

        // TODO: Find a better way to interpolate barycentrics that doesn't result in 0 for 1/triarea2?
        if (rcp_triarea2 == 0)
            rcp_triarea2 = 1;

        int64_t edges[3];
        int64_t edge_dxs[3], edge_dys[3];
        for (int32_t v = 0; v < 3; v++)
        {
            int32_t v1 = (v + 1) % 3;
            int32_t v2 = (v + 2) % 3;

            // find how the edge equation varies along x and y
            edge_dxs[v] = verts[v1].y - verts[v].y;
            edge_dys[v] = verts[v].x - verts[v1].x;

            // convert to 16 base fixed point and divide by 2*TriArea
            edge_dxs[v] = (edge_dxs[v] << 16) * rcp_triarea2;
            edge_dys[v] = (edge_dys[v] << 16) * rcp_triarea2;
            
            // Rounding: mid values are rounded up and Correct by dividing by base
            edge_dxs[v] = (edge_dxs[v] + (1 << 15)) >> 16;
            edge_dys[v] = (edge_dys[v] + (1 << 15)) >> 16;

            // compute edge equation
            // |  x  y  z |
            // | ax ay  0 |
            // | bx by  0 |
            // = ax*by - ay*bx
            // eg: a = (px-v0), b = (v1-v0)
            // note: reusing edge_dxs and edge_dys to incorporate the 1/(2TriArea) term
            edges[v] = (((((int64_t)first_tile_px_x - verts[v].x) << 8) * edge_dxs[v] + (1 << 15)) >> 16) - (((((int64_t)first_tile_px_y - verts[v].y) << 8) * -edge_dys[v] + (1 << 15)) >> 16);

            // Top-left rule: shift top-left edges ever so slightly outward to make the top-left edges be the tie-breakers when rasterizing adjacent triangles
            if ((verts[v].y == verts[v1].y && verts[v].x < verts[v1].x) || verts[v].y > verts[v1].y) edges[v]--;
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

        int32_t tile_row_start = first_tile_y * fb->width_in_tiles;
        for (int32_t tile_y = first_tile_y; tile_y <= last_tile_y; tile_y++)
        {
            int32_t tile_i = tile_row_start + first_tile_x;

            int64_t tile_i_edges[3];
            int64_t tile_i_edge_trivRejs[3];
            int64_t tile_i_edge_trivAccs[3];
            for (int32_t v = 0; v < 3; v++)
            {
                tile_i_edges[v] = edges[v];
                tile_i_edge_trivRejs[v] = edge_trivRejs[v];
                tile_i_edge_trivAccs[v] = edge_trivAccs[v];
            }

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

                        drawtilecmd.verts[v] = verts[rotated_v];

                        // there's something unsound about the way 64 bit edge equations are being passed down as 32 bit
                        // it makes sense that there shouldn't be loss of precision for nearby edges (the edges you need to test against)
                        // however the edge equations that aren't tested (because they are trivially accepted) won't fit in 32 bits
                        // how is that supposed to be dealt with?
                        // in the meantime, just check that at least the checked edges are within range.
                        if (v < num_tests_necessary)
                        {
							assert(tile_i_edges[rotated_v] == (int64_t)((int32_t)tile_i_edges[rotated_v]));
							assert(edge_dxs[rotated_v] == (int64_t)((int32_t)edge_dxs[rotated_v]));
							assert(edge_dys[rotated_v] == (int64_t)((int32_t)edge_dys[rotated_v]));
                        }

                        drawtilecmd.edges[v] = (int32_t)tile_i_edges[rotated_v];
                        drawtilecmd.edge_dxs[v] = (int32_t)edge_dxs[rotated_v];
                        drawtilecmd.edge_dys[v] = (int32_t)edge_dys[rotated_v];
                    }

                    framebuffer_push_tilecmd(fb, tile_i, &drawtilecmd.tilecmd_id, sizeof(drawtilecmd) / sizeof(uint32_t));
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

skiptri:;
} 

void rasterizer_draw(
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

void rasterizer_draw_indexed(
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

void rasterizer_set_color(uint32_t col)
{
    g_Color = col;
}

#ifdef RASTERIZER_UNIT_TESTS

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

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
        int32_t w = TILE_WIDTH_IN_PIXELS * 2;
        int32_t h = TILE_WIDTH_IN_PIXELS * 2;

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
        for (int32_t i = 0; i < fb->pixels_per_slice; i++)
        {
            fb->backbuffer[i] = i;
        }

        framebuffer_pack_row_major(fb, 0, 0, w, h, pixelformat_r8g8b8a8_unorm, rowmajor_data);

        for (int32_t y = 0; y < h; y++)
        {
            int32_t tile_y = y / TILE_WIDTH_IN_PIXELS;

            for (int32_t x = 0; x < w; x++)
            {
                int32_t tile_x = x / TILE_WIDTH_IN_PIXELS;
                int32_t tile_i = tile_y * fb->width_in_tiles + tile_x;
                int32_t topleft_pixel_i = tile_i * PIXELS_PER_TILE;

                int32_t tile_relative_x = x - tile_x * TILE_WIDTH_IN_PIXELS;
                int32_t tile_relative_y = y - tile_y * TILE_WIDTH_IN_PIXELS;
                int32_t rowmajor_i = y * w + x;

                int32_t xmask = TILE_X_SWIZZLE_MASK;
                int32_t ymask = TILE_Y_SWIZZLE_MASK;
                int32_t xbits = pdep_u32(x, xmask);
                int32_t ybits = pdep_u32(y, ymask);
                int32_t swizzled_i = topleft_pixel_i + xbits + ybits;

                assert(rowmajor_data[rowmajor_i * 4 + 0] == ((fb->backbuffer[swizzled_i] & 0x00FF0000) >> 16));
                assert(rowmajor_data[rowmajor_i * 4 + 1] == ((fb->backbuffer[swizzled_i] & 0x0000FF00) >> 8));
                assert(rowmajor_data[rowmajor_i * 4 + 2] == ((fb->backbuffer[swizzled_i] & 0x000000FF) >> 0));
                assert(rowmajor_data[rowmajor_i * 4 + 3] == ((fb->backbuffer[swizzled_i] & 0xFF000000) >> 24));
            }
        }

        // do the test again but this time readback just one tile (that isn't the top left one)
        framebuffer_pack_row_major(fb, TILE_WIDTH_IN_PIXELS, TILE_WIDTH_IN_PIXELS, TILE_WIDTH_IN_PIXELS, TILE_WIDTH_IN_PIXELS, pixelformat_r8g8b8a8_unorm, rowmajor_data);

        for (int32_t rel_y = 0; rel_y < TILE_WIDTH_IN_PIXELS; rel_y++)
        {
            int32_t y = TILE_WIDTH_IN_PIXELS + rel_y;

            int32_t tile_y = y / TILE_WIDTH_IN_PIXELS;

            for (int32_t rel_x = 0; rel_x < TILE_WIDTH_IN_PIXELS; rel_x++)
            {
                int32_t x = TILE_WIDTH_IN_PIXELS + rel_x;

                int32_t tile_x = x / TILE_WIDTH_IN_PIXELS;
                int32_t tile_i = tile_y * (fb->pixels_per_row_of_tiles / PIXELS_PER_TILE) + tile_x;
                int32_t topleft_pixel_i = tile_i * PIXELS_PER_TILE;

                int32_t tile_relative_x = x - tile_x * TILE_WIDTH_IN_PIXELS;
                int32_t tile_relative_y = y - tile_y * TILE_WIDTH_IN_PIXELS;
                int32_t rowmajor_i = tile_relative_y * TILE_WIDTH_IN_PIXELS + tile_relative_x;

                int32_t xmask = TILE_X_SWIZZLE_MASK;
                int32_t ymask = TILE_Y_SWIZZLE_MASK;
                int32_t xbits = pdep_u32(x, xmask);
                int32_t ybits = pdep_u32(y, ymask);
                int32_t swizzled_i = topleft_pixel_i + xbits + ybits;

                assert(rowmajor_data[rowmajor_i * 4 + 0] == ((fb->backbuffer[swizzled_i] & 0x00FF0000) >> 16));
                assert(rowmajor_data[rowmajor_i * 4 + 1] == ((fb->backbuffer[swizzled_i] & 0x0000FF00) >> 8));
                assert(rowmajor_data[rowmajor_i * 4 + 2] == ((fb->backbuffer[swizzled_i] & 0x000000FF) >> 0));
                assert(rowmajor_data[rowmajor_i * 4 + 3] == ((fb->backbuffer[swizzled_i] & 0xFF000000) >> 24));
            }
        }

        free(rowmajor_data);
        delete_framebuffer(fb);
    }

	// large triangle test
	{
		int32_t fbwidth = TILE_WIDTH_IN_PIXELS * 3;
		int32_t fbheight = TILE_WIDTH_IN_PIXELS * 3;
		framebuffer_t* fb = new_framebuffer(fbwidth, fbheight);

        int32_t radius = s1516_div(2 << 16, 2 << 16);

		int32_t verts[] = {
            (-1 << 16), (1 << 16), 0, 1 << 16,
            (-1 << 16) + radius, (1 << 16), 0, 1 << 16,
            (-1 << 16), (1 << 16) - radius, 0, 1 << 16
		};

        rasterizer_set_color(0xFFFF00FF);
		rasterizer_draw(fb, verts, 3);

		// make sure all caches are flushed and yada yada
		framebuffer_resolve(fb);

		// convert framebuffer from bgra to rgba for stbi_image_write
		{
			uint8_t* rgba8_pixels = (uint8_t*)malloc(fbwidth * fbheight * 4);
			assert(rgba8_pixels);

			// readback framebuffer contents
			framebuffer_pack_row_major(fb, 0, 0, fbwidth, fbheight, pixelformat_r8g8b8a8_unorm, rgba8_pixels);

			if (!stbi_write_png("output.png", fbwidth, fbheight, 4, rgba8_pixels, fbwidth * 4))
			{
				fprintf(stderr, "Failed to write image\n");
				exit(1);
			}

			free(rgba8_pixels);
		}

		system("output.png");

		delete_framebuffer(fb);
	}

	// small triangles test
	{
		int32_t fbwidth = TILE_WIDTH_IN_PIXELS * 2;
		int32_t fbheight = TILE_WIDTH_IN_PIXELS * 2;
		framebuffer_t* fb = new_framebuffer(fbwidth, fbheight);

        int32_t radius = s1516_div(2 << 16, 4 << 16);
        int32_t half_radius = s1516_div(radius, 2 << 16);

		int32_t verts[] = {
			// triangle at top left of framebuffer
            (-1 << 16), (1 << 16), 0, 1 << 16,
            (-1 << 16) + radius, (1 << 16), 0, 1 << 16,
            (-1 << 16), (1 << 16) - radius, 0, 1 << 16,

			// triangle in between the first two tiles
            (0 << 16) - half_radius, (1 << 16), 0, 1 << 16,
            (0 << 16) + half_radius, (1 << 16), 0, 1 << 16,
            (0 << 16) - half_radius, (1 << 16) - radius, 0, 1 << 16,

			// triangle in between the first four tiles
            (0 << 16) - half_radius, (0 << 16) + half_radius, 0, 1 << 16,
            (0 << 16) + half_radius, (0 << 16) + half_radius, 0, 1 << 16,
            (0 << 16) - half_radius, (0 << 16) - half_radius, 0, 1 << 16,
		};

        rasterizer_set_color(0xFFFF00FF);
		rasterizer_draw(fb, verts, sizeof(verts)/sizeof(*verts)/4);

		// make sure all caches are flushed and yada yada
		framebuffer_resolve(fb);

		// convert framebuffer from bgra to rgba for stbi_image_write
		{
			uint8_t* rgba8_pixels = (uint8_t*)malloc(fbwidth * fbheight * 4);
			assert(rgba8_pixels);

			// readback framebuffer contents
			framebuffer_pack_row_major(fb, 0, 0, fbwidth, fbheight, pixelformat_r8g8b8a8_unorm, rgba8_pixels);

			if (!stbi_write_png("output.png", fbwidth, fbheight, 4, rgba8_pixels, fbwidth * 4))
			{
				fprintf(stderr, "Failed to write image\n");
				exit(1);
			}

			free(rgba8_pixels);
		}

		system("output.png");

		delete_framebuffer(fb);
	}
}
#endif