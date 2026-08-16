/* overlay.h -- minimal GLES2 on-screen overlay (virtual-cursor crosshair) */
#ifndef __OVERLAY_H__
#define __OVERLAY_H__

// Draw a crosshair cursor centered at (x,y) in screen pixels (y down), with the
// given alpha (0..1). Lazily builds its shader/VBO on first call. Fully sets its
// own GL state and targets the default framebuffer, so call it after the game's
// nativeRender and before eglSwapBuffers. A no-op when alpha <= 0.
void overlay_draw_cursor(float x, float y, float alpha, int screen_w, int screen_h);

#endif
