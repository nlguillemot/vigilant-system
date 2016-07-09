#include <renderer.h>
#include <s1516.h>
#include <Windows.h>
#include <fstream>
#include <vector>

#define FLYTHROUGH_CAMERA_IMPLEMENTATION
#include <flythrough_camera.h>

#include <DirectXMath.h>

typedef struct {
	std::string modelfile;
	std::string modelpath;
} modelfile_t;

int main()
{
    int32_t fbwidth = 1024;
    int32_t fbheight = 768;

    renderer_t* rd = new_renderer(fbwidth, fbheight);

	std::vector<modelfile_t> testModels;

	testModels.push_back({ "assets/cube/cube.obj", "assets/cube/" });
	testModels.push_back({ "assets/gourd/gourd.obj", "assets/gourd/" });

	std::ofstream results("results.csv");

	if (!results.good()) {
		fprintf(stderr, "Error opening results file.\n");
		getc(stdin);
		exit(-1);
	}

	results << "Model Name,Min,Max,Average" << std::endl;

	for (modelfile_t model : testModels) {

		scene_t* sc = new_scene();
		uint32_t first_model_id, num_added_models;
		scene_add_models(sc, model.modelfile.c_str(), model.modelpath.c_str(), &first_model_id, &num_added_models);

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

		LARGE_INTEGER freq;
		LARGE_INTEGER tmin, tmax, tavg;
		LARGE_INTEGER tsum;
		tmin.QuadPart = INT_MAX;
		tmax.QuadPart = tavg.QuadPart = tsum.QuadPart = 0;
		QueryPerformanceFrequency(&freq);
		for (int32_t i = 0; i < 1000; i++)
		{
			LARGE_INTEGER start, finish, diff;
			QueryPerformanceCounter(&start);
			renderer_render_scene(rd, sc);
			QueryPerformanceCounter(&finish);
			diff.QuadPart = finish.QuadPart - start.QuadPart;
			diff.QuadPart *= 1000000;
			diff.QuadPart /= freq.QuadPart;
			if (diff.QuadPart > tmax.QuadPart)
				tmax.QuadPart = diff.QuadPart;

			if (diff.QuadPart < tmin.QuadPart)
				tmin.QuadPart = diff.QuadPart;

			tsum.QuadPart += diff.QuadPart;
		}
		tavg.QuadPart = tsum.QuadPart / 1000;

		results << model.modelfile.c_str() << "," << tmin.QuadPart << "," << tmax.QuadPart << "," << tavg.QuadPart << "," << std::endl;

		delete_scene(sc);
	}

	delete_renderer(rd);
	results.close();

    return 0;
}