// Copyright (C) 2016 The Qt Company Ltd.
// Copyright (C) 2023 L. E. Segovia <amy@amyspark.me>
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#ifndef QWINDOWSEGLCONTEXT_H
#define QWINDOWSEGLCONTEXT_H

#include "qwindowsopenglcontext.h"
#include "qwindowsopengltester.h"
#include "../angle/qangleeglcontext.h"

QT_BEGIN_NAMESPACE
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

    void *createWindowSurface(void *nativeWindow, void *nativeConfig, const QColorSpace &colorSpace,
                              int *err) override;
    void destroyWindowSurface(void *nativeSurface) override;

    QSurfaceFormat formatFromConfig(EGLDisplay display, EGLConfig config,
                                    const QSurfaceFormat &referenceFormat);

    bool hasPixelFormatFloatSupport() const { return m_hasPixelFormatFloatSupport; }

    static QLibEGL libEGL;
    static QLibGLESv2 libGLESv2;

private:
    explicit QWindowsEGLStaticContext(EGLDisplay display);
    static bool initializeAngle(QWindowsOpenGLTester::Renderers preferredType, HDC dc,
                                EGLDisplay *display, EGLint *major, EGLint *minor);

    const EGLDisplay m_display;
    bool m_hasSRGBColorSpaceSupport;
    bool m_hasSCRGBColorSpaceSupport;
    bool m_hasBt2020PQColorSpaceSupport;
    bool m_hasPixelFormatFloatSupport;
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
