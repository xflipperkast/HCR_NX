/* overlay.c -- minimal GLES2 overlay for the docked/no-touch virtual cursor.
 *
 * The game has several touch-only widgets (Camp/Map/Log, main-menu items,
 * Options). We drive a virtual cursor with the right stick and synthesize a
 * touch tap at it (see main.c). This module just draws the crosshair so the
 * player can aim; it owns all its GL state and paints the default framebuffer
 * after the game has rendered its frame.
 */
#include <stddef.h>
#include <GLES2/gl2.h>
#include "overlay.h"

static GLuint s_prog, s_vbo, s_loc_pos, s_loc_color;
static int s_ready = 0;

static GLuint compile(GLenum type, const char *src) {
  GLuint s = glCreateShader(type);
  glShaderSource(s, 1, &src, NULL);
  glCompileShader(s);
  return s;
}

static void lazy_init(void) {
  if (s_ready) return;
  s_ready = 1; // only try once even on failure

  static const char *vs =
    "attribute vec2 a_pos;\n"
    "void main(){ gl_Position = vec4(a_pos, 0.0, 1.0); }\n";
  static const char *fs =
    "precision mediump float;\n"
    "uniform vec4 u_color;\n"
    "void main(){ gl_FragColor = u_color; }\n";

  s_prog = glCreateProgram();
  GLuint v = compile(GL_VERTEX_SHADER, vs);
  GLuint f = compile(GL_FRAGMENT_SHADER, fs);
  glAttachShader(s_prog, v);
  glAttachShader(s_prog, f);
  glBindAttribLocation(s_prog, 0, "a_pos");
  glLinkProgram(s_prog);
  glDeleteShader(v);
  glDeleteShader(f);

  s_loc_pos = 0; // bound above
  s_loc_color = glGetUniformLocation(s_prog, "u_color");
  glGenBuffers(1, &s_vbo);
}

// append one axis-aligned rect (2 triangles) in NDC to buf; returns new vert count
static int push_rect(float *buf, int n, float cx, float cy, float hw, float hh,
                     int sw, int sh) {
  // pixel corners -> NDC (y flips: screen y-down, NDC y-up)
  float x0 = ((cx - hw) / sw) * 2.f - 1.f;
  float x1 = ((cx + hw) / sw) * 2.f - 1.f;
  float y0 = 1.f - ((cy - hh) / sh) * 2.f;
  float y1 = 1.f - ((cy + hh) / sh) * 2.f;
  const float q[12] = { x0,y0, x1,y0, x1,y1,  x0,y0, x1,y1, x0,y1 };
  for (int i = 0; i < 12; i++) buf[n * 2 + i] = q[i];
  return n + 6;
}

// build the crosshair (H bar + V bar) at (cx,cy) with arm/thickness in px
static int build_cross(float *buf, float cx, float cy, float arm, float th,
                       int sw, int sh) {
  int n = 0;
  n = push_rect(buf, n, cx, cy, arm, th * 0.5f, sw, sh); // horizontal
  n = push_rect(buf, n, cx, cy, th * 0.5f, arm, sw, sh); // vertical
  return n; // 12 verts
}

void overlay_draw_cursor(float x, float y, float alpha, int sw, int sh) {
  if (alpha <= 0.f) return;
  if (alpha > 1.f) alpha = 1.f;
  lazy_init();
  if (!s_prog) return;

  float verts[24 * 2];

  // our own GL state; the game re-sets everything each frame so no restore needed
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glViewport(0, 0, sw, sh);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_CULL_FACE);
  glDisable(GL_SCISSOR_TEST);
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  glUseProgram(s_prog);
  glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);

  const float arm = 18.f, th = 5.f;

  // dark outline (slightly larger) then white fill -> readable over any scene
  int n = build_cross(verts, x, y, arm + 2.f, th + 4.f, sw, sh);
  glBufferData(GL_ARRAY_BUFFER, n * 2 * sizeof(float), verts, GL_DYNAMIC_DRAW);
  glUniform4f(s_loc_color, 0.f, 0.f, 0.f, alpha * 0.7f);
  glDrawArrays(GL_TRIANGLES, 0, n);

  n = build_cross(verts, x, y, arm, th, sw, sh);
  glBufferData(GL_ARRAY_BUFFER, n * 2 * sizeof(float), verts, GL_DYNAMIC_DRAW);
  glUniform4f(s_loc_color, 1.f, 1.f, 1.f, alpha);
  glDrawArrays(GL_TRIANGLES, 0, n);

  glDisableVertexAttribArray(0);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glUseProgram(0);
}
