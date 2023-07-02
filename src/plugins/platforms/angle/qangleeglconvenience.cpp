// Copyright (C) 2016 The Qt Company Ltd.
// Copyright (C) 2023 L. E. Segovia <amy@amyspark.me>
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#include "qangleeglconvenience_p.h"

#include "qangleeglcontext.h"

#include <QtCore/qloggingcategory.h>
#include <QtGui/qcolorspace.h>
#include <QtGui/qopenglcontext.h>

#include <EGL/eglext.h>
#include <VersionHelpers.h>

#include <string_view>

using namespace Qt::Literals::StringLiterals;
using namespace std::string_view_literals;

#ifndef EGL_OPENGL_ES3_BIT_KHR
#  define EGL_OPENGL_ES3_BIT_KHR 0x0040
#endif

QT_BEGIN_NAMESPACE

Q_DECLARE_LOGGING_CATEGORY(lcQpaGl); // in qwindowscontext.cpp -- todo: add equivalent for macOS

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
        const QLatin1StringView vendor{ QLibEGL::instance().eglQueryString(
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
        if (!QLibEGL::instance().eglChooseConfig(display(), configureAttributes.data(),
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
        QLibEGL::instance().eglChooseConfig(display(), configureAttributes.data(),
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
        QLibEGL::instance().eglGetConfigAttrib(display(), config, EGL_RED_SIZE, &red);
    if (m_confAttrGreen)
        QLibEGL::instance().eglGetConfigAttrib(display(), config, EGL_GREEN_SIZE,
                                                            &green);
    if (m_confAttrBlue)
        QLibEGL::instance().eglGetConfigAttrib(display(), config, EGL_BLUE_SIZE,
                                                            &blue);
    if (m_confAttrAlpha)
        QLibEGL::instance().eglGetConfigAttrib(display(), config, EGL_ALPHA_SIZE,
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

    QLibEGL::instance().eglGetConfigAttrib(display, config, EGL_RED_SIZE, &redSize);
    QLibEGL::instance().eglGetConfigAttrib(display, config, EGL_GREEN_SIZE,
                                                        &greenSize);
    QLibEGL::instance().eglGetConfigAttrib(display, config, EGL_BLUE_SIZE, &blueSize);
    QLibEGL::instance().eglGetConfigAttrib(display, config, EGL_ALPHA_SIZE,
                                                        &alphaSize);
    QLibEGL::instance().eglGetConfigAttrib(display, config, EGL_DEPTH_SIZE,
                                                        &depthSize);
    QLibEGL::instance().eglGetConfigAttrib(display, config, EGL_STENCIL_SIZE,
                                                        &stencilSize);
    QLibEGL::instance().eglGetConfigAttrib(display, config, EGL_SAMPLES, &sampleCount);
    QLibEGL::instance().eglGetConfigAttrib(display, config, EGL_RENDERABLE_TYPE,
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
                         QLibEGL::instance().eglQueryString(display, EGL_VENDOR))
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
    QLibEGL::instance().eglGetError();

    return format;
}

bool q_hasEglExtension(EGLDisplay display, const char *extensionName) noexcept
{
    const QLatin1StringView extensions{ QLibEGL::instance().eglQueryString(
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
        if (QLibEGL::instance().eglGetConfigAttrib(display, config, attr.attr,
                                                                &value)) {
            qDebug(lcQpaGl, "\t%s: %d", attr.name, (int)value);
        }
    }
}

QT_END_NAMESPACE
