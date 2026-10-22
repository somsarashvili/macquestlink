#define COBJMACROS
#include <windows.h>
#include <d3d11_1.h>
#include <stdint.h>
#include <stdio.h>

// What Chromium's GPU process does with CEF's shared OSR textures: wrap a D3D11 texture in an ANGLE EGLImage, render
// to it through a GL framebuffer and read it back. angletex.exe [fmt] (run with the CEF dir's libEGL/libGLESv2).
typedef void *EGLDisplay, *EGLContext, *EGLSurface, *EGLConfig, *EGLImage, *EGLDeviceEXT;
typedef intptr_t EGLAttrib;
typedef int32_t EGLint;
typedef unsigned GLuint, GLenum;
typedef int GLint, GLsizei;

#define EGL_PLATFORM_ANGLE_ANGLE 0x3202
#define EGL_PLATFORM_ANGLE_TYPE_ANGLE 0x3203
#define EGL_PLATFORM_ANGLE_TYPE_D3D11_ANGLE 0x3208
#define EGL_DEVICE_EXT 0x322C
#define EGL_D3D11_DEVICE_ANGLE 0x33A1
#define EGL_D3D11_TEXTURE_ANGLE 0x3484
#define EGL_TEXTURE_INTERNAL_FORMAT_ANGLE 0x345D
#define EGL_NONE 0x3038
#define EGL_CONTEXT_CLIENT_VERSION 0x3098
#define EGL_RENDERABLE_TYPE 0x3040
#define EGL_OPENGL_ES3_BIT 0x40
#define EGL_SURFACE_TYPE 0x3033
#define EGL_PBUFFER_BIT 0x1
#define EGL_WIDTH 0x3057
#define EGL_HEIGHT 0x3056

static FILE *g_log;

static void say(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

#define LOAD(m, name) name##_t name = (name##_t)GetProcAddress(m, #name)
typedef EGLDisplay(WINAPI *eglGetPlatformDisplayEXT_t)(GLenum, void *, const EGLint *);
typedef int(WINAPI *eglInitialize_t)(EGLDisplay, EGLint *, EGLint *);
typedef int(WINAPI *eglChooseConfig_t)(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *);
typedef EGLContext(WINAPI *eglCreateContext_t)(EGLDisplay, EGLConfig, EGLContext, const EGLint *);
typedef EGLSurface(WINAPI *eglCreatePbufferSurface_t)(EGLDisplay, EGLConfig, const EGLint *);
typedef int(WINAPI *eglMakeCurrent_t)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
typedef EGLint(WINAPI *eglGetError_t)(void);
typedef void *(WINAPI *eglGetProcAddress_t)(const char *);
typedef int(WINAPI *eglQueryDisplayAttribEXT_t)(EGLDisplay, EGLint, EGLAttrib *);
typedef int(WINAPI *eglQueryDeviceAttribEXT_t)(EGLDeviceEXT, EGLint, EGLAttrib *);
typedef EGLImage(WINAPI *eglCreateImageKHR_t)(EGLDisplay, EGLContext, GLenum, void *, const EGLint *);
typedef void(WINAPI *glEGLImageTargetTexture2DOES_t)(GLenum, EGLImage);
typedef void(WINAPI *glGenTextures_t)(GLsizei, GLuint *);
typedef void(WINAPI *glBindTexture_t)(GLenum, GLuint);
typedef void(WINAPI *glGenFramebuffers_t)(GLsizei, GLuint *);
typedef void(WINAPI *glBindFramebuffer_t)(GLenum, GLuint);
typedef void(WINAPI *glFramebufferTexture2D_t)(GLenum, GLenum, GLenum, GLuint, GLint);
typedef GLenum(WINAPI *glCheckFramebufferStatus_t)(GLenum);
typedef GLenum(WINAPI *glGetError_t)(void);
typedef void(WINAPI *glClearColor_t)(float, float, float, float);
typedef void(WINAPI *glClear_t)(GLenum);
typedef void(WINAPI *glFinish_t)(void);
typedef void(WINAPI *glReadPixels_t)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *);
typedef const char *(WINAPI *glGetString_t)(GLenum);

static void WINAPI on_gl_debug(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length,
                               const char *message, const void *user)
{
    say("  GL debug: %s", message);
}

typedef void(WINAPI *glDebugMessageCallbackKHR_t)(void *, const void *);
typedef void(WINAPI *glEnable_t)(GLenum);

int main(int argc, char **argv)
{
    g_log = fopen("C:\\macquestlink\\angletex.log", "w");
    SetDllDirectoryA("C:\\Program Files (x86)\\Steam\\steamapps\\common\\SteamVR\\bin\\vrwebhelper\\win64");
    HMODULE egl = LoadLibraryA("libEGL.dll"), gles = LoadLibraryA("libGLESv2.dll");
    say("libEGL %p libGLESv2 %p", egl, gles);
    LOAD(egl, eglGetPlatformDisplayEXT);
    LOAD(egl, eglInitialize);
    LOAD(egl, eglChooseConfig);
    LOAD(egl, eglCreateContext);
    LOAD(egl, eglCreatePbufferSurface);
    LOAD(egl, eglMakeCurrent);
    LOAD(egl, eglGetError);
    LOAD(egl, eglGetProcAddress);
    LOAD(egl, eglCreateImageKHR);
    eglQueryDisplayAttribEXT_t eglQueryDisplayAttribEXT = eglGetProcAddress("eglQueryDisplayAttribEXT");
    eglQueryDeviceAttribEXT_t eglQueryDeviceAttribEXT = eglGetProcAddress("eglQueryDeviceAttribEXT");
    if (!eglGetPlatformDisplayEXT) {
        eglGetPlatformDisplayEXT = eglGetProcAddress("eglGetPlatformDisplayEXT");
    }
    LOAD(gles, glEGLImageTargetTexture2DOES);
    LOAD(gles, glGenTextures);
    LOAD(gles, glBindTexture);
    LOAD(gles, glGenFramebuffers);
    LOAD(gles, glBindFramebuffer);
    LOAD(gles, glFramebufferTexture2D);
    LOAD(gles, glCheckFramebufferStatus);
    LOAD(gles, glGetError);
    LOAD(gles, glClearColor);
    LOAD(gles, glClear);
    LOAD(gles, glReadPixels);
    LOAD(gles, glGetString);

    EGLint dattr[] = { EGL_PLATFORM_ANGLE_TYPE_ANGLE, EGL_PLATFORM_ANGLE_TYPE_D3D11_ANGLE, EGL_NONE };
    EGLDisplay dpy = eglGetPlatformDisplayEXT(EGL_PLATFORM_ANGLE_ANGLE, (void *)0, dattr);
    EGLint maj, min;
    say("display %p init %d err 0x%x", dpy, eglInitialize(dpy, &maj, &min), eglGetError());
    EGLint cattr[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_NONE };
    EGLConfig cfg;
    EGLint n = 0;
    eglChooseConfig(dpy, cattr, &cfg, 1, &n);
    EGLint ctxattr[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    EGLContext ctx = eglCreateContext(dpy, cfg, NULL, ctxattr);
    EGLint pattr[] = { EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE };
    EGLSurface surf = eglCreatePbufferSurface(dpy, cfg, pattr);
    say("configs %d ctx %p surf %p current %d", n, ctx, surf, eglMakeCurrent(dpy, surf, surf, ctx));
    say("GL_RENDERER %s", glGetString(0x1F01));
    glDebugMessageCallbackKHR_t glDebugMessageCallbackKHR = eglGetProcAddress("glDebugMessageCallbackKHR");
    LOAD(gles, glEnable);
    if (glDebugMessageCallbackKHR) {
        glEnable(0x92E0);
        glEnable(0x8242);
        glDebugMessageCallbackKHR(on_gl_debug, NULL);
    }

    EGLAttrib dev = 0, d3d = 0;
    eglQueryDisplayAttribEXT(dpy, EGL_DEVICE_EXT, &dev);
    eglQueryDeviceAttribEXT((EGLDeviceEXT)dev, EGL_D3D11_DEVICE_ANGLE, &d3d);
    ID3D11Device *angle_dev = (ID3D11Device *)d3d;
    ID3D11Device1 *angle_dev1 = NULL;
    ID3D11Device_QueryInterface(angle_dev, &IID_ID3D11Device1, (void **)&angle_dev1);
    say("ANGLE d3d11 device %p", angle_dev);

    ID3D11Device *other;
    ID3D11DeviceContext *other_ctx;
    D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT, NULL, 0,
                      D3D11_SDK_VERSION, &other, NULL, &other_ctx);

    DXGI_FORMAT fmt = argc > 1 ? atoi(argv[1]) : DXGI_FORMAT_B8G8R8A8_UNORM;
    static const UINT miscs[] = { 0, D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE,
                                  D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE };
    static const char *names[] = { "local", "shared, own device", "NT handle from other device" };
    for (int i = 0; i < 3; i++) {
        D3D11_TEXTURE2D_DESC td = { 64, 64, 1, 1, fmt, { 1, 0 }, D3D11_USAGE_DEFAULT,
                                    D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0, miscs[i] };
        ID3D11Texture2D *tex = NULL;
        HRESULT hr = ID3D11Device_CreateTexture2D(i == 2 ? other : angle_dev, &td, NULL, &tex);
        if (i == 2 && tex) {
            IDXGIResource1 *r1;
            HANDLE h = NULL;
            ID3D11Texture2D_QueryInterface(tex, &IID_IDXGIResource1, (void **)&r1);
            IDXGIResource1_CreateSharedHandle(r1, NULL, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, NULL, &h);
            tex = NULL;
            hr = ID3D11Device1_OpenSharedResource1(angle_dev1, h, &IID_ID3D11Texture2D, (void **)&tex);
        }
        EGLint iattr[] = { EGL_NONE };
        EGLImage img = eglCreateImageKHR(dpy, NULL, EGL_D3D11_TEXTURE_ANGLE, tex, iattr);
        EGLint eerr = eglGetError();
        GLuint gt = 0, fb = 0;
        glGenTextures(1, &gt);
        glBindTexture(0x0DE1, gt);
        glEGLImageTargetTexture2DOES(0x0DE1, img);
        GLenum e1 = glGetError();
        glGenFramebuffers(1, &fb);
        glBindFramebuffer(0x8D40, fb);
        glFramebufferTexture2D(0x8D40, 0x8CE0, 0x0DE1, gt, 0);
        GLenum status = glCheckFramebufferStatus(0x8D40);
        glClearColor(0, 1, 0, 1);
        glClear(0x4000);
        uint32_t px = 0;
        glReadPixels(8, 8, 1, 1, 0x1908, 0x1401, &px);
        GLenum e2 = glGetError();
        say("%-28s create 0x%08lx image %p eglerr 0x%x target glerr 0x%x fbo 0x%x read %08x glerr 0x%x", names[i], hr,
            img, eerr, e1, status, px, e2);
    }
    return 0;
}
