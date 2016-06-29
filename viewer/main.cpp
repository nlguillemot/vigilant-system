#include <renderer.h>
#include <rasterizer.h>
#include <s1516.h>

#include <stdio.h>

#define FLYTHROUGH_CAMERA_IMPLEMENTATION
#include <flythrough_camera.h>

#include <Windows.h>
#include <DirectXMath.h>

#include <glloader.h>

#include <imgui.h>
#include <imgui_impl_win32_gl.h>

#include <string>
#include <vector>

#pragma comment(lib, "OpenGL32.lib")
#pragma comment(lib, "glu32.lib")

LRESULT CALLBACK MyWndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (ImGui_ImplWin32GL_WndProcHandler(hWnd, message, wParam, lParam))
        return true;

    switch (message)
    {
    case WM_CLOSE:
        ExitProcess(0);
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU) // Disable ALT application menu
            return 0;
        break;
    }
    return DefWindowProc(hWnd, message, wParam, lParam);
}

HDC g_hDC;

void init_window(int32_t width, int32_t height)
{
    WNDCLASSEX wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = MyWndProc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)COLOR_BACKGROUND;
    wc.lpszClassName = TEXT("WindowClass");
    RegisterClassEx(&wc);

    RECT wr = { 0, 0, width, height };
    AdjustWindowRect(&wr, 0, FALSE);
    HWND hWnd = CreateWindowEx(
        0, TEXT("WindowClass"),
        TEXT("viewer"),
		WS_POPUP,
        0, 0, wr.right - wr.left, wr.bottom - wr.top,
        0, 0, GetModuleHandle(NULL), 0);

    PIXELFORMATDESCRIPTOR pfd;
    ZeroMemory(&pfd, sizeof(pfd));
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.iLayerType = PFD_MAIN_PLANE;

    g_hDC = GetDC(hWnd);

    int chosenPixelFormat = ChoosePixelFormat(g_hDC, &pfd);
    SetPixelFormat(g_hDC, chosenPixelFormat, &pfd);

    HGLRC hGLRC = wglCreateContext(g_hDC);
    wglMakeCurrent(g_hDC, hGLRC);

	LoadGLProcs();

    ShowWindow(hWnd, SW_SHOWNORMAL);

    ImGui_ImplWin32GL_Init(hWnd);
}

const char* g_GridVS = R"GLSL(#version 150
void main()
{
	if (gl_VertexID == 0)
		gl_Position = vec4(-1,-1,0,1);
	else if (gl_VertexID == 1)
		gl_Position = vec4(3,-1,0,1);
	else if (gl_VertexID == 2)
		gl_Position = vec4(-1,3,0,1);
}
)GLSL";

const char* g_GridFS = R"GLSL(#version 430
layout(location = 0) uniform int show_tiles;
layout(location = 1) uniform int show_coarse;
layout(location = 2) uniform int show_fine;
void main() {
	uvec2 pos = uvec2(gl_FragCoord.xy);
	if (((pos.x & 0x7F) == 0 || (pos.y & 0x7F) == 0) && show_tiles != 0)
		gl_FragColor = vec4(1,1,1,0.5);
	else if (((pos.x & 0xF) == 0 || (pos.y & 0xF) == 0) && show_coarse != 0)
 		gl_FragColor = vec4(1,0.7,0.7,0.5);
	else if (((pos.x & 0x3) == 0 || (pos.y & 0x3) == 0) && show_fine != 0)
 		gl_FragColor = vec4(0.7,1.0,0.7,0.5);
 	else
 		discard;
}
)GLSL";


int main()
{
    int fbwidth = 1024;
    int fbheight = 768;

    SetProcessDPIAware();
    init_window(fbwidth, fbheight);

	GLuint gridsp;
	{
		GLint status;

		GLuint gridvs = glCreateShader(GL_VERTEX_SHADER);
		glShaderSource(gridvs, 1, &g_GridVS, NULL);
		glCompileShader(gridvs);
		glGetShaderiv(gridvs, GL_COMPILE_STATUS, &status);
		assert(status);

		GLuint gridfs = glCreateShader(GL_FRAGMENT_SHADER);
		glShaderSource(gridfs, 1, &g_GridFS, NULL);
		glCompileShader(gridfs);
		glGetShaderiv(gridfs, GL_COMPILE_STATUS, &status);
		assert(status);

		gridsp = glCreateProgram();
		glAttachShader(gridsp, gridvs);
		glAttachShader(gridsp, gridfs);
		glLinkProgram(gridsp);
		glGetProgramiv(gridsp, GL_LINK_STATUS, &status);
		assert(status);
	}

    renderer_t* rd = new_renderer(fbwidth, fbheight);

	const char* all_model_names[] = {
		"cube",
		"gourd"
	};

	static const size_t num_models = sizeof(all_model_names) / sizeof(*all_model_names);

	uint32_t loaded_model_first_ids[num_models];
	uint32_t loaded_model_num_ids[num_models];
	for (size_t i = 0; i < num_models; i++)
	{
		loaded_model_first_ids[i] = -1;
	}

	std::vector<uint32_t> curr_instances;

	int32_t curr_model_index = 0;

    scene_t* sc = new_scene();
    //uint32_t first_model_id, num_added_models;
    //scene_add_models(sc, "assets/gourd/gourd.obj", "assets/gourd/", &first_model_id, &num_added_models);
    //for (uint32_t model_id = first_model_id; model_id < first_model_id + num_added_models; model_id++)
    //{
    //    uint32_t instance_id;
    //    scene_add_instance(sc, model_id, &instance_id);
    //}

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

    float eye[3] = { 0.0f, 0.0f, 3.0f };
    float look[3] = { 0.0f, 0.0f, -1.0f };
    const float up[3] = { 0.0f, 1.0f, 0.0f };

    LARGE_INTEGER then, now, freq;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&then);

    POINT oldcursor;
    GetCursorPos(&oldcursor);

    bool show_tiles = true;
    bool show_coarse_blocks = true;
    bool show_fine_blocks = true;

    uint8_t* rgba8_pixels = (uint8_t*)malloc(fbwidth * fbheight * 4);
    assert(rgba8_pixels);

    // readback framebuffer contents
    framebuffer_t* fb = renderer_get_framebuffer(rd);

    while (!(GetAsyncKeyState(VK_ESCAPE) & 0x8000))
    {
        MSG msg;
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }

        ImGui_ImplWin32GL_NewFrame();
        
		bool switched_model = false;

		switched_model |= loaded_model_first_ids[curr_model_index] == -1;

		ImGui::SetNextWindowSize(ImVec2(400, 200));
        if (ImGui::Begin("Toolbox"))
        {
            ImGui::Checkbox("Show tiles", &show_tiles);
            ImGui::Checkbox("Show coarse blocks", &show_coarse_blocks);
            ImGui::Checkbox("Show fine blocks", &show_fine_blocks);

			if (ImGui::ListBox("Model selection", &curr_model_index, all_model_names, num_models))
			{
				switched_model = true;
			}
        }
		ImGui::End();

		if (switched_model)
		{
			for (uint32_t instance_id : curr_instances)
			{
				scene_remove_instance(sc, instance_id);
			}
			curr_instances.clear();

			if (loaded_model_first_ids[curr_model_index] == -1)
			{
				std::string filename = std::string("assets/") + all_model_names[curr_model_index] + "/" + all_model_names[curr_model_index] + ".obj";
				std::string mtl_basepath = std::string("assets/") + all_model_names[curr_model_index] + "/";
				scene_add_models(sc, filename.c_str(), mtl_basepath.c_str(), &loaded_model_first_ids[curr_model_index], &loaded_model_num_ids[curr_model_index]);
			}

			for (uint32_t model_id = loaded_model_first_ids[curr_model_index]; model_id < loaded_model_first_ids[curr_model_index] + loaded_model_num_ids[curr_model_index]; model_id++)
			{
				uint32_t new_instance_id;
				scene_add_instance(sc, model_id, &new_instance_id);
				curr_instances.push_back(new_instance_id);
			}
		}

        QueryPerformanceCounter(&now);
        float delta_time_sec = (float)(now.QuadPart - then.QuadPart) / freq.QuadPart;

        POINT cursor;
        GetCursorPos(&cursor);

        // only move and rotate camera when right mouse button is pressed
        float activated = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) ? 1.0f : 0.0f;

        float view[16];
        flythrough_camera_update(
            eye, look, up, view,
            delta_time_sec,
            10.0f * ((GetAsyncKeyState(VK_LSHIFT) & 0x8000) ? 2.0f : 1.0f) * activated,
            0.5f * activated,
            80.0f,
            cursor.x - oldcursor.x, cursor.y - oldcursor.y,
            GetAsyncKeyState('W') & 0x8000, GetAsyncKeyState('A') & 0x8000, GetAsyncKeyState('S') & 0x8000, GetAsyncKeyState('D') & 0x8000,
            GetAsyncKeyState(VK_SPACE) & 0x8000, GetAsyncKeyState(VK_LCONTROL) & 0x8000,
            FLYTHROUGH_CAMERA_LEFT_HANDED_BIT);

        int32_t view_s1516[16];
        for (int32_t i = 0; i < 16; i++)
        {
            view_s1516[i] = s1516_flt(view[i]);
        }
        scene_set_view(sc, view_s1516);

        LARGE_INTEGER before_raster, after_raster;
        QueryPerformanceCounter(&before_raster);
        renderer_render_scene(rd, sc);
        QueryPerformanceCounter(&after_raster);

        glClear(GL_COLOR_BUFFER_BIT);

        // render rasterization to screen
        {
            framebuffer_t* fb = renderer_get_framebuffer(rd);
            framebuffer_pack_row_major(fb, 0, 0, fbwidth, fbheight, pixelformat_r8g8b8a8_unorm, rgba8_pixels);
            // flip the rows to appease the OpenGL gods
            for (int32_t row = 0; row < fbheight / 2; row++)
            {
                for (int32_t col = 0; col < fbwidth; col++)
                {
                    uint32_t tmp = *(uint32_t*)&rgba8_pixels[(row * fbwidth + col) * 4];
                    *(uint32_t*)&rgba8_pixels[(row * fbwidth + col) * 4] = *(uint32_t*)&rgba8_pixels[((fbheight - row - 1) * fbwidth + col) * 4];
                    *(uint32_t*)&rgba8_pixels[((fbheight - row - 1) * fbwidth + col) * 4] = tmp;
                }
            }
            glDrawPixels(fbwidth, fbheight, GL_RGBA, GL_UNSIGNED_BYTE, rgba8_pixels);
        }

		if (show_tiles || show_coarse_blocks || show_fine_blocks)
		{
			glEnable(GL_BLEND);
			glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
			glUseProgram(gridsp);
			glUniform1i(0, show_tiles);
			glUniform1i(1, show_coarse_blocks);
			glUniform1i(2, show_fine_blocks);
			glDrawArrays(GL_TRIANGLES, 0, 3);
			glUseProgram(0);
			glBlendFunc(GL_ONE, GL_ZERO);
			glDisable(GL_BLEND);
		}

        if (ImGui::Begin("Info"))
        {
            LONGLONG raster_time = after_raster.QuadPart - before_raster.QuadPart;
            LONGLONG raster_time_us = raster_time * 1000000 / freq.QuadPart;
            ImGui::Text("Raster time: %llu microseconds", raster_time_us);
        }
		ImGui::End();

        ImGui::Render();

        SwapBuffers(g_hDC);

        then = now;
        oldcursor = cursor;
    }

    free(rgba8_pixels);

    delete_scene(sc);
    delete_renderer(rd);

    return 0;
}