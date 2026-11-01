// Hidden-window OpenGL context for the PC builds of the GE renderer. Rendering
// goes to framebuffer objects only, so the window is never shown.
#include "gl_api.hpp"

#define BURNOUT_GL_DEFINE(ret, name, args) PFN_##name name = nullptr;
BURNOUT_GL_FUNCTIONS(BURNOUT_GL_DEFINE)
#undef BURNOUT_GL_DEFINE

namespace burnout::gl {

bool create_context(std::string &error) {
    WNDCLASSA window_class{};
    window_class.style = CS_OWNDC;
    window_class.lpfnWndProc = DefWindowProcA;
    window_class.hInstance = GetModuleHandleA(nullptr);
    window_class.lpszClassName = "burnout_gl_context";
    RegisterClassA(&window_class);
    HWND window = CreateWindowA(window_class.lpszClassName, "burnout", WS_OVERLAPPEDWINDOW, 0, 0, 16, 16, nullptr,
                                nullptr, window_class.hInstance, nullptr);
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
    return true;
}

void swap_buffers() {}

} // namespace burnout::gl
