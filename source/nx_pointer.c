/* nx_pointer.c -- on-screen cursor. See nx_pointer.h.
 *
 * Adapted from the Happy Wheels Switch port (MIT), itself a cut-down version
 * of the reusable nx_pointer for so-loader ports. Kept: the cursor.png loader
 * and the GL overlay, including its careful state save/restore -- saving only
 * a vertex attribute's enabled flag is not enough, and getting that wrong
 * costs you every subsequent draw call in the frame.
 *
 * Dropped: the pad handling. input.c owns the toggle chord (ZL+ZR), the stick
 * movement and the tap, because it also has to suppress the on-screen button
 * bindings while the cursor is up.
 *
 * MIT licensed, see LICENSE.
 */
#include <switch.h>
#include <GLES2/gl2.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <setjmp.h>
#include <png.h>

#include "nx_pointer.h"
#include "rv_log.h"
#include "util.h"   /* log_printf */

/* Cursor travel at full stick deflection, in render-space pixels per frame. */
static struct { int screen_w, screen_h; char data_dir[512]; } s_cfg;
static int s_ready = 0;

/* -1 = never shown yet, 0 = hidden, 1 = visible */
static int   s_visible = 0;
static float s_cx, s_cy;

#define logf_(...) log_printf(__VA_ARGS__)
#define cfg_fopen(p, m) fopen((p), (m))
#define cfg_fclose(f)   fclose(f)

#define CURSOR_MAX_DIM 64

/* Raw file bytes, slurped at init (single-threaded) so the render thread never
 * has to touch the filesystem. Decoded + uploaded lazily in nxp_draw(). */
static uint8_t *s_png_bytes = NULL;
static size_t   s_png_len   = 0;

/* decoded */
static GLuint s_cursor_tex = 0;
static int    s_cursor_w = 0, s_cursor_h = 0;
static int    s_png_tried = 0;      /* decode attempted (success or not) */

static void slurp_cursor_png(void) {
  if (!s_cfg.data_dir[0]) return;
  char path[sizeof s_cfg.data_dir + 16];
  snprintf(path, sizeof path, "%s/cursor.png", s_cfg.data_dir);

  FILE *f = cfg_fopen(path, "rb");     /* locked path: engine threads are live */
  if (!f) { logf_("nxp: no cursor.png (using built-in arrow)\n"); return; }

  fseek(f, 0, SEEK_END);
  long len = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (len <= 0 || len > 4 * 1024 * 1024) { cfg_fclose(f); return; }

  s_png_bytes = malloc((size_t)len);
  if (!s_png_bytes) { cfg_fclose(f); return; }
  s_png_len = fread(s_png_bytes, 1, (size_t)len, f);
  cfg_fclose(f);

  if (s_png_len != (size_t)len) { free(s_png_bytes); s_png_bytes = NULL; s_png_len = 0; return; }
  logf_("nxp: cursor.png loaded (%zu bytes), decoding on first frame\n", s_png_len);
}

/* libpng reader over the in-memory buffer */
typedef struct { const uint8_t *p; size_t len, off; } PngSrc;

static void png_read_mem(png_structp png, png_bytep out, png_size_t n) {
  PngSrc *s = (PngSrc *)png_get_io_ptr(png);
  if (s->off + n > s->len) { png_error(png, "short read"); return; }
  memcpy(out, s->p + s->off, n);
  s->off += n;
}

/* Decode to RGBA8 and upload. Returns 1 on success. Render thread only. */
static int cursor_upload_png(void) {
  if (!s_png_bytes) return 0;

  png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
  if (!png) return 0;
  png_infop info = png_create_info_struct(png);
  if (!info) { png_destroy_read_struct(&png, NULL, NULL); return 0; }

  uint8_t   *pixels = NULL;
  png_bytep *rows   = NULL;
  if (setjmp(png_jmpbuf(png))) {          /* libpng error path */
    free(pixels); free(rows);
    png_destroy_read_struct(&png, &info, NULL);
    logf_("nxp: cursor.png decode failed -- using built-in arrow\n");
    return 0;
  }

  PngSrc src = { s_png_bytes, s_png_len, 0 };
  png_set_read_fn(png, &src, png_read_mem);
  png_read_info(png, info);

  const png_uint_32 w = png_get_image_width(png, info);
  const png_uint_32 h = png_get_image_height(png, info);
  if (w == 0 || h == 0 || w > CURSOR_MAX_DIM || h > CURSOR_MAX_DIM) {
    logf_("nxp: cursor.png is %ux%u -- max is %dx%d, using built-in arrow\n",
          (unsigned)w, (unsigned)h, CURSOR_MAX_DIM, CURSOR_MAX_DIM);
    png_destroy_read_struct(&png, &info, NULL);
    return 0;
  }

  /* normalise anything to 8-bit RGBA so transparency always works */
  const int ct = png_get_color_type(png, info);
  const int bd = png_get_bit_depth(png, info);
  if (bd == 16)                       png_set_strip_16(png);
  if (ct == PNG_COLOR_TYPE_PALETTE)   png_set_palette_to_rgb(png);
  if (ct == PNG_COLOR_TYPE_GRAY && bd < 8) png_set_expand_gray_1_2_4_to_8(png);
  if (png_get_valid(png, info, PNG_INFO_tRNS)) png_set_tRNS_to_alpha(png);
  if (ct == PNG_COLOR_TYPE_GRAY || ct == PNG_COLOR_TYPE_GRAY_ALPHA)
    png_set_gray_to_rgb(png);
  /* ensure an alpha channel exists even for opaque RGB */
  png_set_filler(png, 0xFF, PNG_FILLER_AFTER);
  png_read_update_info(png, info);

  const size_t stride = (size_t)w * 4;
  pixels = malloc(stride * h);
  rows   = malloc(sizeof(png_bytep) * h);
  if (!pixels || !rows) { png_error(png, "oom"); }
  for (png_uint_32 y = 0; y < h; y++) rows[y] = pixels + y * stride;
  png_read_image(png, rows);
  png_read_end(png, NULL);
  png_destroy_read_struct(&png, &info, NULL);
  free(rows);

  /* Save the texture binding we are about to disturb -- this runs inside the
   * engine's context, and leaving its texture unbound would corrupt its frame. */
  GLint prev_active = 0, prev_tex = 0;
  glGetIntegerv(GL_ACTIVE_TEXTURE, &prev_active);
  glActiveTexture(GL_TEXTURE0);
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex);

  glGenTextures(1, &s_cursor_tex);
  glBindTexture(GL_TEXTURE_2D, s_cursor_tex);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  GLint prev_align = 4;
  glGetIntegerv(GL_UNPACK_ALIGNMENT, &prev_align);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, (GLsizei)w, (GLsizei)h, 0,
               GL_RGBA, GL_UNSIGNED_BYTE, pixels);
  glPixelStorei(GL_UNPACK_ALIGNMENT, prev_align);   /* engine uploads assume 4 */
  free(pixels);

  glBindTexture(GL_TEXTURE_2D, (GLuint)prev_tex);   /* put the engine's back */
  glActiveTexture((GLenum)prev_active);

  s_cursor_w = (int)w;
  s_cursor_h = (int)h;
  logf_("nxp: custom cursor %dx%d ready (tex=%u)\n", s_cursor_w, s_cursor_h, s_cursor_tex);
  return 1;
}

/* ------------------------------------------------------------------- init */


void nxp_init(int screen_w, int screen_h, const char *data_dir) {
  if (s_ready) return;
  memset(&s_cfg, 0, sizeof s_cfg);
  s_cfg.screen_w = screen_w > 0 ? screen_w : 1280;
  s_cfg.screen_h = screen_h > 0 ? screen_h : 720;
  if (data_dir)
    snprintf(s_cfg.data_dir, sizeof s_cfg.data_dir, "%s", data_dir);

  s_cx = s_cfg.screen_w * 0.5f;
  s_cy = s_cfg.screen_h * 0.5f;

  slurp_cursor_png();          /* read now; decode on the render thread */

  s_ready = 1;
  logf_("nxp: init %dx%d\n", s_cfg.screen_w, s_cfg.screen_h);
}

static void clamp_cursor(void) {
  if (s_cx < 0) s_cx = 0;
  if (s_cy < 0) s_cy = 0;
  if (s_cx > s_cfg.screen_w - 1) s_cx = (float)(s_cfg.screen_w - 1);
  if (s_cy > s_cfg.screen_h - 1) s_cy = (float)(s_cfg.screen_h - 1);
}

/* Docking swaps the surface size. The cursor keeps its relative position so it
 * does not jump to a corner across the change. */
void nxp_set_screen(int w, int h) {
  if (!s_ready || w <= 0 || h <= 0) return;
  if (w == s_cfg.screen_w && h == s_cfg.screen_h) return;
  const float fx = s_cx / (float)s_cfg.screen_w;
  const float fy = s_cy / (float)s_cfg.screen_h;
  s_cfg.screen_w = w;
  s_cfg.screen_h = h;
  s_cx = fx * (float)w;
  s_cy = fy * (float)h;
  clamp_cursor();
}

/* input.c drives these: it owns the toggle chord and the stick. */
void nxp_move(float dx, float dy) {
  if (!s_ready) return;
  s_cx += dx;
  s_cy += dy;
  clamp_cursor();
}

int  nxp_visible(void) { return s_visible > 0; }
void nxp_pos(float *x, float *y) { if (x) *x = s_cx; if (y) *y = s_cy; }
void nxp_set_visible(int on) {
  s_visible = on ? 1 : 0;
  logf_(on ? "cursor: on (left stick moves it, A taps, ZL+ZR hides it)"
           : "cursor: off");
}



/* ======================= GL overlay ====================================== */

static GLuint s_prog = 0;
static GLint  s_u_screen, s_u_origin, s_u_scale, s_u_colour, s_u_tex, s_u_use_tex;
static int    s_gl_failed = 0;

/* Built-in arrow: tip at (0,0), y down, drawn as a fan from the tip. */
static const GLfloat s_arrow[] = {
   0.0f,  0.0f,   0.0f, 16.0f,   4.0f, 12.0f,   7.0f, 18.0f,
  10.0f, 16.5f,   7.0f, 10.5f,  12.0f, 10.0f,
};
#define ARROW_VERTS 7


/* Reports whether the GL side actually came up. Distinguishes "the toggle is
 * not reaching us" from "the cursor is on but nothing is drawn", which are
 * otherwise indistinguishable from the outside. */
int nxp_gl_state(void) {
  if (s_gl_failed) return -1;
  return s_prog ? 1 : 0;
}

static GLuint mkshader(GLenum t, const char *src) {
  GLuint s = glCreateShader(t);
  glShaderSource(s, 1, &src, NULL);
  glCompileShader(s);
  GLint ok = 0;
  glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (!ok) { glDeleteShader(s); return 0; }
  return s;
}

static int gl_init(void) {
  if (s_prog) return 1;
  if (s_gl_failed) return 0;

  static const char *vs =
    "attribute vec2 aPos;\n"
    "attribute vec2 aUV;\n"
    "varying vec2 vUV;\n"
    "uniform vec2 uScreen;\n"
    "uniform vec2 uOrigin;\n"
    "uniform float uScale;\n"
    "void main() {\n"
    "  vUV = aUV;\n"
    "  vec2 p = uOrigin + aPos * uScale;\n"
    "  gl_Position = vec4((p.x/uScreen.x)*2.0-1.0, 1.0-(p.y/uScreen.y)*2.0, 0.0, 1.0);\n"
    "}\n";
  static const char *fs =
    "precision mediump float;\n"
    "varying vec2 vUV;\n"
    "uniform vec4 uColour;\n"
    "uniform sampler2D uTex;\n"
    "uniform float uUseTex;\n"
    "void main() {\n"
    /* Branch on a uniform (uniform control flow -- always safe in GLES2) so the
     * built-in-arrow path never samples a texture. That means we don't have to
     * bind or disturb ANY texture state unless a custom cursor.png is in use. */
    "  if (uUseTex > 0.5) gl_FragColor = texture2D(uTex, vUV);\n"
    "  else               gl_FragColor = uColour;\n"
    "}\n";

  GLuint v = mkshader(GL_VERTEX_SHADER, vs), f = mkshader(GL_FRAGMENT_SHADER, fs);
  if (!v || !f) { s_gl_failed = 1; logf_("nxp: cursor shader compile failed\n"); return 0; }

  GLuint p = glCreateProgram();
  glAttachShader(p, v);
  glAttachShader(p, f);
  glBindAttribLocation(p, 0, "aPos");
  glBindAttribLocation(p, 1, "aUV");
  glLinkProgram(p);
  glDeleteShader(v);
  glDeleteShader(f);

  GLint ok = 0;
  glGetProgramiv(p, GL_LINK_STATUS, &ok);
  if (!ok) { glDeleteProgram(p); s_gl_failed = 1; logf_("nxp: cursor link failed\n"); return 0; }

  s_prog      = p;
  s_u_screen  = glGetUniformLocation(p, "uScreen");
  s_u_origin  = glGetUniformLocation(p, "uOrigin");
  s_u_scale   = glGetUniformLocation(p, "uScale");
  s_u_colour  = glGetUniformLocation(p, "uColour");
  s_u_tex     = glGetUniformLocation(p, "uTex");
  s_u_use_tex = glGetUniformLocation(p, "uUseTex");
  return 1;
}

/* A vertex attribute's FULL state. Saving only the enabled flag (as we did at
 * first) is not enough: glVertexAttribPointer also records size/type/stride/
 * pointer AND which ARRAY_BUFFER was bound at the time. The engine sets its
 * attribute pointers once and reuses them across frames, so overwriting slot 0
 * with our arrow array left every subsequent engine draw reading garbage
 * geometry -- 25 draw calls a frame, and a black screen. */
typedef struct {
  GLint enabled, size, type, norm, stride, buf;
  void *ptr;
} AttribState;

static void attrib_save(GLuint i, AttribState *a) {
  glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &a->enabled);
  glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_SIZE, &a->size);
  glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_TYPE, &a->type);
  glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED, &a->norm);
  glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &a->stride);
  glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &a->buf);
  glGetVertexAttribPointerv(i, GL_VERTEX_ATTRIB_ARRAY_POINTER, &a->ptr);
}

static void attrib_restore(GLuint i, const AttribState *a) {
  /* glVertexAttribPointer captures whatever ARRAY_BUFFER is bound right now, so
   * re-bind the attribute's original buffer before restoring its pointer. */
  glBindBuffer(GL_ARRAY_BUFFER, (GLuint)a->buf);
  if (a->size > 0)
    glVertexAttribPointer(i, a->size, (GLenum)a->type,
                          (GLboolean)(a->norm ? GL_TRUE : GL_FALSE),
                          a->stride, a->ptr);
  if (a->enabled) glEnableVertexAttribArray(i);
  else            glDisableVertexAttribArray(i);
}

void nxp_draw(void) {
  if (!s_ready || s_visible <= 0) return;
  if (!gl_init()) return;

  /* decode + upload cursor.png on first draw (needs a live GL context) */
  if (!s_png_tried) {
    s_png_tried = 1;
    if (s_png_bytes) {
      if (!cursor_upload_png()) { s_cursor_tex = 0; }
      free(s_png_bytes); s_png_bytes = NULL; s_png_len = 0;   /* done with the bytes */
    }
  }

  /* ---- save every bit of state we touch ---- */
  AttribState a0, a1;
  attrib_save(0, &a0);
  attrib_save(1, &a1);

  GLint prev_prog = 0, prev_buf = 0;
  GLint bs_rgb = 0, bd_rgb = 0, bs_a = 0, bd_a = 0;
  glGetIntegerv(GL_CURRENT_PROGRAM, &prev_prog);
  glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prev_buf);
  glGetIntegerv(GL_BLEND_SRC_RGB, &bs_rgb);
  glGetIntegerv(GL_BLEND_DST_RGB, &bd_rgb);
  glGetIntegerv(GL_BLEND_SRC_ALPHA, &bs_a);
  glGetIntegerv(GL_BLEND_DST_ALPHA, &bd_a);
  const GLboolean was_blend   = glIsEnabled(GL_BLEND);
  const GLboolean was_depth   = glIsEnabled(GL_DEPTH_TEST);
  const GLboolean was_cull    = glIsEnabled(GL_CULL_FACE);
  const GLboolean was_scissor = glIsEnabled(GL_SCISSOR_TEST);

  glDisable(GL_DEPTH_TEST);
  glDisable(GL_CULL_FACE);
  glDisable(GL_SCISSOR_TEST);
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);   /* honours PNG alpha */

  glUseProgram(s_prog);
  glUniform2f(s_u_screen, (GLfloat)s_cfg.screen_w, (GLfloat)s_cfg.screen_h);
  glUniform2f(s_u_origin, s_cx, s_cy);

  if (s_cursor_tex) {
    /* ---- custom PNG: textured quad, hotspot at the top-left ---- */
    const GLfloat w = (GLfloat)s_cursor_w, h = (GLfloat)s_cursor_h;
    const GLfloat quad[] = { 0,0,  w,0,  0,h,  w,h };
    const GLfloat uv[]   = { 0,0,  1,0,  0,1,  1,1 };

    /* Texture units: read the ACTIVE unit first, then switch to unit 0 and read
     * ITS binding. (Reading GL_TEXTURE_BINDING_2D before switching gives you the
     * active unit's binding, and restoring that onto unit 0 corrupts unit 0.) */
    GLint prev_active = 0, tex0 = 0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &prev_active);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex0);

    glBindBuffer(GL_ARRAY_BUFFER, 0);                  /* client-side arrays */
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, quad);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, uv);

    glBindTexture(GL_TEXTURE_2D, s_cursor_tex);
    glUniform1i(s_u_tex, 0);
    glUniform1f(s_u_use_tex, 1.0f);
    glUniform1f(s_u_scale, (GLfloat)s_cfg.screen_h / 1080.0f);
    glUniform4f(s_u_colour, 1, 1, 1, 1);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    glBindTexture(GL_TEXTURE_2D, (GLuint)tex0);        /* restore unit 0 */
    glActiveTexture((GLenum)prev_active);              /* then the active unit */
  } else {
    /* ---- built-in arrow: solid colour, no texture touched at all ---- */
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, s_arrow);
    glUniform1f(s_u_use_tex, 0.0f);                    /* shader will not sample */

    const GLfloat sc = 2.4f * ((GLfloat)s_cfg.screen_h / 1080.0f);
    glUniform1f(s_u_scale, sc * 1.22f);
    glUniform4f(s_u_colour, 0.0f, 0.0f, 0.0f, 0.85f);  /* outline */
    glDrawArrays(GL_TRIANGLE_FAN, 0, ARROW_VERTS);

    glUniform1f(s_u_scale, sc);
    glUniform4f(s_u_colour, 1.0f, 1.0f, 1.0f, 1.0f);   /* fill */
    glDrawArrays(GL_TRIANGLE_FAN, 0, ARROW_VERTS);
  }

  /* ---- restore ----
   * Attributes first (each re-binds its own source buffer), then put the global
   * ARRAY_BUFFER binding back, then the program and the fixed-function bits. */
  attrib_restore(0, &a0);
  attrib_restore(1, &a1);
  glBindBuffer(GL_ARRAY_BUFFER, (GLuint)prev_buf);
  glUseProgram((GLuint)prev_prog);
  glBlendFuncSeparate((GLenum)bs_rgb, (GLenum)bd_rgb, (GLenum)bs_a, (GLenum)bd_a);
  if (!was_blend) glDisable(GL_BLEND); else glEnable(GL_BLEND);
  if (was_depth)   glEnable(GL_DEPTH_TEST);
  if (was_cull)    glEnable(GL_CULL_FACE);
  if (was_scissor) glEnable(GL_SCISSOR_TEST);
}
