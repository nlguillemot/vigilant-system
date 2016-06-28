#include <renderer.h>
#include <rasterizer.h>
#include <s1516.h>

#include <stdio.h>

#define FLYTHROUGH_CAMERA_IMPLEMENTATION
#include <flythrough_camera.h>

#include <Windows.h>
#include <DirectXMath.h>
#include <GL/gl.h>
#include <GL/glu.h>

#include <imgui.h>
#include <imgui_impl_win32_gl.h>

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
        WS_OVERLAPPEDWINDOW,
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

    ShowWindow(hWnd, SW_SHOWNORMAL);

    ImGui_ImplWin32GL_Init(hWnd);
}

int main()
{
    int fbwidth = 1024;
    int fbheight = 768;

    SetProcessDPIAware();
    init_window(fbwidth, fbheight);

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

    float eye[3] = { 0.0f, 0.0f, 3.0f };
    float look[3] = { 0.0f, 0.0f, -1.0f };
    const float up[3] = { 0.0f, 1.0f, 0.0f };

    LARGE_INTEGER then, now, freq;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&then);

    POINT oldcursor;
    GetCursorPos(&oldcursor);

    bool show_tiles = true;

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
        
        if (ImGui::Begin("Toolbox"))
        {
            ImGui::Checkbox("Show tiles", &show_tiles);
            ImGui::End();
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

        renderer_render_scene(rd, sc);

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

        // draw lines showing tiles
        if (show_tiles)
        {
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glMatrixMode(GL_PROJECTION);
            glLoadIdentity();
            gluOrtho2D(0.0, (GLdouble)fbwidth, (GLdouble)fbheight, 0.0);
            glMatrixMode(GL_MODELVIEW);
            glLoadIdentity();
            glBegin(GL_LINES);
            glColor4f(1.0f, 1.0f, 1.0f, 0.5f);
            for (int i = 0; i < fbwidth / 128; i++)
            {
                glVertex2f(i * 128.0f, 0.0f);
                glVertex2f(i * 128.0f, (float)fbheight);
            }
            for (int i = 0; i < fbheight / 128; i++)
            {
                glVertex2f(0.0f, i * 128.0f);
                glVertex2f((float)fbwidth, i * 128.0f);
            }
            glEnd();
            glBlendFunc(GL_ONE, GL_ZERO);
            glDisable(GL_BLEND);
        }

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