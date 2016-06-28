#include <renderer.h>
#include <s1516.h>

#define FLYTHROUGH_CAMERA_IMPLEMENTATION
#include <flythrough_camera.h>

#include <DirectXMath.h>

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

    // compute projection matrix with DirectXMath because lazy
    {
        DirectX::XMMATRIX proj = DirectX::XMMatrixPerspectiveFovLH(DirectX::XMConvertToRadians(70.0f), (float)fbwidth / fbheight, 0.01f, 10.0f);
        DirectX::XMFLOAT4X4 proj4x4;
        DirectX::XMStoreFloat4x4(&proj4x4, proj);

        int32_t fx_proj[16];
        for (int32_t i = 0; i < 16; i++)
        {
            fx_proj[i] = s1516_flt(*((float*)proj4x4.m + i));
        }
        scene_set_projection(sc, fx_proj);
    }

    // compute lookat matrix with flythrough_camera because lazy
    {
        float eye[3] = { 0.0f, 0.0f, 3.0f };
        float look[3] = { 0.0f, 0.0f, -1.0f };
        float up[3] = { 0.0f, 1.0f, 0.0f };
        float view[16];
        flythrough_camera_look_to(eye, look, up, view, FLYTHROUGH_CAMERA_LEFT_HANDED_BIT);
        int32_t view_s1516[16];
        for (int32_t i = 0; i < 16; i++)
        {
            view_s1516[i] = s1516_flt(view[i]);
        }
        scene_set_view(sc, view_s1516);
    }

    for (int32_t i = 0; i < 1000; i++)
    {
        renderer_render_scene(rd, sc);
    }

    delete_scene(sc);
    delete_renderer(rd);

    return 0;
}