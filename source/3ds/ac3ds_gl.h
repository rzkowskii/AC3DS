#pragma once
// Narrow desktop GL interface used by Cube, backed by native PICA200 commands.
#include <cstdint>
using GLenum = unsigned int;
using GLbitfield = unsigned int;
using GLuint = unsigned int;
using GLint = int;
using GLsizei = int;
using GLboolean = unsigned char;
using GLubyte = unsigned char;
using GLushort = unsigned short;
using GLfloat = float;
using GLdouble = double;
using GLvoid = void;

#define GL_ADD 0x0104
#define GL_ALPHA 0x1906
#define GL_ALPHA_SCALE 0x0D1C
#define GL_ALPHA_TEST 0x0BC0
#define GL_ALWAYS 0x0207
#define GL_BACK 0x0405
#define GL_BGRA 0x80E1
#define GL_BLEND 0x0BE2
#define GL_BYTE 0x1400
#define GL_CLAMP 0x2900
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_COLOR_ARRAY 0x8076
#define GL_COLOR_BUFFER_BIT 0x00004000
#define GL_COMBINE_ALPHA_ARB 0x8572
#define GL_COMBINE_ARB 0x8570
#define GL_COMBINE_RGB_ARB 0x8571
#define GL_COMPILE 0x1300
#define GL_CONSTANT_ALPHA 0x8003
#define GL_CONSTANT_ARB 0x8576
#define GL_CONSTANT_COLOR 0x8001
#define GL_CULL_FACE 0x0B44
#define GL_CURRENT_COLOR 0x0B00
#define GL_DECR 0x1E03
#define GL_DECR_WRAP_EXT 0x8508
#define GL_DEPTH_BUFFER_BIT 0x00000100
#define GL_DEPTH_COMPONENT 0x1902
#define GL_DEPTH_TEST 0x0B71
#define GL_DOT3_RGB_ARB 0x86AE
#define GL_DOUBLE 0x140A
#define GL_DST_ALPHA 0x0304
#define GL_DST_COLOR 0x0306
#define GL_EQUAL 0x0202
#define GL_EXTENSIONS 0x1F03
#define GL_FALSE 0
#define GL_FILL 0x1B02
#define GL_FLOAT 0x1406
#define GL_FOG 0x0B60
#define GL_FOG_COLOR 0x0B66
#define GL_FOG_DENSITY 0x0B62
#define GL_FOG_END 0x0B64
#define GL_FOG_HINT 0x0C54
#define GL_FOG_MODE 0x0B65
#define GL_FOG_START 0x0B63
#define GL_FRONT 0x0404
#define GL_FRONT_AND_BACK 0x0408
#define GL_GEQUAL 0x0206
#define GL_GREATER 0x0204
#define GL_INCR 0x1E02
#define GL_INCR_WRAP_EXT 0x8507
#define GL_INT 0x1404
#define GL_INTERPOLATE_ARB 0x8575
#define GL_INVERT 0x150A
#define GL_KEEP 0x1E00
#define GL_LEQUAL 0x0203
#define GL_LESS 0x0201
#define GL_LINE 0x1B01
#define GL_LINEAR 0x2601
#define GL_LINEAR_MIPMAP_LINEAR 0x2703
#define GL_LINEAR_MIPMAP_NEAREST 0x2701
#define GL_LINES 0x0001
#define GL_LINE_LOOP 0x0002
#define GL_LINE_SMOOTH 0x0B20
#define GL_LINE_SMOOTH_HINT 0x0C52
#define GL_LINE_WIDTH 0x0B21
#define GL_LUMINANCE 0x1909
#define GL_LUMINANCE_ALPHA 0x190A
#define GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT 0x84FF
#define GL_MAX_TEXTURE_SIZE 0x0D33
#define GL_MAX_TEXTURE_UNITS_ARB 0x84E2
#define GL_MIRRORED_REPEAT 0x8370
#define GL_MODELVIEW 0x1700
#define GL_MODELVIEW_MATRIX 0x0BA6
#define GL_MODULATE 0x2100
#define GL_NEAREST 0x2600
#define GL_NEAREST_MIPMAP_LINEAR 0x2702
#define GL_NEAREST_MIPMAP_NEAREST 0x2700
#define GL_NEVER 0x0200
#define GL_NICEST 0x1102
#define GL_NORMAL_ARRAY 0x8075
#define GL_NOTEQUAL 0x0205
#define GL_ONE 1
#define GL_ONE_MINUS_CONSTANT_ALPHA 0x8004
#define GL_ONE_MINUS_CONSTANT_COLOR 0x8002
#define GL_ONE_MINUS_DST_ALPHA 0x0305
#define GL_ONE_MINUS_DST_COLOR 0x0307
#define GL_ONE_MINUS_SRC_ALPHA 0x0303
#define GL_ONE_MINUS_SRC_COLOR 0x0301
#define GL_OPERAND0_ALPHA_ARB 0x8598
#define GL_OPERAND0_RGB_ARB 0x8590
#define GL_PACK_ALIGNMENT 0x0D05
#define GL_POINTS 0x0000
#define GL_POLYGON_OFFSET_FILL 0x8037
#define GL_POLYGON_OFFSET_LINE 0x2A02
#define GL_PREVIOUS_ARB 0x8578
#define GL_PRIMARY_COLOR_ARB 0x8577
#define GL_PROJECTION 0x1701
#define GL_PROJECTION_MATRIX 0x0BA7
#define GL_QUADS 0x0007
#define GL_RED 0x1903
#define GL_RENDERER 0x1F01
#define GL_REPEAT 0x2901
#define GL_REPLACE 0x1E01
#define GL_RGB 0x1907
#define GL_RGB565 0x8D62
#define GL_RGBA 0x1908
#define GL_RGB_SCALE_ARB 0x8573
#define GL_SCISSOR_TEST 0x0C11
#define GL_SHORT 0x1402
#define GL_SMOOTH 0x1D01
#define GL_SOURCE0_ALPHA_ARB 0x8588
#define GL_SOURCE0_RGB_ARB 0x8580
#define GL_SRC_ALPHA 0x0302
#define GL_SRC_ALPHA_SATURATE 0x0308
#define GL_SRC_COLOR 0x0300
#define GL_STENCIL_BUFFER_BIT 0x00000400
#define GL_STENCIL_TEST 0x0B90
#define GL_STENCIL_TEST_TWO_SIDE_EXT 0x8910
#define GL_SUBTRACT_ARB 0x84E7
#define GL_TEXTURE 0x1702
#define GL_TEXTURE0_ARB 0x84C0
#define GL_TEXTURE1_ARB 0x84C1
#define GL_TEXTURE2_ARB 0x84C2
#define GL_TEXTURE_2D 0x0DE1
#define GL_TEXTURE_COORD_ARRAY 0x8078
#define GL_TEXTURE_ENV 0x2300
#define GL_TEXTURE_ENV_COLOR 0x2201
#define GL_TEXTURE_ENV_MODE 0x2200
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_MAX_ANISOTROPY_EXT 0x84FE
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_TRIANGLES 0x0004
#define GL_TRIANGLE_FAN 0x0006
#define GL_TRIANGLE_STRIP 0x0005
#define GL_TRUE 1
#define GL_UNPACK_ALIGNMENT 0x0CF5
#define GL_UNSIGNED_BYTE 0x1401
#define GL_UNSIGNED_INT 0x1405
#define GL_UNSIGNED_SHORT 0x1403
#define GL_VENDOR 0x1F00
#define GL_VERSION 0x1F02
#define GL_VERTEX_ARRAY 0x8074
#define GL_ZERO 0

using PFNGLACTIVETEXTUREARBPROC = void (*)(GLenum);
using PFNGLCLIENTACTIVETEXTUREARBPROC = void (*)(GLenum);
using PFNGLMULTITEXCOORD2FARBPROC = void (*)(GLenum, GLfloat, GLfloat);
using PFNGLMULTITEXCOORD3FARBPROC = void (*)(GLenum, GLfloat, GLfloat, GLfloat);
using PFNGLMULTIDRAWARRAYSEXTPROC = void (*)(GLenum, GLint *, GLsizei *, GLsizei);
using PFNGLMULTIDRAWELEMENTSEXTPROC = void (*)(GLenum, const GLsizei *, GLenum, const GLvoid **, GLsizei);
using PFNGLDRAWRANGEELEMENTSEXTPROC = void (*)(GLenum, GLuint, GLuint, GLsizei, GLenum, const GLvoid *);
using PFNGLACTIVESTENCILFACEEXTPROC = void (*)(GLenum);
using PFNGLSTENCILOPSEPARATEATIPROC = void (*)(GLenum, GLenum, GLenum, GLenum);
using PFNGLSTENCILFUNCSEPARATEATIPROC = void (*)(GLenum, GLenum, GLint, GLuint);

bool ac3ds_gl_init();
void ac3ds_gl_shutdown();
void ac3ds_gl_begin_frame();
void ac3ds_gl_end_frame();
void ac3ds_gl_startup_clear();
void ac3ds_gl_startup_draw(bool fog = false);
void ac3ds_gl_end_startup_frame();
void ac3ds_gl_probe_empty();
void ac3ds_gl_probe_minimal();
void ac3ds_gl_probe_projective();
void ac3ds_gl_probe_fog();
void ac3ds_gl_set_command_dumps(unsigned count);
void ac3ds_gl_set_projective(bool allowed);
void ac3ds_gl_startup_state(uint32_t out[6]);
bool ac3ds_gl_queue_readback_565(GLuint id, uint16_t **out, int *outw, int *outh);
bool ac3ds_gl_drain_startup_frame(uint32_t &gpuStatus, int32_t &readResult);
void ac3ds_gl_set_late_vblank(bool enabled);
// Hold a frame back until the previous one has reached the screen, so a quick
// frame after a late one neither tears nor replaces it unseen.
void ac3ds_gl_set_present_guard(bool enabled);
// Hud gun pass: whether it takes the per-pixel fog stage, and whether it gets
// its own depth band in front of the world under the Z-buffer.
void ac3ds_gl_set_gun_isolation(bool fog, bool band);
void ac3ds_gl_set_fast_vertex_offsets(bool enabled);
void ac3ds_gl_set_shared_world_vertices(bool enabled);
void ac3ds_gl_set_joined_water_commands(bool enabled);
void ac3ds_gl_collect_retired();
void *ac3ds_gl_proc(const char *);
bool ac3ds_gl_texture_ready(GLuint);
bool ac3ds_gl_read_ok();
// Diagnostic frame recorder: the right half of the finished top picture for a
// run of consecutive gameplay frames, in the GPU's own tile order. State: 0
// idle, 1 recording, 2 finishing, 3 ready. The data stays valid until freed.
bool ac3ds_gl_burst_start(unsigned frames);
int ac3ds_gl_burst_state();
const void *ac3ds_gl_burst_data(unsigned *bytes);
void ac3ds_gl_burst_free();
void ac3ds_gl_stats(char *, unsigned);
void ac3ds_gl_profile_start(unsigned intervals);
void ac3ds_gl_profile_finish();
bool ac3ds_gl_profile_take(unsigned &intervals, unsigned long long &ticks,
                           unsigned long long &p99Ticks, unsigned long long &maxTicks,
                           unsigned &over20ms, unsigned &vblanks, unsigned &missedVblanks);
bool ac3ds_gl_render_to_texture(GLuint texture);
bool ac3ds_gl_restore_water_world(bool refract, unsigned generation);
bool ac3ds_gl_save_water_world(bool refract, unsigned generation);
void ac3ds_gl_render_to_screen();
bool ac3ds_gl_begin_shadow_target(int size);
bool ac3ds_gl_begin_world_geometry(const void *vertices, GLsizei count);
constexpr int Ac3dsWorldBatchSlots = 6;
void ac3ds_gl_capture_frame_world();
void ac3ds_gl_replay_frame_world();
void ac3ds_gl_draw_world_strips(GLenum mode, const GLint *first, const GLsizei *count, GLsizei draws);
struct Ac3dsWorldStrips { GLenum mode; const GLint *first; const GLsizei *count; GLsizei draws; };
bool ac3ds_gl_begin_cached_world(int slot);
void ac3ds_gl_build_world_cache(int slot, const Ac3dsWorldStrips *strips, int count);
void ac3ds_gl_discard_world_cache(int slot);
bool ac3ds_gl_can_draw_projected_strip();
void ac3ds_gl_draw_projected_strip(const void *source, GLsizei count);
bool ac3ds_gl_can_draw_font_quads();
void ac3ds_gl_font_quad(float x0, float y0, float x1, float y1,
                        float u0, float v0, float u1, float v1);
bool ac3ds_gl_draw_model(unsigned mesh, unsigned pose, unsigned lighting,
                         GLuint low, GLuint high, GLsizei count, const GLushort *indices);
bool ac3ds_gl_draw_stencil_volume(unsigned volume, const float *positions, unsigned vertices,
                                  const float *extrusion, GLsizei count, const GLushort *indices);

void glAlphaFunc(GLenum, GLfloat);
void glBegin(GLenum);
void glBindTexture(GLenum, GLuint);
void glBlendFunc(GLenum, GLenum);
void glCallList(GLuint);
void glClear(GLbitfield);
void glClearColor(GLfloat, GLfloat, GLfloat, GLfloat);
void glClearDepth(GLdouble);
void glClearStencil(GLint);
void glColor3d(GLdouble, GLdouble, GLdouble);
void glColor3f(GLfloat, GLfloat, GLfloat);
void glColor3ub(GLubyte, GLubyte, GLubyte);
void glColor4f(GLfloat, GLfloat, GLfloat, GLfloat);
void glColor4ub(GLubyte, GLubyte, GLubyte, GLubyte);
void glColor4ubv(const GLubyte *);
void glColorMask(GLboolean, GLboolean, GLboolean, GLboolean);
void glColorPointer(GLint, GLenum, GLsizei, const GLvoid *);
void glCullFace(GLenum);
void glDeleteLists(GLuint, GLsizei);
void glDeleteTextures(GLsizei, const GLuint *);
void glDepthFunc(GLenum);
void glDepthMask(GLboolean);
void glDisable(GLenum);
void glDisableClientState(GLenum);
void glDrawArrays(GLenum, GLint, GLsizei);
void glDrawElements(GLenum, GLsizei, GLenum, const GLvoid *);
void glEnable(GLenum);
void glEnableClientState(GLenum);
void glEnd();
void glEndList();
void glFogf(GLenum, GLfloat);
void glFogfv(GLenum, const GLfloat *);
void glFogi(GLenum, GLint);
void glFrustum(GLdouble, GLdouble, GLdouble, GLdouble, GLdouble, GLdouble);
GLuint glGenLists(GLsizei);
void glGenTextures(GLsizei, GLuint *);
void glGetFloatv(GLenum, GLfloat *);
void glGetIntegerv(GLenum, GLint *);
const GLubyte *glGetString(GLenum);
void glGetTexImage(GLenum, GLint, GLenum, GLenum, GLvoid *);
void glHint(GLenum, GLenum);
void glLineWidth(GLfloat);
void glLoadIdentity();
void glLoadMatrixf(const GLfloat *);
void glMatrixMode(GLenum);
void glMultMatrixf(const GLfloat *);
void glNewList(GLuint, GLenum);
void glNormalPointer(GLenum, GLsizei, const GLvoid *);
void glOrtho(GLdouble, GLdouble, GLdouble, GLdouble, GLdouble, GLdouble);
void glPixelStorei(GLenum, GLint);
void glPolygonMode(GLenum, GLenum);
void glPolygonOffset(GLfloat, GLfloat);
void glPopMatrix();
void glPushMatrix();
void glReadPixels(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, GLvoid *);
void glRotatef(GLfloat, GLfloat, GLfloat, GLfloat);
void glScalef(GLfloat, GLfloat, GLfloat);
void glScissor(GLint, GLint, GLsizei, GLsizei);
void glShadeModel(GLenum);
void glStencilFunc(GLenum, GLint, GLuint);
void glStencilOp(GLenum, GLenum, GLenum);
void glTexCoord2f(GLfloat, GLfloat);
void glTexCoord2i(GLint, GLint);
void glTexCoordPointer(GLint, GLenum, GLsizei, const GLvoid *);
void glTexEnvfv(GLenum, GLenum, const GLfloat *);
void glTexEnvi(GLenum, GLenum, GLint);
void glTexImage2D(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const GLvoid *);
void glTexParameteri(GLenum, GLenum, GLint);
void glTranslatef(GLfloat, GLfloat, GLfloat);
void glVertex2d(GLdouble, GLdouble);
void glVertex2f(GLfloat, GLfloat);
void glVertex3f(GLfloat, GLfloat, GLfloat);
void glVertex3fv(const GLfloat *);
void glVertexPointer(GLint, GLenum, GLsizei, const GLvoid *);
void glViewport(GLint, GLint, GLsizei, GLsizei);

// Diagnostic: store linear (W-buffer) depth for perspective draws, as build 11 did.
void ac3ds_gl_set_linear_depth(bool enabled);
// Fog per pixel through the procedural texture unit (default), or the GPU fog table.
void ac3ds_gl_set_pixel_fog(bool enabled);
// With linear depth, the hud gun draws in a depth band in front of the world.
void ac3ds_gl_set_hud_gun_depth(bool on);
bool ac3ds_gl_queue_aim_depth();
bool ac3ds_gl_take_aim_depth(float &depth);
