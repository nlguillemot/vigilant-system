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
// * window_xi, window_yi (i in [0..2]): coordinate of vertex i of the triangle, encoded as 16.8 fixed point.
// * window_zi (i in [0..2]): depth of the vertex i of the triangle.
// Preconditions:
// * The triangle has been clipped to the window (to be lifted later)
// * The triangle vertices are stored clockwise (relative to their position on the display)
// * The triangle is not degenerate (haven't verified if these are handled correctly yet)
void rasterize_triangle_fixed16_8(
    uint32_t* fb, uint32_t fb_width,
    uint32_t window_x0, uint32_t window_y0, uint32_t window_z0,
    uint32_t window_x1, uint32_t window_y1, uint32_t window_z1,
    uint32_t window_x2, uint32_t window_y2, uint32_t window_z2)
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

    // Window-space bounding box
    // Every pixel in this bounding box has a chance to be rasterized.
    uint32_t window_x_min = window_x0, window_x_max = window_x0;
    if (window_x1 < window_x_min) window_x_min = window_x1;
    if (window_x2 < window_x_min) window_x_min = window_x2;
    if (window_x1 > window_x_max) window_x_max = window_x1;
    if (window_x2 > window_x_max) window_x_max = window_x2;

    uint32_t window_y_min = window_y0, window_y_max = window_y0;
    if (window_y1 < window_y_min) window_y_min = window_y1;
    if (window_y2 < window_y_min) window_y_min = window_y2;
    if (window_y1 > window_y_max) window_y_max = window_y1;
    if (window_y2 > window_y_max) window_y_max = window_y2;

    // Derivatives of window-space barycentric coordinates in x and y
    int32_t window_dbary2dy = window_x1 - window_x0;
    int32_t window_dbary0dx = window_y1 - window_y2;
    int32_t window_dbary0dy = window_x2 - window_x1;
    int32_t window_dbary1dx = window_y2 - window_y0;
    int32_t window_dbary1dy = window_x0 - window_x2;
    int32_t window_dbary2dx = window_y0 - window_y1;

    // barycentric coordinates of top left corner coordinate
    int32_t window_min_bary0 = ((window_x2 - window_x1) * (window_y_min - window_y1) - (window_y2 - window_y1) * (window_x_min - window_x1)) >> 8;
    int32_t window_min_bary1 = ((window_x0 - window_x2) * (window_y_min - window_y2) - (window_y0 - window_y2) * (window_x_min - window_x2)) >> 8;
    int32_t window_min_bary2 = ((window_x1 - window_x0) * (window_y_min - window_y0) - (window_y1 - window_y0) * (window_x_min - window_x0)) >> 8;

    // offset barycentrics of non top-left edges, which ever so slightly removes them from the rasterization
    if ((window_y1 != window_y2 || window_x1 > window_x2) && (window_y1 < window_y2)) window_min_bary0 -= 1;
    if ((window_y2 != window_y0 || window_x2 > window_x0) && (window_y2 < window_y0)) window_min_bary1 -= 1;
    if ((window_y0 != window_y1 || window_x0 > window_x1) && (window_y0 < window_y1)) window_min_bary2 -= 1;

    // minimum/maximum pixel center coordinates (center sample locations)
    uint32_t window_x_min_center = (window_x_min & 0xFFFFFF00) | 0x7F;
    uint32_t window_x_max_center = (window_x_max & 0xFFFFFF00) | 0x7F;
    uint32_t window_y_min_center = (window_y_min & 0xFFFFFF00) | 0x7F;
    uint32_t window_y_max_center = (window_y_max & 0xFFFFFF00) | 0x7F;

    // Coarse Rasterization Phase
    // --------------------------

    // Coarse x/y bounding box of the coarse tiles covered by the triangle
    uint32_t coarse_x_min = window_x_min >> 2;
    uint32_t coarse_y_min = window_y_min >> 2;
    uint32_t coarse_x_max = window_x_max >> 2;
    uint32_t coarse_y_max = window_y_max >> 2;

    // These variables move forward with every coarse y
    uint32_t* curr_coarse_row_fb_ptr = fb;

    // Loop over every coarse tile (row major traversal)
    for (uint32_t coarse_y = coarse_y_min; coarse_y <= coarse_y_max; coarse_y += 0x100)
    {
        // Coarse y loop prologue
        // ----------------------

        for (uint32_t coarse_x = coarse_x_min; coarse_x <= coarse_x_max; coarse_x += 0x100)
        {
            // Coarse x loop prologue
            // ----------------------

            uint32_t* curr_coarse_fb_ptr = curr_coarse_row_fb_ptr;

            // Fine Rasterization Phase
            // ------------------------

            // these variables move forward with every y
            int32_t window_curr_bary0_row = window_min_bary0;
            int32_t window_curr_bary1_row = window_min_bary1;
            int32_t window_curr_bary2_row = window_min_bary2;
            uint32_t* curr_px_row = curr_coarse_fb_ptr;

            // fill pixels inside the window coordinate bounding box that are inside all three edges' half-planes
            for (uint32_t window_y = coarse_y * 4; window_y < coarse_y * 4 + 0x400; window_y += 0x100)
            {
                // Fine y loop prologue
                // ----------------------

                // these variables move forward with every x
                int32_t window_curr_bary0 = window_curr_bary0_row;
                int32_t window_curr_bary1 = window_curr_bary1_row;
                int32_t window_curr_bary2 = window_curr_bary2_row;
                uint32_t* curr_px = curr_px_row;

                // fill pixels in this row
                for (uint32_t window_x = coarse_x * 4; window_x < coarse_x * 4 + 0x400; window_x += 0x100)
                {
                    // Fine x loop prologue
                    // ----------------------

                    // Check for coverage (on the right side of each edge)
                    if (window_curr_bary0 >= 0 && window_curr_bary1 >= 0 && window_curr_bary2 >= 0)
                    {
                        *curr_px = g_Color;
                    }

                    // Fine x loop epilogue
                    // ----------------------

                    // move to next pixel in x direction
                    window_curr_bary0 += window_dbary0dx;
                    window_curr_bary1 += window_dbary1dx;
                    window_curr_bary2 += window_dbary2dx;
                    curr_px += 1;
                }

                // Fine y loop epilogue
                // ----------------------

                // move to the start of the next row
                window_curr_bary0_row += window_dbary0dy;
                window_curr_bary1_row += window_dbary1dy;
                window_curr_bary2_row += window_dbary2dy;
                curr_px_row += fb_width;
            }

            // Coarse x loop epilogue
            // ----------------------

            // Move to next tile in x direction
            curr_coarse_fb_ptr += 4;
        }

        // Coarse y loop epilogue
        // ----------------------

        // Move to the start of the next tile row
        curr_coarse_row_fb_ptr += fb_width * 4;
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