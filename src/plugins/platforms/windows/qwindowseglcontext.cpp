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

using namespace Qt::Literals::StringLiterals;
using namespace std::string_view_literals;

#ifndef EGL_OPENGL_ES3_BIT_KHR
#  define EGL_OPENGL_ES3_BIT_KHR 0x0040
#endif

#include "qwindowseglconvenience_p.h"

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

QWindowsLibEGL QWindowsEGLStaticContext::libEGL;
QWindowsLibGLESv2 QWindowsEGLStaticContext::libGLESv2;

#ifdef Q_CC_MINGW
static inline void *resolveFunc(HMODULE lib, const char *name)
{
    const auto baseNameStr{ QString::fromLatin1(name) };
    QString nameStr;
    void *proc = 0;

    // Play nice with 32-bit mingw: Try func first, then func@0, func@4,
    // func@8, func@12, ..., func@64. The def file does not provide any aliases
    // in libEGL and libGLESv2 in these builds which results in exporting
    // function names like eglInitialize@12. This cannot be fixed without
    // breaking binary compatibility. So be flexible here instead.

    int argSize = -1;
    while (!proc && argSize <= 64) {
        nameStr = baseNameStr;
        if (argSize >= 0)
            nameStr += u'@' + QString::number(argSize);
        argSize = argSize < 0 ? 0 : argSize + 4;
        proc = reinterpret_cast<void *>(::GetProcAddress(lib, nameStr.toLatin1().constData()));
    }
    return proc;
}
#else
static inline void *resolveFunc(HMODULE lib, const char *name)
{
    return reinterpret_cast<void *>(::GetProcAddress(lib, name));
}
#endif // Q_CC_MINGW

void *QWindowsLibEGL::resolve(const char *name)
{
    return m_lib ? resolveFunc(m_lib, name) : nullptr;
}

#define GETPROC(name) reinterpret_cast<decltype(name)>(resolve(#name))

#define RESOLVE(name) name = GETPROC(name)

bool QWindowsLibEGL::init()
{
    static constexpr LPCWSTR dllName{ L"libEGL" };

    qCDebug(lcQpaGl) << "Qt: Using EGL from" << dllName;

    m_lib = ::LoadLibraryW(dllName);
    if (!m_lib) {
        qErrnoWarning(::GetLastError(), "Failed to load %s", dllName);
        return false;
    }

    RESOLVE(eglGetError);
    RESOLVE(eglGetDisplay);
    RESOLVE(eglInitialize);
    RESOLVE(eglTerminate);
    RESOLVE(eglChooseConfig);
    RESOLVE(eglGetConfigAttrib);
    RESOLVE(eglCreateWindowSurface);
    RESOLVE(eglCreatePbufferSurface);
    RESOLVE(eglDestroySurface);
    RESOLVE(eglBindAPI);
    RESOLVE(eglSwapInterval);
    RESOLVE(eglCreateContext);
    RESOLVE(eglDestroyContext);
    RESOLVE(eglMakeCurrent);
    RESOLVE(eglGetCurrentContext);
    RESOLVE(eglGetCurrentSurface);
    RESOLVE(eglGetCurrentDisplay);
    RESOLVE(eglSwapBuffers);
    RESOLVE(eglGetProcAddress);

    if (!eglGetError || !eglGetDisplay || !eglInitialize || !eglGetProcAddress || !eglQueryString)
        return false;

    eglGetPlatformDisplayEXT = nullptr;
#ifdef EGL_ANGLE_platform_angle
    eglGetPlatformDisplayEXT = reinterpret_cast<decltype(eglGetPlatformDisplayEXT)>(
            eglGetProcAddress("eglGetPlatformDisplayEXT"));
#endif

    return true;
}

void *QWindowsLibGLESv2::resolve(const char *name)
{
    return m_lib ? resolveFunc(m_lib, name) : nullptr;
}

bool QWindowsLibGLESv2::init()
{
    static constexpr LPCWSTR dllName{ L"libGLESv2" };

    qCDebug(lcQpaGl) << "Qt: Using OpenGL ES 2.0 from" << dllName;

    m_lib = ::LoadLibraryW(dllName);
    if (!m_lib) {
        qErrnoWarning(int(GetLastError()), "Failed to load %s", dllName);
        return false;
    }

    void(APIENTRY * glBindTexture)(GLenum target, GLuint texture){ nullptr };
    GLuint(APIENTRY * glCreateShader)(GLenum type){ nullptr };
    void(APIENTRY * glClearDepthf)(GLclampf depth){ nullptr };
    RESOLVE(glBindTexture);
    RESOLVE(glCreateShader);
    RESOLVE(glClearDepthf);
    RESOLVE(glGetString);

    return glBindTexture && glCreateShader && glClearDepthf;
}

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
    if (libEGL.eglGetPlatformDisplayEXT
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
            *display = libEGL.eglGetPlatformDisplayEXT(EGL_PLATFORM_ANGLE_ANGLE, dc, attributes);
            if (!libEGL.eglInitialize(*display, major, minor)) {
                qWarning("%s: Unable to initialize ANGLE: error 0x%x", __FUNCTION__,
                         libEGL.eglGetError());
                libEGL.eglTerminate(*display);
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

    if (!libEGL.init()) {
        qWarning("%s: Failed to load and resolve libEGL functions", __FUNCTION__);
        return nullptr;
    }
    if (!libGLESv2.init()) {
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
        display = libEGL.eglGetDisplay(dc);
    if (!display) {
        qWarning("%s: Could not obtain EGL display", __FUNCTION__);
        return nullptr;
    }

    if (!major && !libEGL.eglInitialize(display, &major, &minor)) {
        const auto err{ libEGL.eglGetError() };
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
    libEGL.eglTerminate(m_display);
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

    EGLSurface surface{ libEGL.eglCreateWindowSurface(
            m_display, nativeConfig, static_cast<EGLNativeWindowType>(nativeWindow),
            attributes.data()) };
    if (surface == EGL_NO_SURFACE) {
        *err = libEGL.eglGetError();
        qWarning("%s: Could not create the EGL window surface: 0x%x", __FUNCTION__, *err);
    }

    return surface;
}

void QWindowsEGLStaticContext::destroyWindowSurface(void *nativeSurface)
{
    libEGL.eglDestroySurface(m_display, nativeSurface);
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

    QWindowsEGLStaticContext::libEGL.eglBindAPI(m_api);
    m_eglContext = QWindowsEGLStaticContext::libEGL.eglCreateContext(
            m_eglDisplay, m_eglConfig, m_shareContext, contextAttrs.data());
    if (m_eglContext == EGL_NO_CONTEXT && m_shareContext != EGL_NO_CONTEXT) {
        m_shareContext = nullptr;
        m_eglContext = QWindowsEGLStaticContext::libEGL.eglCreateContext(
                m_eglDisplay, m_eglConfig, nullptr, contextAttrs.data());
    }

    if (m_eglContext == EGL_NO_CONTEXT) {
        const auto err{ QWindowsEGLStaticContext::libEGL.eglGetError() };
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
    EGLSurface pbuffer{ QWindowsEGLStaticContext::libEGL.eglCreatePbufferSurface(
            m_eglDisplay, m_eglConfig, pbufferAttributes.data()) };
    if (pbuffer == EGL_NO_SURFACE)
        return;

    EGLDisplay prevDisplay{ QWindowsEGLStaticContext::libEGL.eglGetCurrentDisplay() };
    if (prevDisplay == EGL_NO_DISPLAY) // when no context is current
        prevDisplay = m_eglDisplay;
    EGLContext prevContext{ QWindowsEGLStaticContext::libEGL.eglGetCurrentContext() };
    EGLSurface prevSurfaceDraw{ QWindowsEGLStaticContext::libEGL.eglGetCurrentSurface(EGL_DRAW) };
    EGLSurface prevSurfaceRead{ QWindowsEGLStaticContext::libEGL.eglGetCurrentSurface(EGL_READ) };

    if (QWindowsEGLStaticContext::libEGL.eglMakeCurrent(m_eglDisplay, pbuffer, pbuffer,
                                                        m_eglContext)) {
        const GLubyte *s{ QWindowsEGLStaticContext::libGLESv2.glGetString(GL_VERSION) };
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
        QWindowsEGLStaticContext::libEGL.eglMakeCurrent(prevDisplay, prevSurfaceDraw,
                                                        prevSurfaceRead, prevContext);
    }
    QWindowsEGLStaticContext::libEGL.eglDestroySurface(m_eglDisplay, pbuffer);
}

QWindowsEGLContext::~QWindowsEGLContext()
{
    if (m_eglContext != EGL_NO_CONTEXT) {
        QWindowsEGLStaticContext::libEGL.eglDestroyContext(m_eglDisplay, m_eglContext);
        m_eglContext = EGL_NO_CONTEXT;
    }
}

bool QWindowsEGLContext::makeCurrent(QPlatformSurface *surface)
{
    Q_ASSERT(surface->surface()->supportsOpenGL());

    QWindowsEGLStaticContext::libEGL.eglBindAPI(m_api);

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
        QWindowsEGLStaticContext::libEGL.eglDestroyContext(m_eglDisplay, m_eglContext);
        m_eglContext = EGL_NO_CONTEXT;
        return false;
    }

    // shortcut: on some GPUs, eglMakeCurrent is not a cheap operation
    if (QWindowsEGLStaticContext::libEGL.eglGetCurrentContext() == m_eglContext
        && QWindowsEGLStaticContext::libEGL.eglGetCurrentDisplay() == m_eglDisplay
        && QWindowsEGLStaticContext::libEGL.eglGetCurrentSurface(EGL_READ) == eglSurface
        && QWindowsEGLStaticContext::libEGL.eglGetCurrentSurface(EGL_DRAW) == eglSurface) {
        return true;
    }

    const auto ok{ QWindowsEGLStaticContext::libEGL.eglMakeCurrent(m_eglDisplay, eglSurface,
                                                                   eglSurface, m_eglContext) };
    if (ok) {
        const auto requestedSwapInterval{ surface->format().swapInterval() };
        if (requestedSwapInterval >= 0 && m_swapInterval != requestedSwapInterval) {
            m_swapInterval = requestedSwapInterval;
            QWindowsEGLStaticContext::libEGL.eglSwapInterval(m_staticContext->display(),
                                                             m_swapInterval);
        }
    } else {
        err = QWindowsEGLStaticContext::libEGL.eglGetError();
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
    QWindowsEGLStaticContext::libEGL.eglBindAPI(m_api);
    const auto ok{ QWindowsEGLStaticContext::libEGL.eglMakeCurrent(
            m_eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT) };
    if (!ok)
        qWarning("%s: Failed to make no context/surface current. eglError: %d, this: %p",
                 __FUNCTION__, QWindowsEGLStaticContext::libEGL.eglGetError(), this);
}

void QWindowsEGLContext::swapBuffers(QPlatformSurface *surface)
{
    QWindowsEGLStaticContext::libEGL.eglBindAPI(m_api);
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

    const auto ok{ QWindowsEGLStaticContext::libEGL.eglSwapBuffers(m_eglDisplay, eglSurface) };
    if (!ok) {
        err = QWindowsEGLStaticContext::libEGL.eglGetError();
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
    QWindowsEGLStaticContext::libEGL.eglBindAPI(m_api);

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
                    QWindowsEGLStaticContext::libEGL.eglGetProcAddress(procNameView.data()));
        }
    }

    if (!procAddress)
        procAddress = reinterpret_cast<QFunctionPointer>(
                QWindowsEGLStaticContext::libEGL.eglGetProcAddress(procName));

    // We support AllGLFunctionsQueryable, which means this function must be able to
    // return a function pointer for standard GLES2 functions too. These are not
    // guaranteed to be queryable via eglGetProcAddress().
    if (!procAddress)
        procAddress = reinterpret_cast<QFunctionPointer>(
                QWindowsEGLStaticContext::libGLESv2.resolve(procName));

    if (QWindowsContext::verbose > 1)
        qCDebug(lcQpaGl) << __FUNCTION__ << procName
                         << QWindowsEGLStaticContext::libEGL.eglGetCurrentContext() << "returns"
                         << reinterpret_cast<void *>(procAddress);

    return procAddress;
}

std::vector<EGLint> q_createConfigAttributesFromFormat(const QSurfaceFormat &format) noexcept
{
    // clang-format off
    int redSize    { format.redBufferSize() };
    int greenSize  { format.greenBufferSize() };
    int blueSize   { format.blueBufferSize() };
    int alphaSize  { format.alphaBufferSize() };
    int depthSize  { format.depthBufferSize() };
    int stencilSize{ format.stencilBufferSize() };
    int sampleCount{ format.samples() };
    // clang-format on

    std::vector<EGLint> configAttributes;

    // Map default, unspecified values (-1) to 0. This is important due to sorting rule #3
    // in section 3.4.1 of the spec and allows picking a potentially faster 16-bit config
    // over 32-bit ones when there is no explicit request for the color channel sizes:
    //
    // The red/green/blue sizes have a sort priority of 3, so they are sorted by
    // first. (unless a caveat like SLOW or NON_CONFORMANT is present) The sort order is
    // Special and described as "by larger _total_ number of color bits.". So EGL will put
    // 32-bit configs in the list before the 16-bit configs. However, the spec also goes
    // on to say "If the requested number of bits in attrib_list for a particular
    // component is 0, then the number of bits for that component is not considered". This
    // part of the spec also seems to imply that setting the red/green/blue bits to zero
    // means none of the components are considered and EGL disregards the entire sorting
    // rule. It then looks to the next highest priority rule, which is
    // EGL_BUFFER_SIZE. Despite the selection criteria being "AtLeast" for
    // EGL_BUFFER_SIZE, it's sort order is "smaller" meaning 16-bit configs are put in the
    // list before 32-bit configs.
    //
    // This also means that explicitly specifying a size like 565 will still result in
    // having larger (888) configs first in the returned list. We need to handle this
    // ourselves later by manually filtering the list, instead of just blindly taking the
    // first config from it.

    configAttributes.emplace_back(EGL_RED_SIZE);
    configAttributes.emplace_back(redSize > 0 ? redSize : 0);

    configAttributes.emplace_back(EGL_GREEN_SIZE);
    configAttributes.emplace_back(greenSize > 0 ? greenSize : 0);

    configAttributes.emplace_back(EGL_BLUE_SIZE);
    configAttributes.emplace_back(blueSize > 0 ? blueSize : 0);

    configAttributes.emplace_back(EGL_ALPHA_SIZE);
    configAttributes.emplace_back(alphaSize > 0 ? alphaSize : 0);

    configAttributes.emplace_back(EGL_SAMPLES);
    configAttributes.emplace_back(sampleCount > 0 ? sampleCount : 0);

    configAttributes.emplace_back(EGL_SAMPLE_BUFFERS);
    configAttributes.emplace_back(sampleCount > 0);

    if (format.renderableType() != QSurfaceFormat::OpenVG) {
        configAttributes.emplace_back(EGL_DEPTH_SIZE);
        configAttributes.emplace_back(depthSize > 0 ? depthSize : 0);

        configAttributes.emplace_back(EGL_STENCIL_SIZE);
        configAttributes.emplace_back(stencilSize > 0 ? stencilSize : 0);
    } else {
        // OpenVG needs alpha mask for clipping
        configAttributes.emplace_back(EGL_ALPHA_MASK_SIZE);
        configAttributes.emplace_back(8);
    }

    return configAttributes;
}

bool q_reduceConfigAttributes(std::vector<EGLint> &configAttributes) noexcept
{
    // Reduce the complexity of a configuration request to ask for less
    // because the previous request did not result in success.  Returns
    // true if the complexity was reduced, or false if no further
    // reductions in complexity are possible.

    auto i{ std::find(configAttributes.begin(), configAttributes.end(), EGL_SWAP_BEHAVIOR) };
    if (i != configAttributes.end())
        configAttributes.erase(i, i + 2);

#ifdef EGL_VG_ALPHA_FORMAT_PRE_BIT
    // For OpenVG, we sometimes try to create a surface using a pre-multiplied format. If we can't
    // find a config which supports pre-multiplied formats, remove the flag on the surface type:

    i = std::find(configAttributes.begin(), configAttributes.end(), EGL_SURFACE_TYPE);
    if (i != configAttributes.end()) {
        const EGLint surfaceType = *(i + 1);
        if (surfaceType & EGL_VG_ALPHA_FORMAT_PRE_BIT) {
            *(i + 1) = surfaceType ^ EGL_VG_ALPHA_FORMAT_PRE_BIT;
            return true;
        }
    }
#endif

    // EGL chooses configs with the highest color depth over
    // those with smaller (but faster) lower color depths. One
    // way around this is to set EGL_BUFFER_SIZE to 16, which
    // trumps the others. Of course, there may not be a 16-bit
    // config available, so it's the first restraint we remove.
    i = std::find(configAttributes.begin(), configAttributes.end(), EGL_BUFFER_SIZE);
    if (i != configAttributes.end()) {
        if (*(i + 1) == 16) {
            configAttributes.erase(i, i + 2);
            return true;
        }
    }

    i = std::find(configAttributes.begin(), configAttributes.end(), EGL_SAMPLES);
    if (i != configAttributes.end()) {
        EGLint value = *(i + 1);
        if (value > 1)
            *(i + 1) = qMin(EGLint(16), value / 2);
        else
            configAttributes.erase(i, i + 2);
        return true;
    }

    i = std::find(configAttributes.begin(), configAttributes.end(), EGL_SAMPLE_BUFFERS);
    if (i != configAttributes.end()) {
        configAttributes.erase(i, i + 2);
        return true;
    }

    i = std::find(configAttributes.begin(), configAttributes.end(), EGL_DEPTH_SIZE);
    if (i != configAttributes.end()) {
        if (*(i + 1) >= 32)
            *(i + 1) = 24;
        else if (*(i + 1) > 1)
            *(i + 1) = 1;
        else
            configAttributes.erase(i, i + 2);
        return true;
    }

    i = std::find(configAttributes.begin(), configAttributes.end(), EGL_ALPHA_SIZE);
    if (i != configAttributes.end()) {
        configAttributes.erase(i, i + 2);
#if defined(EGL_BIND_TO_TEXTURE_RGBA) && defined(EGL_BIND_TO_TEXTURE_RGB)
        i = std::find(configAttributes.begin(), configAttributes.end(), EGL_BIND_TO_TEXTURE_RGBA);
        if (i != configAttributes.end()) {
            *(i) = EGL_BIND_TO_TEXTURE_RGB;
            *(i + 1) = true;
        }
#endif
        return true;
    }

    i = std::find(configAttributes.begin(), configAttributes.end(), EGL_STENCIL_SIZE);
    if (i != configAttributes.end()) {
        if (*(i + 1) > 1)
            *(i + 1) = 1;
        else
            configAttributes.erase(i, i + 2);
        return true;
    }

#ifdef EGL_BIND_TO_TEXTURE_RGB
    i = std::find(configAttributes.begin(), configAttributes.end(), EGL_BIND_TO_TEXTURE_RGB);
    if (i != configAttributes.end()) {
        configAttributes.erase(i, i + 2);
        return true;
    }
#endif

    return false;
}

QWindowsEglConfigChooser::QWindowsEglConfigChooser(EGLDisplay display)
    : m_display(display),
      m_surfaceType(EGL_WINDOW_BIT),
      m_ignore(false),
      m_confAttrRed(0),
      m_confAttrGreen(0),
      m_confAttrBlue(0),
      m_confAttrAlpha(0)
{
}

QWindowsEglConfigChooser::~QWindowsEglConfigChooser() = default;

EGLConfig QWindowsEglConfigChooser::chooseConfig()
{
    std::vector<EGLint> configureAttributes{ q_createConfigAttributesFromFormat(m_format) };
    configureAttributes.emplace_back(EGL_SURFACE_TYPE);
    configureAttributes.emplace_back(surfaceType());

    configureAttributes.emplace_back(EGL_RENDERABLE_TYPE);
    bool needsES2Plus = false;
    switch (m_format.renderableType()) {
    case QSurfaceFormat::OpenVG:
        configureAttributes.emplace_back(EGL_OPENVG_BIT);
        break;
#ifdef EGL_VERSION_1_4
    case QSurfaceFormat::DefaultRenderableType: {
#  ifndef QT_NO_OPENGL
        // NVIDIA EGL only provides desktop GL for development purposes, and recommends against
        // using it.
        const QLatin1StringView vendor{ QWindowsEGLStaticContext::libEGL.eglQueryString(
                display(), EGL_VENDOR) };
        if (QOpenGLContext::openGLModuleType() == QOpenGLContext::LibGL
            && (vendor.empty() || !vendor.contains("NVIDIA"_L1)))
            configureAttributes.emplace_back(EGL_OPENGL_BIT);
        else
#  endif // QT_NO_OPENGL
            needsES2Plus = true;
        break;
    }
    case QSurfaceFormat::OpenGL:
        configureAttributes.emplace_back(EGL_OPENGL_BIT);
        break;
#endif
    case QSurfaceFormat::OpenGLES:
        if (m_format.majorVersion() == 1) {
            configureAttributes.emplace_back(EGL_OPENGL_ES_BIT);
            break;
        }
        Q_FALLTHROUGH();
    default:
        needsES2Plus = true;
        break;
    }
    if (needsES2Plus) {
        if (m_format.majorVersion() >= 3 && q_hasEglExtension(display(), "EGL_KHR_create_context"))
            configureAttributes.emplace_back(EGL_OPENGL_ES3_BIT_KHR);
        else
            configureAttributes.emplace_back(EGL_OPENGL_ES2_BIT);
    }
    if (m_format.colorSpace().primaries() == QColorSpace::Primaries::ScRGB) {
        configureAttributes.emplace_back(EGL_COLOR_COMPONENT_TYPE_EXT);
        configureAttributes.emplace_back(EGL_COLOR_COMPONENT_TYPE_FLOAT_EXT);
    }
    configureAttributes.emplace_back(EGL_NONE);

    EGLConfig cfg{ nullptr };
    do {
        // Get the number of matching configurations for this set of properties.
        EGLint matching{};
        if (!QWindowsEGLStaticContext::libEGL.eglChooseConfig(display(), configureAttributes.data(),
                                                              nullptr, 0, &matching)
            || !matching)
            continue;

        // Fetch all of the matching configurations and find the
        // first that matches the pixel format we wanted.
        auto i{ std::find(configureAttributes.begin(), configureAttributes.end(), EGL_RED_SIZE) };
        m_confAttrRed = *(i + 1);
        i = std::find(configureAttributes.begin(), configureAttributes.end(), EGL_GREEN_SIZE);
        m_confAttrGreen = *(i + 1);
        i = std::find(configureAttributes.begin(), configureAttributes.end(), EGL_BLUE_SIZE);
        m_confAttrBlue = *(i + 1);
        i = std::find(configureAttributes.begin(), configureAttributes.end(), EGL_ALPHA_SIZE);
        m_confAttrAlpha = i == configureAttributes.end() ? 0 : *(i + 1);

        std::vector<EGLConfig> configs(matching);
        QWindowsEGLStaticContext::libEGL.eglChooseConfig(display(), configureAttributes.data(),
                                                         configs.data(), EGLint(configs.size()),
                                                         &matching);
        if (!cfg && matching > 0)
            cfg = configs.front();

        // Filter the list. Due to the EGL sorting rules configs with higher depth are
        // placed first when the minimum color channel sizes have been specified (i.e. the
        // QSurfaceFormat contains color sizes > 0). To prevent returning a 888 config
        // when the QSurfaceFormat explicitly asked for 565, go through the returned
        // configs and look for one that exactly matches the requested sizes. When no
        // sizes have been given, take the first, which will be a config with the smaller
        // (e.g. 16-bit) depth.
        for (const auto &config : configs) {
            if (filterConfig(config))
                return config;
        }
    } while (q_reduceConfigAttributes(configureAttributes));

    if (!cfg)
        qWarning("Cannot find EGLConfig, returning null config");
    return cfg;
}

bool QWindowsEglConfigChooser::filterConfig(EGLConfig config) const
{
    // If we are fine with the highest depth (e.g. RGB888 configs) even when something
    // smaller (565) was explicitly requested, do nothing.
    if (m_ignore)
        return true;

    EGLint red{};
    EGLint green{};
    EGLint blue{};
    EGLint alpha{};

    // Compare only if a size was given. Otherwise just accept.
    if (m_confAttrRed)
        QWindowsEGLStaticContext::libEGL.eglGetConfigAttrib(display(), config, EGL_RED_SIZE, &red);
    if (m_confAttrGreen)
        QWindowsEGLStaticContext::libEGL.eglGetConfigAttrib(display(), config, EGL_GREEN_SIZE,
                                                            &green);
    if (m_confAttrBlue)
        QWindowsEGLStaticContext::libEGL.eglGetConfigAttrib(display(), config, EGL_BLUE_SIZE,
                                                            &blue);
    if (m_confAttrAlpha)
        QWindowsEGLStaticContext::libEGL.eglGetConfigAttrib(display(), config, EGL_ALPHA_SIZE,
                                                            &alpha);

    return red == m_confAttrRed && green == m_confAttrGreen && blue == m_confAttrBlue
            && alpha == m_confAttrAlpha;
}

EGLConfig q_configFromGLFormat(EGLDisplay display, const QSurfaceFormat &format,
                               bool highestPixelFormat, int surfaceType) noexcept
{
    QWindowsEglConfigChooser chooser(display);
    chooser.setSurfaceFormat(format);
    chooser.setSurfaceType(surfaceType);
    chooser.setIgnoreColorChannels(highestPixelFormat);

    return chooser.chooseConfig();
}

QSurfaceFormat q_glFormatFromConfig(EGLDisplay display, const EGLConfig config,
                                    const QSurfaceFormat &referenceFormat) noexcept
{
    QSurfaceFormat format;
    EGLint redSize{};
    EGLint greenSize{};
    EGLint blueSize{};
    EGLint alphaSize{};
    EGLint depthSize{};
    EGLint stencilSize{};
    EGLint sampleCount{};
    EGLint renderableType{};

    QWindowsEGLStaticContext::libEGL.eglGetConfigAttrib(display, config, EGL_RED_SIZE, &redSize);
    QWindowsEGLStaticContext::libEGL.eglGetConfigAttrib(display, config, EGL_GREEN_SIZE,
                                                        &greenSize);
    QWindowsEGLStaticContext::libEGL.eglGetConfigAttrib(display, config, EGL_BLUE_SIZE, &blueSize);
    QWindowsEGLStaticContext::libEGL.eglGetConfigAttrib(display, config, EGL_ALPHA_SIZE,
                                                        &alphaSize);
    QWindowsEGLStaticContext::libEGL.eglGetConfigAttrib(display, config, EGL_DEPTH_SIZE,
                                                        &depthSize);
    QWindowsEGLStaticContext::libEGL.eglGetConfigAttrib(display, config, EGL_STENCIL_SIZE,
                                                        &stencilSize);
    QWindowsEGLStaticContext::libEGL.eglGetConfigAttrib(display, config, EGL_SAMPLES, &sampleCount);
    QWindowsEGLStaticContext::libEGL.eglGetConfigAttrib(display, config, EGL_RENDERABLE_TYPE,
                                                        &renderableType);

    if (referenceFormat.renderableType() == QSurfaceFormat::OpenVG
        && (renderableType & EGL_OPENVG_BIT))
        format.setRenderableType(QSurfaceFormat::OpenVG);
#ifdef EGL_VERSION_1_4
    else if (referenceFormat.renderableType() == QSurfaceFormat::OpenGL
             && (renderableType & EGL_OPENGL_BIT))
        format.setRenderableType(QSurfaceFormat::OpenGL);
    else if (referenceFormat.renderableType() == QSurfaceFormat::DefaultRenderableType
#  ifndef QT_NO_OPENGL
             && QOpenGLContext::openGLModuleType() == QOpenGLContext::LibGL
             && !QLatin1StringView(
                         QWindowsEGLStaticContext::libEGL.eglQueryString(display, EGL_VENDOR))
                         .contains("NVIDIA"_L1)
#  endif
             && (renderableType & EGL_OPENGL_BIT))
        format.setRenderableType(QSurfaceFormat::OpenGL);
#endif
    else
        format.setRenderableType(QSurfaceFormat::OpenGLES);

    format.setRedBufferSize(redSize);
    format.setGreenBufferSize(greenSize);
    format.setBlueBufferSize(blueSize);
    format.setAlphaBufferSize(alphaSize);
    format.setDepthBufferSize(depthSize);
    format.setStencilBufferSize(stencilSize);
    format.setSamples(sampleCount);
    format.setStereo(false); // EGL doesn't support stereo buffers
    format.setColorSpace(referenceFormat.colorSpace());
    format.setSwapInterval(referenceFormat.swapInterval());

    // Clear the EGL error state because some of the above may
    // have errored out because the attribute is not applicable
    // to the surface type.  Such errors don't matter.
    QWindowsEGLStaticContext::libEGL.eglGetError();

    return format;
}

bool q_hasEglExtension(EGLDisplay display, const char *extensionName) noexcept
{
    const QLatin1StringView extensions{ QWindowsEGLStaticContext::libEGL.eglQueryString(
            display, EGL_EXTENSIONS) };
    const QLatin1StringView extension(extensionName);
    for (const auto &i : QStringTokenizer{ extensions, " "_L1 })
        if (i == extension)
            return true;
    return false;
}

struct AttrInfo
{
    EGLint attr;
    const char *name;
};
static constexpr std::array<AttrInfo, 27> attrs{ {
        { EGL_BUFFER_SIZE, "EGL_BUFFER_SIZE" },
        { EGL_ALPHA_SIZE, "EGL_ALPHA_SIZE" },
        { EGL_BLUE_SIZE, "EGL_BLUE_SIZE" },
        { EGL_GREEN_SIZE, "EGL_GREEN_SIZE" },
        { EGL_RED_SIZE, "EGL_RED_SIZE" },
        { EGL_DEPTH_SIZE, "EGL_DEPTH_SIZE" },
        { EGL_STENCIL_SIZE, "EGL_STENCIL_SIZE" },
        { EGL_CONFIG_CAVEAT, "EGL_CONFIG_CAVEAT" },
        { EGL_CONFIG_ID, "EGL_CONFIG_ID" },
        { EGL_LEVEL, "EGL_LEVEL" },
        { EGL_MAX_PBUFFER_HEIGHT, "EGL_MAX_PBUFFER_HEIGHT" },
        { EGL_MAX_PBUFFER_PIXELS, "EGL_MAX_PBUFFER_PIXELS" },
        { EGL_MAX_PBUFFER_WIDTH, "EGL_MAX_PBUFFER_WIDTH" },
        { EGL_NATIVE_RENDERABLE, "EGL_NATIVE_RENDERABLE" },
        { EGL_NATIVE_VISUAL_ID, "EGL_NATIVE_VISUAL_ID" },
        { EGL_NATIVE_VISUAL_TYPE, "EGL_NATIVE_VISUAL_TYPE" },
        { EGL_SAMPLES, "EGL_SAMPLES" },
        { EGL_SAMPLE_BUFFERS, "EGL_SAMPLE_BUFFERS" },
        { EGL_SURFACE_TYPE, "EGL_SURFACE_TYPE" },
        { EGL_TRANSPARENT_TYPE, "EGL_TRANSPARENT_TYPE" },
        { EGL_TRANSPARENT_BLUE_VALUE, "EGL_TRANSPARENT_BLUE_VALUE" },
        { EGL_TRANSPARENT_GREEN_VALUE, "EGL_TRANSPARENT_GREEN_VALUE" },
        { EGL_TRANSPARENT_RED_VALUE, "EGL_TRANSPARENT_RED_VALUE" },
        { EGL_BIND_TO_TEXTURE_RGB, "EGL_BIND_TO_TEXTURE_RGB" },
        { EGL_BIND_TO_TEXTURE_RGBA, "EGL_BIND_TO_TEXTURE_RGBA" },
        { EGL_MIN_SWAP_INTERVAL, "EGL_MIN_SWAP_INTERVAL" },
        { EGL_MAX_SWAP_INTERVAL, "EGL_MAX_SWAP_INTERVAL" },
} };

void q_printEglConfig(EGLDisplay display, EGLConfig config) noexcept
{
    for (const auto &attr : attrs) {
        EGLint value;
        if (QWindowsEGLStaticContext::libEGL.eglGetConfigAttrib(display, config, attr.attr,
                                                                &value)) {
            qDebug(lcQpaGl, "\t%s: %d", attr.name, (int)value);
        }
    }
}

QT_END_NAMESPACE
