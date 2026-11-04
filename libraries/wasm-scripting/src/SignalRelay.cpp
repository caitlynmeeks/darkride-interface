//
//  SignalRelay.cpp
//  libraries/wasm-scripting/src
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "SignalRelay.h"
#include "ValueCodec.h"

#include <limits>

#include <QtCore/QCoreApplication>
#include <QtCore/QEvent>
#include <QtCore/QMetaObject>
#include <QtCore/QVariant>

namespace {
const int SLOT_OFFSET = QObject::staticMetaObject.methodCount();
// Qt keeps a connection's method index in a ushort (QObjectPrivate::Connection::method_relative, and QMetaCallEvent's):
// past it, the index wraps onto QObject's own methods, deleteLater among them.
const int MAX_SLOT = std::numeric_limits<ushort>::max() - SLOT_OFFSET;

// Posted to the relay as a subscription ends. Posted events reach a receiver in order, so this one arrives after any
// emission queued to the slot before the end (which finds no subscription and is dropped), and from then on the slot
// can serve a new subscription without a late emission reaching the wrong handler.
const QEvent::Type SLOT_FREED = static_cast<QEvent::Type>(QEvent::registerEventType());

class SlotFreedEvent : public QEvent {
public:
    explicit SlotFreedEvent(int slot) : QEvent(SLOT_FREED), slot(slot) {}
    const int slot;
};
}

SignalRelay::SignalRelay(QObject* parent) : QObject(parent) {
}

SignalRelay::~SignalRelay() {
    // QObject's destructor drops every connection this relay receives; nothing to do here.
}

int SignalRelay::connect(QObject* sender, const QString& signal, Handler handler, QString& error) {
    if (!sender) {
        error = "no object";
        return 0;
    }
    const QMetaObject* meta = sender->metaObject();
    int signalIndex = -1;
    if (signal.contains('(')) {
        signalIndex = meta->indexOfSignal(QMetaObject::normalizedSignature(signal.toUtf8().constData()).constData());
    } else {
        const QByteArray name = signal.toUtf8();
        for (int i = 0; i < meta->methodCount(); ++i) {
            QMetaMethod method = meta->method(i);
            if (method.methodType() == QMetaMethod::Signal && method.name() == name) {
                signalIndex = i;
                break;
            }
        }
    }
    if (signalIndex < 0) {
        error = QString("%1 has no signal %2").arg(QString::fromUtf8(meta->className()), signal);
        return 0;
    }
    QMetaMethod method = meta->method(signalIndex);
    QVector<int> types;
    for (int p = 0; p < method.parameterCount(); ++p) {
        int type = method.parameterType(p);
        if (type == QMetaType::UnknownType) {
            type = QMetaType::type(method.parameterTypes().at(p).constData());
        }
        types.append(type);
    }

    int slot;
    if (!_freeSlots.isEmpty()) {
        slot = _freeSlots.takeLast();
    } else if (_nextSlot <= MAX_SLOT) {
        slot = _nextSlot++;
    } else {
        error = QString("too many signal connections at once (%1)").arg(_connections.size());
        return 0;
    }
    // Automatic: direct from the relay's own thread, queued from any other.
    if (!QMetaObject::connect(sender, signalIndex, this, SLOT_OFFSET + slot, Qt::AutoConnection, nullptr)) {
        _freeSlots.append(slot);
        error = QString("could not connect %1.%2").arg(QString::fromUtf8(meta->className()), signal);
        return 0;
    }
    const int connection = _nextConnection++;
    _connections.insert(connection, Connection { QPointer<QObject>(sender), method, types, std::move(handler), SLOT_OFFSET + slot });
    _connectionBySlot.insert(slot, connection);
    return connection;
}

bool SignalRelay::disconnect(int connection) {
    auto found = _connections.find(connection);
    if (found == _connections.end()) {
        return false;
    }
    const int slot = found.value().slot - SLOT_OFFSET;
    if (found.value().sender) {
        QMetaObject::disconnect(found.value().sender.data(), found.value().signal.methodIndex(), this, found.value().slot);
    }
    _connectionBySlot.remove(slot);
    _connections.erase(found);
    QCoreApplication::postEvent(this, new SlotFreedEvent(slot));
    return true;
}

void SignalRelay::disconnectAll() {
    const QList<int> ids = _connections.keys();
    for (int id : ids) {
        disconnect(id);
    }
}

QObject* SignalRelay::sender(int connection) const {
    auto found = _connections.find(connection);
    return found == _connections.end() ? nullptr : found.value().sender.data();
}

QMetaMethod SignalRelay::signal(int connection) const {
    auto found = _connections.find(connection);
    return found == _connections.end() ? QMetaMethod() : found.value().signal;
}

bool SignalRelay::event(QEvent* event) {
    if (event->type() == SLOT_FREED) {
        _freeSlots.append(static_cast<SlotFreedEvent*>(event)->slot);
        return true;
    }
    return QObject::event(event);
}

int SignalRelay::qt_metacall(QMetaObject::Call call, int id, void** arguments) {
    id = QObject::qt_metacall(call, id, arguments);
    if (id < 0) {
        return id;
    }
    if (call == QMetaObject::InvokeMetaMethod) {
        auto found = _connectionBySlot.find(id);
        if (found != _connectionBySlot.end()) {
            // a copy: the handler may disconnect this very connection
            Connection connection = _connections.value(found.value());
            deliver(connection, arguments);
        }
        // a queued emission that arrived after its disconnect lands here with no entry, and is dropped
        return -1;
    }
    return id;
}

void SignalRelay::deliver(const Connection& connection, void** arguments) {
    QVariantList values;
    values.reserve(connection.parameterTypes.size());
    for (int p = 0; p < connection.parameterTypes.size(); ++p) {
        const int type = connection.parameterTypes[p];
        void* argument = arguments[p + 1];
        if (type == QMetaType::UnknownType || !argument) {
            values.append(QVariant());
        } else if (type == QMetaType::QVariant) {
            values.append(*static_cast<const QVariant*>(argument));
        } else {
            values.append(variantOfType(type, argument));
        }
    }
    if (connection.handler) {
        connection.handler(_connectionBySlot.value(connection.slot - SLOT_OFFSET), values);
    }
}
