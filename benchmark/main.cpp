#include <stdio.h>
#include <stdint.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <rasterizer.h>
#include <renderer.h>

    glm::mat4 matWorld = glm::translate(glm::mat4(1.0), glm::vec3(0, 0, 0));
    glm::mat4 matView = glm::lookAt(glm::vec3(0, 0.5f, 5), glm::vec3(0, 0.5f, 0), glm::vec3(0, 1, 0));
    glm::mat4 matProj = glm::perspective(glm::radians(45.f), (float)fbwidth / (float)fbheight, 0.0f, 1.f);

    glm::mat4 wvp = matProj * matView * matWorld;


int main()
{
	int fbwidth = 1024;
	int fbheight = 768;

    renderer_t* rd = new_renderer(fbwidth, fbheight);

    scene_t* sc = new_scene();
    uint32_t first_model_id, num_added_models;
    scene_add_models(sc, "assets/cube/cube.obj", "assets/cube/", &first_model_id, &num_added_models);
    for (uint32_t model_id = first_model_id; model_id < first_model_id + num_added_models; model_id++)
    {
        uint32_t instance_id;
        scene_add_instance(sc, model_id, &instance_id);
    }

    renderer_render_scene(rd, sc);

    // convert framebuffer from bgra to rgba for stbi_image_write
    {
        uint8_t* rgba8_pixels = (uint8_t*)malloc(fbwidth * fbheight * 4);
        assert(rgba8_pixels);

        // readback framebuffer contents
        framebuffer_t* fb = renderer_get_framebuffer(rd);
        framebuffer_pack_row_major(fb, 0, 0, fbwidth, fbheight, pixelformat_r8g8b8a8_unorm, rgba8_pixels);

        if (!stbi_write_png("output.png", fbwidth, fbheight, 4, rgba8_pixels, fbwidth * 4))
        {
            fprintf(stderr, "Failed to write image\n");
            exit(1);
        }

        free(rgba8_pixels);
    }

    delete_scene(sc);
    delete_renderer(rd);

    // display image
    system("output.png");

    return 0;
}