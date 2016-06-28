#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef _WIN32
#ifdef RENDERER_EXPORTS
#define RENDERER_API __declspec(dllexport)
#else
#define RENDERER_API __declspec(dllimport)
#endif
#else
#define RENDERER_API
#endif

struct renderer_t;
struct scene_t;
struct framebuffer_t;

RENDERER_API renderer_t* new_renderer(int32_t fbwidth, int32_t fbheight);
RENDERER_API void delete_renderer(renderer_t* rd);
RENDERER_API void renderer_render_scene(renderer_t* rd, scene_t* sc);
RENDERER_API framebuffer_t* renderer_get_framebuffer(renderer_t* rd);

RENDERER_API scene_t* new_scene();
RENDERER_API void delete_scene(scene_t* sc);
RENDERER_API int32_t scene_add_models(scene_t* sc, const char* filename, const char* mtl_basepath, uint32_t* first_model_id, uint32_t* num_added_models);
RENDERER_API void scene_add_instance(scene_t* sc, uint32_t model_id, uint32_t* instance_id);
RENDERER_API void scene_set_camera_lookat(int32_t eyeX, int32_t eyeY, int32_t eyeZ, int32_t targetX, int32_t targetY, int32_t targetZ, int32_t upX, int32_t upY, int32_t upZ);
RENDERER_API void scene_set_camera_perspective(int32_t fovy, int32_t aspect, int32_t zNear, int32_t zFar);

#ifdef __cplusplus
} // extern "C"
#endif
