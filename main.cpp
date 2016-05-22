#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

uint32_t g_Color;

// Rasterizes a triangle with its vertices represented as 16.8 fixed point values
// Arguments:
// * fb: The framebuffer the triangle is written to. Pixels are assumed in BGRA format
// * fb_width: The width in pixels of the framebuffer
// * xi, yi (i in [0..2]): coordinate of vertex i of the triangle, encoded as 16.8 fixed point.
// * zi (i in [0..2]): depth of the vertex i of the triangle.
// Preconditions:
// * The triangle has been clipped to the viewport (to be lifted later)
// * The triangle vertices are stored clockwise (relative to their position on the display)
// * The triangle is not degenerate (haven't verified if these are handled correctly yet)
void rasterize_triangle_fixed16_8(
    uint32_t* fb, uint32_t fb_width,
    uint32_t x0, uint32_t y0, uint32_t z0,
    uint32_t x1, uint32_t y1, uint32_t z1,
    uint32_t x2, uint32_t y2, uint32_t z2)
{
    // This routine implements a Pineda-style softawre rasterizer.
    // See "A Parallel Algorithm for Polygon Rasterization", by Juan Pineda, SIGGRAPH '88:
    // http://citeseerx.ist.psu.edu/viewdoc/download?doi=10.1.1.157.4621&rep=rep1&type=pdf
    // For a modern take on this algorithm, see Fabian Giesen's GPU pipeline and Software Occlusion Culling blog series:
    // https://fgiesen.wordpress.com/2011/07/09/a-trip-through-the-graphics-pipeline-2011-index/
    // https://fgiesen.wordpress.com/2013/02/17/optimizing-sw-occlusion-culling-index/
    // Another modern take is Michael Abrash's Dr. Dobb's article "Rasterization on Larrabee":
    // http://www.drdobbs.com/parallel/rasterization-on-larrabee/217200602

    // The major phases of this rasterizer are as follows:
    // 1. Coarse Rasterization:
    //    * The screen is split into coarse tiles, and tiles covered by the triangle are identified.
    //    * Each tile passing this test is passed on to fine rasterization.
    // 2. Fine Rasterization:
    //    * Each pixel of the current coarse tile is checked for triangle coverage.
    //    * If the pixel is covered, it is shaded.

    // Window coordinates' bounding box.
    // Every pixel in this bounding box has a chance to be rasterizerized.
    uint32_t x_min = x0, x_max = x0;
    if (x1 < x_min) x_min = x1;
    if (x2 < x_min) x_min = x2;
    if (x1 > x_max) x_max = x1;
    if (x2 > x_max) x_max = x2;

    uint32_t y_min = y0, y_max = y0;
    if (y1 < y_min) y_min = y1;
    if (y2 < y_min) y_min = y2;
    if (y1 > y_max) y_max = y1;
    if (y2 > y_max) y_max = y2;

    // Coarse Rasterization Phase
    // --------------------------
    // This phase splits the framebuffer into 256x256 tiles.
    //
    // Coarse rasterization is important for precision in the multiplication done during barycentrics computation:
    // When multiplying two I.Q fixed-point numbers, the result can have up to 2I + 2Q bits.
    // Thus, with 32 bit integers, we can multiply up to 8.8 fixed point numbers. (2*8 + 2*8 = 32)
    // This means the integer part of inputs to barycentric coordinate computation can only be up to 256, 
    // which means that computing barycentrics fails when rasterizing triangles larger than 256 pixels in x or y.
    // By splitting the framebuffer into 256x256 tiles, we can treat triangles relative to the current tile,
    // which means we can safely do a multiplication by two 8.8 numbers without overflow, since tiles are 256x256.
    //
    // Furthermore, coarse rasterization allows us to cull big empty tiles early in the rasterization process.
    //
    // By dividing 16.8 pixel coordinates by the tile width, we get an 8.8 value.
    // The integer part of this 8.8 value is the tile coordinate,
    // and the fractional part is the pixel coordinate relative to the top left corner of the tile.
    // We lose sub-pixel precision in this representation, but that's fine as long as the tile rasterization is conservative w.r.t sub-pixels.
    // In general, these 8.8 (tile.pixel) coordinates are denoted by variables starting with "coarse_".
    //
    // For the "coarse" barycentrics not to overflow, there need be fewer than 256 coarse tiles in the viewport.
    // That means the viewport is limited to a size of 2^16, which is reasonble (maybe will become a problem depending on how multisampling is done but ok otherwise?)
    // --------------------------

    // Coarse (tile.pixel) x/y bounding box of the coarse tiles covered by the triangle
    uint32_t coarse_x_min = x_min >> 8;
    uint32_t coarse_y_min = y_min >> 8;
    uint32_t coarse_x_max = x_max >> 8;
    uint32_t coarse_y_max = y_max >> 8;

    // Coarse (tile.pixel) coordinates of the vertices
    uint32_t coarse_x0 = x0 >> 8, coarse_y0 = y0 >> 8;
    uint32_t coarse_x1 = x1 >> 8, coarse_y1 = y1 >> 8;
    uint32_t coarse_x2 = x2 >> 8, coarse_y2 = y2 >> 8;

    // Coarse barycentric coordinates of coarse top left tile
    int32_t coarse_min_bary0 = ((coarse_x2 - coarse_x1) * (coarse_y_min - coarse_y1) - (coarse_y2 - coarse_y1) * (coarse_x_min - coarse_x1)) >> 8;
    int32_t coarse_min_bary1 = ((coarse_x0 - coarse_x2) * (coarse_y_min - coarse_y2) - (coarse_y0 - coarse_y2) * (coarse_x_min - coarse_x2)) >> 8;
    int32_t coarse_min_bary2 = ((coarse_x1 - coarse_x0) * (coarse_y_min - coarse_y0) - (coarse_y1 - coarse_y0) * (coarse_x_min - coarse_x0)) >> 8;

    // Loop over every coarse tile (row major traversal)
    for (uint32_t coarse_y_px = coarse_y_min; coarse_y_px <= coarse_y_max; coarse_y_px++)
    {
        for (uint32_t coarse_x_px = coarse_x_min; coarse_x_px <= coarse_x_max; coarse_x_px++)
        {
            // Fine Rasterization Phase
            // ------------------------

            // barycentric coordinates of top left corner coordinate
            int32_t min_bary0 = ((x2 - x1) * (y_min - y1) - (y2 - y1) * (x_min - x1)) >> 8;
            int32_t min_bary1 = ((x0 - x2) * (y_min - y2) - (y0 - y2) * (x_min - x2)) >> 8;
            int32_t min_bary2 = ((x1 - x0) * (y_min - y0) - (y1 - y0) * (x_min - x0)) >> 8;

            // offset barycentrics of non top-left edges, which ever so slightly removes them from the rasterization
            if ((y1 != y2 || x1 > x2) && (y1 < y2)) min_bary0 -= 1;
            if ((y2 != y0 || x2 > x0) && (y2 < y0)) min_bary1 -= 1;
            if ((y0 != y1 || x0 > x1) && (y0 < y1)) min_bary2 -= 1;

            // derivative of barycentric coordinates in x and y
            int32_t dbary0dx = y1 - y2;
            int32_t dbary0dy = x2 - x1;
            int32_t dbary1dx = y2 - y0;
            int32_t dbary1dy = x0 - x2;
            int32_t dbary2dx = y0 - y1;
            int32_t dbary2dy = x1 - x0;

            // minimum/maximum pixel center coordinates (center sample locations)
            uint32_t x_min_center = (x_min & 0xFFFFFF00) | 0x7F;
            uint32_t x_max_center = (x_max & 0xFFFFFF00) | 0x7F;
            uint32_t y_min_center = (y_min & 0xFFFFFF00) | 0x7F;
            uint32_t y_max_center = (y_max & 0xFFFFFF00) | 0x7F;

            // these variables move forward with every y
            int32_t curr_bary0_row = min_bary0;
            int32_t curr_bary1_row = min_bary1;
            int32_t curr_bary2_row = min_bary2;
            uint32_t* curr_px_row = fb;

            // fill pixels inside the window coordinate bounding box that are inside all three edges' half-planes
            for (uint32_t y_px = y_min_center; y_px <= y_max_center; y_px += 0x100)
            {
                // these variables move forward with every x
                int32_t curr_bary0 = curr_bary0_row;
                int32_t curr_bary1 = curr_bary1_row;
                int32_t curr_bary2 = curr_bary2_row;
                uint32_t* curr_px = curr_px_row;

                // fill pixels in this row
                for (uint32_t x_px = x_min_center; x_px <= x_max_center; x_px += 0x100)
                {
                    if (curr_bary0 >= 0 && curr_bary1 >= 0 && curr_bary2 >= 0)
                    {
                        *curr_px = g_Color;
                    }

                    // move to next pixel in x direction
                    curr_bary0 += dbary0dx;
                    curr_bary1 += dbary1dx;
                    curr_bary2 += dbary2dx;
                    curr_px += 1;
                }

                // move to the start of the next row
                curr_bary0_row += dbary0dy;
                curr_bary1_row += dbary1dy;
                curr_bary2_row += dbary2dy;
                curr_px_row += fb_width;
            }
        }
    }
}

int main()
{
    int fbwidth = 256;
    int fbheight = 256;

    uint32_t* fb = (uint32_t*)malloc(fbwidth * fbheight * sizeof(uint32_t));

    // fill with background color
    for (int i = 0; i < fbwidth * fbheight; i++)
    {
        fb[i] = 0x00000000;
    }

    // rasterize triangles
    g_Color = 0xFFFFFF00;
    rasterize_triangle_fixed16_8(
        fb, fbwidth,
        0 << 8, 0 << 8, 0,
        100 << 8, 100 << 8, 0,
        0 << 8, 100 << 8, 0);

    g_Color = 0xFFFF00FF;
    rasterize_triangle_fixed16_8(
        fb, fbwidth,
        0 << 8, 0 << 8, 0,
        100 << 8, 0 << 8, 0,
        100 << 8, 100 << 8, 0);

    // convert framebuffer from bgra to rgba for stbi_image_write
    for (int i = 0; i < fbwidth * fbheight; i++)
    {
        uint32_t src = fb[i];
        uint8_t* dst = (uint8_t*)fb + i * 4;
        dst[0] = (uint8_t)((src & 0x00FF0000) >> 16);
        dst[1] = (uint8_t)((src & 0x0000FF00) >> 8);
        dst[2] = (uint8_t)((src & 0x000000FF) >> 0);
        dst[3] = (uint8_t)((src & 0xFF000000) >> 24);
    }

    if (!stbi_write_png("output.png", fbwidth, fbheight, 4, fb, fbwidth * 4))
    {
        fprintf(stderr, "Failed to write image\n");
        exit(1);
    }

    system("output.png");

    free(fb);

    return 0;
}