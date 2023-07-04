// Copyright (C) 2016 The Qt Company Ltd.
// Copyright (C) 2023 L. E. Segovia <amy@amyspark.me>
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#ifndef QWINDOWSEGLCONTEXT_H
#define QWINDOWSEGLCONTEXT_H

#include "qwindowsopenglcontext.h"
#include "qwindowsopengltester.h"
#include <EGL/egl.h>

QT_BEGIN_NAMESPACE

struct QWindowsLibEGL
{
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

struct QWindowsLibGLESv2
{
    bool init();

    void *moduleHandle() const { return m_lib; }

    const GLubyte *(APIENTRY *glGetString)(GLenum name);

    void *resolve(const char *name);

private:
    HMODULE m_lib;
};

class QWindowsEGLStaticContext : public QWindowsStaticOpenGLContext
{
    Q_DISABLE_COPY_MOVE(QWindowsEGLStaticContext)

public:
    static QWindowsEGLStaticContext *create(QWindowsOpenGLTester::Renderers preferredType);
    ~QWindowsEGLStaticContext() override;

    EGLDisplay display() const { return m_display; }

    QWindowsOpenGLContext *createContext(QOpenGLContext *context) override;
    void *moduleHandle() const override { return libGLESv2.moduleHandle(); }
    QOpenGLContext::OpenGLModuleType moduleType() const override { return QOpenGLContext::LibGLES; }

    void *createWindowSurface(void *nativeWindow, void *nativeConfig, int *err) override;
    void destroyWindowSurface(void *nativeSurface) override;

    QSurfaceFormat formatFromConfig(EGLDisplay display, EGLConfig config,
                                    const QSurfaceFormat &referenceFormat);

    static QWindowsLibEGL libEGL;
    static QWindowsLibGLESv2 libGLESv2;

private:
    explicit QWindowsEGLStaticContext(EGLDisplay display);
    static bool initializeAngle(QWindowsOpenGLTester::Renderers preferredType, HDC dc,
                                EGLDisplay *display, EGLint *major, EGLint *minor);

    const EGLDisplay m_display;
};

class QWindowsEGLContext : public QWindowsOpenGLContext, public QNativeInterface::QEGLContext
{
public:
    explicit QWindowsEGLContext(QWindowsEGLStaticContext *staticContext,
                                const QSurfaceFormat &format, QPlatformOpenGLContext *share);
    explicit QWindowsEGLContext(QWindowsEGLStaticContext *staticContext, HGLRC context,
                                HWND window);
    ~QWindowsEGLContext() override;

    bool makeCurrent(QPlatformSurface *surface) override;
    void doneCurrent() override;
    void swapBuffers(QPlatformSurface *surface) override;
    QFunctionPointer getProcAddress(const char *procName) override;

    QSurfaceFormat format() const override { return m_format; }
    bool isSharing() const override { return m_shareContext != EGL_NO_CONTEXT; }
    bool isValid() const override { return m_eglContext != EGL_NO_CONTEXT && !m_markedInvalid; }

    EGLContext nativeContext() const override { return m_eglContext; }
    EGLDisplay display() const override { return m_eglDisplay; }
    EGLConfig config() const override { return m_eglConfig; }

    virtual void invalidateContext() override { m_markedInvalid = true; }

private:
    QWindowsEGLStaticContext *m_staticContext;
    EGLContext m_eglContext;
    EGLContext m_shareContext;
    EGLDisplay m_eglDisplay;
    EGLConfig m_eglConfig;
    QSurfaceFormat m_format;
    EGLenum m_api = EGL_OPENGL_ES_API;
    int m_swapInterval = -1;

    bool m_markedInvalid = false;
};

QT_END_NAMESPACE

#endif // QWINDOWSEGLCONTEXT_H
