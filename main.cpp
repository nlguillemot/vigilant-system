#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

uint32_t g_Color;

void rasterize_triangle_fixed16_8(
    uint32_t* fb, uint32_t fb_width,
    uint32_t x0, uint32_t y0, uint32_t z0,
    uint32_t x1, uint32_t y1, uint32_t z1,
    uint32_t x2, uint32_t y2, uint32_t z2)
{
    // window coordinates' bounding box
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

    // barycentric coordinates of top left corner coordinate
    // | x  y  z |
    // | vx vy 0 |
    // | ux uy 0 |
    // = z(vx*uy - vy*ux)
    int32_t min_bary0 = ((int64_t)(x2 - x1) * (y_min - y1) - (int64_t)(y2 - y1) * (x_min - x1)) >> 8;
    int32_t min_bary1 = ((int64_t)(x0 - x2) * (y_min - y2) - (int64_t)(y0 - y2) * (x_min - x2)) >> 8;
    int32_t min_bary2 = ((int64_t)(x1 - x0) * (y_min - y0) - (int64_t)(y1 - y0) * (x_min - x0)) >> 8;

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
                *curr_px += g_Color;
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