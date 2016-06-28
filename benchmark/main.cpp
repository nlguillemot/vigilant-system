#include <renderer.h>
#include <s1516.h>

int main()
{
    int32_t fbwidth = 1024;
    int32_t fbheight = 768;

    renderer_t* rd = new_renderer(fbwidth, fbheight);

    scene_t* sc = new_scene();
    uint32_t first_model_id, num_added_models;
    scene_add_models(sc, "assets/cube/cube.obj", "assets/cube/", &first_model_id, &num_added_models);
    for (uint32_t model_id = first_model_id; model_id < first_model_id + num_added_models; model_id++)
    {
        uint32_t instance_id;
        scene_add_instance(sc, model_id, &instance_id);
    }

    scene_set_camera_lookat(s1516_int(5), s1516_int(5), s1516_int(5), 0, 0, 0, 0, s1516_int(1), 0);
    scene_set_camera_perspective(s1516_flt(70.0f * 3.14f / 180.0f), s1516_flt((float)fbwidth / fbheight), s1516_flt(0.01f), s1516_flt(10.0f));

    for (int32_t i = 0; i < 1000; i++)
    {
        renderer_render_scene(rd, sc);
    }

    delete_scene(sc);
    delete_renderer(rd);

    return 0;
}