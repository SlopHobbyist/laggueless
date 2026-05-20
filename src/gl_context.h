#ifndef ME_GL_CONTEXT_H
#define ME_GL_CONTEXT_H

/* Hidden-window OpenGL context for HW-accelerated libretro cores.

   The emulator window itself is owned by the D3D11 presenter; we keep GL
   entirely off to the side on a tiny invisible HWND. The core renders into
   an FBO (set up in step 3); we transport the result into the D3D11
   presenter (step 4). This module owns just the context + proc loading. */

/* Create a GL context.
     core_profile: 1 = request a core-profile context (RETRO_HW_CONTEXT_OPENGL_CORE),
                   0 = request a compatibility context (RETRO_HW_CONTEXT_OPENGL).
     major, minor: minimum GL version the core asked for. Pass 0,0 to let the
                   driver pick whatever it gives back from a legacy create.
   Returns 0 on success, nonzero on failure. */
int me_gl_init(int core_profile, unsigned major, unsigned minor);

/* Make our context current on this thread. Safe to call repeatedly. */
int me_gl_make_current(void);

/* Resolve a GL entry point by name. Mirrors retro_hw_get_proc_address_t:
   tries wglGetProcAddress first (for everything >= GL 1.2 and all extensions),
   then GetProcAddress on opengl32.dll (for the GL 1.0/1.1 entry points that
   WGL won't return). Returns NULL if the symbol doesn't exist. */
void *me_gl_get_proc_address(const char *name);

/* Create the render target the core will draw into.
     max_w, max_h: must be >= the largest framebuffer the core will ever use,
                   as reported by retro_get_system_av_info().geometry.max_*.
     depth:        attach a 24-bit depth buffer (cores set this in
                   retro_hw_render_callback::depth).
     stencil:      attach 8 bits of stencil — combined with depth becomes
                   GL_DEPTH24_STENCIL8.
   Returns 0 on success. The FBO id is exposed via me_gl_fbo_id(); the color
   texture id via me_gl_fbo_color_tex(). */
int me_gl_fbo_create(unsigned max_w, unsigned max_h, int depth, int stencil);

/* The FBO id to pass to the core via get_current_framebuffer. */
unsigned me_gl_fbo_id(void);

/* The color attachment texture id — Step 4 reads pixels out of this. */
unsigned me_gl_fbo_color_tex(void);

/* Read color attachment back to CPU as BGRA bytes, top-down. Used by the
   readback transport path (Step 4). out_buf must hold w*h*4 bytes. */
void me_gl_fbo_readback_bgra(unsigned w, unsigned h, void *out_buf);

/* Tear everything down. */
void me_gl_shutdown(void);

#endif
