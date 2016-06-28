#include <renderer.h>

#include <stdlib.h>

#include <rasterizer.h>
#include <s1516.h>

#define TINYOBJLOADER_IMPLEMENTATION
#include <tiny_obj_loader.h>

#define SCENE_MAX_NUM_MODELS 256
#define SCENE_MAX_NUM_INSTANCES 512

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

typedef struct scene_t
{
    model_t* models;
    uint32_t model_count;

    instance_t* instances;
    uint32_t instance_count;

    int32_t view[16];
    int32_t proj[16];
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

static void s15164x4_mul(const int32_t* a, const int32_t* b, int32_t* dst)
{
    dst[0]  = s1516_fma(a[0],  b[0], s1516_fma(a[4],  b[1], s1516_fma( a[8],  b[2], s1516_mul(a[12],  b[3]))));
    dst[1]  = s1516_fma(a[1],  b[0], s1516_fma(a[5],  b[1], s1516_fma( a[9],  b[2], s1516_mul(a[13],  b[3]))));
    dst[2]  = s1516_fma(a[2],  b[0], s1516_fma(a[6],  b[1], s1516_fma(a[10],  b[2], s1516_mul(a[14],  b[3]))));
    dst[3]  = s1516_fma(a[3],  b[0], s1516_fma(a[7],  b[1], s1516_fma(a[11],  b[2], s1516_mul(a[15],  b[3]))));
    dst[4]  = s1516_fma(a[0],  b[4], s1516_fma(a[4],  b[5], s1516_fma( a[8],  b[6], s1516_mul(a[12],  b[7]))));
    dst[5]  = s1516_fma(a[1],  b[4], s1516_fma(a[5],  b[5], s1516_fma( a[9],  b[6], s1516_mul(a[13],  b[7]))));
    dst[6]  = s1516_fma(a[2],  b[4], s1516_fma(a[6],  b[5], s1516_fma(a[10],  b[6], s1516_mul(a[14],  b[7]))));
    dst[7]  = s1516_fma(a[3],  b[4], s1516_fma(a[7],  b[5], s1516_fma(a[11],  b[6], s1516_mul(a[15],  b[7]))));
    dst[8]  = s1516_fma(a[0],  b[8], s1516_fma(a[4],  b[9], s1516_fma( a[8], b[10], s1516_mul(a[12], b[11]))));
    dst[9]  = s1516_fma(a[1],  b[8], s1516_fma(a[5],  b[9], s1516_fma( a[9], b[10], s1516_mul(a[13], b[11]))));
    dst[10] = s1516_fma(a[2],  b[8], s1516_fma(a[6],  b[9], s1516_fma(a[10], b[10], s1516_mul(a[14], b[11]))));
    dst[11] = s1516_fma(a[3],  b[8], s1516_fma(a[7],  b[9], s1516_fma(a[11], b[10], s1516_mul(a[15], b[11]))));
    dst[12] = s1516_fma(a[0], b[12], s1516_fma(a[4], b[13], s1516_fma( a[8], b[14], s1516_mul(a[12], b[15]))));
    dst[13] = s1516_fma(a[1], b[12], s1516_fma(a[5], b[13], s1516_fma( a[9], b[14], s1516_mul(a[13], b[15]))));
    dst[14] = s1516_fma(a[2], b[12], s1516_fma(a[6], b[13], s1516_fma(a[10], b[14], s1516_mul(a[14], b[15]))));
    dst[15] = s1516_fma(a[3], b[12], s1516_fma(a[7], b[13], s1516_fma(a[11], b[14], s1516_mul(a[15], b[15]))));
}

static void renderer_render_instance(renderer_t* rd, scene_t* sc, instance_t* instance)
{
    int32_t model_id = instance->model_id;
    model_t* model = &sc->models[model_id];

    int32_t viewproj[16];
    s15164x4_mul(sc->proj, sc->view, viewproj);

    for (uint32_t index_id = 0; index_id < model->index_count; index_id += 3)
    {
        int32_t xverts[3][4];

        // TODO: cache transformations based on vertex_id

        for (uint32_t index_off = 0; index_off < 3; index_off++)
        {
            uint32_t vertex_id = model->indices[index_id + index_off];
            int32_t vert[3];
            vert[0] = model->positions[vertex_id * 3 + 0];
            vert[1] = model->positions[vertex_id * 3 + 1];
            vert[2] = model->positions[vertex_id * 3 + 2];

            // TODO: incorporate modelworld matrix
            xverts[index_off][0] = s1516_fma(viewproj[0], vert[0], s1516_fma(viewproj[4], vert[1], s1516_fma(viewproj[8], vert[2],  viewproj[12])));
            xverts[index_off][1] = s1516_fma(viewproj[1], vert[0], s1516_fma(viewproj[5], vert[1], s1516_fma(viewproj[9], vert[2],  viewproj[13])));
            xverts[index_off][2] = s1516_fma(viewproj[2], vert[0], s1516_fma(viewproj[6], vert[1], s1516_fma(viewproj[10], vert[2], viewproj[14])));
            xverts[index_off][3] = s1516_fma(viewproj[3], vert[0], s1516_fma(viewproj[7], vert[1], s1516_fma(viewproj[11], vert[2], viewproj[15])));
        }

        rasterizer_draw(rd->fb, &xverts[0][0], 3);
    }
}

void renderer_render_scene(renderer_t* rd, scene_t* sc)
{
    assert(rd);
    assert(sc);

    framebuffer_clear(rd->fb, 0x00000000);

    for (uint32_t instance_id = 0; instance_id < sc->instance_count; instance_id++)
    {
        instance_t* instance = &sc->instances[instance_id];
        renderer_render_instance(rd, sc, instance);
    }

    framebuffer_resolve(rd->fb);
}

framebuffer_t* renderer_get_framebuffer(renderer_t* rd)
{
    return rd->fb;
}

scene_t* new_scene()
{
    scene_t* sc = (scene_t*)malloc(sizeof(scene_t));
    assert(sc);

    sc->models = (model_t*)malloc(sizeof(model_t) * SCENE_MAX_NUM_MODELS);
    assert(sc->models);

    sc->model_count = 0;

    sc->instances = (instance_t*)malloc(sizeof(instance_t) * SCENE_MAX_NUM_INSTANCES);
    assert(sc->instances);

    sc->instance_count = 0;
    
    return sc;
}

void delete_scene(scene_t* sc)
{
    free(sc->instances);

    for (uint32_t i = 0; i < sc->model_count; i++)
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
            int32_t as_s1516 = s1516_flt(tobj_m.positions[i]);
            mdl->positions[i] = as_s1516;
        }

        for (size_t i = 0; i < tobj_m.indices.size(); i += 3)
        {
            // flip winding (CCW to CW)
            mdl->indices[i + 0] = tobj_m.indices[i + 0];
            mdl->indices[i + 1] = tobj_m.indices[i + 2];
            mdl->indices[i + 2] = tobj_m.indices[i + 1];
        }
    }

    if (first_model_id)
        *first_model_id = tmp_first_model_id;

    if (num_added_models)
        *num_added_models = tmp_num_added_models;

    return 1;
}

void scene_add_instance(scene_t* sc, uint32_t model_id, uint32_t* instance_id)
{
    assert(sc);
    assert(model_id >= 0 && model_id < sc->model_count);
    assert(sc->instance_count < SCENE_MAX_NUM_INSTANCES);

    uint32_t tmp_instance_id = sc->instance_count;
    sc->instance_count++;

    instance_t* instance = &sc->instances[tmp_instance_id];
    instance->model_id = model_id;

    if (instance_id)
        *instance_id = tmp_instance_id;
}

void scene_set_view(scene_t* sc, int32_t view[16])
{
    memcpy(sc->view, view, sizeof(int32_t) * 16);
}

void scene_set_projection(scene_t* sc, int32_t proj[16])
{
    memcpy(sc->proj, proj, sizeof(int32_t) * 16);
}