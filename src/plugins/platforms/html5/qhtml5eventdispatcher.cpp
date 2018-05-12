/****************************************************************************
**
** Copyright (C) 2017 The Qt Company Ltd.
** Contact: https://www.qt.io/licensing/
**
** This file is part of the plugins of the Qt Toolkit.
**
** $QT_BEGIN_LICENSE:GPL$
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see https://www.qt.io/terms-conditions. For further
** information use the contact form at https://www.qt.io/contact-us.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 3 or (at your option) any later version
** approved by the KDE Free Qt Foundation. The licenses are as published by
** the Free Software Foundation and appearing in the file LICENSE.GPL3
** included in the packaging of this file. Please review the following
** information to ensure the GNU General Public License requirements will
** be met: https://www.gnu.org/licenses/gpl-3.0.html.
**
** $QT_END_LICENSE$
**
****************************************************************************/

#include "qhtml5eventdispatcher.h"

#include <QtCore/QDebug>
#include <QtCore/QCoreApplication>

#include <emscripten.h>

class QHtml5EventDispatcherPrivate : public QUnixEventDispatcherQPAPrivate
{

};

QHtml5EventDispatcher *g_htmlEventDispatcher;

QHtml5EventDispatcher::QHtml5EventDispatcher(QObject *parent)
    : QUnixEventDispatcherQPA(parent)
{

    g_htmlEventDispatcher = this;
}

QHtml5EventDispatcher::~QHtml5EventDispatcher()
{
    g_htmlEventDispatcher = nullptr;
}

void QHtml5EventDispatcher::maintainTimers()
{
    if (!g_htmlEventDispatcher || !g_htmlEventDispatcher->m_hasMainLoop)
        return;

    g_htmlEventDispatcher->doMaintainTimers();
}

bool QHtml5EventDispatcher::processEvents(QEventLoop::ProcessEventsFlags flags)
{
    // WaitForMoreEvents is not supported (except for in combination with EventLoopExec below),
    // and we don't want the unix event dispatcher base class to attempt to wait either.
    flags &= ~QEventLoop::WaitForMoreEvents;

    // Handle normal processEvents.
    if (!(flags & QEventLoop::EventLoopExec))
        return QUnixEventDispatcherQPA::processEvents(flags);

    // Handle processEvents from QEventLoop::exec():
    //
    // At this point the application has created its root objects on
    // the stack and has called app.exec() which has called into this
    // function via QEventLoop.
    //
    // The application now expects that exec() will not return until
    // app exit time. However, the browser expects that we return
    // control to it periodically, also after initial setup in main().

    // EventLoopExec for nested event loops is not supported.
    Q_ASSERT(!m_hasMainLoop);
    m_hasMainLoop = true;

    // Call emscripten_set_main_loop_arg() with a callback which processes
    // events. Also set simulateInfiniteLoop to true which makes emscripten
    // return control to the browser without unwinding the C++ stack.
    auto callback = [](void *eventDispatcher) {
        QHtml5EventDispatcher *that = static_cast<QHtml5EventDispatcher *>(eventDispatcher);
        that->processEvents(QEventLoop::AllEvents);
        that->doMaintainTimers();
    };
    int fps = 0; // update using requestAnimationFrame
    int simulateInfiniteLoop = 1;
    emscripten_set_main_loop_arg(callback, this, fps, simulateInfiniteLoop);

    // Note: the above call never returns, not even at app exit
    return false;
}

void QHtml5EventDispatcher::doMaintainTimers()
{
    Q_D(QHtml5EventDispatcher);

    // This functon schedules native timers in order to wake up to
    // process events and activate Qt timers. This is done using the
    // emscripten_async_call() API which schedules a new timer.
    // There is unfortunately no way to cancel or update a current
    // native timer.

    // Schedule a zero-timer to continue processing any pending events.
    if (!m_hasZeroTimer && hasPendingEvents()) {
        auto callback = [](void *eventDispatcher) {
            QHtml5EventDispatcher *that = static_cast<QHtml5EventDispatcher *>(eventDispatcher);
            that->m_hasZeroTimer = false;
            that->QUnixEventDispatcherQPA::processEvents(QEventLoop::AllEvents);

            // Processing events may have posted new events or created new timers
            that->doMaintainTimers();
        };

        emscripten_async_call(callback, this, 0);
        m_hasZeroTimer = true;
        return;
    }

    auto timespecToNanosec = [](timespec ts) -> uint64_t { return ts.tv_sec * 1000 + ts.tv_nsec / (1000 * 1000); };

    // Get current time and time-to-first-Qt-timer. This polls for system
    // time, and we use this time as the current time for the duration of this call.
    timespec toWait;
    bool hasTimers = d->timerList.timerWait(toWait);
    if (!hasTimers)
        return; // no timer needed

    uint64_t currentTime = timespecToNanosec(d->timerList.currentTime);
    uint64_t toWaitDuration = timespecToNanosec(toWait);

    // The currently scheduled timer target is stored in m_currentTargetTime.
    // We can re-use it if the new target is equivalent or later.
    uint64_t newTargetTime = currentTime + toWaitDuration;
    if (newTargetTime >= m_currentTargetTime)
        return; // existing timer is good

    // Schedule a native timer with a callback which processes events (and timers)
    auto callback = [](void *eventDispatcher) {
        QHtml5EventDispatcher *that = static_cast<QHtml5EventDispatcher *>(eventDispatcher);
        that->m_currentTargetTime = std::numeric_limits<uint64_t>::max();
        that->QUnixEventDispatcherQPA::processEvents(QEventLoop::AllEvents);

        // Processing events may have posted new events or created new timers
        that->doMaintainTimers();
    };
    emscripten_async_call(callback, this, toWaitDuration);
    m_currentTargetTime = newTargetTime;
}
