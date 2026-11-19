/*
 * opengl.c — the shared desktop OpenGL presenter.
 *
 * util.h has declared `OpenGLRenderer_Create(struct RendererFuncs *)` for a
 * long time, and mmx23_host_main.inc calls it, but the framework shipped no
 * implementation: every port carried its own copy. Seven of them did, at
 * ~225 lines each, differing only in the name of a per-game viewport helper.
 *
 * That is the shape a shared fix cannot reach, and the cost was already on
 * the board: one port's copy computed
 *
 *     int viewport_y = (viewport_height - viewport_height) >> 1;
 *
 * which is zero for every window size, so its picture never centred
 * vertically. Six other copies had the line right. Nothing could tell them
 * apart because nothing compared them.
 *
 * Presentation policy comes from the framework's own config (`display_aspect`,
 * `ignore_aspect_ratio`, `linear_filtering`, `shader`) via
 * SnesDisplayAspect_ComputeViewport, which already handles all three pixel
 * aspects, integer scaling and seam suppression — the per-game helpers this
 * replaces mostly hardcoded 7:6.
 *
 * Vendored originally from snesrev/smw's src/opengl.c (MIT, (c) 2023 snesrev,
 * (c) 2021 elzo_d); see THIRD_PARTY_ATTRIBUTION.md.
 */

#include "gl_core_3_1.h"
#include "sdl_compat.h"

#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#include "types.h"
#include "util.h"
#include "glsl_shader.h"
#include "config.h"
#include "display_aspect.h"
#include "hd_gpu.h"
#include "host_main.h"
#include "present_shader.h"
#include "stb/stb_image.h"
#if SNESRECOMP_SDL3
#define DeleteGlContext SDL_GL_DestroyContext
#else
#define DeleteGlContext SDL_GL_DeleteContext
#endif

#define CODE(...) #__VA_ARGS__

static SDL_Window *g_window;
static uint8 *g_screen_buffer;
static size_t g_screen_buffer_size;
static int g_draw_width, g_draw_height;
static unsigned int g_program, g_flash_program, g_VAO, g_vbo;
enum { kNumUploadPbos = 3 };
static unsigned int g_pbo[kNumUploadPbos];
static bool g_triple_buffering = false;
static int g_pbo_index;
static bool g_pbo_mapped;
static int g_exact_nearest_loc = -1;
static int g_nearest_viewport_loc = -1;
static int g_flash_color_loc = -1;
static GlTextureWithSize g_texture;
static GlslShader *g_glsl_shader;
static SnesHdGpu *g_hd_gpu;
static bool g_hd_tried;
static const SnesHdFrame *g_hd_frame;
static const uint32_t *g_hd_overlay;
static size_t g_hd_overlay_pitch;
void snesrecomp_opengl_set_hd(const SnesHdFrame *frame,
                            const uint32_t *overlay, size_t pitch) {
  g_hd_frame = frame; g_hd_overlay = overlay; g_hd_overlay_pitch = pitch;
}
static bool g_want_screenshot;
static double g_screenshot_flash_start_time = -1.0;
static const double kScreenshotFlashSeconds = 0.15;

extern void MkDir(const char *s);

/* -1 = decide from config at init. A host that paces presentation itself
 * (an FPS cap, a simulation/presentation split) sets 0 so the swap does not
 * also block; one that wants the driver to pace it sets 1. */
/* -1 is a legal interval (adaptive), so "did the host say anything?" needs
 * its own flag rather than a sentinel value. */
static int g_vsync_override;
static bool g_vsync_set;
static void (*g_compute_viewport)(int, int, int, int, SnesDisplayViewport *);
static int (*g_native_overlay_cb)(SnesNativeOverlay *);
static GLuint g_native_ov_tex;
static uint64_t g_native_ov_key;
static int g_native_ov_w, g_native_ov_h;

void snesrecomp_opengl_set_viewport(void (*compute)(int, int, int, int,
                                                   SnesDisplayViewport *)) {
  g_compute_viewport = compute;
}

void snesrecomp_opengl_set_native_overlay_cb(int (*cb)(SnesNativeOverlay *)) {
  g_native_overlay_cb = cb;
}

static SDL_GLContext g_context;
static void ApplySwapInterval(void);

/* interval: 0 immediate, 1 wait for the panel, -1 late-swap-tearing
 * ("Adaptive"). Stored as-is; OpenGLRenderer_Init applies it. */
void snesrecomp_opengl_set_vsync(int interval) {
  g_vsync_override = interval;
  g_vsync_set = true;
  if (g_context) {
    ApplySwapInterval();
  }
}

void snesrecomp_opengl_set_triple_buffering(int enabled) {
  g_triple_buffering = (enabled != 0);
}

static void GL_APIENTRY MessageCallback(GLenum source,
                GLenum type,
                GLuint id,
                GLenum severity,
                GLsizei length,
                const GLchar *message,
                const void *userParam) {
  (void)source; (void)id; (void)length; (void)userParam;
  if (type == GL_DEBUG_TYPE_OTHER)
    return;

  fprintf(stderr, "GL CALLBACK: %s type = 0x%x, severity = 0x%x, message = %s\n",
          (type == GL_DEBUG_TYPE_ERROR ? "** GL ERROR **" : ""),
          type, severity, message);
  if (type == GL_DEBUG_TYPE_ERROR)
    Die("OpenGL error!\n");
}

/* The shader path that is loaded (or that failed to load), so Reconfigure
 * reloads only when the setting actually changed. */
static char g_shader_path[1024];
static bool g_shader_resolved;

static void ApplySwapInterval(void) {
  int interval = g_vsync_set ? g_vsync_override
                             : (g_config.disable_frame_delay ? 0 : 1);
  /* Not every driver has EXT_swap_control_tear; fall back to an ordinary
   * wait rather than to immediate, which is what the player did not ask
   * for. SDL2 returns 0 on success, SDL3 returns true. */
#if SNESRECOMP_SDL3
  bool ok = SDL_GL_SetSwapInterval(interval);
#else
  bool ok = SDL_GL_SetSwapInterval(interval) == 0;
#endif
  if (!ok && interval < 0) SDL_GL_SetSwapInterval(1);
}

static void LoadShaderFromConfig(void) {
  const char *want = g_config.shader ? g_config.shader : "";
  if (g_shader_resolved && strcmp(want, g_shader_path) == 0)
    return;
  if (g_glsl_shader) {
    GlslShader_Destroy(g_glsl_shader);
    g_glsl_shader = NULL;
  }
  snprintf(g_shader_path, sizeof(g_shader_path), "%s", want);
  g_shader_resolved = true;
  if (want[0])
    g_glsl_shader = GlslShader_CreateFromFile(want);
}

static bool OpenGLRenderer_Init(SDL_Window *window) {
  g_window = window;
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
  g_context = SDL_GL_CreateContext(window);
  if (!g_context) {
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    g_context = SDL_GL_CreateContext(window);
  }
  if (!g_context) return false;

  ApplySwapInterval();
  ogl_LoadFunctions();

  if (!ogl_IsVersionGEQ(3, 3)) {
    DeleteGlContext(g_context); g_context = NULL;
    return false;
  }

  if (kDebugFlag) {
    glEnable(GL_DEBUG_OUTPUT);
    glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
    glDebugMessageCallback(MessageCallback, 0);
  }

  glGenTextures(1, &g_texture.gl_texture);
  glGenBuffers(kNumUploadPbos, g_pbo);
  g_pbo_index = 0;
  g_pbo_mapped = false;

  static const float kVertices[] = {
    // positions          // texture coords
    -1.0f,  1.0f, 0.0f,   0.0f, 0.0f, // top left
    -1.0f, -1.0f, 0.0f,   0.0f, 1.0f, // bottom left
     1.0f,  1.0f, 0.0f,   1.0f, 0.0f, // top right
     1.0f, -1.0f, 0.0f,   1.0f, 1.0f, // bottom right
  };

  // create a vertex buffer object
  glGenBuffers(1, &g_vbo);

  // vertex array object
  glGenVertexArrays(1, &g_VAO);
  // 1. bind Vertex Array Object
  glBindVertexArray(g_VAO);
  // 2. copy our vertices array in a buffer for OpenGL to use
  glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
  glBufferData(GL_ARRAY_BUFFER, sizeof(kVertices), kVertices, GL_STATIC_DRAW);
  // position attribute
  glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void *)0);
  glEnableVertexAttribArray(0);
  // texture coord attribute
  glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void *)(3 * sizeof(float)));
  glEnableVertexAttribArray(1);

  // vertex shader
  const GLchar *vs_code = "#version 330 core\n" CODE(
  layout(location = 0) in vec3 aPos;
  layout(location = 1) in vec2 aTexCoord;
  out vec2 TexCoord;
  void main(void) {
    gl_Position = vec4(aPos, 1.0);
    TexCoord = vec2(aTexCoord.x, aTexCoord.y);
  }
);

  unsigned int vs = glCreateShader(GL_VERTEX_SHADER);
  glShaderSource(vs, 1, &vs_code, NULL);
  glCompileShader(vs);

  int success;
  char infolog[512];
  glGetShaderiv(vs, GL_COMPILE_STATUS, &success);
  if (!success) {
    glGetShaderInfoLog(vs, 512, NULL, infolog);
    printf("%s\n", infolog);
  }

  // fragment shader
  const GLchar *fs_code = SnesGlPresentFragmentShader();

  unsigned int fs = glCreateShader(GL_FRAGMENT_SHADER);
  glShaderSource(fs, 1, &fs_code, NULL);
  glCompileShader(fs);

  glGetShaderiv(fs, GL_COMPILE_STATUS, &success);
  if (!success) {
    glGetShaderInfoLog(fs, 512, NULL, infolog);
    printf("%s\n", infolog);
  }

  // create program
  int program = g_program = glCreateProgram();
  glAttachShader(program, vs);
  glAttachShader(program, fs);
  glLinkProgram(program);
  glGetProgramiv(program, GL_LINK_STATUS, &success);

  if (!success) {
    glGetProgramInfoLog(program, 512, NULL, infolog);
    printf("%s\n", infolog);
  }
  g_exact_nearest_loc = glGetUniformLocation(g_program, "exactNearest");
  g_nearest_viewport_loc = glGetUniformLocation(g_program, "nearestViewport");

  const GLchar *flash_fs_code = "#version 330 core\n" CODE(
  out vec4 FragColor;
  uniform vec4 color;
  void main(void) {
    FragColor = color;
  }
);
  unsigned int flash_fs = glCreateShader(GL_FRAGMENT_SHADER);
  glShaderSource(flash_fs, 1, &flash_fs_code, NULL);
  glCompileShader(flash_fs);
  glGetShaderiv(flash_fs, GL_COMPILE_STATUS, &success);
  if (!success) {
    glGetShaderInfoLog(flash_fs, 512, NULL, infolog);
    printf("%s\n", infolog);
  }
  g_flash_program = glCreateProgram();
  glAttachShader(g_flash_program, vs);
  glAttachShader(g_flash_program, flash_fs);
  glLinkProgram(g_flash_program);
  glGetProgramiv(g_flash_program, GL_LINK_STATUS, &success);
  if (!success) {
    glGetProgramInfoLog(g_flash_program, 512, NULL, infolog);
    printf("%s\n", infolog);
  }
  g_flash_color_loc = glGetUniformLocation(g_flash_program, "color");
  glDeleteShader(vs);
  glDeleteShader(fs);
  glDeleteShader(flash_fs);

  LoadShaderFromConfig();

  return true;
}

/* After another GL context was current (the in-game launcher's), or a
 * presentation setting changed under the live window. Filtering and aspect
 * are read every frame and need nothing here. */
static void OpenGLRenderer_Reconfigure(void) {
  if (g_context) SDL_GL_MakeCurrent(g_window, g_context);
  ApplySwapInterval();
  LoadShaderFromConfig();
}

static void OpenGLRenderer_Destroy(void) {
  SnesHdGpuDestroy(g_hd_gpu); g_hd_gpu = NULL; g_hd_tried = false;
  if (g_glsl_shader) GlslShader_Destroy(g_glsl_shader);
  g_glsl_shader = NULL; g_shader_resolved = false;
  if (g_pbo_mapped && g_pbo[g_pbo_index]) {
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, g_pbo[g_pbo_index]);
    glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    g_pbo_mapped = false;
  }
  if (g_pbo[0] || g_pbo[1]) {
    glDeleteBuffers(kNumUploadPbos, g_pbo);
    memset(g_pbo, 0, sizeof(g_pbo));
  }
  if (g_vbo) {
    glDeleteBuffers(1, &g_vbo);
    g_vbo = 0;
  }
  glDeleteTextures(1, &g_texture.gl_texture); memset(&g_texture, 0, sizeof(g_texture));
  if (g_native_ov_tex) {
    glDeleteTextures(1, &g_native_ov_tex);
    g_native_ov_tex = 0;
  }
  g_native_ov_key = 0;
  g_native_ov_w = 0;
  g_native_ov_h = 0;
  glDeleteProgram(g_program); g_program = 0;
  glDeleteProgram(g_flash_program); g_flash_program = 0;
  glDeleteVertexArrays(1, &g_VAO); g_VAO = 0;
  free(g_screen_buffer); g_screen_buffer = NULL; g_screen_buffer_size = 0;
  DeleteGlContext(g_context); g_context = NULL;
}

static void OpenGLRenderer_GetOutputSize(int *width, int *height) {
  snesrecomp_sdl_get_drawable_size(g_window, width, height);
}

void OpenGLRenderer_RequestScreenshot(void) {
  g_want_screenshot = true;
}

static double MonotonicSeconds(void) {
  return (double)SDL_GetPerformanceCounter() / SDL_GetPerformanceFrequency();
}

static void SaveScreenshotBmp(int width, int height) {
  size_t pixel_size = (size_t)width * (size_t)height * 4;
  uint8 *pixels = (uint8 *)malloc(pixel_size);
  if (!pixels)
    return;

  glReadPixels(0, 0, width, height, GL_BGRA, GL_UNSIGNED_BYTE, pixels);

  MkDir("screenshots");

  time_t t = time(NULL);
  struct tm tm_snapshot;
  struct tm *local = localtime(&t);
  if (local)
    tm_snapshot = *local;
  else
    memset(&tm_snapshot, 0, sizeof(tm_snapshot));

  char path[256];
  snprintf(path, sizeof(path),
           "screenshots/screenshot_%04d%02d%02d_%02d%02d%02d.bmp",
           tm_snapshot.tm_year + 1900, tm_snapshot.tm_mon + 1,
           tm_snapshot.tm_mday, tm_snapshot.tm_hour, tm_snapshot.tm_min,
           tm_snapshot.tm_sec);

  FILE *f = fopen(path, "wb");
  if (f) {
    uint32 pixel_data_size = (uint32)pixel_size;
    uint32 file_size = 14 + 40 + pixel_data_size;
    uint32 pixel_offset = 14 + 40;
    uint32 dib_size = 40;
    int32 w = width;
    int32 h = height;
    uint16 planes = 1;
    uint16 bits = 32;
    uint32 zero = 0;

    fwrite("BM", 1, 2, f);
    fwrite(&file_size, 4, 1, f);
    fwrite(&zero, 4, 1, f);
    fwrite(&pixel_offset, 4, 1, f);
    fwrite(&dib_size, 4, 1, f);
    fwrite(&w, 4, 1, f);
    fwrite(&h, 4, 1, f);
    fwrite(&planes, 2, 1, f);
    fwrite(&bits, 2, 1, f);
    fwrite(&zero, 4, 1, f);
    fwrite(&pixel_data_size, 4, 1, f);
    fwrite(&zero, 4, 1, f);
    fwrite(&zero, 4, 1, f);
    fwrite(&zero, 4, 1, f);
    fwrite(&zero, 4, 1, f);
    fwrite(pixels, 1, pixel_size, f);
    fclose(f);
    printf("Screenshot saved: %s\n", path);
    g_screenshot_flash_start_time = MonotonicSeconds();
  } else {
    fprintf(stderr, "Screenshot: couldn't open '%s' for writing\n", path);
  }

  free(pixels);
}

static bool DrawScreenshotFlash(double elapsed_seconds) {
  if (elapsed_seconds >= kScreenshotFlashSeconds)
    return false;

  float alpha = 0.5f * (float)(1.0 - elapsed_seconds / kScreenshotFlashSeconds);
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  glUseProgram(g_flash_program);
  glUniform4f(g_flash_color_loc, 1.0f, 1.0f, 1.0f, alpha);
  glBindVertexArray(g_VAO);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  glDisable(GL_BLEND);
  return true;
}

static void OpenGLRenderer_BeginDraw(int width, int height, uint8 **pixels, int *pitch) {
  g_hd_frame = NULL; g_hd_overlay = NULL; g_hd_overlay_pitch = 0;
  g_draw_width = width;
  g_draw_height = height;
  *pitch = width * 4;

  size_t size = (size_t)width * (size_t)height;
  size_t byte_size = size * 4u;

  /* Fast zero-copy PBO path when HD Mode 7 CPU readback is not active:
   * map the next double-buffered GL_PIXEL_UNPACK_BUFFER with buffer
   * invalidation (orphaning) so the host writes the staged frame once
   * directly into driver-mapped DMA memory. */
  if (!g_config.mode7.enabled && g_pbo[0] != 0) {
    g_pbo_index = (g_pbo_index + 1) % (g_triple_buffering ? 3 : 2);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, g_pbo[g_pbo_index]);
    glBufferData(GL_PIXEL_UNPACK_BUFFER, (GLsizeiptr)byte_size, NULL, GL_STREAM_DRAW);
    void *mapped = glMapBufferRange(GL_PIXEL_UNPACK_BUFFER, 0, (GLsizeiptr)byte_size,
                                    GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    if (mapped) {
      g_pbo_mapped = true;
      *pixels = (uint8 *)mapped;
      return;
    }
  }

  g_pbo_mapped = false;
  if (size > g_screen_buffer_size) {
    g_screen_buffer_size = size;
    free(g_screen_buffer);
    g_screen_buffer = (uint8 *)malloc(byte_size);
  }
  *pixels = g_screen_buffer;
}

static void DrawNativeOverlayGl(const SnesDisplayViewport *vp, int drawable_width, int drawable_height) {
  if (!g_native_overlay_cb || !vp || vp->width <= 0 || vp->height <= 0) return;
  SnesNativeOverlay ov;
  memset(&ov, 0, sizeof(ov));
  if (!g_native_overlay_cb(&ov)) return;
  if (ov.image_key == 0 || !ov.image_path || !ov.image_path[0] ||
      ov.snes_w <= 0.0f || ov.snes_h <= 0.0f ||
      ov.frame_w <= 0 || ov.frame_h <= 0 || ov.alpha == 0) {
    return;
  }

  if (ov.image_key != g_native_ov_key) {
    g_native_ov_key = ov.image_key;
    g_native_ov_w = 0;
    g_native_ov_h = 0;
    int iw = 0, ih = 0, comp = 0;
    unsigned char *rgba = stbi_load(ov.image_path, &iw, &ih, &comp, 4);
    if (rgba && iw > 0 && ih > 0) {
      if (!g_native_ov_tex) glGenTextures(1, &g_native_ov_tex);
      glActiveTexture(GL_TEXTURE0);
      glBindTexture(GL_TEXTURE_2D, g_native_ov_tex);
      glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
      glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, iw, ih, 0,
                   GL_RGBA, GL_UNSIGNED_BYTE, rgba);
      glGenerateMipmap(GL_TEXTURE_2D);
      g_native_ov_w = iw;
      g_native_ov_h = ih;
    }
    if (rgba) stbi_image_free(rgba);
  }
  if (!g_native_ov_tex || g_native_ov_w <= 0 || g_native_ov_h <= 0) return;

  float box_x = (float)vp->x + (ov.snes_x * (float)vp->width) / (float)ov.frame_w;
  float box_top = (float)(drawable_height - (vp->y + vp->height)) +
                  (ov.snes_y * (float)vp->height) / (float)ov.frame_h;
  float box_w = (ov.snes_w * (float)vp->width) / (float)ov.frame_w;
  float box_h = (ov.snes_h * (float)vp->height) / (float)ov.frame_h;

  float draw_x = box_x, draw_top = box_top, draw_w = box_w, draw_h = box_h;
  if (ov.preserve_aspect) {
    float sx = box_w / (float)g_native_ov_w;
    float sy = box_h / (float)g_native_ov_h;
    float sc = (sx < sy) ? sx : sy;
    draw_w = (float)g_native_ov_w * sc;
    draw_h = (float)g_native_ov_h * sc;
    draw_x = box_x + (box_w - draw_w) * 0.5f;
    draw_top = box_top + (box_h - draw_h) * 0.5f;
  }

  int vx = (int)lroundf(draw_x);
  int vy = drawable_height - (int)lroundf(draw_top + draw_h);
  int vw = (int)lroundf(draw_w);
  int vh = (int)lroundf(draw_h);
  if (vw <= 0 || vh <= 0) return;

  int sx0 = (int)floorf(box_x);
  int sy0 = drawable_height - (int)ceilf(box_top + box_h);
  int sw = (int)ceilf(box_x + box_w) - sx0;
  int sh = (int)ceilf(box_top + box_h) - (int)floorf(box_top);
  if (sw <= 0 || sh <= 0) return;
  if (sx0 < 0) { sw += sx0; sx0 = 0; }
  if (sy0 < 0) { sh += sy0; sy0 = 0; }
  if (sx0 + sw > drawable_width) sw = drawable_width - sx0;
  if (sy0 + sh > drawable_height) sh = drawable_height - sy0;
  if (sw <= 0 || sh <= 0) return;

  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, g_native_ov_tex);
  glUseProgram(g_program);
  glUniform1i(g_exact_nearest_loc, 0);
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  glEnable(GL_SCISSOR_TEST);
  glScissor(sx0, sy0, sw, sh);
  glViewport(vx, vy, vw, vh);
  glBindVertexArray(g_VAO);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_BLEND);
}

static void OpenGLRenderer_EndDraw(void) {
  int drawable_width, drawable_height;

  snesrecomp_sdl_get_drawable_size(g_window, &drawable_width, &drawable_height);

  /* The framework's own aspect maths, rather than a per-game copy: it knows
   * all three pixel aspects the config offers, and centres on both axes. */
  SnesDisplayViewport viewport;
  SnesDisplayAspect_ComputeViewport(
      g_draw_width, g_draw_height, drawable_width, drawable_height,
      SnesDisplayAspect_Clamp(g_config.display_aspect),
      g_config.ignore_aspect_ratio, false, &viewport);
  if (g_compute_viewport)
    g_compute_viewport(g_draw_width, g_draw_height, drawable_width, drawable_height, &viewport);

  GlTextureWithSize image = g_texture;
  SnesHdGpuImage hd_image;
  bool enhanced = false;
  if (g_hd_frame && g_config.mode7.enabled && !g_pbo_mapped) {
    if (!g_hd_tried) { g_hd_gpu = SnesHdGpuCreate(); g_hd_tried = true; }
    enhanced = SnesHdGpuRender(g_hd_gpu, g_hd_frame, g_config.mode7.scale,
        (unsigned)viewport.width, (unsigned)viewport.height,
        !g_config.linear_filtering && !g_glsl_shader, 128u * 1024u * 1024u,
        (const uint32_t *)g_screen_buffer, g_draw_width * 4,
        g_hd_overlay, g_hd_overlay_pitch, &hd_image);
    if (enhanced) image = (GlTextureWithSize){hd_image.texture, hd_image.width, hd_image.height};
  }
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, enhanced ? image.gl_texture : g_texture.gl_texture);
  if (!enhanced) {
    /* Recreate texture storage only on resolution/widescreen resize, never per-frame */
    if (g_draw_width != g_texture.width || g_draw_height != g_texture.height) {
      g_texture.width = g_draw_width;
      g_texture.height = g_draw_height;
      glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, g_draw_width, g_draw_height, 0,
                   GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, NULL);
    }

    if (g_pbo_mapped) {
      glBindBuffer(GL_PIXEL_UNPACK_BUFFER, g_pbo[g_pbo_index]);
      glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
      g_pbo_mapped = false;
      glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, g_draw_width, g_draw_height,
                      GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, (const void *)0);
      glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    } else if (g_pbo[0] != 0 && g_screen_buffer != NULL) {
      size_t byte_size = (size_t)g_draw_width * (size_t)g_draw_height * 4u;
      g_pbo_index = (g_pbo_index + 1) % (g_triple_buffering ? 3 : 2);
      glBindBuffer(GL_PIXEL_UNPACK_BUFFER, g_pbo[g_pbo_index]);
      glBufferData(GL_PIXEL_UNPACK_BUFFER, (GLsizeiptr)byte_size, NULL, GL_STREAM_DRAW);
      void *mapped = glMapBufferRange(GL_PIXEL_UNPACK_BUFFER, 0, (GLsizeiptr)byte_size,
                                      GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT);
      if (mapped) {
        memcpy(mapped, g_screen_buffer, byte_size);
        glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, g_draw_width, g_draw_height,
                        GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, (const void *)0);
      } else {
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, g_draw_width, g_draw_height,
                        GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, g_screen_buffer);
      }
      glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    } else if (g_screen_buffer != NULL) {
      glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, g_draw_width, g_draw_height,
                      GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, g_screen_buffer);
    }
    image = g_texture;
  } else if (g_pbo_mapped) {
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, g_pbo[g_pbo_index]);
    glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    g_pbo_mapped = false;
  }

  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);

  if (g_glsl_shader == NULL) {
    glViewport(viewport.x, viewport.y, viewport.width, viewport.height);
    glUseProgram(g_program);
    glUniform1i(g_exact_nearest_loc, enhanced && !g_config.linear_filtering);
    glUniform4i(g_nearest_viewport_loc, viewport.x, viewport.y, viewport.width, viewport.height);
    int filter = g_config.linear_filtering ? GL_LINEAR : GL_NEAREST;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    glBindVertexArray(g_VAO);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  } else {
    GlslShader_Render(g_glsl_shader, &image, viewport.x, viewport.y,
                      viewport.width, viewport.height);
  }

  DrawNativeOverlayGl(&viewport, drawable_width, drawable_height);

  if (g_want_screenshot) {
    g_want_screenshot = false;
    SaveScreenshotBmp(drawable_width, drawable_height);
  }
  if (g_screenshot_flash_start_time >= 0.0) {
    double now = MonotonicSeconds();
    glViewport(0, 0, drawable_width, drawable_height);
    if (!DrawScreenshotFlash(now - g_screenshot_flash_start_time))
      g_screenshot_flash_start_time = -1.0;
  }

  SDL_GL_SwapWindow(g_window);
}

static const struct RendererFuncs kOpenGLRendererFuncs = {
  &OpenGLRenderer_Init,
  &OpenGLRenderer_Destroy,
  &OpenGLRenderer_GetOutputSize,
  &OpenGLRenderer_BeginDraw,
  &OpenGLRenderer_EndDraw,
  &OpenGLRenderer_Reconfigure,
};

void OpenGLRenderer_Create(struct RendererFuncs *funcs) {
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
  *funcs = kOpenGLRendererFuncs;
}
