#include <renderer.h>

#include <stdlib.h>

#include <rasterizer.h>

#define TINYOBJLOADER_IMPLEMENTATION
#include <tiny_obj_loader.h>

typedef struct model_t
{
    int32_t* positions;
    uint32_t* indices;

    uint32_t vertex_count;
    uint32_t index_count;
} model_t;

typedef struct instance_t
{
    int32_t model_id;
} instance_t;

#define SCENE_MAX_NUM_MODELS 256
#define SCENE_MAX_NUM_INSTANCES 512

typedef struct scene_t
{
    model_t* models;
    int32_t model_count;

    instance_t* instances;
    int32_t instance_count;
} scene_t;

typedef struct renderer_t
{
    framebuffer_t* fb;
} renderer_t;

renderer_t* new_renderer(int32_t fbwidth, int32_t fbheight)
{
    renderer_t* rd = (renderer_t*)malloc(sizeof(renderer_t));
    assert(rd);

    rd->fb = new_framebuffer(fbwidth, fbheight);
    assert(rd->fb);

    return rd;
}

void delete_renderer(renderer_t* rd)
{
    if (!rd)
        return;

    delete_framebuffer(rd->fb);
    free(rd);
}

void renderer_render_scene(renderer_t* rd, scene_t* sc)
{
    assert(rd);
    assert(sc);
}

framebuffer_t* renderer_get_framebuffer(renderer_t* rd)
{
    return rd->fb;
}

scene_t* new_scene()
{
    scene_t* sc = (scene_t*)malloc(sizeof(scene_t));
    assert(sc);

    memset(sc, 0, sizeof(*sc));

    sc->models = (model_t*)malloc(sizeof(model_t) * SCENE_MAX_NUM_MODELS);
    assert(sc->models);

    sc->instances = (instance_t*)malloc(sizeof(instance_t) * SCENE_MAX_NUM_INSTANCES);
    assert(sc->instances);

    return sc;
}

void delete_scene(scene_t* sc)
{
    for (int32_t i = 0; i < sc->model_count; i++)
    {
        free(sc->models[i].positions);
        free(sc->models[i].indices);
    }
    free(sc->models);
    free(sc);
}

int32_t scene_add_models(scene_t* sc, const char* filename, const char* mtl_basepath, uint32_t* first_model_id, uint32_t* num_added_models)
{
    assert(sc);
    assert(filename);

    std::string error;
    std::vector<tinyobj::shape_t> shapes;
    std::vector<tinyobj::material_t> materials;

    if (!tinyobj::LoadObj(shapes, materials, error, filename, mtl_basepath, tinyobj::triangulation))
    {
        fprintf(stderr, "Error loading model file %s: %s\n", filename, error.c_str());
        return 0;
    }

    uint32_t tmp_first_model_id = sc->model_count;
    uint32_t tmp_num_added_models = 0;

    for (size_t shapeIdx = 0; shapeIdx < shapes.size(); shapeIdx++)
    {
        tinyobj::shape_t& tobj_sh = shapes[shapeIdx];
        tinyobj::mesh_t& tobj_m = tobj_sh.mesh;

        assert(sc->model_count + 1 <= SCENE_MAX_NUM_MODELS);
        tmp_num_added_models++;

        model_t* mdl = &sc->models[sc->model_count];
        
        mdl->positions = (int32_t*)malloc(sizeof(int32_t) * tobj_m.positions.size());
        assert(mdl->positions);
        
        mdl->indices = (uint32_t*)malloc(sizeof(uint32_t) * tobj_m.indices.size());
        assert(mdl->indices);

        mdl->vertex_count = (uint32_t)(tobj_m.positions.size() / 3);
        mdl->index_count = (uint32_t)tobj_m.indices.size();

        sc->model_count++;

        for (size_t i = 0; i < tobj_m.positions.size(); i++)
        {
            int32_t as_s1516 = (int32_t)(tobj_m.positions[i] * 65536);
            mdl->positions[i] = as_s1516;
        }

        for (size_t i = 0; i < tobj_m.indices.size(); i++)
        {
            mdl->indices[i] = tobj_m.indices[i];
        }
    }

    if (first_model_id)
        *first_model_id = tmp_first_model_id;

    if (num_added_models)
        *num_added_models = tmp_num_added_models;

    return 1;
}

void scene_add_instance(scene_t* sc, int32_t model_id, uint32_t* instance_id)
{
    assert(sc);
    assert(model_id >= 0 && model_id < sc->model_count);
}