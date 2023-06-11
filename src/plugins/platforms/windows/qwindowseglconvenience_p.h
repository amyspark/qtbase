// Copyright (C) 2020 The Qt Company Ltd.
// Copyright (C) 2023 L. E. Segovia <amy@amyspark.me>
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#ifndef QWINDOWSEGLCONVENIENCE_H
#define QWINDOWSEGLCONVENIENCE_H

//
//  W A R N I N G
//  -------------
//
// This file is not part of the Qt API. It exists purely as an
// implementation detail. This header file may change from version to
// version without notice, or even be removed.
//
// We mean it.
//

#include <vector>
#include <QtGui/qsurfaceformat.h>
#include <QtCore/qsize.h>
#include <EGL/egl.h>

QT_BEGIN_NAMESPACE

std::vector<EGLint> q_createConfigAttributesFromFormat(const QSurfaceFormat &format) noexcept;

bool q_reduceConfigAttributes(std::vector<EGLint> &configAttributes) noexcept;

EGLConfig q_configFromGLFormat(EGLDisplay display, const QSurfaceFormat &format,
                               bool highestPixelFormat = false,
                               int surfaceType = EGL_WINDOW_BIT) noexcept;

QSurfaceFormat q_glFormatFromConfig(EGLDisplay display, const EGLConfig config,
                                    const QSurfaceFormat &referenceFormat = {}) noexcept;

bool q_hasEglExtension(EGLDisplay display, const char *extensionName) noexcept;

void q_printEglConfig(EGLDisplay display, EGLConfig config) noexcept;

class QWindowsEglConfigChooser
{
public:
    QWindowsEglConfigChooser(EGLDisplay display);
    virtual ~QWindowsEglConfigChooser();

    EGLDisplay display() const { return m_display; }

    void setSurfaceType(EGLint surfaceType) { m_surfaceType = surfaceType; }
    EGLint surfaceType() const { return m_surfaceType; }

    void setSurfaceFormat(const QSurfaceFormat &format) { m_format = format; }
    QSurfaceFormat surfaceFormat() const { return m_format; }

    void setIgnoreColorChannels(bool ignore) { m_ignore = ignore; }
    bool ignoreColorChannels() const { return m_ignore; }

    EGLConfig chooseConfig();

protected:
    virtual bool filterConfig(EGLConfig config) const;

    QSurfaceFormat m_format;
    EGLDisplay m_display;
    EGLint m_surfaceType;
    bool m_ignore;

    int m_confAttrRed;
    int m_confAttrGreen;
    int m_confAttrBlue;
    int m_confAttrAlpha;
};

QT_END_NAMESPACE

#endif // QWINDOWSEGLCONVENIENCE_H
