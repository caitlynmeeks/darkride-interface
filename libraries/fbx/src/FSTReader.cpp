//
//  FSTReader.cpp
//
//
//  Created by Clement on 3/26/15.
//  Copyright 2015 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "FSTReader.h"

#include <QBuffer>
#include <QEventLoop>
#include <QNetworkReply>
#include <QNetworkRequest>

#include <BlendshapeConstants.h>
#include <hfm/HFM.h>
#include <NetworkAccessManager.h>
#include <SharedUtil.h>
#include <hfm/ModelFormatLogging.h>

hifi::VariantMultiHash FSTReader::parseMapping(QIODevice* device) {
    hifi::VariantMultiHash properties;

    QByteArray line;
    while (!(line = device->readLine()).isEmpty()) {
        if ((line = line.trimmed()).startsWith('#')) {
            continue; // comment
        }
        QList<QByteArray> sections = line.split('=');
        if (sections.size() < 2) {
            continue;
        }
        QByteArray name = sections.at(0).trimmed();
        if (sections.size() == 2) {
            properties.insert(name, sections.at(1).trimmed());
        } else if (sections.size() == 3) {
            hifi::VariantMultiHash heading = hifi::multiHashFromVariant(properties.value(name));
            heading.insert(sections.at(1).trimmed(), sections.at(2).trimmed());
            properties.replace(name, hifi::variantFromMultiHash(heading));
        } else if (sections.size() >= 4) {
            hifi::VariantMultiHash heading = hifi::multiHashFromVariant(properties.value(name));
            QVariantList contents;
            for (int i = 2; i < sections.size(); i++) {
                contents.append(sections.at(i).trimmed());
            }
            heading.insert(sections.at(1).trimmed(), contents);
            properties.replace(name, hifi::variantFromMultiHash(heading));
        }
    }

    return properties;
}

static void removeBlendshape(hifi::VariantMultiHash& bs, const QString& key) {
    if (bs.contains(key)) {
        bs.remove(key);
    }
}

static void splitBlendshapes(hifi::VariantMultiHash& bs, const QString& key, const QString& leftKey, const QString& rightKey) {
    if (bs.contains(key) && !(bs.contains(leftKey) || bs.contains(rightKey))) {
        // key has been split into leftKey and rightKey blendshapes
        QVariantList origShapes = bs.values(key);
        QVariantList halfShapes;
        for (int i = 0; i < origShapes.size(); i++) {
            QVariantList origShape = origShapes[i].toList();
            QVariantList halfShape;
            halfShape.append(origShape[0]);
            halfShape.append(QVariant(0.5f * origShape[1].toFloat()));
            bs.insert(leftKey, halfShape);
            bs.insert(rightKey, halfShape);
        }
    }
}

// convert legacy blendshapes to arkit blendshapes
static void fixUpLegacyBlendshapes(hifi::VariantMultiHash& properties) {
    hifi::VariantMultiHash bs = hifi::multiHashFromVariant(properties.value("bs"));

    // These blendshapes have no ARKit equivalent, so we remove them.
    removeBlendshape(bs, "JawChew");
    removeBlendshape(bs, "ChinLowerRaise");
    removeBlendshape(bs, "ChinUpperRaise");
    removeBlendshape(bs, "LipsUpperOpen");
    removeBlendshape(bs, "LipsLowerOpen");

    // These blendshapes are split in ARKit, we replace them with their left and right sides with a weight of 1/2.
    splitBlendshapes(bs, "LipsUpperUp", "MouthUpperUp_L", "MouthUpperUp_R");
    splitBlendshapes(bs, "LipsLowerDown", "MouthLowerDown_L", "MouthLowerDown_R");
    splitBlendshapes(bs, "Sneer", "NoseSneer_L", "NoseSneer_R");

    // re-insert new mutated bs hash into mapping properties.
    properties.replace("bs", hifi::variantFromMultiHash(bs));
}

hifi::VariantMultiHash FSTReader::readMapping(const QByteArray& data) {
    QBuffer buffer(const_cast<QByteArray*>(&data));
    buffer.open(QIODevice::ReadOnly);
    hifi::VariantMultiHash mapping = FSTReader::parseMapping(&buffer);
    fixUpLegacyBlendshapes(mapping);
    return mapping;
}

void FSTReader::applyBlendshapeGains(const hifi::VariantMultiHash& mapping, hfm::Model& model) {
    const hifi::VariantMultiHash gains = hifi::multiHashFromVariant(mapping.value(BLENDSHAPE_GAIN_FIELD));
    for (auto it = gains.constBegin(); it != gains.constEnd(); ++it) {
        const int index = BLENDSHAPE_LOOKUP_MAP.value(it.key(), -1);
        bool ok = false;
        const float gain = it.value().toFloat(&ok);
        if (index < 0 || !ok) {
            continue;
        }
        if (model.proceduralBlendshapeGains.size() <= index) {
            model.proceduralBlendshapeGains.resize((int)Blendshapes::BlendshapeCount);
            model.proceduralBlendshapeGains.fill(1.0f);
        }
        model.proceduralBlendshapeGains[index] = glm::clamp(gain, 0.0f, 4.0f);
    }
}

hifi::VariantMultiHash FSTReader::getBlendshapeMappings(const hifi::VariantMultiHash& mapping) {
    hifi::VariantMultiHash blendshapes = hifi::multiHashFromVariant(mapping.value(BLENDSHAPE_FIELD));
    const hifi::VariantMultiHash faceTracking = hifi::multiHashFromVariant(mapping.value(FACE_TRACKING_BLENDSHAPE_FIELD));
    for (auto it = faceTracking.constBegin(); it != faceTracking.constEnd(); ++it) {
        blendshapes.insert(it.key(), it.value());
    }
    return blendshapes;
}

QStringList FSTReader::getFaceTrackingOnlyBlendshapes(const hifi::VariantMultiHash& mapping) {
    const hifi::VariantMultiHash blendshapes = hifi::multiHashFromVariant(mapping.value(BLENDSHAPE_FIELD));
    QStringList names;
    for (const QString& name : hifi::multiHashFromVariant(mapping.value(FACE_TRACKING_BLENDSHAPE_FIELD)).uniqueKeys()) {
        if (!blendshapes.contains(name)) {
            names << name;
        }
    }
    return names;
}

// A line of an .fst, written only when it stays one line: its keys and values can come from a model file's names.
static void writeLine(QBuffer& buffer, const QByteArray& line) {
    if (!FSTReader::isOneLine(QString::fromUtf8(line))) {
        qCWarning(modelformat) << "FSTReader: left out a line holding a line break:" << line.left(line.indexOf(' '));
        return;
    }
    buffer.write(line + "\n");
}

// A heading's value as a list of values ("bs = JawOpen = MouthOpen = 0.7"), or empty for a single value. Only a real list
// counts, as Qt 5's QVariant::toList() did: Qt 6's toList() also turns a text value into a list of its characters.
static QVariantList listOf(const QVariant& value) {
    const int type = value.userType();
    return type == QMetaType::QVariantList || type == QMetaType::QStringList ? value.toList() : QVariantList();
}

void FSTReader::writeVariant(QBuffer& buffer, hifi::VariantMultiHash::const_iterator& it) {
    QByteArray key = it.key().toUtf8() + " = ";
    const hifi::VariantMultiHash hashValue = hifi::multiHashFromVariant(it.value());
    if (hashValue.isEmpty()) {
        writeLine(buffer, key + it.value().toByteArray());
        return;
    }
    for (hifi::VariantMultiHash::const_iterator second = hashValue.constBegin(); second != hashValue.constEnd(); second++) {
        QByteArray extendedKey = key + second.key().toUtf8();
        QVariantList listValue = listOf(second.value());
        if (listValue.isEmpty()) {
            writeLine(buffer, extendedKey + " = " + second.value().toByteArray());
            continue;
        }
        QByteArray line = extendedKey;
        for (QVariantList::const_iterator third = listValue.constBegin(); third != listValue.constEnd(); third++) {
            line += " = " + third->toByteArray();
        }
        writeLine(buffer, line);
    }
}

bool FSTReader::isOneLine(const QString& text) {
    for (const QChar c : text) {
        const QChar::Category category = c.category();
        if (category == QChar::Other_Control || category == QChar::Separator_Line ||
            category == QChar::Separator_Paragraph) {
            return false;
        }
    }
    return true;
}

bool FSTReader::isLineValue(const QString& text) {
    return !text.contains('=') && isOneLine(text);
}

QString FSTReader::jsonLineValue(const QJsonDocument& json) {
    const QString compact = QString::fromUtf8(json.toJson(QJsonDocument::Compact));
    QString value;
    value.reserve(compact.size());
    for (const QChar c : compact) {
        if (c == '=' || !isOneLine(QString(c))) {
            value += QString("\\u%1").arg((uint)c.unicode(), 4, 16, QChar('0'));
        } else {
            value += c;
        }
    }
    return value;
}

QByteArray FSTReader::writeMapping(const hifi::VariantMultiHash& mapping) {
    static const QStringList PREFERED_ORDER = QStringList() << NAME_FIELD << TYPE_FIELD << SCALE_FIELD << FILENAME_FIELD
    << MARKETPLACE_ID_FIELD << TEXDIR_FIELD << SCRIPT_FIELD << JOINT_FIELD
    << BLENDSHAPE_FIELD << JOINT_INDEX_FIELD;
    QBuffer buffer;
    buffer.open(QIODevice::WriteOnly);

    for (auto key : PREFERED_ORDER) {
        auto it = mapping.find(key);
        if (it != mapping.constEnd()) {
            if (key == SCRIPT_FIELD) { // writeVariant does not handle strings added using insertMulti.
                for (auto multi : mapping.values(key)) {
                    writeLine(buffer, key.toUtf8() + " = " + multi.toByteArray());
                }
            } else {
                writeVariant(buffer, it);
            }
        }
    }

    for (auto it = mapping.constBegin(); it != mapping.constEnd(); it++) {
        if (!PREFERED_ORDER.contains(it.key())) {
            writeVariant(buffer, it);
        }
    }
    return buffer.data();
}

QHash<FSTReader::ModelType, QString> FSTReader::_typesToNames;
QString FSTReader::getNameFromType(ModelType modelType) {
    if (_typesToNames.size() == 0) {
        _typesToNames[ENTITY_MODEL] = "entity";
        _typesToNames[HEAD_MODEL] = "head";
        _typesToNames[BODY_ONLY_MODEL] = "body";
        _typesToNames[HEAD_AND_BODY_MODEL] = "body+head";
        _typesToNames[ATTACHMENT_MODEL] = "attachment";
    }
    return _typesToNames[modelType];
}

QHash<QString, FSTReader::ModelType> FSTReader::_namesToTypes;
FSTReader::ModelType FSTReader::getTypeFromName(const QString& name) {
    if (_namesToTypes.size() == 0) {
        _namesToTypes["entity"] = ENTITY_MODEL;
        _namesToTypes["head"] = HEAD_MODEL ;
        _namesToTypes["body"] = BODY_ONLY_MODEL;
        _namesToTypes["body+head"] = HEAD_AND_BODY_MODEL;
        
        // NOTE: this is not yet implemented, but will be used to allow you to attach fully independent models to your avatar
        _namesToTypes["attachment"] = ATTACHMENT_MODEL; 
    }
    return _namesToTypes[name];
}

FSTReader::ModelType FSTReader::predictModelType(const hifi::VariantMultiHash& mapping) {

    const hifi::VariantMultiHash joints = hifi::multiHashFromVariant(mapping.value("joint"));

    // if the mapping includes the type hint... then we trust the mapping
    if (mapping.contains(TYPE_FIELD)) {
        return FSTReader::getTypeFromName(mapping.value(TYPE_FIELD).toString());
    }
    
    // check for blendshapes
    bool hasBlendshapes = mapping.contains(BLENDSHAPE_FIELD);

    // a Head needs to have these minimum fields...
    //joint = jointEyeLeft = EyeL = 1
    //joint = jointEyeRight = EyeR = 1
    //joint = jointNeck = Head = 1
    bool hasHeadMinimum = joints.contains("jointNeck") && joints.contains("jointEyeLeft") && joints.contains("jointEyeRight");

    // a Body needs to have these minimum fields...
    //joint = jointRoot = Hips
    //joint = jointLean = Spine
    //joint = jointNeck = Neck
    //joint = jointHead = HeadTop_End

    bool hasBodyMinimumJoints = joints.contains("jointRoot") && joints.contains("jointLean") && joints.contains("jointNeck")  
                                        && joints.contains("jointHead");
    
    bool isLikelyHead = hasBlendshapes || hasHeadMinimum;

    if (isLikelyHead && hasBodyMinimumJoints) {
        return HEAD_AND_BODY_MODEL;
    }
      
    if (isLikelyHead) {
        return HEAD_MODEL;
    }

    if (hasBodyMinimumJoints) {
        return BODY_ONLY_MODEL;
    }
    
    return ENTITY_MODEL;
}

QVector<QString> FSTReader::getScripts(const QUrl& url, const hifi::VariantMultiHash& mapping) {

    auto fstMapping = mapping.isEmpty() ? downloadMapping(url.toString()) : mapping;
    QVector<QString> scriptPaths;
    if (!fstMapping.value(SCRIPT_FIELD).isNull()) {
        auto scripts = fstMapping.values(SCRIPT_FIELD).toVector();
        for (auto &script : scripts) {
            QString scriptPath = script.toString();
            if (QUrl(scriptPath).isRelative()) {
                if (scriptPath.at(0) == '/') {
                    scriptPath = scriptPath.right(scriptPath.length() - 1);
                }
                scriptPath = url.resolved(QUrl(scriptPath)).toString();
            }
            scriptPaths.push_back(scriptPath);
        }
    }
    return scriptPaths;
}

hifi::VariantMultiHash FSTReader::downloadMapping(const QString& url) {
    QNetworkAccessManager& networkAccessManager = NetworkAccessManager::getInstance();
    QNetworkRequest networkRequest = QNetworkRequest(url);
    networkRequest.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    networkRequest.setHeader(QNetworkRequest::UserAgentHeader, TIVOLI_CLOUD_VR_USER_AGENT);
    QNetworkReply* reply = networkAccessManager.get(networkRequest);
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();
    QByteArray fstContents = reply->readAll();
    delete reply;
    return FSTReader::readMapping(fstContents);
}
