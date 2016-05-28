// This program implements a Pineda-style software rasterizer inspired from Larrabee's rasterizer.
// See "A Parallel Algorithm for Polygon Rasterization", by Juan Pineda, SIGGRAPH '88:
// http://citeseerx.ist.psu.edu/viewdoc/download?doi=10.1.1.157.4621&rep=rep1&type=pdf
// For a modern take on this algorithm, see Fabian Giesen's GPU pipeline and Software Occlusion Culling blog series:
// https://fgiesen.wordpress.com/2011/07/09/a-trip-through-the-graphics-pipeline-2011-index/
// https://fgiesen.wordpress.com/2013/02/17/optimizing-sw-occlusion-culling-index/
// Another modern take is Michael Abrash's Dr. Dobb's article "Rasterization on Larrabee":
// http://www.drdobbs.com/parallel/rasterization-on-larrabee/217200602

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include "rasterizer.h"

int main()
{
    int fbwidth = 256;
    int fbheight = 256;

    framebuffer_t* fb = new_framebuffer(fbwidth, fbheight);

    // rasterize triangles
    {
        int32_t verts[] = {
            0 << 8, 0 << 8, 0, 1 << 8,
            100 << 8, 100 << 8, 0, 1 << 8,
            0 << 8, 100 << 8, 0, 1 << 8
        };

        g_Color = 0xFFFFFF00;
        draw(fb, verts, sizeof(verts)/sizeof(*verts)/4);
    }

    {
        int32_t verts[] = {
            0 << 8, 0 << 8, 0, 1 << 8,
            100 << 8, 0 << 8, 0, 1 << 8,
            100 << 8, 100 << 8, 0, 1 << 8
        };

        uint32_t idxs[] = {
            0, 1, 2
        };

        g_Color = 0xFFFF00FF;
        draw_indexed(fb, verts, idxs, sizeof(idxs)/sizeof(*idxs));
    }

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

    // display image
    system("output.png");

    delete_framebuffer(fb);

    return 0;
}