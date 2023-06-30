// Copyright (C) 2018 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#ifndef QCOLORTRANSFERFUNCTION_P_H
#define QCOLORTRANSFERFUNCTION_P_H

//
//  W A R N I N G
//  -------------
//
// This file is not part of the Qt API.  It exists purely as an
// implementation detail.  This header file may change from version to
// version without notice, or even be removed.
//
// We mean it.
//

#include <QtGui/private/qtguiglobal_p.h>

#include <cmath>

QT_BEGIN_NAMESPACE

namespace {
enum class Hints : quint32 { Calculated = 1, IsGamma = 2, IsLinear = 4, IsSRgb = 8 };

inline bool paramCompare(float p1, float p2)
{
    // Much fuzzier than fuzzy compare.
    // It tries match parameters that has been passed through a 8.8
    // fixed point form.
    return (qAbs(p1 - p2) <= (1.0f / 512.0f));
}
} // namespace

struct Q_GUI_EXPORT QColorTransferFunctionPrivate
{
    virtual float apply(float x) const = 0;
    virtual bool matches(const QColorTransferFunctionPrivate &rhs) const = 0;
    virtual void updateHints(quint32 &m_flags) const = 0;
    virtual std::unique_ptr<QColorTransferFunctionPrivate> inverted() const = 0;
};

// Defines a ICC parametric curve type 4
class Q_GUI_EXPORT QICCColorTransferFunction : public QColorTransferFunctionPrivate
{
public:
    QICCColorTransferFunction() noexcept
        : QColorTransferFunctionPrivate(),
          m_a(1.0f),
          m_b(0.0f),
          m_c(1.0f),
          m_d(0.0f),
          m_e(0.0f),
          m_f(0.0f),
          m_g(1.0f)
    {
    }
    QICCColorTransferFunction(float a, float b, float c, float d, float e, float f,
                              float g) noexcept
        : QColorTransferFunctionPrivate(), m_a(a), m_b(b), m_c(c), m_d(d), m_e(e), m_f(f), m_g(g)
    {
    }

    bool matches(const QColorTransferFunctionPrivate &rhs) const override
    {
        if (const QICCColorTransferFunction *o =
                    dynamic_cast<const QICCColorTransferFunction *>(&rhs)) {
            return paramCompare(m_a, o->m_a) && paramCompare(m_b, o->m_b)
                    && paramCompare(m_c, o->m_c) && paramCompare(m_d, o->m_d)
                    && paramCompare(m_e, o->m_e) && paramCompare(m_f, o->m_f)
                    && paramCompare(m_g, o->m_g);
        } else {
            return false;
        }
    }

    float apply(float x) const override
    {
        if (x < m_d)
            return m_c * x + m_f;
        else
            return std::pow(m_a * x + m_b, m_g) + m_e;
    }

    std::unique_ptr<QColorTransferFunctionPrivate> inverted() const override
    {
        float a, b, c, d, e, f, g;

        d = m_c * m_d + m_f;

        if (!qFuzzyIsNull(m_c)) {
            c = 1.0f / m_c;
            f = -m_f / m_c;
        } else {
            c = 0.0f;
            f = 0.0f;
        }

        if (!qFuzzyIsNull(m_a) && !qFuzzyIsNull(m_g)) {
            a = std::pow(1.0f / m_a, m_g);
            b = -a * m_e;
            e = -m_b / m_a;
            g = 1.0f / m_g;
        } else {
            a = 0.0f;
            b = 0.0f;
            e = 1.0f;
            g = 1.0f;
        }

        return std::make_unique<QICCColorTransferFunction>(a, b, c, d, e, f, g);
    }

    // A few predefined curves:
    static std::unique_ptr<QColorTransferFunctionPrivate> fromGamma(float gamma)
    {
        return std::make_unique<QICCColorTransferFunction>(1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                                           gamma);
    }
    static std::unique_ptr<QColorTransferFunctionPrivate> fromSRgb()
    {
        return std::make_unique<QICCColorTransferFunction>(
                1.0f / 1.055f, 0.055f / 1.055f, 1.0f / 12.92f, 0.04045f, 0.0f, 0.0f, 2.4f);
    }
    static std::unique_ptr<QColorTransferFunctionPrivate> fromProPhotoRgb()
    {
        return std::make_unique<QICCColorTransferFunction>(1.0f, 0.0f, 1.0f / 16.0f, 16.0f / 512.0f,
                                                           0.0f, 0.0f, 1.8f);
    }

    void updateHints(quint32 &m_flags) const override
    {
        if (m_flags & quint32(Hints::Calculated))
            return;
        // We do not consider the case with m_d = 1.0f linear or simple,
        // since it wouldn't be linear for applyExtended().
        bool simple = paramCompare(m_a, 1.0f) && paramCompare(m_b, 0.0f) && paramCompare(m_d, 0.0f)
                && paramCompare(m_e, 0.0f);
        if (simple) {
            m_flags |= quint32(Hints::IsGamma);
            if (qFuzzyCompare(m_g, 1.0f))
                m_flags |= quint32(Hints::IsLinear);
        } else {
            if (matches(*fromSRgb()))
                m_flags |= quint32(Hints::IsSRgb);
        }
        m_flags |= quint32(Hints::Calculated);
    }

    float m_a;
    float m_b;
    float m_c;
    float m_d;
    float m_e;
    float m_f;
    float m_g;
};

// Defines a CICP or another type of standards-specified transfer curve.
// For HDR cases such as BT.2020, the PQ and HLG forward and inverse transfer
// curves are specified in the respective standards.
template<typename T, typename U>
class Q_GUI_EXPORT QCustomColorTransferFunction : public QColorTransferFunctionPrivate
{
public:
    using transferFunction = std::function<float(float)>;

    QCustomColorTransferFunction(const T eotf, const U oetf) : m_eotf(eotf), m_oetf(oetf) { }

    bool matches(const QColorTransferFunctionPrivate &rhs) const override
    {
        if (const auto *o = dynamic_cast<const QCustomColorTransferFunction *>(&rhs)) {
            return m_eotf.target<T>() == o->m_eotf.template target<T>()
                    && m_oetf.target<U>() == o->m_oetf.template target<U>();
        } else {
            return false;
        }
    }

    float apply(float x) const override { return m_eotf(x); }

    std::unique_ptr<QColorTransferFunctionPrivate> inverted() const override
    {
        return std::make_unique<QCustomColorTransferFunction<U, T>>(*m_oetf.target<U>(),
                                                                    *m_eotf.target<T>());
    }

private:
    transferFunction m_eotf;
    transferFunction m_oetf;

    void updateHints(quint32 &m_flags) const override { m_flags = quint32(Hints::Calculated); }
};

// Base class for ICC and CICP based transfer functions
class Q_GUI_EXPORT QColorTransferFunction
{
public:
    QColorTransferFunction() : m_impl(new QICCColorTransferFunction()) { }
    QColorTransferFunction(std::unique_ptr<QColorTransferFunctionPrivate> ptr)
        : m_impl(ptr.release())
    {
    }

    QColorTransferFunction(float a, float b, float c, float d, float e, float f, float g)
        : m_impl(QSharedPointer<QICCColorTransferFunction>::create(a, b, c, d, e, f, g))
    {
    }

    bool isGamma() const
    {
        m_impl->updateHints(m_flags);
        return m_flags & quint32(Hints::IsGamma);
    }
    bool isLinear() const
    {
        m_impl->updateHints(m_flags);
        return m_flags & quint32(Hints::IsLinear);
    }
    bool isSRgb() const
    {
        m_impl->updateHints(m_flags);
        return m_flags & quint32(Hints::IsSRgb);
    }

    float apply(float x) const { return m_impl->apply(x); }

    QColorTransferFunction inverted() const { return { m_impl->inverted() }; }

    bool matches(const QColorTransferFunction &o) const
    {
        return o.m_impl && m_impl->matches(*o.m_impl.get());
    }

    friend inline bool operator==(const QColorTransferFunction &f1,
                                  const QColorTransferFunction &f2);
    friend inline bool operator!=(const QColorTransferFunction &f1,
                                  const QColorTransferFunction &f2);

    // A few predefined curves:
    static QColorTransferFunction fromGamma(float gamma)
    {
        return { QICCColorTransferFunction::fromGamma(gamma) };
    }
    static QColorTransferFunction fromSRgb() { return { QICCColorTransferFunction::fromSRgb() }; }
    static QColorTransferFunction fromProPhotoRgb()
    {
        return { QICCColorTransferFunction::fromProPhotoRgb() };
    }

    QSharedPointer<QColorTransferFunctionPrivate> m_impl;

protected:
    mutable quint32 m_flags;
};

inline bool operator==(const QColorTransferFunction &f1, const QColorTransferFunction &f2)
{
    return f1.matches(f2);
}
inline bool operator!=(const QColorTransferFunction &f1, const QColorTransferFunction &f2)
{
    return !f1.matches(f2);
}

QT_END_NAMESPACE

#endif // QCOLORTRANSFERFUNCTION_P_H
