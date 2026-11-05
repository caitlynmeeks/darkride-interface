//
//  FSTReader.h
//
//
//  Created by Clement on 3/26/15.
//  Copyright 2015 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#ifndef hifi_FSTReader_h
#define hifi_FSTReader_h

#include <QBuffer>
#include <QJsonDocument>
#include <QStringList>
#include <QVariantHash>

#include <shared/HifiTypes.h>

static const unsigned int FST_VERSION = 1;
static const QString FST_VERSION_FIELD = "version";
static const QString NAME_FIELD = "name";
static const QString TYPE_FIELD = "type";
static const QString FILENAME_FIELD = "filename";
static const QString PREVIEW_FIELD = "preview";
static const QString MARKETPLACE_ID_FIELD = "marketplaceID";
static const QString TEXDIR_FIELD = "texdir";
static const QString LOD_FIELD = "lod";
static const QString JOINT_INDEX_FIELD = "jointIndex";
static const QString SCALE_FIELD = "scale";
static const QString TRANSLATION_X_FIELD = "tx";
static const QString TRANSLATION_Y_FIELD = "ty";
static const QString TRANSLATION_Z_FIELD = "tz";
static const QString JOINT_FIELD = "joint";
static const QString BLENDSHAPE_FIELD = "bs";
// lines like bs's for the face shapes only face tracking (or a script) moves: the procedural face leaves them alone
static const QString FACE_TRACKING_BLENDSHAPE_FIELD = "faceTrackingBs";
static const QString SCRIPT_FIELD = "script";
static const QString JOINT_NAME_MAPPING_FIELD = "jointMap";
static const QString MATERIAL_MAPPING_FIELD = "materialMap";
static const QString COMMENT_FIELD = "comment";

class FSTReader {
public:

    enum ModelType {
        ENTITY_MODEL,
        HEAD_MODEL,
        BODY_ONLY_MODEL,
        HEAD_AND_BODY_MODEL,
        ATTACHMENT_MODEL
    };

    /// Reads an FST mapping from the supplied data.
    static hifi::VariantMultiHash readMapping(const QByteArray& data);

    /// Writes an FST mapping to a byte array.
    static QByteArray writeMapping(const hifi::VariantMultiHash& mapping);

    /// Whether text stays on one line of an .fst: no line break or other control character. The reader would start a
    /// new line there, with a key of its own.
    static bool isOneLine(const QString& text);

    /// Whether text can be one value of an .fst line without changing what the line says: one line (isOneLine) and no
    /// '=' (the reader splits a line at every '='). Model files and packages name joints and shape keys freely, so a
    /// line built from such a name is written only when the name passes this.
    static bool isLineValue(const QString& text);

    /// JSON as one value of an .fst line: compact, with each character isLineValue refuses written as a \u escape
    /// (such a character can only be inside a JSON string, where the escape means the same), so it always passes.
    static QString jsonLineValue(const QJsonDocument& json);

    /// Predicts the type of model by examining the mapping 
    static ModelType predictModelType(const hifi::VariantMultiHash& mapping);

    static QVector<QString> getScripts(const QUrl& fstUrl, const hifi::VariantMultiHash& mapping = hifi::VariantMultiHash());

    static QString getNameFromType(ModelType modelType);
    static FSTReader::ModelType getTypeFromName(const QString& name);
    static hifi::VariantMultiHash downloadMapping(const QString& url);

    /// The engine's face shapes to the model's own, as the serializers read them: the bs lines and the faceTrackingBs
    /// lines together, keyed by the engine's name, each value [model's shape, weight].
    static hifi::VariantMultiHash getBlendshapeMappings(const hifi::VariantMultiHash& mapping);

    /// The engine's names that only faceTrackingBs lines map (no bs line names them).
    static QStringList getFaceTrackingOnlyBlendshapes(const hifi::VariantMultiHash& mapping);

private:
    static void writeVariant(QBuffer& buffer, hifi::VariantMultiHash::const_iterator& it);
    static hifi::VariantMultiHash parseMapping(QIODevice* device);

    static QHash<FSTReader::ModelType, QString> _typesToNames;
    static QHash<QString, FSTReader::ModelType> _namesToTypes;
};

#endif // hifi_FSTReader_h