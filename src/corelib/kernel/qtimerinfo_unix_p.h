// Copyright (C) 2016 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#ifndef QTIMERINFO_UNIX_P_H
#define QTIMERINFO_UNIX_P_H

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

#include <QtCore/private/qglobal_p.h>

#include "qabstracteventdispatcher.h"

#include <sys/time.h> // struct timespec
#include <chrono>

QT_BEGIN_NAMESPACE

// internal timer info
struct QTimerInfo {
    int id;           // - timer identifier
    Qt::TimerType timerType; // - timer type
    std::chrono::milliseconds interval; // - timer interval
    std::chrono::steady_clock::time_point timeout; // - when to actually fire
    QObject *obj;     // - object to receive event
    QTimerInfo **activateRef; // - ref from activateTimers
};

class Q_CORE_EXPORT QTimerInfoList : public QList<QTimerInfo*>
{
    // state variables used by activateTimers()
    QTimerInfo *firstTimerInfo = nullptr;

public:
    QTimerInfoList();

    std::chrono::steady_clock::time_point currentTime;

    bool timerWait(timespec &);
    void timerInsert(QTimerInfo *);

    qint64 timerRemainingTime(int timerId);
    std::chrono::milliseconds remainingDuration(int timerId);

    void registerTimer(int timerId, qint64 interval, Qt::TimerType timerType, QObject *object);
    void registerTimer(int timerId, std::chrono::milliseconds interval, Qt::TimerType timerType,
                       QObject *object);
    bool unregisterTimer(int timerId);
    bool unregisterTimers(QObject *object);
    QList<QAbstractEventDispatcher::TimerInfo> registeredTimers(QObject *object) const;

    int activateTimers();
    bool hasPendingTimers();

    QList::const_iterator findTimerById(int timerId) const
    {
        auto matchesId = [timerId](const QTimerInfo *t) { return t->id == timerId; };
        return std::find_if(cbegin(), cend(), matchesId);
    }

private:
    std::chrono::steady_clock::time_point updateCurrentTime();
};

QT_END_NAMESPACE

#endif // QTIMERINFO_UNIX_P_H
