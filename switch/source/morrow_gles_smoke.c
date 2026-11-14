#include "morrow_gles_smoke.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include <switch.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>

#include "elf_probe.h"
#include "android_egl_gles_compat.h"

static void report(FILE *out, const char *format, ...)
{
    if (!out) return;
    va_list args;
    va_start(args, format);
    vfprintf(out, format, args);
    va_end(args);
    fputc('\n', out);
    fflush(out);
}

static FILE *new_graphics_report(void)
{
    if (mkdir(ENGINE_REPORT_DIR, 0777) != 0 && errno != EEXIST) return NULL;
    for (unsigned i = 1; i <= 99; i++) {
        char path[128];
        const int n = snprintf(path, sizeof(path),
                               ENGINE_REPORT_DIR "/graphics-%02u.txt", i);
        if (n <= 0 || (size_t)n >= sizeof(path)) break;
        const int fd = open(path, O_CREAT | O_EXCL | O_WRONLY, 0600);
        if (fd < 0) {
            if (errno == EEXIST) continue;
            break;
        }
        FILE *out = fdopen(fd, "w");
        if (out) return out;
        close(fd);
        break;
    }
    return NULL;
}

static GLuint compile_shader(GLenum kind, const char *source, FILE *log)
{
    GLuint shader = glCreateShader(kind);
    if (!shader) {
        report(log, "gl_create_shader_failed_type=0x%x", (unsigned)kind);
        return 0;
    }
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);
    GLint compiled = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (compiled != GL_TRUE) {
        char message[256] = {0};
        glGetShaderInfoLog(shader, sizeof(message), NULL, message);
        report(log, "gl_shader_error=%s", message);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

static GLuint create_triangle_program(FILE *log)
{
    // The simplest shader-based GLES2 draw exercises actual compilation,
    // buffer upload and presentation. It never claims to render Roblox data.
    static const char *vertex =
        "attribute vec2 aPosition;\n"
        "attribute vec3 aColour;\n"
        "varying vec3 vColour;\n"
        "void main() {\n"
        "  gl_Position = vec4(aPosition, 0.0, 1.0);\n"
        "  vColour = aColour;\n"
        "}\n";
    static const char *fragment =
        "precision mediump float;\n"
        "varying vec3 vColour;\n"
        "void main() { gl_FragColor = vec4(vColour, 1.0); }\n";
    GLuint vert = compile_shader(GL_VERTEX_SHADER, vertex, log);
    if (!vert) return 0;
    GLuint frag = compile_shader(GL_FRAGMENT_SHADER, fragment, log);
    if (!frag) {
        glDeleteShader(vert);
        return 0;
    }
    GLuint program = glCreateProgram();
    if (program) {
        glAttachShader(program, vert);
        glAttachShader(program, frag);
        glBindAttribLocation(program, 0, "aPosition");
        glBindAttribLocation(program, 1, "aColour");
        glLinkProgram(program);
        GLint linked = GL_FALSE;
        glGetProgramiv(program, GL_LINK_STATUS, &linked);
        if (linked != GL_TRUE) {
            char message[256] = {0};
            glGetProgramInfoLog(program, sizeof(message), NULL, message);
            report(log, "gl_link_error=%s", message);
            glDeleteProgram(program);
            program = 0;
        }
    }
    glDeleteShader(vert);
    glDeleteShader(frag);
    return program;
}

void morrow_gles_smoke_run(void)
{
    FILE *log = new_graphics_report();
    report(log, "# Morrow v0.38 native Mesa EGL/GLES2 smoke test");
    report(log, "roblox_engine_executed=no");
    report(log, "roblox_frame_rendered=no");
    uint64_t bridge_symbol = 0;
    const bool bridge_gl = android_egl_gles_compat_resolve(
        "glCreateShader", NULL, NULL, &bridge_symbol);
    report(log, "native_guest_gl_symbol_resolved=%s", bridge_gl ? "yes" : "no");
    report(log, "guest_egl_window_surface_bridge=no");

    EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLContext context = EGL_NO_CONTEXT;
    EGLSurface surface = EGL_NO_SURFACE;
    GLuint program = 0;
    GLuint vertex_buffer = 0;
    if (display == EGL_NO_DISPLAY) {
        report(log, "failure=egl_get_display:0x%x", eglGetError());
        goto done;
    }
    EGLint major = 0, minor = 0;
    if (eglInitialize(display, &major, &minor) != EGL_TRUE) {
        report(log, "failure=egl_initialize:0x%x", eglGetError());
        goto done;
    }
    report(log, "egl_version=%d.%d", major, minor);
    if (eglBindAPI(EGL_OPENGL_ES_API) != EGL_TRUE) {
        report(log, "failure=egl_bind_gles:0x%x", eglGetError());
        goto done;
    }
    static const EGLint config_attributes[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
        EGL_NONE
    };
    EGLConfig config;
    EGLint count = 0;
    if (eglChooseConfig(display, config_attributes, &config, 1, &count) != EGL_TRUE ||
        count == 0) {
        report(log, "failure=egl_choose_config:0x%x", eglGetError());
        goto done;
    }
    surface = eglCreateWindowSurface(display, config, nwindowGetDefault(), NULL);
    if (surface == EGL_NO_SURFACE) {
        report(log, "failure=egl_create_window_surface:0x%x", eglGetError());
        goto done;
    }
    static const EGLint context_attributes[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE
    };
    context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attributes);
    if (context == EGL_NO_CONTEXT) {
        report(log, "failure=egl_create_context:0x%x", eglGetError());
        goto done;
    }
    if (eglMakeCurrent(display, surface, surface, context) != EGL_TRUE) {
        report(log, "failure=egl_make_current:0x%x", eglGetError());
        goto done;
    }
    report(log, "egl_swap_interval_1=%s",
           eglSwapInterval(display, 1) == EGL_TRUE ? "yes" : "no");
    const GLubyte *version = glGetString(GL_VERSION);
    report(log, "gles_version=%s", version ? (const char *)version : "unavailable");
    program = create_triangle_program(log);
    if (!program) {
        report(log, "failure=gl_create_triangle_program");
        goto done;
    }

    static const GLfloat triangle[] = {
        // x, y, r, g, b: three real GLES2 vertices, not engine content.
         0.0f,  0.7f, 0.95f, 0.65f, 0.95f,
        -0.7f, -0.6f, 0.20f, 0.85f, 0.75f,
         0.7f, -0.6f, 0.35f, 0.55f, 0.95f
    };
    glGenBuffers(1, &vertex_buffer);
    if (!vertex_buffer) {
        report(log, "failure=gl_create_vertex_buffer");
        goto done;
    }
    glBindBuffer(GL_ARRAY_BUFFER, vertex_buffer);
    glBufferData(GL_ARRAY_BUFFER, sizeof(triangle), triangle, GL_STATIC_DRAW);
    glUseProgram(program);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(GLfloat), (void *)0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(GLfloat),
                          (void *)(2 * sizeof(GLfloat)));

    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);
    report(log, "event=first_gles_frame_submitted");
    bool first_swap = false;
    unsigned frames = 0;
    while (appletMainLoop()) {
        // Break when Plus/B is pressed; this diagnostic returns to hbmenu.
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & (HidNpadButton_Plus | HidNpadButton_B))
            break;
        glViewport(0, 0, 1280, 720);
        glClearColor(0.07f, 0.06f, 0.16f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        if (!first_swap) {
            const GLenum error = glGetError();
            report(log, "first_gles_draw_error=0x%x", (unsigned)error);
            if (error != GL_NO_ERROR) break;
        }
        if (eglSwapBuffers(display, surface) != EGL_TRUE) {
            report(log, "failure=egl_swap_buffers:0x%x", eglGetError());
            break;
        }
        if (!first_swap) {
            first_swap = true;
            report(log, "egl_first_swap_succeeded=yes");
        }
        // A diagnostic should never leave someone stuck in a GPU loop if
        // controller input is unavailable. Thirty seconds is enough to see it.
        if (++frames >= 1800) break;
    }
    report(log, "egl_frames_swapped=%u", frames);

done:
    if (vertex_buffer) glDeleteBuffers(1, &vertex_buffer);
    if (program) glDeleteProgram(program);
    if (display != EGL_NO_DISPLAY) {
        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (context != EGL_NO_CONTEXT) eglDestroyContext(display, context);
        if (surface != EGL_NO_SURFACE) eglDestroySurface(display, surface);
        eglTerminate(display);
    }
    report(log, "event=graphics_test_finished");
    if (log) fclose(log);
}
