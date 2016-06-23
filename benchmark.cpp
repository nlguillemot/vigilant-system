#include "benchmark.h"
#include "glm.hpp"
#include "gtc/matrix_transform.hpp"
#define TINYOBJLOADER_IMPLEMENTATION
#include "tiny_obj_loader.h"

#include <string>
#include <fstream>
#include <cmath>
#include <algorithm>




void load_model(const std::string& modelfile, int fbwidth, int fbheight) {
	std::string error;

	std::vector<tinyobj::shape_t> shapes;
	std::vector<tinyobj::material_t> mats;

	bool ret = tinyobj::LoadObj(shapes, mats, error, modelfile.c_str(), nullptr, 3u);

	if (!error.empty()) {
		fprintf(stderr, "Error loading model file %s.\n", modelfile.c_str());
		exit(1);
	}

	glm::mat4 matWorld = glm::translate(glm::mat4(1.0), glm::vec3(0, 0, 0));
	glm::mat4 matView = glm::lookAt(glm::vec3(0, 2.5f, 5), glm::vec3(0, 0.5f, 0), glm::vec3(0, 1, 0));
	glm::mat4 matProj = glm::perspective(glm::radians(45.f), (float)fbwidth / (float)fbheight, 0.0f, 1.f);

	glm::mat4 wvp = matProj * matView * matWorld;

	std::string outputfilename;
	for (int i = modelfile.length() - 1; i >= 0; --i) {
		if (modelfile.at(i) == '.') {
			outputfilename = modelfile.substr(0, i) + ".vig";
		}
	}

	std::ofstream outputfile(outputfilename.c_str());

	if (!outputfile.good()) {
		fprintf(stderr, "Error opening output file %s\n.", outputfilename.c_str());
	}

	if (shapes.size() > 0) {
		outputfile << shapes.at(0).mesh.positions.size() / 3 << std::endl;
		assert(shapes.at(0).mesh.positions.size() % 3 == 0);
		assert(shapes.at(0).mesh.indices.size() % 3 == 0);
		for (int i = 0; i < shapes.at(0).mesh.positions.size(); i += 3) {
			glm::vec4 pos = glm::vec4(shapes.at(0).mesh.positions.at(i), shapes.at(0).mesh.positions.at(i + 1), shapes.at(0).mesh.positions.at(i + 2), 1.0f);
			pos = wvp * pos;
			outputfile << pos.x << " " << pos.y << " " << pos.z << " " << std::max(1.f, pos.w) << std::endl;
		}

		outputfile << shapes.at(0).mesh.indices.size() << std::endl;
		for (int i = 0; i < shapes.at(0).mesh.indices.size(); i += 3) {
			outputfile << shapes.at(0).mesh.indices.at(i + 2) << " " << shapes.at(0).mesh.indices.at(i + 1) << " " << shapes.at(0).mesh.indices.at(i) << std::endl;
		}
	}

	outputfile.close();
}

void draw_model(const std::string& vigmodelfile, framebuffer_t* fb) {
	std::ifstream inputfile(vigmodelfile);

	if (!inputfile.good()) {
		fprintf(stderr, "Failed to open vigilant model file %s.\n", vigmodelfile.c_str());
		exit(1);
	}

	int32_t* verts;
	uint32_t* indices;

	uint32_t num_verts = 0;

	inputfile >> num_verts;

	assert(num_verts > 0);

	verts = (int32_t*) malloc(sizeof(int32_t) * num_verts * 4);

	for (uint32_t i = 0; i < num_verts * 4; ++i) {
		float pos;
		inputfile >> pos;
		verts[i] = (int)(pos * (65536 / 2));
	}

	uint32_t num_indices = 0;

	inputfile >> num_indices;

	assert(num_indices % 3 == 0);

	indices = (uint32_t*)malloc(sizeof(uint32_t) * num_indices);

	for (uint32_t i = 0; i < num_indices; ++i) {
		inputfile >> indices[i];
	}

	inputfile.close();

	printf("Vertices: %i, Indices: %i\n", num_verts, num_indices);

	draw_indexed(fb, verts, indices, num_indices);

	free(verts);
	free(indices);

}

