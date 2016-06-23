#pragma once
#include <string>
#include "rasterizer.h"

void load_model(const std::string& modelfile, int fbwidth, int fbheight);

void draw_model(const std::string& vigmodelfile, framebuffer_t* fb);