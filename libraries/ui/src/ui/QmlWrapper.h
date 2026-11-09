//
//  Created by Anthony J. Thibault on 2016-12-12
//  Copyright 2013-2016 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#ifndef hifi_QmlWrapper_h
#define hifi_QmlWrapper_h

#include <QtCore/QObject>
#include <QtCore/QVariant>

class QmlWrapper : public QObject {
    Q_OBJECT
public:
    QmlWrapper(QObject* qmlObject, QObject* parent = nullptr);

    Q_INVOKABLE void writeProperty(QString propertyName, QVariant propertyValue);
    Q_INVOKABLE void writeProperties(QVariant propertyMap);
    Q_INVOKABLE QVariant readProperty(const QString& propertyName);
    Q_INVOKABLE QVariant readProperties(const QVariant& propertyList);

protected:
    QObject* _qmlObject{ nullptr };
};

#endif