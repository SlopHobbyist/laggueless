#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include "gl_context.h"

/* WGL ARB context-creation constants. We don't pull in wglext.h because
   MinGW's headers are inconsistent across distributions — these are
   stable and tiny. */
#define WGL_CONTEXT_MAJOR_VERSION_ARB             0x2091
#define WGL_CONTEXT_MINOR_VERSION_ARB             0x2092
#define WGL_CONTEXT_PROFILE_MASK_ARB              0x9126
#define WGL_CONTEXT_CORE_PROFILE_BIT_ARB          0x00000001
#define WGL_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB 0x00000002
#define WGL_CONTEXT_FLAGS_ARB                     0x2094
#define WGL_CONTEXT_DEBUG_BIT_ARB                 0x00000001

typedef HGLRC (WINAPI *PFN_wglCreateContextAttribsARB)(HDC, HGLRC, const int *);

/* GL enums + entry points we use ourselves (for FBO setup + readback). The
   core has its own loader for everything else; we just need enough to build
   and read from the render target. */
#define GL_FRAMEBUFFER             0x8D40
#define GL_READ_FRAMEBUFFER        0x8CA8
#define GL_DRAW_FRAMEBUFFER        0x8CA9
#define GL_RENDERBUFFER            0x8D41
#define GL_COLOR_ATTACHMENT0       0x8CE0
#define GL_DEPTH_ATTACHMENT        0x8D00
#define GL_DEPTH_STENCIL_ATTACHMENT 0x821A
#define GL_DEPTH_COMPONENT24       0x81A6
#define GL_DEPTH24_STENCIL8        0x88F0
#define GL_FRAMEBUFFER_COMPLETE    0x8CD5
#define GL_TEXTURE_2D              0x0DE1
#define GL_RGBA                    0x1908
#define GL_RGBA8                   0x8058
#define GL_BGRA                    0x80E1
#define GL_UNSIGNED_BYTE           0x1401
#define GL_NEAREST                 0x2600
#define GL_TEXTURE_MIN_FILTER      0x2801
#define GL_TEXTURE_MAG_FILTER      0x2800
#define GL_PACK_ALIGNMENT          0x0D05
#define GL_UNPACK_ALIGNMENT        0x0CF5

typedef unsigned GLuint;
typedef int      GLint;
typedef unsigned GLenum;
typedef int      GLsizei;
typedef void     GLvoid;

typedef void (APIENTRY *PFN_glGenTextures)(GLsizei, GLuint *);
typedef void (APIENTRY *PFN_glDeleteTextures)(GLsizei, const GLuint *);
typedef void (APIENTRY *PFN_glBindTexture)(GLenum, GLuint);
typedef void (APIENTRY *PFN_glTexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const GLvoid *);
typedef void (APIENTRY *PFN_glTexParameteri)(GLenum, GLenum, GLint);
typedef void (APIENTRY *PFN_glPixelStorei)(GLenum, GLint);
typedef void (APIENTRY *PFN_glReadPixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, GLvoid *);
typedef void (APIENTRY *PFN_glFinish)(void);
typedef GLenum (APIENTRY *PFN_glGetError)(void);
typedef void (APIENTRY *PFN_glGenFramebuffers)(GLsizei, GLuint *);
typedef void (APIENTRY *PFN_glDeleteFramebuffers)(GLsizei, const GLuint *);
typedef void (APIENTRY *PFN_glBindFramebuffer)(GLenum, GLuint);
typedef void (APIENTRY *PFN_glFramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);
typedef void (APIENTRY *PFN_glFramebufferRenderbuffer)(GLenum, GLenum, GLenum, GLuint);
typedef void (APIENTRY *PFN_glGenRenderbuffers)(GLsizei, GLuint *);
typedef void (APIENTRY *PFN_glDeleteRenderbuffers)(GLsizei, const GLuint *);
typedef void (APIENTRY *PFN_glBindRenderbuffer)(GLenum, GLuint);
typedef void (APIENTRY *PFN_glRenderbufferStorage)(GLenum, GLenum, GLsizei, GLsizei);
typedef GLenum (APIENTRY *PFN_glCheckFramebufferStatus)(GLenum);

static struct {
    PFN_glGenTextures            GenTextures;
    PFN_glDeleteTextures         DeleteTextures;
    PFN_glBindTexture            BindTexture;
    PFN_glTexImage2D             TexImage2D;
    PFN_glTexParameteri          TexParameteri;
    PFN_glPixelStorei            PixelStorei;
    PFN_glReadPixels             ReadPixels;
    PFN_glFinish                 Finish;
    PFN_glGetError               GetError;
    PFN_glGenFramebuffers        GenFramebuffers;
    PFN_glDeleteFramebuffers     DeleteFramebuffers;
    PFN_glBindFramebuffer        BindFramebuffer;
    PFN_glFramebufferTexture2D   FramebufferTexture2D;
    PFN_glFramebufferRenderbuffer FramebufferRenderbuffer;
    PFN_glGenRenderbuffers       GenRenderbuffers;
    PFN_glDeleteRenderbuffers    DeleteRenderbuffers;
    PFN_glBindRenderbuffer       BindRenderbuffer;
    PFN_glRenderbufferStorage    RenderbufferStorage;
    PFN_glCheckFramebufferStatus CheckFramebufferStatus;
} gl;

static GLuint g_fbo = 0, g_color_tex = 0, g_depth_rbo = 0;
static unsigned g_fbo_w = 0, g_fbo_h = 0;

/* WGL_NV_DX_interop2 — runtime-resolved.

   The flow:
     1. wglDXOpenDeviceNV(d3d_device) -> opaque interop device handle
     2. Generate a fresh GL texture name (g_interop_gl_tex)
     3. wglDXRegisterObjectNV(dev, d3d_tex, gl_tex, GL_TEXTURE_2D,
                              WGL_ACCESS_WRITE_DISCARD_NV) -> object handle
     4. Detach the old color texture from the FBO, attach gl_tex
     5. Each frame: LockObjects -> render -> UnlockObjects

   When unlocked, D3D11 can sample gl_tex's backing memory via the SRV
   it created earlier; when locked, GL has exclusive access. */
#define WGL_ACCESS_READ_ONLY_NV     0x00000000
#define WGL_ACCESS_READ_WRITE_NV    0x00000001
#define WGL_ACCESS_WRITE_DISCARD_NV 0x00000002

typedef HANDLE (WINAPI *PFN_wglDXOpenDeviceNV)(void *);
typedef BOOL   (WINAPI *PFN_wglDXCloseDeviceNV)(HANDLE);
typedef HANDLE (WINAPI *PFN_wglDXRegisterObjectNV)(HANDLE, void *, GLuint, GLenum, GLenum);
typedef BOOL   (WINAPI *PFN_wglDXUnregisterObjectNV)(HANDLE, HANDLE);
typedef BOOL   (WINAPI *PFN_wglDXLockObjectsNV)(HANDLE, GLint, HANDLE *);
typedef BOOL   (WINAPI *PFN_wglDXUnlockObjectsNV)(HANDLE, GLint, HANDLE *);

static struct {
    PFN_wglDXOpenDeviceNV       Open;
    PFN_wglDXCloseDeviceNV      Close;
    PFN_wglDXRegisterObjectNV   Register;
    PFN_wglDXUnregisterObjectNV Unregister;
    PFN_wglDXLockObjectsNV      Lock;
    PFN_wglDXUnlockObjectsNV    Unlock;
} wgldx;

static HANDLE g_interop_dev      = NULL;  /* from wglDXOpenDeviceNV */
static HANDLE g_interop_object   = NULL;  /* from wglDXRegisterObjectNV */
static GLuint g_interop_gl_tex   = 0;     /* GL name bound to the D3D11 tex */
static int    g_interop_active   = 0;
static int    g_interop_locked   = 0;

static HMODULE g_opengl32 = NULL;
static HWND    g_hwnd     = NULL;
static HDC     g_hdc      = NULL;
static HGLRC   g_glrc     = NULL;
static ATOM    g_class    = 0;
static const wchar_t *g_class_name = L"me_gl_hidden";

static LRESULT CALLBACK gl_wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    return DefWindowProcW(h, m, w, l);
}

static int register_class(void) {
    if (g_class) return 1;
    WNDCLASSW wc = {0};
    wc.lpfnWndProc   = gl_wndproc;
    wc.hInstance     = GetModuleHandleW(NULL);
    wc.lpszClassName = g_class_name;
    g_class = RegisterClassW(&wc);
    if (!g_class) {
        fprintf(stderr, "[gl] RegisterClass failed (err=%lu)\n", GetLastError());
        return 0;
    }
    return 1;
}

/* Set a generic, GL-friendly pixel format on the given HDC. We render to an
   FBO so the default framebuffer is throwaway — 32-bit color, no depth/
   stencil/aux. The core's FBO will get its own depth attachment via
   retro_hw_render_callback::depth. */
static int set_pixel_format(HDC hdc) {
    PIXELFORMATDESCRIPTOR pfd = {0};
    pfd.nSize        = sizeof(pfd);
    pfd.nVersion     = 1;
    pfd.dwFlags      = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType   = PFD_TYPE_RGBA;
    pfd.cColorBits   = 32;
    pfd.iLayerType   = PFD_MAIN_PLANE;
    int pf = ChoosePixelFormat(hdc, &pfd);
    if (!pf) {
        fprintf(stderr, "[gl] ChoosePixelFormat failed (err=%lu)\n", GetLastError());
        return 0;
    }
    if (!SetPixelFormat(hdc, pf, &pfd)) {
        fprintf(stderr, "[gl] SetPixelFormat failed (err=%lu)\n", GetLastError());
        return 0;
    }
    return 1;
}

int me_gl_init(int core_profile, unsigned major, unsigned minor) {
    if (g_glrc) return 0;  /* idempotent */

    g_opengl32 = LoadLibraryW(L"opengl32.dll");
    if (!g_opengl32) {
        fprintf(stderr, "[gl] LoadLibrary opengl32.dll failed\n");
        return 1;
    }

    if (!register_class()) return 1;

    /* 1×1 invisible popup window — we never show it, never pump it. WGL
       just needs a real HWND with a pixel format set on its DC. */
    g_hwnd = CreateWindowExW(0, g_class_name, L"", WS_POPUP,
                             0, 0, 1, 1, NULL, NULL,
                             GetModuleHandleW(NULL), NULL);
    if (!g_hwnd) {
        fprintf(stderr, "[gl] CreateWindow failed (err=%lu)\n", GetLastError());
        return 1;
    }
    g_hdc = GetDC(g_hwnd);
    if (!g_hdc) {
        fprintf(stderr, "[gl] GetDC failed\n");
        return 1;
    }
    if (!set_pixel_format(g_hdc)) return 1;

    /* Bootstrap: create a legacy context just so we can call
       wglGetProcAddress to find wglCreateContextAttribsARB, then throw it
       away and create the real (versioned, optionally core-profile) one. */
    HGLRC tmp = wglCreateContext(g_hdc);
    if (!tmp) {
        fprintf(stderr, "[gl] wglCreateContext (temp) failed (err=%lu)\n", GetLastError());
        return 1;
    }
    if (!wglMakeCurrent(g_hdc, tmp)) {
        fprintf(stderr, "[gl] wglMakeCurrent (temp) failed (err=%lu)\n", GetLastError());
        wglDeleteContext(tmp);
        return 1;
    }

    PFN_wglCreateContextAttribsARB wglCreateContextAttribsARB =
        (PFN_wglCreateContextAttribsARB)wglGetProcAddress("wglCreateContextAttribsARB");

    if (wglCreateContextAttribsARB) {
        /* Default to GL 3.3 if the core didn't specify. mupen64plus-next
           passes 0/0 for "I don't care", but a core-profile context refuses
           to come up unless you ask for >= 3.2. */
        unsigned want_major = major ? major : 3;
        unsigned want_minor = (major == 0) ? 3 : minor;
        int profile_bit = core_profile
            ? WGL_CONTEXT_CORE_PROFILE_BIT_ARB
            : WGL_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB;
        int attribs[] = {
            WGL_CONTEXT_MAJOR_VERSION_ARB, (int)want_major,
            WGL_CONTEXT_MINOR_VERSION_ARB, (int)want_minor,
            WGL_CONTEXT_PROFILE_MASK_ARB,  profile_bit,
            0
        };
        g_glrc = wglCreateContextAttribsARB(g_hdc, NULL, attribs);
        if (!g_glrc) {
            fprintf(stderr,
                    "[gl] wglCreateContextAttribsARB(%u.%u %s) failed (err=%lu); "
                    "falling back to legacy context\n",
                    want_major, want_minor,
                    core_profile ? "core" : "compat",
                    GetLastError());
        }
    } else {
        fprintf(stderr, "[gl] wglCreateContextAttribsARB unavailable; using legacy context\n");
    }

    wglMakeCurrent(NULL, NULL);
    if (g_glrc) {
        wglDeleteContext(tmp);
    } else {
        /* Keep the legacy context as a last resort. Compat-profile cores
           that don't need >=3.2 features may still work. */
        g_glrc = tmp;
    }

    if (!wglMakeCurrent(g_hdc, g_glrc)) {
        fprintf(stderr, "[gl] wglMakeCurrent (final) failed (err=%lu)\n", GetLastError());
        return 1;
    }

    /* Log the version we actually got. glGetString is a GL 1.0 entry point
       in opengl32.dll so it's always linkable. */
    typedef const unsigned char *(WINAPI *PFN_glGetString)(unsigned);
    PFN_glGetString p_glGetString =
        (PFN_glGetString)GetProcAddress(g_opengl32, "glGetString");
    if (p_glGetString) {
        const unsigned char *ver = p_glGetString(0x1F02 /* GL_VERSION */);
        const unsigned char *ren = p_glGetString(0x1F01 /* GL_RENDERER */);
        fprintf(stderr, "[gl] GL_VERSION = %s\n", ver ? (const char *)ver : "?");
        fprintf(stderr, "[gl] GL_RENDERER = %s\n", ren ? (const char *)ren : "?");
        fflush(stderr);
    }

    return 0;
}

int me_gl_make_current(void) {
    if (!g_glrc) return 1;
    if (!wglMakeCurrent(g_hdc, g_glrc)) {
        fprintf(stderr, "[gl] wglMakeCurrent failed (err=%lu)\n", GetLastError());
        return 1;
    }
    return 0;
}

void *me_gl_get_proc_address(const char *name) {
    if (!name) return NULL;
    /* wglGetProcAddress requires a current context — caller is responsible
       for that (the core calls this from inside retro_run / context_reset,
       and we make our context current before either). */
    PROC p = wglGetProcAddress(name);
    /* wglGetProcAddress returns 1, 2, 3, or -1 for "not found" on some
       drivers — only a 0 return is universally "not found", but NULL/-1
       both mean "fall back to GetProcAddress". */
    if (p == NULL || p == (PROC)0x1 || p == (PROC)0x2 ||
        p == (PROC)0x3 || p == (PROC)-1) {
        if (g_opengl32) p = GetProcAddress(g_opengl32, name);
    }
    return (void *)p;
}

static int load_gl_funcs(void) {
    /* Idempotent — me_gl_fbo_create may be called more than once if the
       core triggers a context-reset rebuild. */
    if (gl.GenFramebuffers) return 1;
#define LOAD(field, name) do { \
        gl.field = (PFN_gl##field)me_gl_get_proc_address("gl" #field); \
        if (!gl.field) { fprintf(stderr, "[gl] missing entry point gl%s\n", #field); return 0; } \
    } while (0)
    LOAD(GenTextures, GenTextures);
    LOAD(DeleteTextures, DeleteTextures);
    LOAD(BindTexture, BindTexture);
    LOAD(TexImage2D, TexImage2D);
    LOAD(TexParameteri, TexParameteri);
    LOAD(PixelStorei, PixelStorei);
    LOAD(ReadPixels, ReadPixels);
    LOAD(Finish, Finish);
    LOAD(GetError, GetError);
    LOAD(GenFramebuffers, GenFramebuffers);
    LOAD(DeleteFramebuffers, DeleteFramebuffers);
    LOAD(BindFramebuffer, BindFramebuffer);
    LOAD(FramebufferTexture2D, FramebufferTexture2D);
    LOAD(FramebufferRenderbuffer, FramebufferRenderbuffer);
    LOAD(GenRenderbuffers, GenRenderbuffers);
    LOAD(DeleteRenderbuffers, DeleteRenderbuffers);
    LOAD(BindRenderbuffer, BindRenderbuffer);
    LOAD(RenderbufferStorage, RenderbufferStorage);
    LOAD(CheckFramebufferStatus, CheckFramebufferStatus);
#undef LOAD
    return 1;
}

int me_gl_fbo_create(unsigned max_w, unsigned max_h, int depth, int stencil) {
    if (!g_glrc) { fprintf(stderr, "[gl] fbo_create called before init\n"); return 1; }
    if (me_gl_make_current() != 0) return 1;
    if (!load_gl_funcs()) return 1;

    /* Tear down any previous FBO (recreated on geometry change / context reset). */
    if (g_fbo)       { gl.DeleteFramebuffers(1, &g_fbo);       g_fbo = 0; }
    if (g_color_tex) { gl.DeleteTextures(1, &g_color_tex);     g_color_tex = 0; }
    if (g_depth_rbo) { gl.DeleteRenderbuffers(1, &g_depth_rbo); g_depth_rbo = 0; }

    gl.GenTextures(1, &g_color_tex);
    gl.BindTexture(GL_TEXTURE_2D, g_color_tex);
    gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)max_w, (GLsizei)max_h,
                  0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    /* Nearest filtering: irrelevant for the readback path (we don't sample
       this texture in GL), but matters for interop later where D3D11 may
       sample it directly. Nearest matches our integer-scaling philosophy. */
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    gl.BindTexture(GL_TEXTURE_2D, 0);

    if (depth) {
        gl.GenRenderbuffers(1, &g_depth_rbo);
        gl.BindRenderbuffer(GL_RENDERBUFFER, g_depth_rbo);
        gl.RenderbufferStorage(GL_RENDERBUFFER,
                               stencil ? GL_DEPTH24_STENCIL8 : GL_DEPTH_COMPONENT24,
                               (GLsizei)max_w, (GLsizei)max_h);
        gl.BindRenderbuffer(GL_RENDERBUFFER, 0);
    }

    gl.GenFramebuffers(1, &g_fbo);
    gl.BindFramebuffer(GL_FRAMEBUFFER, g_fbo);
    gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                            GL_TEXTURE_2D, g_color_tex, 0);
    if (g_depth_rbo) {
        gl.FramebufferRenderbuffer(GL_FRAMEBUFFER,
                                   stencil ? GL_DEPTH_STENCIL_ATTACHMENT : GL_DEPTH_ATTACHMENT,
                                   GL_RENDERBUFFER, g_depth_rbo);
    }

    GLenum status = gl.CheckFramebufferStatus(GL_FRAMEBUFFER);
    gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "[gl] FBO incomplete: status=0x%04x\n", status);
        return 1;
    }
    g_fbo_w = max_w; g_fbo_h = max_h;
    fprintf(stderr, "[gl] FBO ready: %ux%u color=RGBA8%s%s fbo_id=%u tex_id=%u\n",
            max_w, max_h,
            depth   ? " depth24" : "",
            stencil ? "+stencil8" : "",
            g_fbo, g_color_tex);
    fflush(stderr);
    return 0;
}

unsigned me_gl_fbo_id(void)        { return g_fbo; }
unsigned me_gl_fbo_color_tex(void) { return g_color_tex; }

void me_gl_fbo_readback_bgra(unsigned w, unsigned h, void *out_buf) {
    if (!g_fbo || !out_buf) return;
    /* glReadPixels from the FBO directly — no Bind to default framebuffer
       needed. Pixels come out bottom-up (GL convention); the D3D11
       presenter flips Y via its shader (Step 5). */
    gl.BindFramebuffer(GL_READ_FRAMEBUFFER, g_fbo);
    gl.PixelStorei(GL_PACK_ALIGNMENT, 4);
    gl.ReadPixels(0, 0, (GLsizei)w, (GLsizei)h, GL_BGRA, GL_UNSIGNED_BYTE, out_buf);
    gl.BindFramebuffer(GL_READ_FRAMEBUFFER, 0);
}

int me_gl_interop_attach(void *d3d_device, void *d3d_tex) {
    if (g_interop_active) return 0;
    if (!g_glrc || !g_fbo || !d3d_device || !d3d_tex) return 1;
    if (me_gl_make_current() != 0) return 1;

    /* Resolve the six WGL_NV_DX_interop2 entry points up front. wglGet
       returns NULL when the driver doesn't support the extension. */
    wgldx.Open       = (PFN_wglDXOpenDeviceNV)      me_gl_get_proc_address("wglDXOpenDeviceNV");
    wgldx.Close      = (PFN_wglDXCloseDeviceNV)     me_gl_get_proc_address("wglDXCloseDeviceNV");
    wgldx.Register   = (PFN_wglDXRegisterObjectNV)  me_gl_get_proc_address("wglDXRegisterObjectNV");
    wgldx.Unregister = (PFN_wglDXUnregisterObjectNV)me_gl_get_proc_address("wglDXUnregisterObjectNV");
    wgldx.Lock       = (PFN_wglDXLockObjectsNV)     me_gl_get_proc_address("wglDXLockObjectsNV");
    wgldx.Unlock     = (PFN_wglDXUnlockObjectsNV)   me_gl_get_proc_address("wglDXUnlockObjectsNV");
    if (!wgldx.Open || !wgldx.Register || !wgldx.Lock || !wgldx.Unlock) {
        fprintf(stderr, "[gl] WGL_NV_DX_interop2 not available on this driver\n");
        return 1;
    }

    g_interop_dev = wgldx.Open(d3d_device);
    if (!g_interop_dev) {
        fprintf(stderr, "[gl] wglDXOpenDeviceNV failed (err=%lu)\n", GetLastError());
        return 1;
    }

    /* Fresh GL texture name — the WGL extension wants an unused name; it
       takes ownership and binds it to the D3D11 texture's memory. We do
       NOT call glTexImage2D on it. */
    gl.GenTextures(1, &g_interop_gl_tex);

    g_interop_object = wgldx.Register(g_interop_dev, d3d_tex, g_interop_gl_tex,
                                      GL_TEXTURE_2D, WGL_ACCESS_WRITE_DISCARD_NV);
    if (!g_interop_object) {
        fprintf(stderr, "[gl] wglDXRegisterObjectNV failed (err=%lu)\n", GetLastError());
        gl.DeleteTextures(1, &g_interop_gl_tex); g_interop_gl_tex = 0;
        wgldx.Close(g_interop_dev); g_interop_dev = NULL;
        return 1;
    }

    /* Swap the FBO's color attachment over to the interop texture. The
       core's get_current_framebuffer keeps returning g_fbo; what's behind
       the attachment changes underneath it.

       Spec requires the texture to be LOCKED before any GL operation that
       touches it, INCLUDING framebufferTexture2D. So lock briefly. */
    HANDLE objs[1] = { g_interop_object };
    if (!wgldx.Lock(g_interop_dev, 1, objs)) {
        fprintf(stderr, "[gl] wglDXLockObjectsNV (attach) failed (err=%lu)\n", GetLastError());
        wgldx.Unregister(g_interop_dev, g_interop_object); g_interop_object = NULL;
        gl.DeleteTextures(1, &g_interop_gl_tex); g_interop_gl_tex = 0;
        wgldx.Close(g_interop_dev); g_interop_dev = NULL;
        return 1;
    }
    gl.BindFramebuffer(GL_FRAMEBUFFER, g_fbo);
    gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                            GL_TEXTURE_2D, g_interop_gl_tex, 0);
    GLenum st = gl.CheckFramebufferStatus(GL_FRAMEBUFFER);
    gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
    wgldx.Unlock(g_interop_dev, 1, objs);

    if (st != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "[gl] interop FBO incomplete after attach (0x%04x)\n", st);
        wgldx.Unregister(g_interop_dev, g_interop_object); g_interop_object = NULL;
        gl.DeleteTextures(1, &g_interop_gl_tex); g_interop_gl_tex = 0;
        wgldx.Close(g_interop_dev); g_interop_dev = NULL;
        /* Restore old color attachment so the readback path still works. */
        gl.BindFramebuffer(GL_FRAMEBUFFER, g_fbo);
        gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                GL_TEXTURE_2D, g_color_tex, 0);
        gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
        return 1;
    }

    g_interop_active = 1;
    fprintf(stderr, "[gl] WGL_NV_DX_interop2 enabled (zero-copy transport)\n");
    fflush(stderr);
    return 0;
}

void me_gl_interop_lock(void) {
    if (!g_interop_active || g_interop_locked) return;
    HANDLE objs[1] = { g_interop_object };
    if (wgldx.Lock(g_interop_dev, 1, objs)) g_interop_locked = 1;
}

void me_gl_interop_unlock(void) {
    if (!g_interop_active || !g_interop_locked) return;
    HANDLE objs[1] = { g_interop_object };
    if (wgldx.Unlock(g_interop_dev, 1, objs)) g_interop_locked = 0;
}

int me_gl_interop_active(void) { return g_interop_active; }

void me_gl_shutdown(void) {
    if (g_glrc && gl.DeleteFramebuffers) {
        wglMakeCurrent(g_hdc, g_glrc);
        if (g_interop_active) {
            if (g_interop_locked) me_gl_interop_unlock();
            if (g_interop_object) wgldx.Unregister(g_interop_dev, g_interop_object);
            if (g_interop_gl_tex) gl.DeleteTextures(1, &g_interop_gl_tex);
            if (g_interop_dev)    wgldx.Close(g_interop_dev);
            g_interop_object = NULL; g_interop_gl_tex = 0; g_interop_dev = NULL;
            g_interop_active = 0;
        }
        if (g_fbo)       { gl.DeleteFramebuffers(1, &g_fbo);       g_fbo = 0; }
        if (g_color_tex) { gl.DeleteTextures(1, &g_color_tex);     g_color_tex = 0; }
        if (g_depth_rbo) { gl.DeleteRenderbuffers(1, &g_depth_rbo); g_depth_rbo = 0; }
    }
    if (g_glrc) { wglMakeCurrent(NULL, NULL); wglDeleteContext(g_glrc); g_glrc = NULL; }
    if (g_hdc)  { ReleaseDC(g_hwnd, g_hdc); g_hdc = NULL; }
    if (g_hwnd) { DestroyWindow(g_hwnd); g_hwnd = NULL; }
    /* Don't UnregisterClass — process is exiting anyway, and re-init paths
       would just need to re-register. Don't FreeLibrary opengl32 either. */
}
