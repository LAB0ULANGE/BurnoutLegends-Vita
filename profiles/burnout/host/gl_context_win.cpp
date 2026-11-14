// OpenGL context for the PC builds of the GE renderer. The headless runner
// keeps the window hidden (rendering goes to framebuffer objects only); the
// playable PC build (main_pc.cpp) asks for a visible window first
// (set_window), and the presented picture goes to it.
#include "gl_api.hpp"

#include <atomic>

#define BURNOUT_GL_DEFINE(ret, name, args) PFN_##name name = nullptr;
BURNOUT_GL_FUNCTIONS(BURNOUT_GL_DEFINE)
#undef BURNOUT_GL_DEFINE

namespace burnout::gl {

namespace {

bool g_visible{};
std::string g_title{"burnout"};
int g_start_width{960}, g_start_height{544};
bool g_start_fullscreen{};
HWND g_window{};
HDC g_dc{};
std::atomic<bool> g_closed{false};
std::atomic<bool> g_focused{true};
std::atomic<int> g_client_width{960}, g_client_height{544};
bool g_fullscreen{};
WINDOWPLACEMENT g_placement{sizeof(WINDOWPLACEMENT)};

void toggle_fullscreen() {
    const LONG style = GetWindowLongA(g_window, GWL_STYLE);
    if (!g_fullscreen) {
        MONITORINFO monitor{sizeof(MONITORINFO)};
        if (GetWindowPlacement(g_window, &g_placement) &&
            GetMonitorInfoA(MonitorFromWindow(g_window, MONITOR_DEFAULTTOPRIMARY), &monitor)) {
            SetWindowLongA(g_window, GWL_STYLE, style & ~WS_OVERLAPPEDWINDOW);
            SetWindowPos(g_window, HWND_TOP, monitor.rcMonitor.left, monitor.rcMonitor.top,
                         monitor.rcMonitor.right - monitor.rcMonitor.left, monitor.rcMonitor.bottom - monitor.rcMonitor.top,
                         SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
            g_fullscreen = true;
        }
    } else {
        SetWindowLongA(g_window, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(g_window, &g_placement);
        SetWindowPos(g_window, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        g_fullscreen = false;
    }
}

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM w, LPARAM l) {
    switch (message) {
    case WM_CLOSE: g_closed = true; return 0;  // the game is stopped by the runner, then the process ends
    case WM_SIZE:
        g_client_width = LOWORD(l);
        g_client_height = HIWORD(l);
        return 0;
    case WM_ACTIVATE: g_focused = LOWORD(w) != WA_INACTIVE; break;
    case WM_SYSKEYDOWN:
        if (w == VK_RETURN && (HIWORD(l) & KF_ALTDOWN) != 0) {  // Alt + Enter
            toggle_fullscreen();
            return 0;
        }
        break;
    case WM_KEYDOWN:
        if (w == VK_F11 && (HIWORD(l) & KF_REPEAT) == 0) {
            toggle_fullscreen();
            return 0;
        }
        break;
    case WM_SETCURSOR:
        if (g_fullscreen && LOWORD(l) == HTCLIENT) {  // no mouse pointer over the game in full screen
            SetCursor(nullptr);
            return TRUE;
        }
        break;
    case WM_SYSCOMMAND:
        if ((w & 0xFFF0u) == SC_KEYMENU) return 0;  // Alt alone does not open the menu
        break;
    default: break;
    }
    return DefWindowProcA(window, message, w, l);
}

} // namespace

void set_window(const std::string &title, int width, int height, bool fullscreen) {
    g_visible = true;
    g_title = title;
    g_start_width = width;
    g_start_height = height;
    g_start_fullscreen = fullscreen;
}
bool window_visible() { return g_visible; }
bool window_closed() { return g_closed.load(); }
bool window_focused() { return g_focused.load(); }
void window_size(int &width, int &height) {
    width = g_client_width.load();
    height = g_client_height.load();
}

bool create_context(std::string &error) {
    WNDCLASSA window_class{};
    window_class.style = CS_OWNDC;
    window_class.lpfnWndProc = g_visible ? window_proc : DefWindowProcA;
    window_class.hInstance = GetModuleHandleA(nullptr);
    window_class.hCursor = LoadCursor(nullptr, IDC_ARROW);
    window_class.hIcon = LoadIcon(GetModuleHandleA(nullptr), MAKEINTRESOURCE(1));
    window_class.lpszClassName = "burnout_gl_context";
    RegisterClassA(&window_class);
    RECT rect{0, 0, g_visible ? g_start_width : 16, g_visible ? g_start_height : 16};
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    HWND window = CreateWindowA(window_class.lpszClassName, g_title.c_str(), WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr, window_class.hInstance, nullptr);
    if (window == nullptr) { error = "CreateWindow failed"; return false; }
    HDC dc = GetDC(window);
    PIXELFORMATDESCRIPTOR format{};
    format.nSize = sizeof(format);
    format.nVersion = 1;
    format.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    format.iPixelType = PFD_TYPE_RGBA;
    format.cColorBits = 32;
    format.cDepthBits = 24;
    format.cStencilBits = 8;
    const int index = ChoosePixelFormat(dc, &format);
    if (index == 0 || !SetPixelFormat(dc, index, &format)) { error = "no OpenGL pixel format"; return false; }
    HGLRC context = wglCreateContext(dc);
    if (context == nullptr || !wglMakeCurrent(dc, context)) { error = "wglCreateContext failed"; return false; }
#define BURNOUT_GL_LOAD(ret, name, args)                                            \
    name = reinterpret_cast<PFN_##name>(wglGetProcAddress(#name));                  \
    if (name == nullptr) { error = "missing OpenGL entry point " #name; return false; }
    BURNOUT_GL_FUNCTIONS(BURNOUT_GL_LOAD)
#undef BURNOUT_GL_LOAD
    if (g_visible) {
        g_window = window;
        g_dc = dc;
        // The game paces itself (59.94 Hz vblanks): no wait for the display.
        using SwapInterval = BOOL(WINAPI *)(int);
        if (auto interval = reinterpret_cast<SwapInterval>(wglGetProcAddress("wglSwapIntervalEXT"))) interval(0);
        RECT client{};
        GetClientRect(window, &client);
        g_client_width = client.right;
        g_client_height = client.bottom;
        ShowWindow(window, SW_SHOW);
        if (g_start_fullscreen) toggle_fullscreen();
        SetForegroundWindow(window);
    }
    return true;
}

void swap_buffers() {
    if (g_window == nullptr) return;
    SwapBuffers(g_dc);
    // The window was made on this thread (the GL thread): its messages here.
    MSG message;
    while (PeekMessageA(&message, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message);
        DispatchMessageA(&message);
    }
}

std::string memory_report() { return {}; }

} // namespace burnout::gl
