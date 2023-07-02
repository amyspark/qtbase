// Copyright (C) 2016 The Qt Company Ltd.
// Copyright (C) 2023 L. E. Segovia <amy@amyspark.me>
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#include <Qt>

#include <EGL/egl.h>
#include <GLES/gl.h>

QT_BEGIN_NAMESPACE

struct Q_DECL_HIDDEN QLibEGL
{
    static QLibEGL &instance();

    bool init();

    EGLint(EGLAPIENTRY *eglGetError)(void);
    EGLDisplay(EGLAPIENTRY *eglGetDisplay)(EGLNativeDisplayType display_id);
    EGLBoolean(EGLAPIENTRY *eglInitialize)(EGLDisplay dpy, EGLint *major, EGLint *minor);
    EGLBoolean(EGLAPIENTRY *eglTerminate)(EGLDisplay dpy);
    EGLBoolean(EGLAPIENTRY *eglChooseConfig)(EGLDisplay dpy, const EGLint *attrib_list,
                                             EGLConfig *configs, EGLint config_size,
                                             EGLint *num_config);
    EGLBoolean(EGLAPIENTRY *eglGetConfigAttrib)(EGLDisplay dpy, EGLConfig config, EGLint attribute,
                                                EGLint *value);
    EGLSurface(EGLAPIENTRY *eglCreateWindowSurface)(EGLDisplay dpy, EGLConfig config,
                                                    EGLNativeWindowType win,
                                                    const EGLint *attrib_list);
    EGLSurface(EGLAPIENTRY *eglCreatePbufferSurface)(EGLDisplay dpy, EGLConfig config,
                                                     const EGLint *attrib_list);
    EGLBoolean(EGLAPIENTRY *eglDestroySurface)(EGLDisplay dpy, EGLSurface surface);
    EGLBoolean(EGLAPIENTRY *eglBindAPI)(EGLenum api);
    EGLBoolean(EGLAPIENTRY *eglSwapInterval)(EGLDisplay dpy, EGLint interval);
    EGLContext(EGLAPIENTRY *eglCreateContext)(EGLDisplay dpy, EGLConfig config,
                                              EGLContext share_context, const EGLint *attrib_list);
    EGLBoolean(EGLAPIENTRY *eglDestroyContext)(EGLDisplay dpy, EGLContext ctx);
    EGLBoolean(EGLAPIENTRY *eglMakeCurrent)(EGLDisplay dpy, EGLSurface draw, EGLSurface read,
                                            EGLContext ctx);
    EGLContext(EGLAPIENTRY *eglGetCurrentContext)(void);
    EGLSurface(EGLAPIENTRY *eglGetCurrentSurface)(EGLint readdraw);
    EGLDisplay(EGLAPIENTRY *eglGetCurrentDisplay)(void);
    EGLBoolean(EGLAPIENTRY *eglSwapBuffers)(EGLDisplay dpy, EGLSurface surface);
    const char *(EGLAPIENTRY *eglQueryString)(EGLDisplay dpy, EGLint name);
    QFunctionPointer(EGLAPIENTRY *eglGetProcAddress)(const char *procname);

    EGLDisplay(EGLAPIENTRY *eglGetPlatformDisplayEXT)(EGLenum platform, void *native_display,
                                                      const EGLint *attrib_list);

private:
    void *resolve(const char *name);
    HMODULE m_lib;
};

struct Q_DECL_HIDDEN QLibGLESv2
{
    static QLibGLESv2 &instance();

    bool init();

    void *moduleHandle() const { return m_lib; }

    const GLubyte *(APIENTRY *glGetString)(GLenum name);

    void *resolve(const char *name);

private:
    HMODULE m_lib;
};

QT_END_NAMESPACE
