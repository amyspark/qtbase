// Copyright (C) 2016 The Qt Company Ltd.
// Copyright (C) 2023 L. E. Segovia <amy@amyspark.me>
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#include "qangleeglcontext.h"
#include "../windows/qwindowscontext.h"

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
