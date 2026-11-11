//
//  EntityContentAddressesTests.cpp
//  tests/entity-content/src
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//  See the accompanying file LICENSE-MIT or https://opensource.org/licenses/MIT
//

#include "EntityContentAddressesTests.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>

#include <EntityContentAddresses.h>
#include <EntityItemProperties.h>
#include <RegisteredMetaTypes.h>

QTEST_GUILESS_MAIN(EntityContentAddressesTests)

using namespace EntityContentAddresses;

static QJsonObject parse(const QString& json) {
    return QJsonDocument::fromJson(json.toUtf8()).object();
}

void EntityContentAddressesTests::testThisComputer() {
    const QStringList onThisComputer = {
        "file:///Users/me/car.glb", "FILE:///C:/car.glb", "File:///x", "file://localhost/x", " file:///x ",
        "/Users/me/car.glb", "C:\\models\\car.glb", "C:/models/car.glb", "c:", "\\\\host\\share\\car.glb",
        "//host/share/car.glb", "models/car.glb", "car.glb",
        "/~/../escape.png", "/~/ok/../../escape.png", "/~/%2e%2e/escape.png", "/~//../escape.png", "/~/..\\escape.png",
        "/~/..%5c..", "/~/%2e%2e%5c",
        "data:image/png;base64,AAAA", "about:blank"
    };
    for (const QString& address : onThisComputer) {
        QVERIFY2(isOnThisComputer(address), qPrintable("should name this computer: " + address));
    }
    const QStringList elsewhere = {
        "", "  ", "http://host/car.glb", "https://host/car.glb", "HTTPS://host/x", "ftp://host/x",
        "atp:/models/car.glb", "atp:0123456789abcdef.glb", "tea://user/x.glb", "qrc:///x.png",
        "/~/images/ok.png", "/~/", "/~/a/../b.png", "/~/a/b/../../c.png",
        "/~/%252e%252e/ok.png"   // decoded once, as the loader does: a folder named "%2e%2e", inside
    };
    for (const QString& address : elsewhere) {
        QVERIFY2(!isOnThisComputer(address), qPrintable("should not name this computer: " + address));
    }
}

void EntityContentAddressesTests::testPathOnThisComputer() {
    const QStringList paths = {
        "file:///x.html", "FILE:///x", "/Users/me/x.html", "\\\\host\\x", "//host/x", "C:\\x.html", "C:/x.html",
        "/~/../x"
    };
    for (const QString& address : paths) {
        QVERIFY2(isPathOnThisComputer(address), qPrintable("should be a path on this computer: " + address));
    }
    const QStringList notPaths = {
        "", "Roboto", "Courier New", "page.html", "about:blank", "data:text/html,hi", "javascript:void(0)",
        "http://host/x", "https://x", "qrc:///x", "/~/fonts/x.sdff"
    };
    for (const QString& address : notPaths) {
        QVERIFY2(!isPathOnThisComputer(address), qPrintable("should not be a path on this computer: " + address));
    }
}

void EntityContentAddressesTests::testClearsProperties() {
    EntityItemProperties properties;
    properties.setModelURL("file:///Users/me/car.glb");
    properties.setCompoundShapeURL("/Users/me/hull.obj");
    properties.setCollisionSoundURL("C:\\sounds\\hit.wav");
    properties.getAnimation().setURL("\\\\host\\share\\run.fbx");
    properties.getSkybox().setURL("FILE:///sky.png");
    properties.getAmbientLight().setAmbientURL("/~/../ambient.ktx");
    properties.setImageURL("pictures/relative.png");
    properties.setMaterialURL("file:///Users/me/m.json");
    properties.setTerrainURL("file:///Users/me/t.drkt");
    properties.getGrab().setEquippableIndicatorURL("/Users/me/hand.fbx");
    properties.setFilterURL("file:///Users/me/filter.js");
    properties.setSourceUrl("file:///Users/me/page.html");
    properties.setScriptURL("/Users/me/inject.js");
    properties.setFont("C:/fonts/x.sdff");
    properties.setHref("file:///Users/me/world.json");

    const QStringList cleared = clearThisComputer(properties);
    QCOMPARE(cleared, propertyNames());

    QCOMPARE(properties.getModelURL(), QString());
    QCOMPARE(properties.getCompoundShapeURL(), QString());
    QCOMPARE(properties.getCollisionSoundURL(), QString());
    QCOMPARE(properties.getAnimation().getURL(), QString());
    QCOMPARE(properties.getSkybox().getURL(), QString());
    QCOMPARE(properties.getAmbientLight().getAmbientURL(), QString());
    QCOMPARE(properties.getImageURL(), QString());
    QCOMPARE(properties.getMaterialURL(), QString());
    QCOMPARE(properties.getTerrainURL(), QString());
    QCOMPARE(properties.getGrab().getEquippableIndicatorURL(), QString());
    QCOMPARE(properties.getFilterURL(), QString());
    QCOMPARE(properties.getSourceUrl(), QString());
    QCOMPARE(properties.getScriptURL(), QString());
    QCOMPARE(properties.getFont(), QString());
    QCOMPARE(properties.getHref(), QString());
    // the emptied value counts as a change, so it is sent on and carried by a copy or an export
    QVERIFY(properties.modelURLChanged());
    QVERIFY(properties.getAnimation().urlChanged());
    QVERIFY(properties.hrefChanged());

    // a second pass finds nothing
    QVERIFY(clearThisComputer(properties).isEmpty());
}

void EntityContentAddressesTests::testPartialEditUntouched() {
    // a world script's edit carries only the properties it sets: the others are neither looked at nor marked changed
    EntityItemProperties edit;
    edit.setName("x");
    QVERIFY(clearThisComputer(edit).isEmpty());
    QVERIFY(edit.nameChanged());
    QVERIFY(!edit.modelURLChanged());
    QVERIFY(!edit.getAnimation().urlChanged());
    QVERIFY(!edit.hrefChanged());
    QVERIFY(!edit.fontChanged());
    QVERIFY(!edit.sourceUrlChanged());
    QVERIFY(!edit.texturesChanged());
    QVERIFY(!edit.materialDataChanged());
    QVERIFY(!edit.userDataChanged());

    // a changed address from elsewhere keeps its value and its change
    edit.setModelURL("https://cdn/x.glb");
    QVERIFY(clearThisComputer(edit).isEmpty());
    QCOMPARE(edit.getModelURL(), QString("https://cdn/x.glb"));
    QVERIFY(edit.modelURLChanged());

    // an address on this computer that the object holds but does not carry as changed is left alone
    EntityItemProperties held;
    held.setModelURL("file:///Users/me/x.glb");
    held.setModelURLChanged(false);
    held.setTextures("{\"tex\":\"file:///Users/me/t.png\"}");
    held.setTexturesChanged(false);
    QVERIFY(clearThisComputer(held).isEmpty());
    QCOMPARE(held.getModelURL(), QString("file:///Users/me/x.glb"));
    QVERIFY(!held.modelURLChanged());
    QVERIFY(!held.texturesChanged());
}

void EntityContentAddressesTests::testKeepsFetchedAndSpecialValues() {
    EntityItemProperties defaults;
    QVERIFY(clearThisComputer(defaults).isEmpty());

    EntityItemProperties properties;
    properties.setModelURL("https://cdn.darkride.app/car.glb");
    properties.setCompoundShapeURL("atp:/hull.obj");
    properties.setMaterialURL("materialData");
    properties.setImageURL("tea://user/pictures/a.png");
    properties.getSkybox().setURL("qrc:///sky.png");
    properties.getAmbientLight().setAmbientURL("/~/images/ambient.ktx");
    properties.setFont("Courier New");
    properties.setSourceUrl("about:blank");
    properties.setScriptURL("https://cdn.darkride.app/inject.js");
    properties.setHref("/1,2,3/0,0,0,1");
    properties.setTextures("{\"tex.body\":\"https://cdn.darkride.app/body.png\"}");
    properties.setMaterialData("{\"materials\":{\"albedoMap\":\"https://cdn.darkride.app/a.png\"}}");
    properties.setUserData("{\"impostor\":{\"albedo\":\"https://cdn.darkride.app/a.png\",\"normal\":\"atp:/n.png\"}}");
    QVERIFY(clearThisComputer(properties).isEmpty());
    QCOMPARE(properties.getModelURL(), QString("https://cdn.darkride.app/car.glb"));
    QCOMPARE(properties.getMaterialURL(), QString("materialData"));
    QCOMPARE(properties.getFont(), QString("Courier New"));
    QCOMPARE(properties.getSourceUrl(), QString("about:blank"));
    QCOMPARE(properties.getHref(), QString("/1,2,3/0,0,0,1"));
    QCOMPARE(properties.getTextures(), QString("{\"tex.body\":\"https://cdn.darkride.app/body.png\"}"));

    // href: a world's name, a world link and a drive path are not file: addresses (AddressManager ignores the last)
    for (const QString& href : { "darkside", "hifi://darkside/0,0,0", "C:/Users/me/world.json" }) {
        properties.setHref(href);
        QVERIFY(clearThisComputer(properties).isEmpty());
        QCOMPARE(properties.getHref(), href);
    }
}

void EntityContentAddressesTests::testTextures() {
    EntityItemProperties properties;
    // a Model's map: only the entry on this computer is emptied
    properties.setTextures("{\"tex.body\":\"file:///Users/me/body.png\",\"tex.wheel\":\"https://cdn/w.png\"}");
    QCOMPARE(clearThisComputer(properties), QStringList({ "textures" }));
    QJsonObject map = parse(properties.getTextures());
    QCOMPARE(map.value("tex.body").toString(), QString());
    QCOMPARE(map.value("tex.wheel").toString(), QString("https://cdn/w.png"));
    QVERIFY(properties.texturesChanged());

    // the legacy list form is read the same way and written back as JSON
    properties.setTextures("tex.body:\"C:/Users/me/body.png\",\ntex.wheel:\"https://cdn/w.png\"");
    QCOMPARE(clearThisComputer(properties), QStringList({ "textures" }));
    map = parse(properties.getTextures());
    QCOMPARE(map.value("tex.body").toString(), QString());
    QCOMPARE(map.value("tex.wheel").toString(), QString("https://cdn/w.png"));

    // a ParticleEffect's or PolyLine's one address
    properties.setTextures("file:///Users/me/spark.png");
    QCOMPARE(clearThisComputer(properties), QStringList({ "textures" }));
    QCOMPARE(properties.getTextures(), QString());
    properties.setTextures("https://cdn/spark.png");
    QVERIFY(clearThisComputer(properties).isEmpty());
    QCOMPARE(properties.getTextures(), QString("https://cdn/spark.png"));

    // a value the Model renderer can't read and no other renderer fetches is emptied
    properties.setTextures("{oops");
    QCOMPARE(clearThisComputer(properties), QStringList({ "textures" }));
    QCOMPARE(properties.getTextures(), QString());

    // the legacy list form with a leading space: the Model renderer reads it as a map with the key " {x", and so
    // does the helper, which empties the entry
    properties.setTextures(" {x:\"file:///etc/passwd\"");
    QCOMPARE(clearThisComputer(properties), QStringList({ "textures" }));
    map = parse(properties.getTextures());
    QCOMPARE(map.keys(), QStringList({ " {x" }));
    QCOMPARE(map.value(" {x").toString(), QString());
    // a leading newline makes the legacy path produce no map at all: the renderer ignores it, the helper empties it
    properties.setTextures(" \n{\"tex\":\"file:///Users/me/t.png\"}");
    QCOMPARE(clearThisComputer(properties), QStringList({ "textures" }));
    QCOMPARE(properties.getTextures(), QString());
}

void EntityContentAddressesTests::testTexturesReadAsRenderer() {
    // the helper's reading of textures and the Model renderer's (parseTexturesToMap) must agree, form by form
    const QStringList forms = {
        "{\"tex.body\":\"https://cdn/b.png\",\"tex.wheel\":\"file:///Users/me/w.png\"}",
        "tex.body:\"https://cdn/b.png\",\ntex.wheel:\"file:///Users/me/w.png\"",
        " {x:\"file:///etc/passwd\"",
        " \n{\"tex\":\"file:///Users/me/t.png\"}",
        "https://cdn/spark.png",
        "{oops",
        "  {\"tex\":\"https://cdn/t.png\"}",
        "{\"tex\":\"https://cdn/t.png\"} ",
        ""
    };
    for (const QString& form : forms) {
        QJsonObject helperMap;
        const bool helperReads = readTexturesMap(form, helperMap);
        const QVariantMap rendererMap = parseTexturesToMap(form, QVariantMap());
        QCOMPARE(helperReads, !rendererMap.isEmpty());
        QCOMPARE(helperMap.keys(), rendererMap.keys());
        for (const QString& key : helperMap.keys()) {
            QCOMPARE(helperMap.value(key).toString(), rendererMap.value(key).toUrl().toString());
        }
    }
}

void EntityContentAddressesTests::testMaterialData() {
    EntityItemProperties properties;
    properties.setMaterialData(
        "{\"materialVersion\":1,\"materials\":["
        "{\"name\":\"a\",\"albedoMap\":\"file:///Users/me/a.png\",\"normalMap\":\"https://cdn/n.png\","
        "\"roughness\":0.5,\"procedural\":{\"shaderUrl\":\"file:///Users/me/s.fs\","
        "\"channels\":[\"https://cdn/c.png\",\"/Users/me/c2.png\"]}},"
        "{\"name\":\"b\",\"albedoMap\":\"atp:/b.png\"}]}");
    QCOMPARE(clearThisComputer(properties), QStringList({ "materialData" }));
    QJsonObject resource = parse(properties.getMaterialData());
    QCOMPARE(resource.value("materialVersion").toInt(), 1);
    QJsonArray materials = resource.value("materials").toArray();
    QCOMPARE(materials.size(), 2);
    QJsonObject a = materials.at(0).toObject();
    QCOMPARE(a.value("albedoMap").toString(), QString());
    QCOMPARE(a.value("normalMap").toString(), QString("https://cdn/n.png"));
    QCOMPARE(a.value("roughness").toDouble(), 0.5);
    QJsonObject procedural = a.value("procedural").toObject();
    QCOMPARE(procedural.value("shaderUrl").toString(), QString());
    QJsonArray channels = procedural.value("channels").toArray();
    QCOMPARE(channels.at(0).toString(), QString("https://cdn/c.png"));
    QCOMPARE(channels.at(1).toString(), QString());
    QCOMPARE(materials.at(1).toObject().value("albedoMap").toString(), QString("atp:/b.png"));
    QVERIFY(properties.materialDataChanged());

    // a map naming a Web entity is no address and stays; a "{" that isn't an ID is still judged as an address
    properties.setMaterialData(
        "{\"materials\":{\"emissiveMap\":\"{01e142aa-848f-5b8c-9ce1-210f0f18f79b}\",\"albedoMap\":\"{not-an-id}\"}}");
    QCOMPARE(clearThisComputer(properties), QStringList({ "materialData" }));
    QJsonObject withID = parse(properties.getMaterialData()).value("materials").toObject();
    QCOMPARE(withID.value("emissiveMap").toString(), QString("{01e142aa-848f-5b8c-9ce1-210f0f18f79b}"));
    QCOMPARE(withID.value("albedoMap").toString(), QString());
    properties.setMaterialData("{\"materials\":{\"emissiveMap\":\"{01e142aa-848f-5b8c-9ce1-210f0f18f79b}\"}}");
    QCOMPARE(clearThisComputer(properties), QStringList());

    // every key the loader resolves against the material's address: each "...Map", lightMapDirection, the procedural
    // shader's addresses; a non-address key beside them stays
    const QStringList pictureKeys = { "emissiveMap", "albedoMap", "opacityMap", "roughnessMap", "glossMap",
                                      "metallicMap", "specularMap", "ormMap", "normalMap", "bumpMap", "occlusionMap",
                                      "scatteringMap", "lightMap", "lightMapDirection" };
    QJsonObject every;
    for (const QString& key : pictureKeys) {
        every.insert(key, "file:///Users/me/" + key + ".png");
    }
    every.insert("procedural", QJsonObject({ { "shaderUrl", "file:///Users/me/s.fs" },
                                             { "fragmentShaderURL", "/Users/me/f.fs" },
                                             { "vertexShaderURL", "C:/Users/me/v.vs" },
                                             { "channels", QJsonArray({ "file:///Users/me/c.png" }) } }));
    every.insert("roughness", 0.5);
    every.insert("name", "every");
    properties.setMaterialData(QString::fromUtf8(QJsonDocument(QJsonObject({ { "materials", every } })).toJson()));
    QCOMPARE(clearThisComputer(properties), QStringList({ "materialData" }));
    QJsonObject emptied = parse(properties.getMaterialData()).value("materials").toObject();
    for (const QString& key : pictureKeys) {
        QVERIFY2(emptied.value(key).isString() && emptied.value(key).toString().isEmpty(), qPrintable(key));
    }
    QJsonObject emptiedProcedural = emptied.value("procedural").toObject();
    for (const QString& key : { "shaderUrl", "fragmentShaderURL", "vertexShaderURL" }) {
        QVERIFY2(emptiedProcedural.value(key).toString().isEmpty(), qPrintable(key));
    }
    QCOMPARE(emptiedProcedural.value("channels").toArray().at(0).toString(), QString());
    QCOMPARE(emptied.value("roughness").toDouble(), 0.5);
    QCOMPARE(emptied.value("name").toString(), QString("every"));

    // one material, not a list
    properties.setMaterialData(
        "{\"materials\":{\"albedoMap\":\"C:/Users/me/a.png\",\"emissiveMap\":\"https://cdn/e.png\"}}");
    QCOMPARE(clearThisComputer(properties), QStringList({ "materialData" }));
    QJsonObject material = parse(properties.getMaterialData()).value("materials").toObject();
    QCOMPARE(material.value("albedoMap").toString(), QString());
    QCOMPARE(material.value("emissiveMap").toString(), QString("https://cdn/e.png"));

    properties.setMaterialData("not json");
    QVERIFY(clearThisComputer(properties).isEmpty());
    QCOMPARE(properties.getMaterialData(), QString("not json"));
}

void EntityContentAddressesTests::testImpostorEntry() {
    EntityItemProperties properties;
    properties.setUserData(
        "{\"grabbableKey\":{\"grabbable\":true},\"impostor\":{\"albedo\":\"file:///Users/me/a.png\","
        "\"normal\":\"file:///Users/me/n.png\",\"material\":\"\",\"frames\":\"cube6\",\"size\":256,\"radius\":1.5}}");
    QCOMPARE(clearThisComputer(properties), QStringList({ "userData.impostor" }));
    QJsonObject userData = parse(properties.getUserData());
    QVERIFY(!userData.contains("impostor"));
    QVERIFY(userData.value("grabbableKey").toObject().value("grabbable").toBool());
    QVERIFY(properties.userDataChanged());

    // one atlas on this computer is enough: the card can't draw without it
    properties.setUserData("{\"impostor\":{\"albedo\":\"https://cdn/a.png\",\"normal\":\"https://cdn/n.png\","
                           "\"material\":\"/Users/me/m.png\"}}");
    QCOMPARE(clearThisComputer(properties), QStringList({ "userData.impostor" }));
    QVERIFY(!parse(properties.getUserData()).contains("impostor"));

    properties.setUserData("{\"impostor\":{\"albedo\":\"https://cdn/a.png\",\"normal\":\"https://cdn/n.png\"}}");
    QVERIFY(clearThisComputer(properties).isEmpty());
    QVERIFY(parse(properties.getUserData()).contains("impostor"));

    properties.setUserData("an impostor, not JSON");
    QVERIFY(clearThisComputer(properties).isEmpty());
    QCOMPARE(properties.getUserData(), QString("an impostor, not JSON"));

    // the card readers look only at userData containing "impostor" (ImpostorCard.cpp:77, RenderableEntityItem.cpp:687),
    // so an entry spelled otherwise is no card and is skipped by both
    properties.setUserData("{\"Impostor\":{\"albedo\":\"file:///Users/me/a.png\",\"normal\":\"file:///n.png\"}}");
    QVERIFY(clearThisComputer(properties).isEmpty());
    QVERIFY(parse(properties.getUserData()).contains("Impostor"));
}

void EntityContentAddressesTests::testProceduralShaders() {
    EntityItemProperties properties;
    // under "ProceduralEntity"
    properties.setUserData("{\"grabbableKey\":{\"grabbable\":false},\"ProceduralEntity\":{\"version\":2,"
                           "\"shaderUrl\":\"file:///Users/me/s.fs\",\"vertexShaderURL\":\"https://cdn/v.vs\","
                           "\"channels\":[\"https://cdn/c.png\",\"/Users/me/c2.png\"],\"uniforms\":{\"speed\":1}}}");
    QCOMPARE(clearThisComputer(properties), QStringList({ "userData.procedural" }));
    QJsonObject userData = parse(properties.getUserData());
    QJsonObject procedural = userData.value("ProceduralEntity").toObject();
    QCOMPARE(procedural.value("shaderUrl").toString(), QString());
    QCOMPARE(procedural.value("vertexShaderURL").toString(), QString("https://cdn/v.vs"));
    QCOMPARE(procedural.value("channels").toArray().at(0).toString(), QString("https://cdn/c.png"));
    QCOMPARE(procedural.value("channels").toArray().at(1).toString(), QString());
    QCOMPARE(procedural.value("version").toInt(), 2);
    QCOMPARE(procedural.value("uniforms").toObject().value("speed").toInt(), 1);
    QVERIFY(!userData.value("grabbableKey").toObject().value("grabbable").toBool());

    // at the top level (getProceduralData takes the whole object when the key is absent)
    properties.setUserData("{\"fragmentShaderURL\":\"C:\\\\shaders\\\\f.fs\","
                           "\"channels\":[\"file:///Users/me/c.png\"]}");
    QCOMPARE(clearThisComputer(properties), QStringList({ "userData.procedural" }));
    userData = parse(properties.getUserData());
    QCOMPARE(userData.value("fragmentShaderURL").toString(), QString());
    QCOMPARE(userData.value("channels").toArray().at(0).toString(), QString());

    // fetched addresses stay, and so does userData that isn't JSON
    properties.setUserData("{\"shaderUrl\":\"https://cdn/s.fs\",\"channels\":[\"atp:/c.png\"]}");
    QVERIFY(clearThisComputer(properties).isEmpty());
    QCOMPARE(parse(properties.getUserData()).value("shaderUrl").toString(), QString("https://cdn/s.fs"));
    properties.setUserData("shaderUrl: file:///Users/me/s.fs, not JSON");
    QVERIFY(clearThisComputer(properties).isEmpty());
    QCOMPARE(properties.getUserData(), QString("shaderUrl: file:///Users/me/s.fs, not JSON"));

    // both kinds in one userData
    properties.setUserData("{\"impostor\":{\"albedo\":\"file:///Users/me/a.png\",\"normal\":\"https://cdn/n.png\"},"
                           "\"ProceduralEntity\":{\"shaderUrl\":\"/Users/me/s.fs\"}}");
    QCOMPARE(clearThisComputer(properties), QStringList({ "userData.impostor", "userData.procedural" }));
    userData = parse(properties.getUserData());
    QVERIFY(!userData.contains("impostor"));
    QCOMPARE(userData.value("ProceduralEntity").toObject().value("shaderUrl").toString(), QString());
}
