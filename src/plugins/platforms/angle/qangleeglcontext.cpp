// Copyright (C) 2016 The Qt Company Ltd.
// Copyright (C) 2023 L. E. Segovia <amy@amyspark.me>
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#include "qangleeglcontext.h"
#include "qangleeglconvenience_p.h"
#include "../windows/qwindowscontext.h"

#include <string_view>

using namespace std::string_view_literals;

QT_BEGIN_NAMESPACE

#ifdef Q_CC_MINGW
static inline void *resolveFunc(HMODULE lib, const char *name)
{
    const auto baseNameStr{ QString::fromLatin1(name) };
    QString nameStr;
    void *proc = nullptr;

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

void *QLibEGL::resolve(const char *name)
{
    return m_lib ? resolveFunc(m_lib, name) : nullptr;
}

#define GETPROC(name) reinterpret_cast<decltype(name)>(resolve(#name))

#define RESOLVE(name) name = GETPROC(name)

bool QLibEGL::init()
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

void *QLibGLESv2::resolve(const char *name)
{
    return m_lib ? resolveFunc(m_lib, name) : nullptr;
}

bool QLibGLESv2::init()
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

QLibEGL &QLibEGL::instance()
{
    static QLibEGL instance;
    return instance;
}

QLibGLESv2 &QLibGLESv2::instance()
{
    static QLibGLESv2 instance;
    return instance;
}

QANGLEContext::QANGLEContext(EGLDisplay display,
                                       const QSurfaceFormat &format, QPlatformOpenGLContext *share)
    : m_eglDisplay(display)
{
    m_eglConfig = q_configFromGLFormat(m_eglDisplay, format);
    m_format = q_glFormatFromConfig(m_eglDisplay, m_eglConfig, format);
    m_shareContext = [&]() -> EGLContext {
        if (!share)
            return nullptr;
        if (const auto realShare = dynamic_cast<QANGLEContext *>(share))
            return realShare->m_eglContext;
        return nullptr;
    }();

    const EGLint major{ m_format.majorVersion() };
    const EGLint minor{ m_format.minorVersion() };
    if (major > 3 || (major == 3 && minor > 0))
        qWarning("QANGLEContext: ANGLE only partially supports OpenGL ES > 3.0");
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
        qWarning("QANGLEContext: Failed to create context, eglError: %x, this: %p", err, this);
        // ANGLE gives bad alloc when it fails to reset a previously lost D3D device.
        // A common cause for this is disabling the graphics adapter used by the app.
        if (err == EGL_BAD_ALLOC)
            qWarning("QANGLEContext: Graphics device lost. (Did the adapter get disabled?)");
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

QANGLEContext::~QANGLEContext()
{
    if (m_eglContext != EGL_NO_CONTEXT) {
        QLibEGL::instance().eglDestroyContext(m_eglDisplay, m_eglContext);
        m_eglContext = EGL_NO_CONTEXT;
    }
}

void QANGLEContext::doneCurrent()
{
    QLibEGL::instance().eglBindAPI(m_api);
    const auto ok{ QLibEGL::instance().eglMakeCurrent(
            m_eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT) };
    if (!ok)
        qWarning("%s: Failed to make no context/surface current. eglError: %d, this: %p",
                 __FUNCTION__, QLibEGL::instance().eglGetError(), this);
}

QFunctionPointer QANGLEContext::getProcAddress(const char *procName)
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
