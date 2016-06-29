#include <renderer.h>
#include <rasterizer.h>
#include <s1516.h>

#include <stdio.h>

#define FLYTHROUGH_CAMERA_IMPLEMENTATION
#include <flythrough_camera.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

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

HWND g_hWnd;

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

    DWORD dwStyle = WS_OVERLAPPEDWINDOW & ~(WS_MAXIMIZEBOX);
    RECT wr = { 0, 0, width, height };
    AdjustWindowRect(&wr, dwStyle, FALSE);
    g_hWnd = CreateWindowEx(
        0, TEXT("WindowClass"),
        TEXT("viewer"),
        dwStyle,
        CW_USEDEFAULT, CW_USEDEFAULT, wr.right - wr.left, wr.bottom - wr.top,
        0, 0, GetModuleHandle(NULL), 0);

    PIXELFORMATDESCRIPTOR pfd;
    ZeroMemory(&pfd, sizeof(pfd));
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.iLayerType = PFD_MAIN_PLANE;

    HDC hDC = GetDC(g_hWnd);

    int chosenPixelFormat = ChoosePixelFormat(hDC, &pfd);
    SetPixelFormat(hDC, chosenPixelFormat, &pfd);

    HGLRC hGLRC = wglCreateContext(hDC);
    wglMakeCurrent(hDC, hGLRC);

	LoadGLProcs();

    ShowWindow(g_hWnd, SW_SHOWNORMAL);

    ImGui_ImplWin32GL_Init(g_hWnd);
}

std::wstring WideFromMultiByte(const char* s)
{
    int bufSize = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, NULL, 0);
    assert(bufSize != 0);

    std::wstring ws(bufSize, 0);
    assert(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, &ws[0], bufSize));
    ws.pop_back(); // remove null terminator
    return ws;
}

std::wstring WideFromMultiByte(const std::string& s)
{
    return WideFromMultiByte(s.c_str());
}

std::string MultiByteFromWide(const wchar_t* ws)
{
    int bufSize = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, ws, -1, NULL, 0, NULL, NULL);
    assert(bufSize != 0);

    std::string s(bufSize, 0);
    assert(WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, ws, -1, &s[0], bufSize, NULL, NULL));
    s.pop_back(); // remove null terminator
    return s;
}

std::string MultiByteFromWide(const std::wstring& ws)
{
    return MultiByteFromWide(ws.c_str());
}

std::string GetSaveFileNameEasy()
{
    // open a file name
    WCHAR szFile[MAX_PATH*2];
    OPENFILENAMEW ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = NULL;
    ofn.lpstrFile = szFile;
    ofn.lpstrFile[0] = '\0';
    ofn.nMaxFile = sizeof(szFile);
    ofn.lpstrFilter = L"All\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.lpstrFileTitle = NULL;
    ofn.nMaxFileTitle = 0;
    ofn.lpstrInitialDir = NULL;
    ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;

    if (!GetSaveFileNameW(&ofn))
    {
        return "";
    }
    else
    {
        return MultiByteFromWide(ofn.lpstrFile);
    }
}

std::string GetOpenFileNameEasy()
{
    // open a file name
    WCHAR szFile[MAX_PATH * 2];
    OPENFILENAMEW ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = NULL;
    ofn.lpstrFile = szFile;
    ofn.lpstrFile[0] = '\0';
    ofn.nMaxFile = sizeof(szFile);
    ofn.lpstrFilter = L"All\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.lpstrFileTitle = NULL;
    ofn.nMaxFileTitle = 0;
    ofn.lpstrInitialDir = NULL;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST;

    if (!GetOpenFileNameW(&ofn))
    {
        return "";
    }
    else
    {
        return MultiByteFromWide(ofn.lpstrFile);
    }
}

__forceinline uint32_t pdep_u32(uint32_t source, uint32_t mask)
{
    // horribly inefficient, but that's life without AVX2.
    // however, typically not a problem since you only need to swizzle once up front.
    // Implementation based on the pseudocode in http://www.felixcloutier.com/x86/PDEP.html
    uint32_t temp = source;
    uint32_t dest = 0;
    uint32_t m = 0, k = 0;
    while (m < 32)
    {
        if (mask & (1 << m))
        {
            dest = (dest & ~(1 << m)) | (((temp & (1 << k)) >> k) << m);
            k = k + 1;
        }
        m = m + 1;
    }
    return dest;
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
layout(origin_upper_left) in vec4 gl_FragCoord;
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
    float up[3] = { 0.0f, 1.0f, 0.0f };
    float view[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };

    LARGE_INTEGER then, now, freq;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&then);

    POINT oldcursor;
    GetCursorPos(&oldcursor);

    bool show_tiles = true;
    bool show_coarse_blocks = false;
    bool show_fine_blocks = false;

    uint8_t* rgba8_pixels = (uint8_t*)malloc(fbwidth * fbheight * 4);
    assert(rgba8_pixels);

    // readback framebuffer contents
    framebuffer_t* fb = renderer_get_framebuffer(rd);

    while (!(GetActiveWindow() == g_hWnd && (GetAsyncKeyState(VK_ESCAPE) & 0x8000)))
    {
        MSG msg;
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }

        ImGui_ImplWin32GL_NewFrame();
        
		bool switched_model = false;

        bool requested_screenshot = false;
        std::string screenshot_filename;

		switched_model |= loaded_model_first_ids[curr_model_index] == -1;

		ImGui::SetNextWindowSize(ImVec2(400, 200));
        if (ImGui::Begin("Toolbox"))
        {
            ImGui::Checkbox("Show tiles", &show_tiles);
            ImGui::Checkbox("Show coarse blocks", &show_coarse_blocks);
            ImGui::Checkbox("Show fine blocks", &show_fine_blocks);

            if (ImGui::Button("Save camera"))
            {
                std::string camfile = GetSaveFileNameEasy();
                FILE* f = fopen(camfile.c_str(), "wb");
                if (f)
                {
                    fwrite(eye, sizeof(eye), 1, f);
                    fwrite(look, sizeof(look), 1, f);
                    fwrite(up, sizeof(up), 1, f);
                    fwrite(view, sizeof(view), 1, f);
                    fclose(f);
                }
            }
            
            ImGui::SameLine();
            if (ImGui::Button("Load camera"))
            {
                std::string camfile = GetOpenFileNameEasy();
                FILE* f = fopen(camfile.c_str(), "rb");
                if (f)
                {
                    fread(eye, sizeof(eye), 1, f);
                    fread(look, sizeof(look), 1, f);
                    fread(up, sizeof(up), 1, f);
                    fread(view, sizeof(view), 1, f);
                    fclose(f);
                }
            }

            if (ImGui::Button("Take screenshot"))
            {
                screenshot_filename = GetSaveFileNameEasy();
                if (!screenshot_filename.empty())
                {
                    requested_screenshot = true;
                    size_t found_dot = screenshot_filename.find_last_of('.');
                    if (found_dot == std::string::npos || screenshot_filename.substr(found_dot) != std::string(".png"))
                    {
                        screenshot_filename += ".png";
                    }
                }
            }

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
        float activated = GetActiveWindow() == g_hWnd && (GetAsyncKeyState(VK_RBUTTON) & 0x8000) ? 1.0f : 0.0f;
        bool bactivated = activated != 0.0f;

        auto keypressed = [bactivated](int vkey) {
            return bactivated && (GetAsyncKeyState(vkey) & 0x8000);
        };

        flythrough_camera_update(
            eye, look, up, view,
            delta_time_sec,
            (10.0f * keypressed(VK_LSHIFT) ? 2.0f : 1.0f) * activated,
            0.5f * activated,
            80.0f,
            cursor.x - oldcursor.x, cursor.y - oldcursor.y,
            keypressed('W'), keypressed('A'), keypressed('S'), keypressed('D'),
            keypressed(VK_SPACE), keypressed(VK_LCONTROL),
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
            
            if (requested_screenshot)
            {
                stbi_write_png(screenshot_filename.c_str(), fbwidth, fbheight, 4, rgba8_pixels, fbwidth * 4);
            }
            
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

            // flip again so the rgba8_pixels is coherent between frames (easier debugging)
            for (int32_t row = 0; row < fbheight / 2; row++)
            {
                for (int32_t col = 0; col < fbwidth; col++)
                {
                    uint32_t tmp = *(uint32_t*)&rgba8_pixels[(row * fbwidth + col) * 4];
                    *(uint32_t*)&rgba8_pixels[(row * fbwidth + col) * 4] = *(uint32_t*)&rgba8_pixels[((fbheight - row - 1) * fbwidth + col) * 4];
                    *(uint32_t*)&rgba8_pixels[((fbheight - row - 1) * fbwidth + col) * 4] = tmp;
                }
            }
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
            POINT cursorpos;
            if (GetCursorPos(&cursorpos) && ScreenToClient(g_hWnd, &cursorpos))
            {
                ImGui::Text("CursorPos: (%d, %d)", cursorpos.x, cursorpos.y);
                
                if (cursorpos.x >= 0 && cursorpos.x < fbwidth &&
                    cursorpos.y >= 0 && cursorpos.y < fbheight)
                {
                    int tile_y = cursorpos.y / 128;
                    int tile_x = cursorpos.x / 128;
                    int width_in_tiles = (fbwidth + 127) / 128;
                    int tile_i = tile_y * width_in_tiles + tile_x;
                    ImGui::Text("TileID: %d", tile_i);
                    int tile_start = tile_i * 128 * 128;
                    int swizzled = pdep_u32(cursorpos.x, 0x55555555 & (128 * 128 - 1));
                    swizzled |= pdep_u32(cursorpos.y, 0xAAAAAAAA & (128 * 128 - 1));
                    ImGui::Text("Swizzled pixel: %d + %d = %d", tile_start, swizzled, tile_start + swizzled);
                }
            }
            
            LONGLONG raster_time = after_raster.QuadPart - before_raster.QuadPart;
            LONGLONG raster_time_us = raster_time * 1000000 / freq.QuadPart;
            ImGui::Text("Raster time: %llu microseconds", raster_time_us);
        }
		ImGui::End();

        ImGui::Render();

        SwapBuffers(GetDC(g_hWnd));

        then = now;
        oldcursor = cursor;
    }

    free(rgba8_pixels);

    delete_scene(sc);
    delete_renderer(rd);

    return 0;
}