// Copyright (C) 2016 The Qt Company Ltd.
// Copyright (C) 2023 L. E. Segovia <amy@amyspark.me>
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#include "qwindowseglcontext.h"
#include "qwindowscontext.h"
#include "qwindowswindow.h"

#include <QtCore/qdebug.h>
#include <QtGui/qopenglcontext.h>

#if QT_CONFIG(angle) || defined(QT_OPENGL_DYNAMIC)
#  include <EGL/eglext.h>
#  include <VersionHelpers.h>
#endif

#include <string_view>

using namespace std::string_view_literals;

#include "../angle/qangleeglconvenience_p.h"

QT_BEGIN_NAMESPACE

/*!
    \class QWindowsEGLStaticContext
    \brief Static data for QWindowsEGLContext.

    Keeps the display. The class is shared via QSharedPointer in the windows, the
    contexts and in QWindowsIntegration. The display will be closed if the last instance
    is deleted.

    No EGL or OpenGL functions are called directly. Instead, they are resolved
    dynamically. This works even if the plugin links directly to libegl/libglesv2 so
    there is no need to differentiate between dynamic or Angle-only builds in here.

    \internal
*/

QWindowsEGLStaticContext::QWindowsEGLStaticContext(EGLDisplay display)
    : m_display(display),
      m_hasSRGBColorSpaceSupport(false),
      m_hasSCRGBColorSpaceSupport(false),
      m_hasBt2020PQColorSpaceSupport(false),
      m_hasPixelFormatFloatSupport(false)
{
    m_hasSRGBColorSpaceSupport = q_hasEglExtension(display, "EGL_KHR_gl_colorspace");
    m_hasSCRGBColorSpaceSupport = q_hasEglExtension(display, "EGL_EXT_gl_colorspace_scrgb_linear");
    m_hasBt2020PQColorSpaceSupport = q_hasEglExtension(display, "EGL_EXT_gl_colorspace_bt2020_pq");
    m_hasPixelFormatFloatSupport = q_hasEglExtension(display, "EGL_EXT_pixel_format_float");
    if (m_hasSCRGBColorSpaceSupport && !m_hasPixelFormatFloatSupport) {
        qWarning("%s: EGL_EXT_gl_colorspace_scrgb_linear supported but EGL_EXT_pixel_format_float "
                 "not available!", __FUNCTION__);
        m_hasSCRGBColorSpaceSupport = false;
    }
}

bool QWindowsEGLStaticContext::initializeAngle(QWindowsOpenGLTester::Renderers preferredType,
                                               HDC dc, EGLDisplay *display, EGLint *major,
                                               EGLint *minor)
{
#ifdef EGL_ANGLE_platform_angle
    if (QLibEGL::instance().eglGetPlatformDisplayEXT
        && (preferredType & QWindowsOpenGLTester::AngleBackendMask)) {
        static constexpr std::array<std::array<EGLint, 8>, 5> anglePlatformAttributes{ {
                { EGL_PLATFORM_ANGLE_TYPE_ANGLE, EGL_PLATFORM_ANGLE_TYPE_D3D11_ANGLE, EGL_NONE },
                { EGL_PLATFORM_ANGLE_TYPE_ANGLE, EGL_PLATFORM_ANGLE_TYPE_D3D9_ANGLE, EGL_NONE },
                { EGL_PLATFORM_ANGLE_TYPE_ANGLE, EGL_PLATFORM_ANGLE_TYPE_D3D11_ANGLE,
                  EGL_PLATFORM_ANGLE_DEVICE_TYPE_ANGLE,
                  EGL_PLATFORM_ANGLE_DEVICE_TYPE_D3D_WARP_ANGLE, EGL_NONE },
                { EGL_PLATFORM_ANGLE_TYPE_ANGLE, EGL_PLATFORM_ANGLE_TYPE_D3D11_ANGLE,
                  EGL_PLATFORM_ANGLE_D3D11ON12_ANGLE, EGL_TRUE, EGL_NONE },
                { EGL_PLATFORM_ANGLE_TYPE_ANGLE, EGL_PLATFORM_ANGLE_TYPE_OPENGL_ANGLE, EGL_NONE },
        } };
        const EGLint *attributes = nullptr;
        if (preferredType & QWindowsOpenGLTester::AngleRendererD3d11)
            attributes = anglePlatformAttributes[0].data();
        else if (preferredType & QWindowsOpenGLTester::AngleRendererD3d9)
            attributes = anglePlatformAttributes[1].data();
        else if (preferredType & QWindowsOpenGLTester::AngleRendererD3d11Warp)
            attributes = anglePlatformAttributes[2].data();
        else if (preferredType & QWindowsOpenGLTester::AngleRendererD3d11On12) {
            if (IsWindows10OrGreater()) {
                attributes = anglePlatformAttributes[3].data();
            } else {
                qWarning("%s: Attempted to use D3d11on12 in an unsupported version of windows. "
                         "Retargeting for D3d11Warp",
                         __FUNCTION__);
                attributes = anglePlatformAttributes[2].data();
            }
        } else if (preferredType & QWindowsOpenGLTester::AngleRendererOpenGL)
            attributes = anglePlatformAttributes[4].data();
        if (attributes) {
            *display = QLibEGL::instance().eglGetPlatformDisplayEXT(EGL_PLATFORM_ANGLE_ANGLE, dc, attributes);
            if (!QLibEGL::instance().eglInitialize(*display, major, minor)) {
                qWarning("%s: Unable to initialize ANGLE: error 0x%x", __FUNCTION__,
                         QLibEGL::instance().eglGetError());
                QLibEGL::instance().eglTerminate(*display);
                *display = EGL_NO_DISPLAY;
                *major = *minor = 0;
                return false;
            }
        }
    }
#else // EGL_ANGLE_platform_angle
    Q_UNUSED(preferredType);
    Q_UNUSED(dc);
    Q_UNUSED(display);
    Q_UNUSED(major);
    Q_UNUSED(minor);
#endif
    return true;
}

QWindowsEGLStaticContext *
QWindowsEGLStaticContext::create(QWindowsOpenGLTester::Renderers preferredType)
{
    const HDC dc{ QWindowsContext::instance()->displayContext() };
    if (!dc) {
        qWarning("%s: No Display", __FUNCTION__);
        return nullptr;
    }

    if (!QLibEGL::instance().init()) {
        qWarning("%s: Failed to load and resolve libEGL functions", __FUNCTION__);
        return nullptr;
    }
    if (!QLibGLESv2::instance().init()) {
        qWarning("%s: Failed to load and resolve libGLESv2 functions", __FUNCTION__);
        return nullptr;
    }

    EGLDisplay display{ EGL_NO_DISPLAY };
    EGLint major{ 0 };
    EGLint minor{ 0 };

    if (!initializeAngle(preferredType, dc, &display, &major, &minor)
        && (preferredType & QWindowsOpenGLTester::AngleRendererD3d11)) {
        preferredType &= ~QWindowsOpenGLTester::AngleRendererD3d11;
        initializeAngle(preferredType, dc, &display, &major, &minor);
    }

    if (display == EGL_NO_DISPLAY)
        display = QLibEGL::instance().eglGetDisplay(dc);
    if (!display) {
        qWarning("%s: Could not obtain EGL display", __FUNCTION__);
        return nullptr;
    }

    if (!major && !QLibEGL::instance().eglInitialize(display, &major, &minor)) {
        const auto err{ QLibEGL::instance().eglGetError() };
        qWarning("%s: Could not initialize EGL display: error 0x%x", __FUNCTION__, err);
        if (err == EGL_NOT_INITIALIZED)
            qWarning("%s: When using ANGLE, check if d3dcompiler_4x.dll is available",
                     __FUNCTION__);
        return nullptr;
    }

    qCDebug(lcQpaGl) << __FUNCTION__ << "Created EGL display" << display << 'v' << major << '.'
                     << minor;
    return new QWindowsEGLStaticContext(display);
}

QWindowsEGLStaticContext::~QWindowsEGLStaticContext()
{
    qCDebug(lcQpaGl) << __FUNCTION__ << "Releasing EGL display " << m_display;
    QLibEGL::instance().eglTerminate(m_display);
}

QWindowsOpenGLContext *QWindowsEGLStaticContext::createContext(QOpenGLContext *context)
{
    return new QWindowsEGLContext(this, context->format(), context->shareHandle());
}

void *QWindowsEGLStaticContext::createWindowSurface(void *nativeWindow, void *nativeConfig,
                                                    const QColorSpace &colorSpace, int *err)
{
    *err = 0;

    EGLint eglColorSpace{ EGL_GL_COLORSPACE_LINEAR_KHR };
    bool colorSpaceSupported{ colorSpace.isValid() };

    const auto primaries{ colorSpace.primaries() };
    const auto transferFunction{ colorSpace.transferFunction() };

    if (primaries == QColorSpace::Primaries::SRgb) {
        colorSpaceSupported = m_hasSRGBColorSpaceSupport;
        switch (transferFunction) {
        case QColorSpace::TransferFunction::SRgb:
            eglColorSpace = EGL_GL_COLORSPACE_SRGB_KHR;
            break;
        case QColorSpace::TransferFunction::Linear:
            eglColorSpace = EGL_GL_COLORSPACE_LINEAR_KHR;
            break;
        default:
            colorSpaceSupported = false;
            break;
        }
    } else if (primaries == QColorSpace::Primaries::ScRGB
               && transferFunction == QColorSpace::TransferFunction::Linear) {
        colorSpaceSupported = m_hasSCRGBColorSpaceSupport;
        switch (transferFunction) {
        case QColorSpace::TransferFunction::SRgb:
            eglColorSpace = EGL_GL_COLORSPACE_SCRGB_EXT;
            break;
        case QColorSpace::TransferFunction::Linear:
            eglColorSpace = EGL_GL_COLORSPACE_SCRGB_LINEAR_EXT;
            break;
        default:
            colorSpaceSupported = false;
            break;
        }
    } else if (primaries == QColorSpace::Primaries::Bt2020) {
        colorSpaceSupported = m_hasBt2020PQColorSpaceSupport;
        switch (transferFunction) {
        case QColorSpace::TransferFunction::PQ:
            eglColorSpace = EGL_GL_COLORSPACE_BT2020_PQ_EXT;
            break;
        case QColorSpace::TransferFunction::Linear:
            eglColorSpace = EGL_GL_COLORSPACE_BT2020_LINEAR_EXT;
            break;
        default:
            colorSpaceSupported = false;
            break;
        }
    }

    std::vector<EGLint> attributes;

    if (colorSpaceSupported) {
        attributes.emplace_back(EGL_GL_COLORSPACE);
        attributes.emplace_back(eglColorSpace);
    }

    attributes.emplace_back(EGL_NONE);

    if (!colorSpaceSupported && colorSpace.isValid())
        qWarning("%s: Requested color space is not supported by EGL implementation: %s %s (egl: 0x%x)",
                 __FUNCTION__,
                 QMetaEnum::fromType<QColorSpace::Primaries>().valueToKey(int(colorSpace.primaries())),
                 QMetaEnum::fromType<QColorSpace::TransferFunction>().valueToKey(int(colorSpace.transferFunction())),
                 eglColorSpace);

    EGLSurface surface{ QLibEGL::instance().eglCreateWindowSurface(
            m_display, nativeConfig, static_cast<EGLNativeWindowType>(nativeWindow),
            attributes.data()) };
    if (surface == EGL_NO_SURFACE) {
        *err = QLibEGL::instance().eglGetError();
        qWarning("%s: Could not create the EGL window surface: 0x%x", __FUNCTION__, *err);
    }

    return surface;
}

void QWindowsEGLStaticContext::destroyWindowSurface(void *nativeSurface)
{
    QLibEGL::instance().eglDestroySurface(m_display, nativeSurface);
}

/*!
    \class QWindowsEGLContext
    \brief Open EGL context.

    \section1 Using QWindowsEGLContext for Desktop with ANGLE
    \section2 Build Instructions
    \list
    \o Install the Direct X SDK
    \o Checkout and build ANGLE (SVN repository) as explained here:
       \l{https://chromium.googlesource.com/angle/angle/+/master/README.md}
       When building for 64bit, de-activate the "WarnAsError" option
       in every project file (as otherwise integer conversion
       warnings will break the build).
    \o Run configure.exe with the options "-opengl es2".
    \o Build qtbase and test some examples.
    \endlist

    \internal
*/
QWindowsEGLContext::QWindowsEGLContext(QWindowsEGLStaticContext *staticContext,
                                       const QSurfaceFormat &format, QPlatformOpenGLContext *share)
    : m_staticContext(staticContext), m_eglDisplay(staticContext->display())
{
    if (!m_staticContext)
        return;

    m_eglConfig = q_configFromGLFormat(m_eglDisplay, format);
    m_format = q_glFormatFromConfig(m_eglDisplay, m_eglConfig, format);
    m_shareContext = [&]() -> EGLContext {
        if (!share)
            return nullptr;
        if (const auto realShare = dynamic_cast<QWindowsEGLContext *>(share))
            return realShare->m_eglContext;
        return nullptr;
    }();

    const EGLint major{ m_format.majorVersion() };
    const EGLint minor{ m_format.minorVersion() };
    if (major > 3 || (major == 3 && minor > 0))
        qWarning("QWindowsEGLContext: ANGLE only partially supports OpenGL ES > 3.0");
    const std::array<EGLint, 5> contextAttrs{
        EGL_CONTEXT_MAJOR_VERSION, major, EGL_CONTEXT_MINOR_VERSION, minor, EGL_NONE,
    };

    QLibEGL::instance().eglBindAPI(m_api);
    m_eglContext = QLibEGL::instance().eglCreateContext(
            m_eglDisplay, m_eglConfig, m_shareContext, contextAttrs.data());
    if (m_eglContext == EGL_NO_CONTEXT && m_shareContext != EGL_NO_CONTEXT) {
        m_shareContext = nullptr;
        m_eglContext = QLibEGL::instance().eglCreateContext(
                m_eglDisplay, m_eglConfig, nullptr, contextAttrs.data());
    }

    if (m_eglContext == EGL_NO_CONTEXT) {
        const auto err{ QLibEGL::instance().eglGetError() };
        qWarning("QWindowsEGLContext: Failed to create context, eglError: %x, this: %p", err, this);
        // ANGLE gives bad alloc when it fails to reset a previously lost D3D device.
        // A common cause for this is disabling the graphics adapter used by the app.
        if (err == EGL_BAD_ALLOC)
            qWarning("QWindowsEGLContext: Graphics device lost. (Did the adapter get disabled?)");
        return;
    }

    // Make the context current to ensure the GL version query works. This needs a surface too.
    static constexpr std::array<EGLint, 7> pbufferAttributes{
        EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_LARGEST_PBUFFER, EGL_FALSE, EGL_NONE
    };
    EGLSurface pbuffer{ QLibEGL::instance().eglCreatePbufferSurface(
            m_eglDisplay, m_eglConfig, pbufferAttributes.data()) };
    if (pbuffer == EGL_NO_SURFACE)
        return;

    EGLDisplay prevDisplay{ QLibEGL::instance().eglGetCurrentDisplay() };
    if (prevDisplay == EGL_NO_DISPLAY) // when no context is current
        prevDisplay = m_eglDisplay;
    EGLContext prevContext{ QLibEGL::instance().eglGetCurrentContext() };
    EGLSurface prevSurfaceDraw{ QLibEGL::instance().eglGetCurrentSurface(EGL_DRAW) };
    EGLSurface prevSurfaceRead{ QLibEGL::instance().eglGetCurrentSurface(EGL_READ) };

    if (QLibEGL::instance().eglMakeCurrent(m_eglDisplay, pbuffer, pbuffer,
                                                        m_eglContext)) {
        const GLubyte *s{ QLibGLESv2::instance().glGetString(GL_VERSION) };
        if (s) {
            const QByteArray version(reinterpret_cast<const char *>(s));
            int major{};
            int minor{};
            if (QPlatformOpenGLContext::parseOpenGLVersion(version, major, minor)) {
                m_format.setMajorVersion(major);
                m_format.setMinorVersion(minor);
            }
        }
        m_format.setProfile(QSurfaceFormat::NoProfile);
        m_format.setOptions(QSurfaceFormat::FormatOptions());
        QLibEGL::instance().eglMakeCurrent(prevDisplay, prevSurfaceDraw,
                                                        prevSurfaceRead, prevContext);
    }
    QLibEGL::instance().eglDestroySurface(m_eglDisplay, pbuffer);
}

QWindowsEGLContext::~QWindowsEGLContext()
{
    if (m_eglContext != EGL_NO_CONTEXT) {
        QLibEGL::instance().eglDestroyContext(m_eglDisplay, m_eglContext);
        m_eglContext = EGL_NO_CONTEXT;
    }
}

bool QWindowsEGLContext::makeCurrent(QPlatformSurface *surface)
{
    Q_ASSERT(surface->surface()->supportsOpenGL());

    QLibEGL::instance().eglBindAPI(m_api);

    auto *window{ dynamic_cast<QWindowsWindow *>(surface) };
    Q_ASSERT(window);
    window->aboutToMakeCurrent();
    int err{};
    auto eglSurface{ static_cast<EGLSurface>(window->surface(m_eglConfig, &err)) };
    if (eglSurface == EGL_NO_SURFACE) {
        if (err == EGL_CONTEXT_LOST) {
            m_eglContext = EGL_NO_CONTEXT;
            qCDebug(lcQpaGl) << "Got EGL context lost in createWindowSurface() for context" << this;
        } else if (err == EGL_BAD_ACCESS) {
            // With ANGLE this means no (D3D) device and can happen when disabling/changing graphics
            // adapters.
            qCDebug(lcQpaGl) << "Bad access (missing device?) in createWindowSurface() for context"
                             << this;
        } else if (err == EGL_BAD_MATCH) {
            qCDebug(lcQpaGl) << "Got bad match in createWindowSurface() for context" << this
                             << "Check color space configuration.";
        }
        // Simulate context loss as the context is useless.
        QLibEGL::instance().eglDestroyContext(m_eglDisplay, m_eglContext);
        m_eglContext = EGL_NO_CONTEXT;
        return false;
    }

    // shortcut: on some GPUs, eglMakeCurrent is not a cheap operation
    if (QLibEGL::instance().eglGetCurrentContext() == m_eglContext
        && QLibEGL::instance().eglGetCurrentDisplay() == m_eglDisplay
        && QLibEGL::instance().eglGetCurrentSurface(EGL_READ) == eglSurface
        && QLibEGL::instance().eglGetCurrentSurface(EGL_DRAW) == eglSurface) {
        return true;
    }

    const auto ok{ QLibEGL::instance().eglMakeCurrent(m_eglDisplay, eglSurface,
                                                                   eglSurface, m_eglContext) };
    if (ok) {
        const auto requestedSwapInterval{ surface->format().swapInterval() };
        if (requestedSwapInterval >= 0 && m_swapInterval != requestedSwapInterval) {
            m_swapInterval = requestedSwapInterval;
            QLibEGL::instance().eglSwapInterval(m_staticContext->display(),
                                                             m_swapInterval);
        }
    } else {
        err = QLibEGL::instance().eglGetError();
        // EGL_CONTEXT_LOST (loss of the D3D device) is not necessarily fatal.
        // Qt Quick is able to recover for example.
        if (err == EGL_CONTEXT_LOST) {
            m_eglContext = EGL_NO_CONTEXT;
            qCDebug(lcQpaGl) << "Got EGL context lost in makeCurrent() for context" << this;
            // Drop the surface. Will recreate on the next makeCurrent.
            window->invalidateSurface();
        } else {
            qWarning("%s: Failed to make surface current. eglError: %x, this: %p", __FUNCTION__,
                     err, this);
        }
    }

    return ok;
}

void QWindowsEGLContext::doneCurrent()
{
    QLibEGL::instance().eglBindAPI(m_api);
    const auto ok{ QLibEGL::instance().eglMakeCurrent(
            m_eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT) };
    if (!ok)
        qWarning("%s: Failed to make no context/surface current. eglError: %d, this: %p",
                 __FUNCTION__, QLibEGL::instance().eglGetError(), this);
}

void QWindowsEGLContext::swapBuffers(QPlatformSurface *surface)
{
    QLibEGL::instance().eglBindAPI(m_api);
    auto *window{ dynamic_cast<QWindowsWindow *>(surface) };
    Q_ASSERT(window);
    int err{};
    auto eglSurface{ static_cast<EGLSurface>(window->surface(m_eglConfig, &err)) };
    if (eglSurface == EGL_NO_SURFACE) {
        if (err == EGL_CONTEXT_LOST) {
            m_eglContext = EGL_NO_CONTEXT;
            qCDebug(lcQpaGl) << "Got EGL context lost in createWindowSurface() for context" << this;
        }
        return;
    }

    const auto ok{ QLibEGL::instance().eglSwapBuffers(m_eglDisplay, eglSurface) };
    if (!ok) {
        err = QLibEGL::instance().eglGetError();
        if (err == EGL_CONTEXT_LOST) {
            m_eglContext = EGL_NO_CONTEXT;
            qCDebug(lcQpaGl) << "Got EGL context lost in eglSwapBuffers()";
        } else {
            qWarning("%s: Failed to swap buffers. eglError: %d, this: %p", __FUNCTION__, err, this);
        }
    }
}

QFunctionPointer QWindowsEGLContext::getProcAddress(const char *procName)
{
    QLibEGL::instance().eglBindAPI(m_api);

    QFunctionPointer procAddress{ nullptr };

    // Special logic for ANGLE extensions for blitFramebuffer and
    // renderbufferStorageMultisample. In version 2 contexts the extensions
    // must be used instead of the suffixless, version 3.0 functions.
    if (m_format.majorVersion() < 3) {
        std::string_view procNameView{ procName };
        if (procNameView == "glBlitFramebuffer"sv
            || procNameView == "glRenderbufferStorageMultisample"sv) {
            std::string extName{ procNameView };
            extName += "ANGLE"sv;
            procAddress = reinterpret_cast<QFunctionPointer>(
                    QLibEGL::instance().eglGetProcAddress(procNameView.data()));
        }
    }

    if (!procAddress)
        procAddress = reinterpret_cast<QFunctionPointer>(
                QLibEGL::instance().eglGetProcAddress(procName));

    // We support AllGLFunctionsQueryable, which means this function must be able to
    // return a function pointer for standard GLES2 functions too. These are not
    // guaranteed to be queryable via eglGetProcAddress().
    if (!procAddress)
        procAddress = reinterpret_cast<QFunctionPointer>(
                QLibGLESv2::instance().resolve(procName));

    if (QWindowsContext::verbose > 1)
        qCDebug(lcQpaGl) << __FUNCTION__ << procName
                         << QLibEGL::instance().eglGetCurrentContext() << "returns"
                         << reinterpret_cast<void *>(procAddress);

    return procAddress;
}

QT_END_NAMESPACE
