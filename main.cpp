#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include "rasterizer.h"
#include "benchmark.h"

//#define _BENCHMARK

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
    result = (int32_t)(temp / b);

    return result;
}

int main()
{
	int fbwidth = 1024;
	int fbheight = 768;

    framebuffer_t* fb = new_framebuffer(fbwidth, fbheight);

#ifdef _BENCHMARK
	load_model("cube.obj", fbwidth, fbheight);
	g_Color = 0xFF00FFFF;
	draw_model("cube.vig", fb);
	framebuffer_resolve(fb);
#else
    int32_t radius = s1516_div(2 << 16, 4 << 16);

    // rasterize triangles
    {
        int32_t verts[] = {
            (-1 << 16), (1 << 16), 0, 1 << 16,
            (-1 << 16) + radius, (1 << 16) - radius, 0, 1 << 16,
            (-1 << 16), (1 << 16) - radius, 0, 1 << 16,
        };

        g_Color = 0xFFFFFF00;
        draw(fb, verts, sizeof(verts)/sizeof(*verts)/4);
    }
    
    // make sure all caches are flushed and yada yada
    framebuffer_resolve(fb);

    // rasterize triangles
    {
        int32_t verts[] = {
            (-1 << 16), (1 << 16), 0, 1 << 16,
            (-1 << 16) + radius, (1 << 16), 0, 1 << 16,
            (-1 << 16) + radius, (1 << 16) - radius, 0, 1 << 16,
        };

        uint32_t idxs[] = {
            0, 1, 2
        };

        g_Color = 0xFFFF00FF;
        draw_indexed(fb, verts, idxs, sizeof(idxs)/sizeof(*idxs));
    }

    // make sure all caches are flushed and yada yada
    framebuffer_resolve(fb);
#endif

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