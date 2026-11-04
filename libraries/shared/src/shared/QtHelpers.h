//
//  Created by Bradley Austin Davis on 2017/06/29
//  Copyright 2013-2015 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#pragma once
#ifndef hifi_Shared_QtHelpers_h
#define hifi_Shared_QtHelpers_h

#include <QtCore/QObject>

#include <functional>
#include <utility>

#if defined(Q_OS_WIN)
// Enable event queue debugging
#define DEBUG_EVENT_QUEUE
#endif

namespace hifi { namespace qt {
void addBlockingForbiddenThread(const QString& name, QThread* thread = nullptr);

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
// Qt 6.5's Q_ARG and Q_RETURN_ARG make typed arguments (QMetaMethodArgument, QTemplatedMetaMethodReturnArgument<T>),
// not QGenericArgument, and Qt's own invokeMethod takes any number of them. checkedBlockingInvoke gives the warnings and
// the profile range of the Qt 5 forms below, around the call.
bool checkedBlockingInvoke(const char* function, const std::function<bool()>& invoke);
template <typename... Args>
bool blockingInvokeMethod(const char* function, QObject* obj, const char* member, Args&&... args) {
    return checkedBlockingInvoke(function, [&] {
        return QMetaObject::invokeMethod(obj, member, Qt::BlockingQueuedConnection, std::forward<Args>(args)...);
    });
}
#else
bool blockingInvokeMethod(
    const char* function,
    QObject *obj, const char *member,
    QGenericReturnArgument ret,
    QGenericArgument val0 = QGenericArgument(Q_NULLPTR),
    QGenericArgument val1 = QGenericArgument(),
    QGenericArgument val2 = QGenericArgument(),
    QGenericArgument val3 = QGenericArgument(),
    QGenericArgument val4 = QGenericArgument(),
    QGenericArgument val5 = QGenericArgument(),
    QGenericArgument val6 = QGenericArgument(),
    QGenericArgument val7 = QGenericArgument(),
    QGenericArgument val8 = QGenericArgument(),
    QGenericArgument val9 = QGenericArgument());

bool blockingInvokeMethod(
    const char* function,
    QObject *obj, const char *member,
    QGenericArgument val0 = QGenericArgument(Q_NULLPTR),
    QGenericArgument val1 = QGenericArgument(),
    QGenericArgument val2 = QGenericArgument(),
    QGenericArgument val3 = QGenericArgument(),
    QGenericArgument val4 = QGenericArgument(),
    QGenericArgument val5 = QGenericArgument(),
    QGenericArgument val6 = QGenericArgument(),
    QGenericArgument val7 = QGenericArgument(),
    QGenericArgument val8 = QGenericArgument(),
    QGenericArgument val9 = QGenericArgument());
#endif

// The same hop for a callable: f runs on obj's thread and the caller waits. The warnings (a forbidden thread) and the
// profile range are the named forms'; on obj's own thread f runs in place, with the warning the named forms give,
// where they would wait for themselves. blockingInvokeResult returns what f returns, value-initialised if the hop
// fails. BLOCKING_INVOKE(obj, callable) names the caller for the warnings, like BLOCKING_INVOKE_METHOD.
void blockingInvoke(const char* function, QObject* obj, const std::function<void()>& f);
template <typename F>
auto blockingInvokeResult(const char* function, QObject* obj, F&& f) -> decltype(f()) {
    decltype(f()) result{};
    blockingInvoke(function, obj, [&] { result = f(); });
    return result;
}

// Inspecting of the qt event queue
// requres access to private Qt datastructures
// Querying the event queue should be done with
// care as it could lock the threadData->postEventList.mutex
// The code uses a tryLock to avoid the possability of a
// deadlock during a call to this code, although that is unlikely
// When getEventQueueSize fails to get the lock, it returns -1
#ifdef DEBUG_EVENT_QUEUE
    int getEventQueueSize(QThread* thread);
    void dumpEventQueue(QThread* thread);
#endif // DEBUG_EVENT_QUEUE

} }

#define BLOCKING_INVOKE_METHOD(obj, member, ...) \
    ::hifi::qt::blockingInvokeMethod(__FUNCTION__, obj, member, ##__VA_ARGS__)
#define BLOCKING_INVOKE(obj, callable) \
    ::hifi::qt::blockingInvokeResult(__FUNCTION__, obj, callable)

#endif
