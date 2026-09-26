/* A Kingdom for Keflings VR -- 32-bit opengl32.dll proxy, shared declarations. */
#ifndef KEFLINGS_PROXY_H
#define KEFLINGS_PROXY_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <GL/gl.h>

/* Filled by generated_thunks.c */
extern void *g_real[];
extern unsigned g_real_count[];
extern unsigned g_real_calls;
extern const int g_real_n;
extern const char *const k_names_px_[];
void proxy_bind_passthrough(HMODULE h);

void kv_log(const char *fmt, ...);

/* This DLL's own handle.  Do NOT use GetModuleHandleA("opengl32.dll") to find
   it: the real system DLL is loaded under the same base name and the match is
   ambiguous. */
extern HMODULE g_self;

/* Resolve an entry point in the REAL opengl32.dll.  Calling our own exports
   from inside the proxy would work but routes back through the hooks, and
   linking opengl32.lib would make the proxy import from itself. */
/* A returned log has to name the build it came from. 0.x while it is still
   being tested -- see version-numbers-earn-1-0. */
#define KV_VERSION "1.0"
void *proxy_real_proc(const char *name);

/* dump.c: recover the engine ini vocabulary from the decrypted image. */
#include <stdio.h>
FILE *open_out(const char *kind);
void dump_engine_strings(void);
void dump_camera_world(float fovy, float aspect, float znear,
                       float zfar);
/* cullhunt.c: what clips the ground objects? */
void cullhunt_frame(unsigned frame);
void cullradius_apply(void);
/* camprobe.c: test each field of the camera object in turn. */
/* campitch.c: rotate the engine camera so its culling follows the gaze. */
void campitch_apply(float deg, const float *view);
void camprobe_view(void *view_ptr);
void camprobe_frame(unsigned frame);
/* scenecache.c: keep drawing scenery the engine stops drawing. */
void scenecache_begin(const float *view, unsigned frame);
void scenecache_note(unsigned list, const float *mv, unsigned tex,
                     const float *colour);
void scenecache_replay(void (*set_state)(const float *, unsigned,
                                        const float *),
                       void (*call_list)(unsigned));
void scenecache_report(void);
void scenecache_flush(void);
/* tilescan.c: find the tile radius without writing anything. */
void tilescan_frame(void);
unsigned kv_draw_count(void);
unsigned kv_elem_count(void);
unsigned kv_list_count(void);
int kv_in_level(void);
void dump_view_neighbourhood(const void *view_matrix, const float *vm,
                             float fovy, float znear, float zfar);
void dump_heap_cameras(const float *vm);
void dump_modules(void);
/* keyspy.c: watch which keys the game asks Windows about. */
/* cursorspy.c: how the game actually tracks the mouse. */
void cursorspy_install(void);
void cursorspy_report(void);
void keyspy_install(void);
void keyspy_report(void);
void keyspy_watch_window(HWND h);
/* input_emu.c: the controllers, as keyboard and mouse. */
/* 1 while the 2D pass is drawing a menu rather than the HUD.  Owned by
   proxy.c, which decides it from the draw count. */
int  kv_menu_up(void);
/* pointer.c: the cursor goes where the controller points. */
int  pointer_frame(HWND wnd);
void pointer_selfcheck(int w, int h);
/* mousepatch.c: the game's own cursor, driven inside the game. */
void mouse_scan(int cx, int cy);
void mouse_force(int x, int y);
int  mouse_candidates(void);
void input_emu_frame(HDC hdc);
/* 1 while photo mode hides the HUD, the menus and their dim */
int  kv_photo_mode(void);
/* 1 while the right grip is held: the grip mouse (input_emu.c) */
int  kv_mouse_mode(void);
/* pointer.c: what to do with the game's cursor sprite this frame --
   0 draw it as the game does, 1 hide it, 2 draw it shifted by (dx, dy)
   canvas pixels -- and where our cursor is, in canvas pixels (y up). */
int  kv_cursor_draw(float *dx, float *dy);
int  kv_cursor_target(float *tx, float *ty);
int  kv_cursor_canvas(float *x, float *y);
/* proxy.c: the engine's 2D canvas size */
int  kv_canvas_size(float *w, float *h);
int  kv_hud_hit(float x, float y, int freeze, float pad);
int  kv_front_end(void);
int  kv_scene_drawn(void);
void input_emu_shutdown(void);
void dump_rawinput(void);
void dump_game_strings(void);
void dump_camera_neighbourhood(const void *proj_matrix);

/* fovpatch.c: widen the field of view the ENGINE draws and culls to. */
void fov_scan(float fovy, float aspect);
void aspect_scan(float aspect);
void aspect_apply(float want, float original);
void fov_apply(float want, float original);
void fov_apply_second(float want, float original);
unsigned fov_candidate_count(void);
void *fov_candidate(unsigned i);
/* camrot.c: can the engine camera be turned? */
void camrot_locate(const float *view_matrix);
void camrot_frame(const float *view_matrix, unsigned frame);
void far_scan(float far_value, float near_value);
void far_apply(float want, float original);
/* Find the camera ORIENTATION inside the blocks that hold its field of
   view, so the engine can be made to look where the head looks. */
void orientation_scan(const float *view_matrix);
void camera_layout_dump(const float *vm, float fovy, float aspect,
                        float znear, float zfar);
void fov_report(void);
void fov_note_caller(void *ret);

/* Hook implementations (proxy.c).  The generated file emits an undecorated
   naked wrapper per name so the .def can alias these by plain name. */
BOOL  WINAPI hk_wglSwapBuffers(HDC hdc);
BOOL  WINAPI hk_wglSwapLayerBuffers(HDC hdc, UINT planes);
HGLRC WINAPI hk_wglCreateContext(HDC hdc);
BOOL  WINAPI hk_wglMakeCurrent(HDC hdc, HGLRC rc);
BOOL  WINAPI hk_wglDeleteContext(HGLRC rc);
PROC  WINAPI hk_wglGetProcAddress(LPCSTR name);

void APIENTRY hk_glViewport(GLint x, GLint y, GLsizei w, GLsizei h);
void APIENTRY hk_glDrawBuffer(GLenum buf);
void APIENTRY hk_glReadBuffer(GLenum buf);
void APIENTRY hk_glBlendFunc(GLenum sfactor, GLenum dfactor);
void APIENTRY hk_glScissor(GLint x, GLint y, GLsizei w, GLsizei h);
/* If the engine reads the projection back out of GL, it may be deriving its
   culling frustum from it -- in which case we control the culling outright. */
void APIENTRY hk_glGetFloatv(GLenum pname, GLfloat *params);
void APIENTRY hk_glGetIntegerv(GLenum pname, GLint *params);
void APIENTRY hk_glGetDoublev(GLenum pname, GLdouble *params);
/* Drawing paths the per-eye duplication does NOT cover.  If the engine puts
   any UI through these it would be drawn once, at the engine's own viewport,
   and land correctly in neither eye -- which is what invisible-but-clickable
   save entries look like. */
void APIENTRY hk_glDrawPixels(GLsizei w, GLsizei h, GLenum fmt, GLenum type,
                              const GLvoid *px);
void APIENTRY hk_glBitmap(GLsizei w, GLsizei h, GLfloat x0, GLfloat y0,
                          GLfloat xm, GLfloat ym, const GLubyte *bm);
void APIENTRY hk_glCopyPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum t);
void APIENTRY hk_glCopyTexImage2D(GLenum t, GLint l, GLenum ifmt, GLint x,
                                  GLint y, GLsizei w, GLsizei h, GLint b);
void APIENTRY hk_glCopyTexSubImage2D(GLenum t, GLint l, GLint xo, GLint yo,
                                     GLint x, GLint y, GLsizei w, GLsizei h);
void APIENTRY hk_glMatrixMode(GLenum mode);
void APIENTRY hk_glLoadIdentity(void);
void APIENTRY hk_glLoadMatrixf(const GLfloat *m);
void APIENTRY hk_glLoadMatrixd(const GLdouble *m);
void APIENTRY hk_glMultMatrixf(const GLfloat *m);
void APIENTRY hk_glMultMatrixd(const GLdouble *m);
void APIENTRY hk_glFrustum(GLdouble l, GLdouble r, GLdouble b, GLdouble t,
                           GLdouble n, GLdouble f);
void APIENTRY hk_glOrtho(GLdouble l, GLdouble r, GLdouble b, GLdouble t,
                         GLdouble n, GLdouble f);
void APIENTRY hk_glBegin(GLenum mode);
void APIENTRY hk_glEnd(void);
/* Hooked only to measure how much of the 2D canvas a primitive covers, which
   is what tells a dimming backdrop from an icon.  Otherwise pass-throughs. */
void APIENTRY hk_glRectf(GLfloat x1, GLfloat y1, GLfloat x2, GLfloat y2);
void APIENTRY hk_glRecti(GLint x1, GLint y1, GLint x2, GLint y2);
void APIENTRY hk_glRectd(GLdouble x1, GLdouble y1, GLdouble x2, GLdouble y2);
void APIENTRY hk_glVertex2f(GLfloat x, GLfloat y);
void APIENTRY hk_glVertex2i(GLint x, GLint y);
void APIENTRY hk_glVertex2d(GLdouble x, GLdouble y);
void APIENTRY hk_glVertex3f(GLfloat x, GLfloat y, GLfloat z);
void APIENTRY hk_glVertex3i(GLint x, GLint y, GLint z);
void APIENTRY hk_glVertex3d(GLdouble x, GLdouble y, GLdouble z);
void APIENTRY hk_glNewList(GLuint list, GLenum mode);
void APIENTRY hk_glEndList(void);
void APIENTRY hk_glCallList(GLuint list);
void APIENTRY hk_glCallLists(GLsizei n, GLenum type, const GLvoid *lists);
void APIENTRY hk_glDrawArrays(GLenum mode, GLint first, GLsizei count);
void APIENTRY hk_glDrawElements(GLenum mode, GLsizei count, GLenum type,
                                const GLvoid *indices);
void APIENTRY hk_glClear(GLbitfield mask);
void APIENTRY hk_glEnable(GLenum cap);
void APIENTRY hk_glFinish(void);
void APIENTRY hk_glDisable(GLenum cap);
void APIENTRY hk_glPushMatrix(void);
void APIENTRY hk_glPopMatrix(void);
void APIENTRY hk_glPushAttrib(GLbitfield mask);
void APIENTRY hk_glPopAttrib(void);
void APIENTRY hk_glTexEnvi(GLenum target, GLenum pname, GLint param);
void APIENTRY hk_glTexEnvf(GLenum target, GLenum pname, GLfloat param);
void APIENTRY hk_glAlphaFunc(GLenum func, GLclampf ref);
void APIENTRY hk_glDepthMask(GLboolean flag);
void APIENTRY hk_glArrayElement(GLint i);
void APIENTRY hk_glTranslatef(GLfloat x, GLfloat y, GLfloat z);
GLenum APIENTRY hk_glGetError(void);
void APIENTRY hk_glGetMaterialfv(GLenum face, GLenum pname, GLfloat *params);
void APIENTRY hk_glGetLightfv(GLenum light, GLenum pname, GLfloat *params);
void APIENTRY hk_glLightfv(GLenum light, GLenum pname, const GLfloat *params);
void APIENTRY hk_glLightf(GLenum light, GLenum pname, GLfloat param);
void APIENTRY hk_glTranslated(GLdouble x, GLdouble y, GLdouble z);
void APIENTRY hk_glRotatef(GLfloat a, GLfloat x, GLfloat y, GLfloat z);
void APIENTRY hk_glRotated(GLdouble a, GLdouble x, GLdouble y, GLdouble z);
void APIENTRY hk_glScalef(GLfloat x, GLfloat y, GLfloat z);
void APIENTRY hk_glScaled(GLdouble x, GLdouble y, GLdouble z);
void APIENTRY hk_glVertexPointer(GLint size, GLenum type, GLsizei stride,
                                 const GLvoid *p);
void APIENTRY hk_glNormalPointer(GLenum type, GLsizei stride, const GLvoid *p);
void APIENTRY hk_glTexCoordPointer(GLint size, GLenum type, GLsizei stride,
                                   const GLvoid *p);
void APIENTRY hk_glColorPointer(GLint size, GLenum type, GLsizei stride,
                                const GLvoid *p);
void APIENTRY hk_glEnableClientState(GLenum cap);
void APIENTRY hk_glDisableClientState(GLenum cap);
void APIENTRY hk_glShadeModel(GLenum mode);
void APIENTRY hk_glDepthFunc(GLenum func);
void APIENTRY hk_glMateriali(GLenum face, GLenum pname, GLint param);
void APIENTRY hk_glMaterialfv(GLenum face, GLenum pname, const GLfloat *v);
void APIENTRY hk_glBindTexture(GLenum target, GLuint texture);
void APIENTRY hk_glColor4ub(GLubyte r, GLubyte g, GLubyte b, GLubyte a);
void APIENTRY hk_glColor4f(GLfloat r, GLfloat g, GLfloat b, GLfloat a);
void APIENTRY hk_glColor4fv(const GLfloat *v);
void APIENTRY hk_glNormal3f(GLfloat x, GLfloat y, GLfloat z);
void APIENTRY hk_glTexCoord2f(GLfloat s, GLfloat t);

#endif
