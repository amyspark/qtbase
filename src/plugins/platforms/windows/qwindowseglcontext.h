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
    void *moduleHandle() const override { return QLibGLESv2::instance().moduleHandle(); }
    QOpenGLContext::OpenGLModuleType moduleType() const override { return QOpenGLContext::LibGLES; }

    void *createWindowSurface(void *nativeWindow, void *nativeConfig, const QColorSpace &colorSpace,
                              int *err) override;
    void destroyWindowSurface(void *nativeSurface) override;

    QSurfaceFormat formatFromConfig(EGLDisplay display, EGLConfig config,
                                    const QSurfaceFormat &referenceFormat);

    bool hasPixelFormatFloatSupport() const { return m_hasPixelFormatFloatSupport; }

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

class QWindowsEGLContext : public QANGLEContext, public QWindowsOpenGLContext
{
public:
    explicit QWindowsEGLContext(QWindowsEGLStaticContext *staticContext,
                                const QSurfaceFormat &format, QPlatformOpenGLContext *share);
    ~QWindowsEGLContext() override;



    bool makeCurrent(QPlatformSurface *surface) override;
    void doneCurrent() override { return QANGLEContext::doneCurrent(); }
    void swapBuffers(QPlatformSurface *surface) override;
    QFunctionPointer getProcAddress(const char *procName) override { return QANGLEContext::getProcAddress(procName); }

    QSurfaceFormat format() const override { return QANGLEContext::format(); }
    bool isSharing() const override { return QANGLEContext::isSharing(); }
    bool isValid() const override { return QANGLEContext::isValid(); }

    void *nativeDisplay() const override { return display(); }
    void *nativeConfig() const override { return config(); }

private:
    QWindowsEGLStaticContext *m_staticContext;
};

QT_END_NAMESPACE

#endif // QWINDOWSEGLCONTEXT_H
