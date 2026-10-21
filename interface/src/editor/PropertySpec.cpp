//
//  PropertySpec.cpp
//  interface/src/editor
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "PropertySpec.h"

#include <cmath>
#include <functional>

#include <QtCore/QDateTime>
#include <QtCore/QHash>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QRect>
#include <QtGui/QColor>
#include <QtGui/QVector2D>
#include <QtGui/QVector3D>

#include <ColorUtils.h>
#include <GLMHelpers.h>
#include <PhysicsCollisionGroups.h>
#include <graphics/Material.h>

#include "JsonOrder.h"

namespace {
    using EP = EntityItemProperties;

    // a light's Shadows field: an entry of userData, and canCastShadow with it (see PropertySpecs::storedKeys)
    const char* const SHADOWS_KEY = "userData:shadows";

    struct Accessor {
        std::function<QVariant(const EP&)> get;
        std::function<void(EP&, const QVariant&)> set;
    };

    // --- conversions ----------------------------------------------------------------------------

    QColor toColor(const glm::u8vec3& c) { return QColor(c.r, c.g, c.b); }
    glm::u8vec3 fromColor(const QColor& c) { return glm::u8vec3(c.red(), c.green(), c.blue()); }
    QColor toColor(const glm::vec3& c) {
        return QColor(glm::clamp((int)std::lround(c.r), 0, 255), glm::clamp((int)std::lround(c.g), 0, 255),
                      glm::clamp((int)std::lround(c.b), 0, 255));
    }
    glm::vec3 fromFloatColor(const QColor& c) { return glm::vec3(c.red(), c.green(), c.blue()); }
    QVector3D toQ(const glm::vec3& v) { return QVector3D(v.x, v.y, v.z); }
    glm::vec3 fromQ(const QVector3D& v) { return glm::vec3(v.x(), v.y(), v.z()); }
    QVector2D toQ(const glm::vec2& v) { return QVector2D(v.x, v.y); }
    glm::vec2 fromQ(const QVector2D& v) { return glm::vec2(v.x(), v.y()); }
    QVector3D toEuler(const glm::quat& q) { return toQ(glm::degrees(safeEulerAngles(q))); }
    glm::quat fromEuler(const QVector3D& e) { return glm::quat(glm::radians(fromQ(e))); }

    // a material's colour as its JSON holds it: [r, g, b] or {"red": r, "green": g, "blue": b}, from 0 to 1, sRGB
    // unless an array's fourth element is false (as the material's parser reads it); `unset` when there is none
    QColor jsonColor(const QJsonValue& value, const QJsonValue& unset) {
        glm::vec3 color;
        bool sRGB = true;
        if (value.isArray() && value.toArray().size() >= 3) {
            QJsonArray array = value.toArray();
            color = glm::vec3(array[0].toDouble(), array[1].toDouble(), array[2].toDouble());
            sRGB = array.size() < 4 || !array[3].isBool() || array[3].toBool();
        } else if (value.isObject()) {
            QJsonObject object = value.toObject();
            auto component = [&object](std::initializer_list<const char*> names) {
                for (const char* name : names) {
                    if (object.contains(name)) {
                        return object.value(name).toDouble();
                    }
                }
                return 0.0;
            };
            color = glm::vec3(component({ "red", "r", "x" }), component({ "green", "g", "y" }), component({ "blue", "b", "z" }));
        } else {
            return unset.isUndefined() ? QColor() : jsonColor(unset, QJsonValue::Undefined);
        }
        if (!sRGB) {
            color = ColorUtils::tosRGBVec3(color);
        }
        color = glm::clamp(color, 0.0f, 1.0f);
        return QColor::fromRgbF(color.r, color.g, color.b);
    }

    // the key light's direction, as the old panel showed it: elevation and azimuth (Vec3.toPolar)
    QVector2D toPolar(const glm::vec3& v) {
        float radius = glm::length(v);
        if (radius < 1.0e-6f) {
            return QVector2D(0.0f, 0.0f);
        }
        glm::vec3 u = v / radius;
        return QVector2D(glm::degrees(std::asin(glm::clamp(-u.y, -1.0f, 1.0f))), glm::degrees(std::atan2(v.x, v.z)));
    }
    glm::vec3 fromPolar(const QVector2D& p) {
        float elevation = glm::radians(p.x());
        float azimuth = glm::radians(p.y());
        return glm::vec3(std::cos(elevation) * std::sin(azimuth), -std::sin(elevation), std::cos(elevation) * std::cos(azimuth));
    }

    // the collision mask as the comma list the script API uses: "static,dynamic,kinematic,myAvatar,otherAvatar"
    const QVector<QPair<QString, uint16_t>> COLLISION_GROUPS = {
        { "static", USER_COLLISION_GROUP_STATIC },
        { "dynamic", USER_COLLISION_GROUP_DYNAMIC },
        { "kinematic", USER_COLLISION_GROUP_KINEMATIC },
        { "myAvatar", USER_COLLISION_GROUP_MY_AVATAR },
        { "otherAvatar", USER_COLLISION_GROUP_OTHER_AVATAR }
    };
    QString maskToGroups(uint16_t mask) {
        QStringList groups;
        for (const auto& group : COLLISION_GROUPS) {
            if (mask & group.second) {
                groups << group.first;
            }
        }
        return groups.join(",");
    }
    uint16_t groupsToMask(const QString& text) {
        uint16_t mask = 0;
        QStringList groups = text.split(',', QString::SkipEmptyParts);
        for (const auto& group : COLLISION_GROUPS) {
            if (groups.contains(group.first)) {
                mask |= group.second;
            }
        }
        return mask;
    }

    // --- the accessors --------------------------------------------------------------------------

    QHash<QString, Accessor> buildAccessors() {
        QHash<QString, Accessor> a;
        auto add = [&](const char* key, std::function<QVariant(const EP&)> get, std::function<void(EP&, const QVariant&)> set) {
            a.insert(key, Accessor { get, set });
        };

#define P_BOOL(key, N)  add(key, [](const EP& p) { return QVariant(p.get##N()); }, [](EP& p, const QVariant& v) { p.set##N(v.toBool()); })
#define P_FLOAT(key, N) add(key, [](const EP& p) { return QVariant((double)p.get##N()); }, [](EP& p, const QVariant& v) { p.set##N((float)v.toDouble()); })
#define P_DEG(key, N)   add(key, [](const EP& p) { return QVariant((double)glm::degrees(p.get##N())); }, [](EP& p, const QVariant& v) { p.set##N(glm::radians((float)v.toDouble())); })
#define P_INT(key, N, T) add(key, [](const EP& p) { return QVariant((int)p.get##N()); }, [](EP& p, const QVariant& v) { p.set##N((T)v.toInt()); })
#define P_STR(key, N)   add(key, [](const EP& p) { return QVariant(p.get##N()); }, [](EP& p, const QVariant& v) { p.set##N(v.toString()); })
#define P_UUID(key, N)  add(key, [](const EP& p) { return QVariant(p.get##N().toString(QUuid::WithoutBraces)); }, [](EP& p, const QVariant& v) { p.set##N(QUuid(v.toString())); })
#define P_COLOR(key, N) add(key, [](const EP& p) { return QVariant(toColor(p.get##N())); }, [](EP& p, const QVariant& v) { p.set##N(fromColor(v.value<QColor>())); })
#define P_FCOLOR(key, N) add(key, [](const EP& p) { return QVariant(toColor(p.get##N())); }, [](EP& p, const QVariant& v) { p.set##N(fromFloatColor(v.value<QColor>())); })
#define P_VEC2(key, N)  add(key, [](const EP& p) { return QVariant(toQ(p.get##N())); }, [](EP& p, const QVariant& v) { p.set##N(fromQ(v.value<QVector2D>())); })
#define P_VEC3(key, N)  add(key, [](const EP& p) { return QVariant(toQ(p.get##N())); }, [](EP& p, const QVariant& v) { p.set##N(fromQ(v.value<QVector3D>())); })
#define P_EULER(key, N) add(key, [](const EP& p) { return QVariant(toEuler(p.get##N())); }, [](EP& p, const QVariant& v) { p.set##N(fromEuler(v.value<QVector3D>())); })
#define P_RECT(key, N)  add(key, [](const EP& p) { return QVariant(p.get##N()); }, [](EP& p, const QVariant& v) { p.set##N(v.toRect()); })
#define P_ENUM(key, N)  add(key, [](const EP& p) { return QVariant(p.get##N##AsString()); }, [](EP& p, const QVariant& v) { p.set##N##FromString(v.toString()); })
// members of a property group: G is the group's getter
#define G_BOOL(key, G, N)  add(key, [](const EP& p) { return QVariant(p.G().get##N()); }, [](EP& p, const QVariant& v) { p.G().set##N(v.toBool()); })
#define G_FLOAT(key, G, N) add(key, [](const EP& p) { return QVariant((double)p.G().get##N()); }, [](EP& p, const QVariant& v) { p.G().set##N((float)v.toDouble()); })
#define G_STR(key, G, N)   add(key, [](const EP& p) { return QVariant(p.G().get##N()); }, [](EP& p, const QVariant& v) { p.G().set##N(v.toString()); })
#define G_COLOR(key, G, N) add(key, [](const EP& p) { return QVariant(toColor(p.G().get##N())); }, [](EP& p, const QVariant& v) { p.G().set##N(fromColor(v.value<QColor>())); })

        // base
        P_STR("name", Name);
        P_STR("description", Description);
        P_UUID("parentID", ParentID);
        // the entity's own id, shown to be copied (for Parent, for scripts); never set
        add("id", [](const EP& p) { return QVariant(p.getID().toString(QUuid::WithoutBraces)); }, [](EP&, const QVariant&) {});
        P_INT("parentJointIndex", ParentJointIndex, quint16);
        P_BOOL("locked", Locked);
        P_BOOL("visible", Visible);
        P_ENUM("entityPriority", EntityPriority);
        P_ENUM("renderLayer", RenderLayer);
        P_ENUM("primitiveMode", PrimitiveMode);

        // spatial
        P_VEC3("position", Position);
        P_EULER("rotation", Rotation);
        P_VEC3("dimensions", Dimensions);
        // Scale is dimensions measured in the entity's own size; it is never set directly: the
        // editor turns an edit of it into a dimensions edit, as only it knows each entity's size
        add("scale", [](const EP& p) {
                glm::vec3 natural = PropertySpecs::naturalSize(p);
                if (natural == glm::vec3(0.0f)) {
                    return QVariant();   // a model that has not loaded yet
                }
                return QVariant(toQ(p.getDimensions() / natural));
            }, [](EP&, const QVariant&) {});
        add("type", [](const EP& p) { return QVariant(EntityTypes::getEntityTypeName(p.getType())); },
            [](EP&, const QVariant&) {});
        P_VEC3("localPosition", LocalPosition);
        P_EULER("localRotation", LocalRotation);
        P_VEC3("localDimensions", LocalDimensions);
        P_VEC3("registrationPoint", RegistrationPoint);

        // shape
        P_STR("shape", Shape);
        P_COLOR("color", Color);
        P_FLOAT("alpha", Alpha);

        // text
        P_STR("text", Text);
        P_COLOR("textColor", TextColor);
        P_FLOAT("textAlpha", TextAlpha);
        P_COLOR("backgroundColor", BackgroundColor);
        P_FLOAT("backgroundAlpha", BackgroundAlpha);
        P_FLOAT("lineHeight", LineHeight);
        P_STR("font", Font);
        P_ENUM("textEffect", TextEffect);
        P_COLOR("textEffectColor", TextEffectColor);
        P_FLOAT("textEffectThickness", TextEffectThickness);
        P_ENUM("billboardMode", BillboardMode);
        P_FLOAT("topMargin", TopMargin);
        P_FLOAT("rightMargin", RightMargin);
        P_FLOAT("bottomMargin", BottomMargin);
        P_FLOAT("leftMargin", LeftMargin);
        P_BOOL("unlit", Unlit);

        // zone
        P_ENUM("shapeType", ShapeType);
        P_STR("compoundShapeURL", CompoundShapeURL);
        P_BOOL("flyingAllowed", FlyingAllowed);
        P_BOOL("ghostingAllowed", GhostingAllowed);
        P_STR("filterURL", FilterURL);
        P_ENUM("keyLightMode", KeyLightMode);
        G_COLOR("keyLight.color", getKeyLight, Color);
        G_FLOAT("keyLight.intensity", getKeyLight, Intensity);
        add("keyLight.direction", [](const EP& p) { return QVariant(toPolar(p.getKeyLight().getDirection())); },
            [](EP& p, const QVariant& v) { p.getKeyLight().setDirection(fromPolar(v.value<QVector2D>())); });
        G_BOOL("keyLight.castShadows", getKeyLight, CastShadows);
        G_FLOAT("keyLight.shadowBias", getKeyLight, ShadowBias);
        G_FLOAT("keyLight.shadowMaxDistance", getKeyLight, ShadowMaxDistance);
        P_ENUM("skyboxMode", SkyboxMode);
        G_COLOR("skybox.color", getSkybox, Color);
        G_STR("skybox.url", getSkybox, URL);
        P_ENUM("ambientLightMode", AmbientLightMode);
        G_FLOAT("ambientLight.ambientIntensity", getAmbientLight, AmbientIntensity);
        G_STR("ambientLight.ambientURL", getAmbientLight, AmbientURL);
        P_ENUM("hazeMode", HazeMode);
        G_FLOAT("haze.hazeRange", getHaze, HazeRange);
        G_BOOL("haze.hazeAltitudeEffect", getHaze, HazeAltitudeEffect);
        G_FLOAT("haze.hazeBaseRef", getHaze, HazeBaseRef);
        G_FLOAT("haze.hazeCeiling", getHaze, HazeCeiling);
        G_COLOR("haze.hazeColor", getHaze, HazeColor);
        G_FLOAT("haze.hazeBackgroundBlend", getHaze, HazeBackgroundBlend);
        G_BOOL("haze.hazeEnableGlare", getHaze, HazeEnableGlare);
        G_COLOR("haze.hazeGlareColor", getHaze, HazeGlareColor);
        G_FLOAT("haze.hazeGlareAngle", getHaze, HazeGlareAngle);
        P_ENUM("bloomMode", BloomMode);
        G_FLOAT("bloom.bloomIntensity", getBloom, BloomIntensity);
        G_FLOAT("bloom.bloomThreshold", getBloom, BloomThreshold);
        G_FLOAT("bloom.bloomSize", getBloom, BloomSize);
        P_ENUM("zoneCullingMode", ZoneCullingMode);
        P_ENUM("toneMappingMode", ToneMappingMode);
        G_FLOAT("toneMapping.exposure", getToneMapping, Exposure);
        P_ENUM("avatarPriority", AvatarPriority);

        // model
        P_STR("modelURL", ModelURL);
        G_STR("animation.url", getAnimation, URL);
        G_BOOL("animation.running", getAnimation, Running);
        G_BOOL("animation.loop", getAnimation, Loop);
        G_BOOL("animation.allowTranslation", getAnimation, AllowTranslation);
        G_BOOL("animation.hold", getAnimation, Hold);
        G_FLOAT("animation.currentFrame", getAnimation, CurrentFrame);
        G_FLOAT("animation.firstFrame", getAnimation, FirstFrame);
        G_FLOAT("animation.lastFrame", getAnimation, LastFrame);
        G_FLOAT("animation.fps", getAnimation, FPS);
        P_STR("textures", Textures);
        P_BOOL("groupCulled", GroupCulled);

        // image
        P_STR("imageURL", ImageURL);
        P_BOOL("emissive", Emissive);
        P_RECT("subImage", SubImage);
        P_BOOL("keepAspectRatio", KeepAspectRatio);

        // web
        P_STR("sourceUrl", SourceUrl);
        P_INT("dpi", DPI, uint16_t);
        P_INT("maxFPS", MaxFPS, uint8_t);
        P_ENUM("inputMode", InputMode);
        P_BOOL("showKeyboardFocusHighlight", ShowKeyboardFocusHighlight);
        P_BOOL("transparentBackground", TransparentBackground);
        P_STR("scriptURL", ScriptURL);

        // light
        P_FLOAT("intensity", Intensity);
        P_FLOAT("falloffRadius", FalloffRadius);
        P_BOOL("isSpotlight", IsSpotlight);
        // a light's Type, as Unity has it, and its Range: how far it reaches, half its largest dimension
        // (setting all three is right for a spot light too: the engine shapes it from its length)
        add("lightType", [](const EP& p) { return QVariant(p.getIsSpotlight() ? "spot" : "point"); },
            [](EP& p, const QVariant& v) { p.setIsSpotlight(v.toString() == "spot"); });
        add("range", [](const EP& p) { return QVariant((double)glm::compMax(p.getDimensions()) * 0.5); },
            [](EP& p, const QVariant& v) { p.setDimensions(glm::vec3((float)v.toDouble() * 2.0f)); });
        P_FLOAT("exponent", Exponent);
        P_FLOAT("cutoff", Cutoff);

        // material
        // cleared, the URL goes back to the material's own data rather than to nothing applied
        add("materialURL", [](const EP& p) { return QVariant(p.getMaterialURL()); }, [](EP& p, const QVariant& v) {
            QString url = v.toString().trimmed();
            p.setMaterialURL(url.isEmpty() ? QStringLiteral("materialData") : url);
        });
        P_STR("materialData", MaterialData);
        P_STR("parentMaterialName", ParentMaterialName);
        P_INT("priority", Priority, quint16);
        P_ENUM("materialMappingMode", MaterialMappingMode);
        P_VEC2("materialMappingPos", MaterialMappingPos);
        P_VEC2("materialMappingScale", MaterialMappingScale);
        P_FLOAT("materialMappingRot", MaterialMappingRot);
        P_BOOL("materialRepeat", MaterialRepeat);

        // grid
        P_BOOL("followCamera", FollowCamera);
        P_INT("majorGridEvery", MajorGridEvery, uint32_t);
        P_FLOAT("minorGridEvery", MinorGridEvery);

        // terrain
        P_FLOAT("voxelSize", VoxelSize);
        P_STR("terrainURL", TerrainURL);
        P_INT("terrainVersion", TerrainVersion, uint32_t);
        P_STR("terrainLayers", TerrainLayers);

        // vehicle (PHYSICS.md 4.8): the group's definition, which the simulator rebuilds the vehicle from whenever it
        // changes, and the state the simulator writes (shown read-only)
#define G_VEC3(key, G, N)   add(key, [](const EP& p) { return QVariant(toQ(p.G().get##N())); }, [](EP& p, const QVariant& v) { p.G().set##N(fromQ(v.value<QVector3D>())); })
#define G_INT(key, G, N, T) add(key, [](const EP& p) { return QVariant((int)p.G().get##N()); }, [](EP& p, const QVariant& v) { p.G().set##N((T)v.toInt()); })
        G_STR("vehicle.type", getVehicle, Type);
        G_FLOAT("vehicle.mass", getVehicle, Mass);
        G_VEC3("vehicle.centerOfMass", getVehicle, CenterOfMass);
        G_STR("vehicle.wheels", getVehicle, Wheels);
        G_STR("vehicle.seats", getVehicle, Seats);
        G_FLOAT("vehicle.maxSteerAngle", getVehicle, MaxSteerAngle);
        G_FLOAT("vehicle.maxTorque", getVehicle, MaxTorque);
        G_FLOAT("vehicle.minRPM", getVehicle, MinRPM);
        G_FLOAT("vehicle.maxRPM", getVehicle, MaxRPM);
        G_STR("vehicle.gearRatios", getVehicle, GearRatios);
        G_FLOAT("vehicle.reverseGearRatio", getVehicle, ReverseGearRatio);
        G_FLOAT("vehicle.finalDriveRatio", getVehicle, FinalDriveRatio);
        G_FLOAT("vehicle.maxBrakeTorque", getVehicle, MaxBrakeTorque);
        G_FLOAT("vehicle.maxHandBrakeTorque", getVehicle, MaxHandBrakeTorque);
        G_STR("vehicle.driveType", getVehicle, DriveType);
        G_FLOAT("vehicle.suspensionFrequency", getVehicle, SuspensionFrequency);
        G_FLOAT("vehicle.suspensionDamping", getVehicle, SuspensionDamping);
        G_STR("vehicle.wheel", getVehicle, Wheel);
        G_STR("vehicle.axle", getVehicle, Axle);
        add("vehicle.driver", [](const EP& p) { return QVariant(p.getVehicle().getDriver().toString(QUuid::WithoutBraces)); },
            [](EP& p, const QVariant& v) { p.getVehicle().setDriver(QUuid(v.toString())); });
        G_INT("vehicle.inputSequence", getVehicle, InputSequence, quint32);
        G_FLOAT("vehicle.throttle", getVehicle, Throttle);
        G_FLOAT("vehicle.steering", getVehicle, Steering);
        G_FLOAT("vehicle.brake", getVehicle, Brake);
        G_FLOAT("vehicle.handBrake", getVehicle, HandBrake);
        G_FLOAT("vehicle.engineRPM", getVehicle, EngineRPM);
        G_INT("vehicle.gear", getVehicle, Gear, int);
        // the domain time of the state (microseconds since the epoch) as a clock time; the simulator's to write
        add("vehicle.stateTime", [](const EP& p) {
                quint64 time = p.getVehicle().getStateTime();
                return QVariant(time == 0 ? QString() : QDateTime::fromMSecsSinceEpoch((qint64)(time / 1000)).toString("HH:mm:ss.zzz"));
            }, [](EP&, const QVariant&) {});
        G_FLOAT("vehicle.envelopeVolume", getVehicle, EnvelopeVolume);
        G_VEC3("vehicle.centerOfBuoyancy", getVehicle, CenterOfBuoyancy);
        G_FLOAT("vehicle.gasDensity", getVehicle, GasDensity);
        G_FLOAT("vehicle.maxThrust", getVehicle, MaxThrust);
        G_FLOAT("vehicle.maxVerticalThrust", getVehicle, MaxVerticalThrust);
        G_VEC3("vehicle.thrustPoint", getVehicle, ThrustPoint);
        G_VEC3("vehicle.dragArea", getVehicle, DragArea);
        G_VEC3("vehicle.angularDragArea", getVehicle, AngularDragArea);
        G_FLOAT("vehicle.rudderArea", getVehicle, RudderArea);
        G_FLOAT("vehicle.elevatorArea", getVehicle, ElevatorArea);
        G_VEC3("vehicle.finPoint", getVehicle, FinPoint);
        G_FLOAT("vehicle.maxFinAngle", getVehicle, MaxFinAngle);
        G_FLOAT("vehicle.ballastRate", getVehicle, BallastRate);
        G_FLOAT("vehicle.ventRate", getVehicle, VentRate);
        G_FLOAT("vehicle.pitch", getVehicle, Pitch);
        G_FLOAT("vehicle.lift", getVehicle, Lift);
        G_FLOAT("vehicle.trim", getVehicle, Trim);
        G_FLOAT("vehicle.ballast", getVehicle, Ballast);
        G_FLOAT("vehicle.fill", getVehicle, Fill);
        G_FLOAT("vehicle.wingArea", getVehicle, WingArea);
        G_FLOAT("vehicle.wingSpan", getVehicle, WingSpan);
        G_VEC3("vehicle.wingPoint", getVehicle, WingPoint);
        G_FLOAT("vehicle.tilt", getVehicle, Tilt);

        // particles
        P_BOOL("isEmitting", IsEmitting);
        P_FLOAT("lifespan", Lifespan);
        P_INT("maxParticles", MaxParticles, quint32);
        P_FLOAT("emitRate", EmitRate);
        P_FLOAT("emitSpeed", EmitSpeed);
        P_FLOAT("speedSpread", SpeedSpread);
        P_VEC3("emitDimensions", EmitDimensions);
        P_FLOAT("emitRadiusStart", EmitRadiusStart);
        P_EULER("emitOrientation", EmitOrientation);
        P_BOOL("emitterShouldTrail", EmitterShouldTrail);
        P_FLOAT("radiusStart", RadiusStart);
        P_FLOAT("particleRadius", ParticleRadius);
        P_FLOAT("radiusFinish", RadiusFinish);
        P_FLOAT("radiusSpread", RadiusSpread);
        P_FCOLOR("colorStart", ColorStart);
        P_FCOLOR("colorFinish", ColorFinish);
        P_COLOR("colorSpread", ColorSpread);
        P_FLOAT("alphaStart", AlphaStart);
        P_FLOAT("alphaFinish", AlphaFinish);
        P_FLOAT("alphaSpread", AlphaSpread);
        P_VEC3("emitAcceleration", EmitAcceleration);
        P_VEC3("accelerationSpread", AccelerationSpread);
        P_DEG("spinStart", SpinStart);
        P_DEG("particleSpin", ParticleSpin);
        P_DEG("spinFinish", SpinFinish);
        P_DEG("spinSpread", SpinSpread);
        P_BOOL("rotateWithEntity", RotateWithEntity);
        P_DEG("polarStart", PolarStart);
        P_DEG("polarFinish", PolarFinish);
        P_DEG("azimuthStart", AzimuthStart);
        P_DEG("azimuthFinish", AzimuthFinish);

        // behavior
        G_BOOL("grab.grabbable", getGrab, Grabbable);
        G_BOOL("grab.triggerable", getGrab, Triggerable);
        G_BOOL("grab.grabFollowsController", getGrab, GrabFollowsController);
        P_BOOL("cloneable", Cloneable);
        P_FLOAT("cloneLifetime", CloneLifetime);
        P_FLOAT("cloneLimit", CloneLimit);
        P_BOOL("cloneDynamic", CloneDynamic);
        P_BOOL("cloneAvatarEntity", CloneAvatarEntity);
        P_BOOL("cloneGrabbable", CloneGrabbable);
        P_BOOL("canCastShadow", CanCastShadow);
        P_STR("href", Href);
        P_BOOL("ignorePickIntersection", IgnorePickIntersection);
        P_STR("script", Script);
        P_STR("serverScripts", ServerScripts);
        P_FLOAT("lifetime", Lifetime);
        P_STR("userData", UserData);

        // collision and physics
        P_BOOL("collisionless", Collisionless);
        add("collisionMask", [](const EP& p) { return QVariant(maskToGroups(p.getCollisionMask())); },
            [](EP& p, const QVariant& v) { p.setCollisionMask(groupsToMask(v.toString())); });
        P_STR("collisionSoundURL", CollisionSoundURL);
        P_BOOL("dynamic", Dynamic);
        // the velocity and angularVelocity properties are the entity's in its parent's frame (what the local ones
        // would be); EntityItem::getProperties leaves localVelocity and localAngularVelocity unfilled
        P_VEC3("velocity", Velocity);
        P_FLOAT("damping", Damping);
        add("angularVelocity", [](const EP& p) { return QVariant(toQ(glm::degrees(p.getAngularVelocity()))); },
            [](EP& p, const QVariant& v) { p.setAngularVelocity(glm::radians(fromQ(v.value<QVector3D>()))); });
        P_FLOAT("angularDamping", AngularDamping);
        P_FLOAT("restitution", Restitution);
        P_FLOAT("friction", Friction);
        P_FLOAT("density", Density);
        P_VEC3("gravity", Gravity);
        P_VEC3("acceleration", Acceleration);

#undef P_BOOL
#undef P_FLOAT
#undef P_DEG
#undef P_INT
#undef P_STR
#undef P_UUID
#undef P_COLOR
#undef P_FCOLOR
#undef P_VEC2
#undef P_VEC3
#undef P_EULER
#undef P_RECT
#undef P_ENUM
#undef G_BOOL
#undef G_FLOAT
#undef G_STR
#undef G_COLOR

        // entries of userData shown as fields of their own (see PropertySpecs::isUserDataEntry)
        auto userDataEntry = [](const EP& p, const QString& name) {
            return QJsonDocument::fromJson(p.getUserData().toUtf8()).object().value(name).toString();
        };
        // sets the entry, or removes it for an empty value; userData that isn't a set of entries (a script's own
        // text) is left alone
        auto setUserDataEntry = [](EP& p, const QString& name, const QString& value) {
            QString text = p.getUserData();
            QJsonDocument document = QJsonDocument::fromJson(text.toUtf8());
            if (!text.trimmed().isEmpty() && !document.isObject()) {
                return;
            }
            QJsonObject object = document.object();
            if (value.isEmpty()) {
                object.remove(name);
            } else {
                object.insert(name, value);
            }
            // in the order the entries had, a new one last (the User Data list keeps that order)
            p.setUserData(object.isEmpty() ? QString() : JsonOrder::write(object, JsonOrder::scan(text)));
        };
        for (const char* entry : { "cookie" }) {
            QString name = entry;
            a.insert(QStringLiteral("userData:") + name, Accessor {
                [=](const EP& p) { return QVariant(userDataEntry(p, name)); },
                [=](EP& p, const QVariant& v) { setUserDataEntry(p, name, v.toString()); }
            });
        }
        // a shape's Mirror: the entry "mirror", "picture" (a picture of the reflection mapped onto the face; true
        // and "true" mean the same) or "screen" (the reflection drawn pixel for pixel where the face is on screen);
        // Off removes the entry (Mirrors.h)
        a.insert(QStringLiteral("userData:mirror"), Accessor {
            [=](const EP& p) {
                QJsonValue entry = QJsonDocument::fromJson(p.getUserData().toUtf8()).object().value("mirror");
                QString text = entry.toBool() ? QStringLiteral("picture") : entry.toString().toLower();
                return QVariant(text == "screen" ? "screen" : (text == "picture" || text == "true") ? "picture" : "off");
            },
            [=](EP& p, const QVariant& v) {
                QString mode = v.toString();
                setUserDataEntry(p, "mirror", mode == "off" ? QString() : mode);
            }
        });
        // a shape's reflection probe (LIGHTMAPS.md §7), the entry "reflectionProbe": { "url", "boxOffset": [x, y, z],
        // "intensity", "importance", "blendDistance", "boxProjection" }; the Reflection Probe field makes or removes
        // the entry, the others are its fields
        auto probeEntry = [](const EP& p) {
            return QJsonDocument::fromJson(p.getUserData().toUtf8()).object().value("reflectionProbe");
        };
        auto setProbeEntry = [](EP& p, const QJsonValue& probe) {
            QString text = p.getUserData();
            QJsonDocument document = QJsonDocument::fromJson(text.toUtf8());
            if (!text.trimmed().isEmpty() && !document.isObject()) {
                return;
            }
            QJsonObject object = document.object();
            if (probe.isUndefined() || probe.isNull()) {
                object.remove("reflectionProbe");
            } else {
                object.insert("reflectionProbe", probe);
            }
            p.setUserData(object.isEmpty() ? QString() : JsonOrder::write(object, JsonOrder::scan(text)));
        };
        auto probeObject = [=](const EP& p) {
            QJsonValue entry = probeEntry(p);
            return entry.isObject() ? entry.toObject() : QJsonObject();
        };
        add("userData:reflectionProbe",
            [=](const EP& p) { QJsonValue entry = probeEntry(p); return QVariant(entry.isObject() || entry.toBool()); },
            [=](EP& p, const QVariant& v) {
                if (!v.toBool()) {
                    setProbeEntry(p, QJsonValue());
                } else if (!probeEntry(p).isObject()) {
                    setProbeEntry(p, QJsonObject { { "intensity", 1.0 }, { "blendDistance", 1.0 }, { "boxProjection", true } });
                }
            });
        add("userData:reflectionProbe:url",
            [=](const EP& p) { return QVariant(probeObject(p).value("url").toString()); },
            [=](EP& p, const QVariant& v) {
                QJsonObject probe = probeObject(p);
                QString url = v.toString().trimmed();
                if (url.isEmpty()) {
                    probe.remove("url");
                } else {
                    probe.insert("url", url);
                }
                setProbeEntry(p, probe);
            });
        add("userData:reflectionProbe:intensity",
            [=](const EP& p) { return QVariant(probeObject(p).value("intensity").toDouble(1.0)); },
            [=](EP& p, const QVariant& v) {
                QJsonObject probe = probeObject(p);
                probe.insert("intensity", std::round(v.toDouble() * 10000.0) / 10000.0);
                setProbeEntry(p, probe);
            });
        add("userData:reflectionProbe:importance",
            [=](const EP& p) { return QVariant(probeObject(p).value("importance").toInt(0)); },
            [=](EP& p, const QVariant& v) {
                QJsonObject probe = probeObject(p);
                probe.insert("importance", v.toInt());
                setProbeEntry(p, probe);
            });
        add("userData:reflectionProbe:blendDistance",
            [=](const EP& p) { return QVariant(probeObject(p).value("blendDistance").toDouble(1.0)); },
            [=](EP& p, const QVariant& v) {
                QJsonObject probe = probeObject(p);
                probe.insert("blendDistance", std::round(std::max(0.0, v.toDouble()) * 10000.0) / 10000.0);
                setProbeEntry(p, probe);
            });
        add("userData:reflectionProbe:boxProjection",
            [=](const EP& p) { return QVariant(probeObject(p).value("boxProjection").toBool(true)); },
            [=](EP& p, const QVariant& v) {
                QJsonObject probe = probeObject(p);
                probe.insert("boxProjection", v.toBool());
                setProbeEntry(p, probe);
            });
        add("userData:reflectionProbe:boxOffset",
            [=](const EP& p) {
                QJsonArray offset = probeObject(p).value("boxOffset").toArray();
                return QVariant(toQ(offset.size() == 3 ? glm::vec3(offset[0].toDouble(), offset[1].toDouble(), offset[2].toDouble()) : glm::vec3(0.0f)));
            },
            [=](EP& p, const QVariant& v) {
                QJsonObject probe = probeObject(p);
                glm::vec3 offset = fromQ(v.value<QVector3D>());
                auto rounded = [](float x) { return std::round(x * 10000.0) / 10000.0; };
                if (offset == glm::vec3(0.0f)) {
                    probe.remove("boxOffset");
                } else {
                    probe.insert("boxOffset", QJsonArray { rounded(offset.x), rounded(offset.y), rounded(offset.z) });
                }
                setProbeEntry(p, probe);
            });
        // a zone's probe volume (LIGHTMAPS.md step 4), the entry "probeVolume": { "url", "intensity" }; clearing the
        // file removes the entry
        auto volumeObject = [](const EP& p) {
            QJsonValue entry = QJsonDocument::fromJson(p.getUserData().toUtf8()).object().value("probeVolume");
            return entry.isObject() ? entry.toObject() : QJsonObject();
        };
        auto setVolumeEntry = [](EP& p, const QJsonObject& volume) {
            QString text = p.getUserData();
            QJsonDocument document = QJsonDocument::fromJson(text.toUtf8());
            if (!text.trimmed().isEmpty() && !document.isObject()) {
                return;
            }
            QJsonObject object = document.object();
            if (volume.value("url").toString().isEmpty()) {
                object.remove("probeVolume");
            } else {
                object.insert("probeVolume", volume);
            }
            p.setUserData(object.isEmpty() ? QString() : JsonOrder::write(object, JsonOrder::scan(text)));
        };
        add("userData:probeVolume:url",
            [=](const EP& p) { return QVariant(volumeObject(p).value("url").toString()); },
            [=](EP& p, const QVariant& v) {
                QJsonObject volume = volumeObject(p);
                QString url = v.toString().trimmed();
                if (url.isEmpty()) {
                    volume.remove("url");
                } else {
                    volume.insert("url", url);
                }
                setVolumeEntry(p, volume);
            });
        add("userData:probeVolume:intensity",
            [=](const EP& p) { return QVariant(volumeObject(p).value("intensity").toDouble(1.0)); },
            [=](EP& p, const QVariant& v) {
                QJsonObject volume = volumeObject(p);
                volume.insert("intensity", std::round(v.toDouble() * 10000.0) / 10000.0);
                setVolumeEntry(p, volume);
            });
        // an entity's impostor card (IMPOSTORS.md), the entry "impostor" the baker writes, as one value: its JSON,
        // shown by the Impostor section's own editor; an empty value takes the entry off (Remove), nothing else is
        // written through here (the bake writes the entry itself)
        add("userData:impostor",
            [](const EP& p) {
                QJsonValue entry = QJsonDocument::fromJson(p.getUserData().toUtf8()).object().value("impostor");
                return QVariant(entry.isObject() ? QString::fromUtf8(QJsonDocument(entry.toObject()).toJson(QJsonDocument::Compact)) : QString());
            },
            [](EP& p, const QVariant& v) {
                QString text = p.getUserData();
                QJsonDocument document = QJsonDocument::fromJson(text.toUtf8());
                if (!text.trimmed().isEmpty() && !document.isObject()) {
                    return;
                }
                QJsonObject object = document.object();
                QJsonDocument entry = QJsonDocument::fromJson(v.toString().toUtf8());
                if (entry.isObject() && !entry.object().isEmpty()) {
                    object.insert("impostor", entry.object());
                } else {
                    object.remove("impostor");
                }
                p.setUserData(object.isEmpty() ? QString() : JsonOrder::write(object, JsonOrder::scan(text)));
            });
        // a model's lightmap, the entry "lightmap" (LIGHTMAPS.md §3.2): { "url", "scaleOffset": [sx, sy, ox, oy],
        // "mode": "indirect" | "full", "params": [offset, scale] }, shown as the Lightmap section's fields; clearing
        // the picture removes the entry
        auto lightmapEntry = [](const EP& p) {
            return QJsonDocument::fromJson(p.getUserData().toUtf8()).object().value("lightmap").toObject();
        };
        auto setLightmapEntry = [](EP& p, const QJsonObject& lightmap) {
            QString text = p.getUserData();
            QJsonDocument document = QJsonDocument::fromJson(text.toUtf8());
            if (!text.trimmed().isEmpty() && !document.isObject()) {
                return;
            }
            QJsonObject object = document.object();
            if (lightmap.isEmpty()) {
                object.remove("lightmap");
            } else {
                object.insert("lightmap", lightmap);
            }
            p.setUserData(object.isEmpty() ? QString() : JsonOrder::write(object, JsonOrder::scan(text)));
        };
        auto lightmapScaleOffset = [](const QJsonObject& lightmap) {
            QJsonArray array = lightmap.value("scaleOffset").toArray();
            return array.size() == 4 ? glm::vec4(array[0].toDouble(), array[1].toDouble(), array[2].toDouble(), array[3].toDouble())
                                     : glm::vec4(1.0f, 1.0f, 0.0f, 0.0f);
        };
        auto setLightmapScaleOffset = [=](EP& p, const glm::vec4& scaleOffset) {
            QJsonObject lightmap = lightmapEntry(p);
            auto number = [](double c) { return std::round(c * 10000.0) / 10000.0; };
            lightmap.insert("scaleOffset", QJsonArray { number(scaleOffset.x), number(scaleOffset.y), number(scaleOffset.z), number(scaleOffset.w) });
            setLightmapEntry(p, lightmap);
        };
        add("userData:lightmap:url",
            [=](const EP& p) { return QVariant(lightmapEntry(p).value("url").toString()); },
            [=](EP& p, const QVariant& v) {
                QJsonObject lightmap = lightmapEntry(p);
                QString url = v.toString();
                if (url.isEmpty()) {
                    setLightmapEntry(p, QJsonObject());
                } else {
                    lightmap.insert("url", url);
                    setLightmapEntry(p, lightmap);
                }
            });
        add("userData:lightmap:scale",
            [=](const EP& p) { glm::vec4 so = lightmapScaleOffset(lightmapEntry(p)); return QVariant(toQ(glm::vec2(so.x, so.y))); },
            [=](EP& p, const QVariant& v) {
                glm::vec4 so = lightmapScaleOffset(lightmapEntry(p));
                glm::vec2 scale = fromQ(v.value<QVector2D>());
                setLightmapScaleOffset(p, glm::vec4(scale.x, scale.y, so.z, so.w));
            });
        add("userData:lightmap:offset",
            [=](const EP& p) { glm::vec4 so = lightmapScaleOffset(lightmapEntry(p)); return QVariant(toQ(glm::vec2(so.z, so.w))); },
            [=](EP& p, const QVariant& v) {
                glm::vec4 so = lightmapScaleOffset(lightmapEntry(p));
                glm::vec2 offset = fromQ(v.value<QVector2D>());
                setLightmapScaleOffset(p, glm::vec4(so.x, so.y, offset.x, offset.y));
            });
        add("userData:lightmap:directionalUrl",
            [=](const EP& p) { return QVariant(lightmapEntry(p).value("directionalUrl").toString()); },
            [=](EP& p, const QVariant& v) {
                QJsonObject lightmap = lightmapEntry(p);
                QString url = v.toString();
                if (url.isEmpty()) {
                    lightmap.remove("directionalUrl");
                } else {
                    lightmap.insert("directionalUrl", url);
                }
                setLightmapEntry(p, lightmap);
            });
        add("userData:lightmap:mode",
            [=](const EP& p) { return QVariant(lightmapEntry(p).value("mode").toString() == "full" ? "full" : "indirect"); },
            [=](EP& p, const QVariant& v) {
                QJsonObject lightmap = lightmapEntry(p);
                if (v.toString() == "full") {
                    lightmap.insert("mode", "full");
                } else {
                    lightmap.remove("mode");
                }
                setLightmapEntry(p, lightmap);
            });
        // the params' scale (light = offset + scale * map); the offset stays in the JSON
        add("userData:lightmap:brightness",
            [=](const EP& p) {
                QJsonArray params = lightmapEntry(p).value("params").toArray();
                return QVariant(params.size() == 2 && params[1].isDouble() ? params[1].toDouble() : 1.0);
            },
            [=](EP& p, const QVariant& v) {
                QJsonObject lightmap = lightmapEntry(p);
                QJsonArray params = lightmap.value("params").toArray();
                double offset = params.size() == 2 && params[0].isDouble() ? params[0].toDouble() : 0.0;
                lightmap.insert("params", QJsonArray { offset, std::round(v.toDouble() * 10000.0) / 10000.0 });
                setLightmapEntry(p, lightmap);
            });
        // a light's Shadows: the entry "shadows", "on" or "important" (Important goes before the other lights when
        // more cast shadows than there are maps); without it the light casts none. Off also turns Cast Shadows off,
        // which is all that clients from before 2026-10-21 read, so that they leave it without shadows too
        a.insert(SHADOWS_KEY, Accessor {
            [=](const EP& p) {
                QString mode = userDataEntry(p, "shadows");
                if (!p.getCanCastShadow() || (mode != "on" && mode != "important")) {
                    return QVariant("off");
                }
                return QVariant(mode);
            },
            [=](EP& p, const QVariant& v) {
                QString mode = v.toString();
                p.setCanCastShadow(mode != "off");
                setUserDataEntry(p, "shadows", mode == "off" ? QString() : mode);
            }
        });

        // entries of materialData shown as fields of their own, as Unity's material Inspector shows a shader's
        // properties. A material entity whose URL is "materialData" uses the first of its materials ("materials" is one
        // material or an array of them), so an entry is read from that one and written to it; Packing, which says how
        // the set's pictures are laid out, is written to every material. An entry the JSON doesn't hold shows the
        // value the material has without it. A choice or a picture set back to that is left out of the JSON; numbers
        // and colours stay as set, so that a slider passing the default doesn't make its entry come and go.
        auto firstMaterial = [](const EP& p) -> QJsonObject {
            QJsonValue materials = QJsonDocument::fromJson(p.getMaterialData().toUtf8()).object().value("materials");
            if (materials.isArray()) {
                QJsonArray array = materials.toArray();
                return array.isEmpty() ? QJsonObject() : array.first().toObject();
            }
            return materials.toObject();
        };
        // changes the first material, or every one, by `edit`; data that isn't a set of materials is left alone
        auto editMaterials = [](EP& p, bool everyMaterial, const std::function<void(QJsonObject&)>& edit) {
            QString text = p.getMaterialData();
            QJsonObject document = QJsonDocument::fromJson(text.toUtf8()).object();
            QJsonValue materials = document.value("materials");
            if (!text.trimmed().isEmpty() && !materials.isObject() && !materials.isArray()) {
                return;
            }
            if (materials.isArray()) {
                QJsonArray array = materials.toArray();
                if (array.isEmpty()) {
                    array.append(QJsonObject());
                }
                for (int i = 0; i < (everyMaterial ? array.size() : 1); ++i) {
                    QJsonObject material = array[i].toObject();
                    edit(material);
                    array[i] = material;
                }
                document.insert("materials", array);
            } else {
                QJsonObject material = materials.toObject();
                edit(material);
                document.insert("materials", material);
            }
            p.setMaterialData(QString::fromUtf8(QJsonDocument(document).toJson(QJsonDocument::Compact)));
        };

        enum class Entry { Text, Number, Bool, Color };
        struct MaterialEntry {
            const char* name;
            Entry type;
            QJsonValue unset;       // the material's value without the entry
            bool everyMaterial;
            const char* map { nullptr };    // a picture that changes what the material has without the entry, to:
            QJsonValue unsetWithMap {};
        };
        const QVector<MaterialEntry> materialEntries = {
            { "packing", Entry::Text, "custom", true },
            { "cullFaceMode", Entry::Text, "CULL_BACK", false },
            { "unlit", Entry::Bool, false, false },
            { "albedoMap", Entry::Text, "", false },
            { "albedo", Entry::Color, QJsonArray { 1.0, 1.0, 1.0 }, false },    // no albedo: the Base Map as it is
            { "metallicMap", Entry::Text, "", false },
            { "ormMap", Entry::Text, "", false },
            { "metallic", Entry::Number, graphics::Material::DEFAULT_METALLIC, false },
            { "smoothness", Entry::Number, 0.5, false },    // URP's default, as the material's parser has it
            { "smoothnessSource", Entry::Text, "metallicAlpha", false },
            { "roughnessMap", Entry::Text, "", false },
            { "roughness", Entry::Number, graphics::Material::DEFAULT_ROUGHNESS, false },
            { "normalMap", Entry::Text, "", false },
            { "occlusionMap", Entry::Text, "", false },
            { "occlusionStrength", Entry::Number, graphics::Material::DEFAULT_OCCLUSION_STRENGTH, false },
            { "emissiveMap", Entry::Text, "", false },
            // no emission, or with an Emission Map, the map as it is
            { "emissive", Entry::Color, QJsonArray { 0.0, 0.0, 0.0 }, false, "emissiveMap", QJsonArray { 1.0, 1.0, 1.0 } },
            { "lightMap", Entry::Text, "", false },
            { "lightMapDirection", Entry::Text, "", false },
            { "lightmapMode", Entry::Text, "indirect", false },
            { "opacity", Entry::Number, graphics::Material::DEFAULT_OPACITY, false },
            { "opacityCutoff", Entry::Number, graphics::Material::DEFAULT_OPACITY_CUTOFF, false }
        };
        for (const MaterialEntry& entry : materialEntries) {
            QString name = entry.name;
            Entry type = entry.type;
            QJsonValue unset = entry.unset;
            QString map = entry.map;
            QJsonValue unsetWithMap = entry.unsetWithMap;
            bool everyMaterial = entry.everyMaterial;
            a.insert(QStringLiteral("materialData:") + name, Accessor {
                [=](const EP& p) {
                    QJsonObject first = firstMaterial(p);
                    QJsonValue value = first.value(name);
                    QJsonValue without = (!map.isEmpty() && !first.value(map).toString().isEmpty()) ? unsetWithMap : unset;
                    switch (type) {
                        case Entry::Text:
                            return QVariant(value.toString(without.toString()));
                        case Entry::Number:
                            return QVariant(value.isDouble() ? value.toDouble() : without.toDouble());
                        case Entry::Bool:
                            return QVariant(value.isBool() ? value.toBool() : without.toBool());
                        case Entry::Color:
                            return QVariant(jsonColor(value, without));
                    }
                    return QVariant();
                },
                [=](EP& p, const QVariant& v) {
                    editMaterials(p, everyMaterial, [&](QJsonObject& material) {
                        switch (type) {
                            case Entry::Text: {
                                QString value = v.toString();
                                if (value.isEmpty() || value == unset.toString()) {
                                    material.remove(name);
                                } else {
                                    material.insert(name, value);
                                }
                                break;
                            }
                            case Entry::Number:
                                material.insert(name, std::round(v.toDouble() * 10000.0) / 10000.0);
                                break;
                            case Entry::Bool:
                                if (v.toBool() == unset.toBool()) {
                                    material.remove(name);
                                } else {
                                    material.insert(name, v.toBool());
                                }
                                break;
                            case Entry::Color: {
                                QColor color = v.value<QColor>();
                                auto channel = [](double c) { return std::round(c * 1000.0) / 1000.0; };
                                material.insert(name, QJsonArray { channel(color.redF()), channel(color.greenF()),
                                                                   channel(color.blueF()) });
                                break;
                            }
                        }
                    });
                }
            });
        }

        // Unity's Surface Type and Alpha Clipping: two fields over one entry, opacityMapMode. Transparent blends the
        // Base Map's alpha, Alpha Clipping cuts the surface out where the alpha is below the Threshold (opacityCutoff),
        // and together they cut out and then blend what is left. Without the entry the material is opaque, unless
        // "opacityMap" names the Base Map, which lets the picture decide: Opaque is then written out so that it holds.
        using Mode = graphics::MaterialKey::OpacityMapMode;
        auto modeOf = [=](const EP& p) {
            Mode mode = graphics::Material::DEFAULT_OPACITY_MAP_MODE;
            Mode named;
            if (graphics::MaterialKey::getOpacityMapModeFromName(
                    firstMaterial(p).value("opacityMapMode").toString().toStdString(), named)) {
                mode = named;
            }
            return mode;
        };
        auto isTransparent = [](Mode mode) { return mode == Mode::OPACITY_MAP_BLEND || mode == Mode::OPACITY_MAP_MASK_BLEND; };
        auto isClipping = [](Mode mode) { return mode == Mode::OPACITY_MAP_MASK || mode == Mode::OPACITY_MAP_MASK_BLEND; };
        auto setMode = [=](EP& p, bool transparent, bool clipping) {
            Mode mode = clipping ? (transparent ? Mode::OPACITY_MAP_MASK_BLEND : Mode::OPACITY_MAP_MASK)
                                 : (transparent ? Mode::OPACITY_MAP_BLEND : Mode::OPACITY_MAP_OPAQUE);
            editMaterials(p, false, [&](QJsonObject& material) {
                if (mode == Mode::OPACITY_MAP_OPAQUE && material.value("opacityMap").toString().isEmpty()) {
                    material.remove("opacityMapMode");
                } else {
                    material.insert("opacityMapMode", QString::fromStdString(graphics::MaterialKey::getOpacityMapModeName(mode)));
                }
            });
        };
        add("materialData:surfaceType",
            [=](const EP& p) { return QVariant(isTransparent(modeOf(p)) ? "transparent" : "opaque"); },
            [=](EP& p, const QVariant& v) { setMode(p, v.toString() == "transparent", isClipping(modeOf(p))); });
        add("materialData:alphaClipping",
            [=](const EP& p) { return QVariant(isClipping(modeOf(p))); },
            [=](EP& p, const QVariant& v) { setMode(p, isTransparent(modeOf(p)), v.toBool()); });
        // the light map's scale, lightmapParams[1] (light = offset + scale * map); the offset stays in the JSON
        add("materialData:lightmapScale",
            [=](const EP& p) {
                QJsonArray params = firstMaterial(p).value("lightmapParams").toArray();
                return QVariant(params.size() == 2 && params[1].isDouble() ? params[1].toDouble() : 1.0);
            },
            [=](EP& p, const QVariant& v) {
                editMaterials(p, false, [&](QJsonObject& material) {
                    QJsonArray params = material.value("lightmapParams").toArray();
                    double offset = params.size() == 2 && params[0].isDouble() ? params[0].toDouble() : 0.0;
                    material.insert("lightmapParams", QJsonArray { offset, std::round(v.toDouble() * 10000.0) / 10000.0 });
                });
            });
        return a;
    }

    const QHash<QString, Accessor>& accessors() {
        static const QHash<QString, Accessor> table = buildAccessors();
        return table;
    }

    // --- the table ------------------------------------------------------------------------------

    const QStringList MODE_VALUES = { "inherit:Inherit", "disabled:Off", "enabled:On" };
    const QStringList BILLBOARD_VALUES = { "none:None", "yaw:Yaw", "full:Full" };

    PropertySpec spec(const char* key, const char* label, PropertyKind kind, const char* section) {
        PropertySpec s;
        s.key = key;
        s.label = label;
        s.kind = kind;
        s.section = section;
        if (kind == PropertyKind::Int) {
            s.step = 1;
            s.decimals = 0;
        }
        return s;
    }
    PropertySpec& range(PropertySpec& s, double min, double max, double step, int decimals, const char* unit = "") {
        s.min = min;
        s.max = max;
        s.step = step;
        s.decimals = decimals;
        s.unit = unit;
        return s;
    }
    PropertySpec& stepping(PropertySpec& s, double step, int decimals, const char* unit = "") {
        s.step = step;
        s.decimals = decimals;
        s.unit = unit;
        return s;
    }

    QVector<PropertySpec> buildTable() {
        QVector<PropertySpec> t;
        auto add = [&](PropertySpec s) -> PropertySpec& { t.append(s); return t.last(); };
        auto en = [&](const char* key, const char* label, const char* section, const QStringList& values, const char* showWhen = "") {
            PropertySpec s = spec(key, label, PropertyKind::Enum, section);
            s.enumValues = values;
            s.showWhen = showWhen;
            return add(s);
        };
        auto with = [](PropertySpec& s, const char* showWhen) -> PropertySpec& { s.showWhen = showWhen; return s; };
        using K = PropertyKind;

        // base
        add(spec("name", "Name", K::String, "base"));
        add(spec("id", "ID", K::String, "base")).readOnly = true;   // to copy: for Parent, for scripts
        add(spec("description", "Description", K::String, "base"));
        add(spec("parentID", "Parent", K::String, "base"));
        add(spec("parentJointIndex", "Parent Joint", K::Int, "base"));
        add(spec("visible", "Visible", K::Bool, "base"));
        add(spec("locked", "Locked", K::Bool, "base"));
        en("entityPriority", "Priority", "base", { "automatic:Automatic", "prioritized:Prioritized", "static:Static" });
        en("renderLayer", "Render Layer", "base", { "world:World", "front:Front", "hud:HUD" });
        en("primitiveMode", "Primitive Mode", "base", { "solid:Solid", "lines:Wireframe" });

        // transform: metres and degrees go without saying here, as in Unity (no unit on the labels)
        stepping(add(spec("position", "Position", K::Vec3, "spatial")), 0.1, 3);
        stepping(add(spec("rotation", "Rotation", K::Euler, "spatial")), 1.0, 2);
        // Unity's order: Position, Rotation, Scale; then the size in metres it amounts to
        with(range(add(spec("scale", "Scale", K::Vec3, "spatial")), 0.0001, DBL_MAX, 0.01, 3), "type not in Light");
        with(range(add(spec("dimensions", "Dimensions", K::Vec3, "spatial")), 0.001, DBL_MAX, 0.01, 3), "type not in Light");
        with(stepping(add(spec("localPosition", "Local Position", K::Vec3, "spatial")), 0.1, 3), "parentID!=");
        with(stepping(add(spec("localRotation", "Local Rotation", K::Euler, "spatial")), 1.0, 2), "parentID!=");
        range(add(spec("registrationPoint", "Pivot", K::Vec3, "spatial")), 0, 1, 0.01, 3);

        // shape; one that is not drawn (an Empty, a parent with nothing of its own to show) has no shape to show
        // fields for: Visible brings them
        en("shape", "Shape", "shape", { "Cube:Box", "Sphere:Sphere", "Tetrahedron:Tetrahedron", "Octahedron:Octahedron",
            "Icosahedron:Icosahedron", "Dodecahedron:Dodecahedron", "Hexagon:Hexagon", "Triangle:Triangle", "Octagon:Octagon",
            "Cylinder:Cylinder", "Cone:Cone", "Circle:Circle", "Quad:Quad" }, "visible=true");
        with(add(spec("color", "Color", K::Color, "shape")), "visible=true");
        with(range(add(spec("alpha", "Alpha", K::Float, "shape")), 0, 1, 0.01, 2), "visible=true");
        // the shape's +Z face shows what the viewer would see reflected in it (a few mirrors a frame, the biggest
        // on screen first); its colour shows while it waits for a turn. Picture maps a picture of the reflection
        // onto the face; Screen draws the reflection pixel for pixel where the face is on screen
        en("userData:mirror", "Mirror", "shape", { "off:Off", "picture:Picture", "screen:Screen" });

        // a reflection probe (LIGHTMAPS.md §7): the shape is the box inside which surfaces reflect the probe's cube
        // map (a 1:6 strip, as the zone's ambient map) instead of the zone's, the probe at the shape's position
        // (Acts as Probe: the shape is a probe, which is not the same as the shape being lit by one);
        // Importance orders probes that overlap, Blend Distance fades the probe in from the box's faces, Box
        // Projection reflects the box's faces from the right place (off: the cube map at infinity), Box Offset
        // moves the box from the probe
        add(spec("userData:reflectionProbe", "Acts as Probe", K::Bool, "reflectionProbe"));
        with(add(spec("userData:reflectionProbe:url", "Cube Map", K::Image, "reflectionProbe")), "userData:reflectionProbe=true");
        with(range(add(spec("userData:reflectionProbe:intensity", "Intensity", K::Float, "reflectionProbe")), 0, 16, 0.01, 2), "userData:reflectionProbe=true");
        with(stepping(add(spec("userData:reflectionProbe:importance", "Importance", K::Int, "reflectionProbe")), 1, 0), "userData:reflectionProbe=true");
        with(range(add(spec("userData:reflectionProbe:blendDistance", "Blend Distance", K::Float, "reflectionProbe")), 0, 100, 0.1, 2, "m"), "userData:reflectionProbe=true");
        with(add(spec("userData:reflectionProbe:boxProjection", "Box Projection", K::Bool, "reflectionProbe")), "userData:reflectionProbe=true");
        with(stepping(add(spec("userData:reflectionProbe:boxOffset", "Box Offset", K::Vec3, "reflectionProbe")), 0.1, 2, "m"), "userData:reflectionProbe=true");

        // text
        add(spec("text", "Text", K::String, "text"));
        add(spec("textColor", "Text Color", K::Color, "text"));
        range(add(spec("textAlpha", "Text Alpha", K::Float, "text")), 0, 1, 0.01, 2);
        add(spec("backgroundColor", "Background", K::Color, "text"));
        range(add(spec("backgroundAlpha", "Background Alpha", K::Float, "text")), 0, 1, 0.01, 2);
        range(add(spec("lineHeight", "Line Height", K::Float, "text")), 0, DBL_MAX, 0.01, 3, "m");
        add(spec("font", "Font", K::String, "text"));
        en("textEffect", "Effect", "text", { "none:None", "outline:Outline", "outline fill:Outline with fill", "shadow:Shadow" });
        add(spec("textEffectColor", "Effect Color", K::Color, "text"));
        range(add(spec("textEffectThickness", "Effect Thickness", K::Float, "text")), 0, 0.5, 0.01, 2);
        en("billboardMode", "Billboard", "text", BILLBOARD_VALUES);
        stepping(add(spec("topMargin", "Top Margin", K::Float, "text")), 0.01, 2);
        stepping(add(spec("rightMargin", "Right Margin", K::Float, "text")), 0.01, 2);
        stepping(add(spec("bottomMargin", "Bottom Margin", K::Float, "text")), 0.01, 2);
        stepping(add(spec("leftMargin", "Left Margin", K::Float, "text")), 0.01, 2);
        add(spec("unlit", "Unlit", K::Bool, "text"));

        // zone
        en("shapeType", "Shape", "zone", { "box:Box", "sphere:Sphere", "ellipsoid:Ellipsoid", "cylinder-y:Cylinder", "compound:Compound Shape URL" });
        add(spec("compoundShapeURL", "Compound Shape", K::Url, "zone"));
        add(spec("flyingAllowed", "Flying Allowed", K::Bool, "zone"));
        add(spec("ghostingAllowed", "Ghosting Allowed", K::Bool, "zone"));
        add(spec("filterURL", "Filter", K::Url, "zone"));
        en("keyLightMode", "Key Light", "zone", MODE_VALUES);
        with(add(spec("keyLight.color", "Key Light Color", K::Color, "zone")), "keyLightMode=enabled");
        with(range(add(spec("keyLight.intensity", "Light Intensity", K::Float, "zone")), 0, 40, 0.01, 2), "keyLightMode=enabled");
        with(stepping(add(spec("keyLight.direction", "Light Angle", K::Polar, "zone")), 0.5, 1, "°"), "keyLightMode=enabled");
        with(add(spec("keyLight.castShadows", "Cast Shadows", K::Bool, "zone")), "keyLightMode=enabled");
        with(range(add(spec("keyLight.shadowBias", "Shadow Bias", K::Float, "zone")), 0, 1, 0.01, 2), "keyLightMode=enabled");
        with(range(add(spec("keyLight.shadowMaxDistance", "Shadow Distance", K::Float, "zone")), 1, 250, 0.1, 1, "m"), "keyLightMode=enabled");
        en("skyboxMode", "Skybox", "zone", MODE_VALUES);
        with(add(spec("skybox.color", "Skybox Color", K::Color, "zone")), "skyboxMode=enabled");
        with(add(spec("skybox.url", "Skybox Source", K::Url, "zone")), "skyboxMode=enabled");
        en("ambientLightMode", "Ambient Light", "zone", MODE_VALUES);
        with(range(add(spec("ambientLight.ambientIntensity", "Ambient Intensity", K::Float, "zone")), 0, 200, 0.1, 2), "ambientLightMode=enabled");
        with(add(spec("ambientLight.ambientURL", "Ambient Source", K::Url, "zone")), "ambientLightMode=enabled");
        // the zone's probe volume (LIGHTMAPS.md step 4): the file the Unity exporter writes (probes/ProbeVolume.bin),
        // light probes on a grid, sampled by position for the ambient diffuse light of whatever has no lightmap
        // (avatars, things that move) in place of the ambient source's one sphere; it rides with the ambient light
        with(add(spec("userData:probeVolume:url", "Probe Volume", K::Url, "zone")), "ambientLightMode=enabled");
        with(range(add(spec("userData:probeVolume:intensity", "Probe Volume Intensity", K::Float, "zone")), 0, 16, 0.01, 2), "ambientLightMode=enabled");
        en("hazeMode", "Haze", "zone", MODE_VALUES);
        with(range(add(spec("haze.hazeRange", "Haze Range", K::Float, "zone")), 1, 10000, 1, 0, "m"), "hazeMode=enabled");
        with(add(spec("haze.hazeAltitudeEffect", "Use Altitude", K::Bool, "zone")), "hazeMode=enabled");
        with(range(add(spec("haze.hazeBaseRef", "Haze Base", K::Float, "zone")), -16000, 16000, 1, 0, "m"), "hazeMode=enabled");
        with(range(add(spec("haze.hazeCeiling", "Haze Ceiling", K::Float, "zone")), -16000, 16000, 1, 0, "m"), "hazeMode=enabled");
        with(add(spec("haze.hazeColor", "Haze Color", K::Color, "zone")), "hazeMode=enabled");
        with(range(add(spec("haze.hazeBackgroundBlend", "Background Blend", K::Float, "zone")), 0, 1, 0.001, 3), "hazeMode=enabled");
        with(add(spec("haze.hazeEnableGlare", "Glare", K::Bool, "zone")), "hazeMode=enabled");
        with(add(spec("haze.hazeGlareColor", "Glare Color", K::Color, "zone")), "hazeMode=enabled");
        with(range(add(spec("haze.hazeGlareAngle", "Glare Angle", K::Float, "zone")), 0, 180, 1, 0, "°"), "hazeMode=enabled");
        en("bloomMode", "Bloom", "zone", MODE_VALUES);
        with(range(add(spec("bloom.bloomIntensity", "Bloom Intensity", K::Float, "zone")), 0, 1, 0.001, 3), "bloomMode=enabled");
        with(range(add(spec("bloom.bloomThreshold", "Bloom Threshold", K::Float, "zone")), 0, 1, 0.001, 3), "bloomMode=enabled");
        with(range(add(spec("bloom.bloomSize", "Bloom Size", K::Float, "zone")), 0, 2, 0.001, 3), "bloomMode=enabled");
        en("zoneCullingMode", "Zone Culling", "zone", { "inherit:Inherit", "outside:Cull Outside", "disabled:Disabled" });
        en("toneMappingMode", "Tone Mapping", "zone", { "inherit:Inherit", "rgb:RGB", "srgb:sRGB", "reinhard:Reinhard", "filmic:Filmic" });
        with(range(add(spec("toneMapping.exposure", "Exposure", K::Float, "zone")), -4, 4, 0.25, 2), "toneMappingMode in rgb,srgb,reinhard,filmic");
        en("avatarPriority", "Avatar Priority", "zone", { "inherit:Inherit", "crowd:Crowd", "hero:Hero" });

        // model
        add(spec("modelURL", "Model", K::Url, "model"));
        en("shapeType", "Collision Shape", "model", { "none:No Collision", "box:Box", "sphere:Sphere", "compound:Compound",
            "simple-hull:Basic, whole model", "simple-compound:Good, sub-meshes", "static-mesh:Exact, not dynamic" });
        add(spec("compoundShapeURL", "Compound Shape", K::Url, "model"));
        add(spec("animation.url", "Animation", K::Url, "model"));
        add(spec("animation.running", "Play Automatically", K::Bool, "model"));
        add(spec("animation.loop", "Loop", K::Bool, "model"));
        add(spec("animation.allowTranslation", "Allow Translation", K::Bool, "model"));
        add(spec("animation.hold", "Hold", K::Bool, "model"));
        stepping(add(spec("animation.currentFrame", "Frame", K::Float, "model")), 1, 1);
        stepping(add(spec("animation.firstFrame", "First Frame", K::Float, "model")), 1, 0);
        stepping(add(spec("animation.lastFrame", "Last Frame", K::Float, "model")), 1, 0);
        stepping(add(spec("animation.fps", "FPS", K::Float, "model")), 1, 1);
        add(spec("textures", "Textures", K::Json, "model"));

        // a model's lightmap (LIGHTMAPS.md §3.2): the picture, the second texture coordinates' scale and offset (Unity's
        // lightmapScaleOffset), how it lights, and a brightness (the params' scale)
        add(spec("userData:lightmap:url", "Lightmap", K::Image, "lightmap"));
        with(add(spec("userData:lightmap:directionalUrl", "Direction Map", K::Image, "lightmap")), "userData:lightmap:url!=");
        with(stepping(add(spec("userData:lightmap:scale", "Scale", K::Vec2, "lightmap")), 0.01, 4), "userData:lightmap:url!=");
        with(stepping(add(spec("userData:lightmap:offset", "Offset", K::Vec2, "lightmap")), 0.01, 4), "userData:lightmap:url!=");
        en("userData:lightmap:mode", "Mode", "lightmap", { "indirect:Indirect (Baked Indirect)", "full:Full Bake" },
            "userData:lightmap:url!=");
        with(range(add(spec("userData:lightmap:brightness", "Brightness", K::Float, "lightmap")), 0, 16, 0.01, 2),
            "userData:lightmap:url!=");
        add(spec("groupCulled", "Group Culled", K::Bool, "model"));

        // the entity's impostor card (IMPOSTORS.md): a Model's, or a Shape's that groups models (a Unity export's
        // empty parent); the section's editor shows the bake and runs it
        add(spec("userData:impostor", "Impostor", K::Impostor, "impostor"));

        // image
        add(spec("imageURL", "Image", K::Url, "image"));
        add(spec("color", "Color", K::Color, "image"));
        range(add(spec("alpha", "Alpha", K::Float, "image")), 0, 1, 0.01, 2);
        add(spec("emissive", "Emissive", K::Bool, "image"));
        add(spec("subImage", "Sub Image", K::Rect, "image"));
        en("billboardMode", "Billboard", "image", BILLBOARD_VALUES);
        add(spec("keepAspectRatio", "Keep Aspect Ratio", K::Bool, "image"));

        // web
        add(spec("sourceUrl", "Source", K::Url, "web"));
        add(spec("dpi", "Resolution", K::Int, "web"));
        add(spec("color", "Color", K::Color, "web"));
        range(add(spec("alpha", "Alpha", K::Float, "web")), 0, 1, 0.001, 3);
        add(spec("maxFPS", "Max FPS", K::Int, "web"));
        en("billboardMode", "Billboard", "web", BILLBOARD_VALUES);
        en("inputMode", "Input", "web", { "touch:Touch", "mouse:Mouse", "none:None" });
        add(spec("showKeyboardFocusHighlight", "Focus Highlight", K::Bool, "web"));
        add(spec("transparentBackground", "Transparent", K::Bool, "web"));
        add(spec("scriptURL", "Script URL", K::Url, "web"));

        // light
        // Unity's rows, the numbers together: Type, Color, Range, Intensity. Range is where the light ends (what its
        // sphere or cone shows) and sets its dimensions, which are not shown for a light; Falloff Radius is how fast
        // it dims within that (a quarter as bright that far out), and an edit of Range scales it along
        en("lightType", "Type", "light", { "point:Point", "spot:Spot" });
        add(spec("color", "Color", K::Color, "light"));
        range(add(spec("range", "Range", K::Float, "light")), 0.01, 5000, 0.1, 2, "m");
        range(add(spec("intensity", "Intensity", K::Float, "light")), 0, 10000, 0.1, 2);
        range(add(spec("falloffRadius", "Falloff Radius", K::Float, "light")), 0, 10000, 0.1, 2, "m");
        // whether the light casts shadows, which a light does only when turned On (Cast Shadows, which the Behavior
        // section leaves out for a light, is on for every entity unless turned off), and whether it comes before the
        // other lights for one of the shadow maps there are at once (eight for spot lights, four for point lights) and
        // casts at any distance; On and Important are kept in userData
        en(SHADOWS_KEY, "Shadows", "light", { "off:Off", "on:On", "important:Important" });
        with(range(add(spec("exponent", "Spot Exponent", K::Float, "light")), 0, DBL_MAX, 0.01, 2), "isSpotlight=true");
        with(range(add(spec("cutoff", "Spot Cutoff", K::Float, "light")), 0, 90, 0.5, 1, "°"), "isSpotlight=true");
        // a picture the light shines through (Unity's cookie): a spot light projects it down its cone, as a slide in a
        // projector; a point light hangs it around itself as a panorama, the equirectangular picture a skybox takes
        // (its middle straight ahead of the light, down its -Z), turned with the light; kept in userData
        add(spec("userData:cookie", "Cookie", K::Image, "light"));

        // material: where it comes from and how it lies on what it's applied to
        add(spec("materialURL", "Material URL", K::Url, "material"));
        add(spec("parentMaterialName", "Target", K::String, "material"));
        add(spec("priority", "Priority", K::Int, "material"));
        en("materialMappingMode", "Mapping", "material", { "uv:UV space", "projected:3D projected" });
        range(add(spec("materialMappingPos", "Position", K::Vec2, "material")), 0, 1, 0.1, 3);
        range(add(spec("materialMappingScale", "Scale", K::Vec2, "material")), 0, DBL_MAX, 0.1, 3);
        stepping(add(spec("materialMappingRot", "Rotation", K::Float, "material")), 1, 1, "°");
        add(spec("materialRepeat", "Repeat", K::Bool, "material"));

        // a material whose data is its own, laid out as Unity's URP/Lit Inspector: its options, then its inputs, each
        // shown where it applies (Metallic only without a Metallic Map, as in Unity; each packing its own maps)
#define OWN_DATA "materialURL=materialData"
#define PACKING_KEY "materialData:packing"
        // how occlusion, roughness and metallic are packed into pictures
        en(PACKING_KEY, "Packing", "material_surface", { "unity:Unity (Metallic + Smoothness)", "orm:ORM (glTF, Unreal)",
            "custom:Custom" }, OWN_DATA);
        // what the Base Map's alpha does: Transparent blends by it; Alpha Clipping cuts the surface out where it is
        // below the Threshold (both: cut out, then blend the rest). Opacity, below, fades the whole surface
        en("materialData:surfaceType", "Surface Type", "material_surface", { "opaque:Opaque", "transparent:Transparent" },
            OWN_DATA);
        en("materialData:cullFaceMode", "Render Face", "material_surface", { "CULL_BACK:Front", "CULL_FRONT:Back",
            "CULL_NONE:Both" }, OWN_DATA);
        with(add(spec("materialData:alphaClipping", "Alpha Clipping", K::Bool, "material_surface")), OWN_DATA);
        with(range(add(spec("materialData:opacityCutoff", "Threshold", K::Slider, "material_surface")), 0, 1, 0.01, 2),
            OWN_DATA " and materialData:alphaClipping=true");
        with(add(spec("materialData:unlit", "Unlit", K::Bool, "material_surface")), OWN_DATA);
        auto input = [&](const char* key, const char* label, PropertyKind kind, const char* showWhen) -> PropertySpec& {
            PropertySpec& s = with(add(spec(key, label, kind, "material_inputs")), showWhen);
            return kind == K::Slider ? range(s, 0, 1, 0.01, 2) : s;
        };
        input("materialData:albedoMap", "Base Map", K::Image, OWN_DATA);
        input("materialData:albedo", "Base Color", K::Color, OWN_DATA);
        input("materialData:metallicMap", "Metallic Map", K::Image, OWN_DATA " and " PACKING_KEY " in unity,custom");
        input("materialData:ormMap", "ORM Map", K::Image, OWN_DATA " and " PACKING_KEY "=orm");
        input("materialData:metallic", "Metallic", K::Slider,
            OWN_DATA " and " PACKING_KEY "=custom or "
            OWN_DATA " and " PACKING_KEY "=unity and materialData:metallicMap= or "
            OWN_DATA " and " PACKING_KEY "=orm and materialData:ormMap=");
        input("materialData:smoothness", "Smoothness", K::Slider, OWN_DATA " and " PACKING_KEY "=unity");
        en("materialData:smoothnessSource", "Smoothness Source", "material_inputs", { "metallicAlpha:Metallic Alpha",
            "albedoAlpha:Albedo Alpha" }, OWN_DATA " and " PACKING_KEY "=unity");
        input("materialData:roughnessMap", "Roughness Map", K::Image, OWN_DATA " and " PACKING_KEY "=custom");
        input("materialData:roughness", "Roughness", K::Slider,
            OWN_DATA " and " PACKING_KEY "=custom or "
            OWN_DATA " and " PACKING_KEY "=orm and materialData:ormMap=");
        input("materialData:normalMap", "Normal Map", K::Image, OWN_DATA);
        input("materialData:occlusionMap", "Occlusion Map", K::Image, OWN_DATA " and " PACKING_KEY " in unity,custom");
        input("materialData:occlusionStrength", "Occlusion Strength", K::Slider,
            OWN_DATA " and " PACKING_KEY " in unity,custom and materialData:occlusionMap!= or "
            OWN_DATA " and " PACKING_KEY "=orm and materialData:ormMap!=");
        input("materialData:emissiveMap", "Emission Map", K::Image, OWN_DATA);
        input("materialData:emissive", "Emission Color", K::Color, OWN_DATA);
        input("materialData:opacity", "Opacity", K::Slider, OWN_DATA);
        // a baked lightmap over the second texture coordinates (LIGHTMAPS.md): Indirect is Unity's Baked Indirect (the
        // baked light in place of the ambient, the lights as usual), Full Bake High Fidelity's (the whole lighting)
        input("materialData:lightMap", "Lightmap", K::Image, OWN_DATA);
        // Unity's directional lightmap: the baked light's direction, which lets the normal map shape it
        input("materialData:lightMapDirection", "Direction Map", K::Image, OWN_DATA " and materialData:lightMap!=");
        en("materialData:lightmapMode", "Lightmap Mode", "material_inputs", { "indirect:Indirect (Baked Indirect)",
            "full:Full Bake" }, OWN_DATA " and materialData:lightMap!=");
        with(range(add(spec("materialData:lightmapScale", "Lightmap Scale", K::Float, "material_inputs")), 0, 16, 0.01, 2),
            OWN_DATA " and materialData:lightMap!=");
#undef OWN_DATA
#undef PACKING_KEY
        // all of it as JSON, for scripters and for what has no field of its own
        add(spec("materialData", "Material Data", K::Json, "material_data"));

        // grid
        add(spec("color", "Color", K::Color, "grid"));
        add(spec("followCamera", "Follow Camera", K::Bool, "grid"));
        add(spec("majorGridEvery", "Major Every", K::Int, "grid"));
        range(add(spec("minorGridEvery", "Minor Every", K::Float, "grid")), 0, DBL_MAX, 0.01, 2);

        // terrain
        range(add(spec("voxelSize", "Voxel Size", K::Float, "terrain")), 0.05, 16, 0.05, 2, "m");
        add(spec("terrainURL", "Terrain File", K::Url, "terrain"));
        add(spec("terrainVersion", "Version", K::Int, "terrain")).readOnly = true;
        add(spec("terrainClosedBox", "Closed Box", K::TerrainCaps, "terrain"));
        add(spec("terrainLayers", "Layers", K::TerrainLayers, "terrain"));

        // vehicle (PHYSICS.md 4.8): the entity is the chassis or hull, with the names Unity gives a Rigidbody and a
        // WheelCollider where it has them and Jolt's for the engine and transmission; the simulator (the physics server,
        // or the owning client) rebuilds the vehicle as any of these change, so they can be tuned while it drives.
        // The state at the bottom is what the simulator last published: read it, the simulator writes it.
        const char* ANY_VEHICLE = "vehicle.type in wheeled,airship,aircraft";
        const char* WHEELED = "vehicle.type=wheeled";
        const char* WHEEL = "vehicle.type=wheel";
        const char* AIRSHIP = "vehicle.type=airship";
        const char* AIRCRAFT = "vehicle.type=aircraft";
        const char* FLYING = "vehicle.type in airship,aircraft";
        en("vehicle.type", "Type", "vehicle", { "none:None", "wheeled:Wheeled", "wheel:Wheel (of the parent)", "airship:Airship",
            "aircraft:Aircraft" });
        with(range(add(spec("vehicle.mass", "Mass", K::Float, "vehicle")), 0, 10000000, 1, 1, "kg"), ANY_VEHICLE);
        with(stepping(add(spec("vehicle.centerOfMass", "Center of Mass", K::Vec3, "vehicle")), 0.01, 3, "m"), ANY_VEHICLE);
        // the wheels and seats take the whole width, under their own headings
        with(add(spec("vehicle.wheels", "Wheels", K::VehicleWheels, "vehicle")), WHEELED);
        with(add(spec("vehicle.seats", "Seats", K::VehicleSeats, "vehicle")), ANY_VEHICLE);
        with(range(add(spec("vehicle.maxSteerAngle", "Steering Lock", K::Float, "vehicle")), 0, 80, 1, 1, "°"), WHEELED);
        en("vehicle.driveType", "Drive", "vehicle", { "rear:Rear wheels", "front:Front wheels", "all:All wheels" }, WHEELED);
        with(range(add(spec("vehicle.maxTorque", "Engine Torque", K::Float, "vehicle")), 0, 1000000, 10, 0, "N m"), WHEELED);
        with(range(add(spec("vehicle.minRPM", "Idle RPM", K::Float, "vehicle")), 1, 50000, 100, 0), WHEELED);
        with(range(add(spec("vehicle.maxRPM", "Red Line RPM", K::Float, "vehicle")), 2, 50000, 100, 0), WHEELED);
        with(add(spec("vehicle.gearRatios", "Gear Ratios", K::String, "vehicle")), WHEELED);
        with(range(add(spec("vehicle.reverseGearRatio", "Reverse Ratio", K::Float, "vehicle")), 0.01, 100, 0.01, 2), WHEELED);
        with(range(add(spec("vehicle.finalDriveRatio", "Final Drive Ratio", K::Float, "vehicle")), 0.01, 100, 0.01, 2), WHEELED);
        with(range(add(spec("vehicle.maxBrakeTorque", "Brake Torque", K::Float, "vehicle")), 0, 1000000, 10, 0, "N m"), WHEELED);
        with(range(add(spec("vehicle.maxHandBrakeTorque", "Hand Brake Torque", K::Float, "vehicle")), 0, 1000000, 10, 0, "N m"), WHEELED);
        with(range(add(spec("vehicle.suspensionFrequency", "Suspension Frequency", K::Slider, "vehicle")), 0.1, 5, 0.01, 2, "Hz"), WHEELED);
        with(range(add(spec("vehicle.suspensionDamping", "Suspension Damping", K::Slider, "vehicle")), 0, 2, 0.01, 2), WHEELED);
        // a wheel entity: a child of the chassis that shows one of its wheels
        with(add(spec("vehicle.wheel", "Wheel Name", K::String, "vehicle")), WHEEL);
        en("vehicle.axle", "Axle Axis", "vehicle", { "x:X (a model made for Unity)", "y:Y (a Cylinder shape)", "z:Z" }, WHEEL);
        // an airship: a hull with buoyancy, propellers, fins and ballast
        with(range(add(spec("vehicle.envelopeVolume", "Envelope Volume", K::Float, "vehicle")), 0, 10000000, 10, 1, "m³"), AIRSHIP);
        with(stepping(add(spec("vehicle.centerOfBuoyancy", "Center of Buoyancy", K::Vec3, "vehicle")), 0.01, 3, "m"), AIRSHIP);
        with(range(add(spec("vehicle.gasDensity", "Gas Density", K::Float, "vehicle")), 0, 2, 0.0001, 4, "kg/m³"), AIRSHIP);
        with(range(add(spec("vehicle.maxThrust", "Thrust", K::Float, "vehicle")), 0, 10000000, 10, 0, "N"), FLYING);
        with(range(add(spec("vehicle.maxVerticalThrust", "Vertical Thrust", K::Float, "vehicle")), 0, 10000000, 10, 0, "N"), AIRSHIP);
        with(stepping(add(spec("vehicle.thrustPoint", "Thrust Point", K::Vec3, "vehicle")), 0.01, 3, "m"), FLYING);
        // an aircraft: a wing, tilting engines and a tail
        with(range(add(spec("vehicle.wingArea", "Wing Area", K::Float, "vehicle")), 0, 100000, 0.1, 2, "m²"), AIRCRAFT);
        with(range(add(spec("vehicle.wingSpan", "Wing Span", K::Float, "vehicle")), 0, 1000, 0.1, 2, "m"), AIRCRAFT);
        with(stepping(add(spec("vehicle.wingPoint", "Wing Point", K::Vec3, "vehicle")), 0.01, 3, "m"), AIRCRAFT);
        with(range(add(spec("vehicle.rudderArea", "Rudder Area", K::Float, "vehicle")), 0, 100000, 0.1, 2, "m²"), FLYING);
        with(range(add(spec("vehicle.elevatorArea", "Elevator Area", K::Float, "vehicle")), 0, 100000, 0.1, 2, "m²"), FLYING);
        with(stepping(add(spec("vehicle.finPoint", "Fin Point", K::Vec3, "vehicle")), 0.01, 3, "m"), FLYING);
        with(range(add(spec("vehicle.maxFinAngle", "Fin Deflection", K::Float, "vehicle")), 0, 90, 1, 1, "°"), FLYING);
        with(stepping(add(spec("vehicle.dragArea", "Drag Area", K::Vec3, "vehicle")), 0.1, 2, "m²"), FLYING);
        with(stepping(add(spec("vehicle.angularDragArea", "Angular Drag Area", K::Vec3, "vehicle")), 0.1, 2, "m⁵"), FLYING);
        with(range(add(spec("vehicle.ballastRate", "Ballast Drop Rate", K::Float, "vehicle")), 0, 100000, 1, 1, "kg/s"), AIRSHIP);
        with(range(add(spec("vehicle.ventRate", "Vent Rate", K::Float, "vehicle")), 0, 1, 0.001, 3, "/s"), AIRSHIP);
        with(range(add(spec("vehicle.ballast", "Ballast", K::Float, "vehicle")), 0, 10000000, 1, 1, "kg"), AIRSHIP);
        // the state, read-only
        auto state = [&](const char* key, const char* label, const char* showWhen, double min, double max, int decimals, const char* unit = "") {
            PropertySpec& s = with(range(add(spec(key, label, K::Float, "vehicle_state")), min, max, 0.01, decimals, unit), showWhen);
            s.readOnly = true;
            return &s;
        };
        with(add(spec("vehicle.driver", "Driver", K::String, "vehicle_state")), ANY_VEHICLE).readOnly = true;
        state("vehicle.inputSequence", "Input Sequence", ANY_VEHICLE, 0, DBL_MAX, 0);
        state("vehicle.throttle", "Throttle", ANY_VEHICLE, -1, 1, 2);
        state("vehicle.steering", "Steering", ANY_VEHICLE, -1, 1, 2);
        state("vehicle.brake", "Brake", WHEELED, 0, 1, 2);
        state("vehicle.handBrake", "Hand Brake", WHEELED, 0, 1, 2);
        state("vehicle.engineRPM", "Engine RPM", WHEELED, 0, DBL_MAX, 0);
        state("vehicle.gear", "Gear", WHEELED, -1, 100, 0);
        state("vehicle.pitch", "Pitch", FLYING, -1, 1, 2);
        state("vehicle.lift", "Lift", FLYING, -1, 1, 2);
        state("vehicle.trim", "Trim", AIRSHIP, -1, 1, 2);
        state("vehicle.fill", "Fill", AIRSHIP, 0, 1, 3);
        state("vehicle.tilt", "Tilt", AIRCRAFT, 0, 1, 2);
        with(add(spec("vehicle.stateTime", "State Time", K::String, "vehicle_state")), ANY_VEHICLE).readOnly = true;

        // particles
        add(spec("isEmitting", "Emit", K::Bool, "particles"));
        range(add(spec("lifespan", "Lifespan", K::Float, "particles")), 0, 86400, 0.01, 2, "s");
        range(add(spec("maxParticles", "Max Particles", K::Int, "particles")), 1, 100000, 1, 0);
        add(spec("textures", "Texture", K::Url, "particles"));
        range(add(spec("emitRate", "Emit Rate", K::Float, "particles_emit")), 0, 100000, 1, 1);
        range(add(spec("emitSpeed", "Emit Speed", K::Float, "particles_emit")), -1000, 1000, 0.1, 2);
        range(add(spec("speedSpread", "Speed Spread", K::Float, "particles_emit")), -1000, 1000, 0.1, 2);
        en("shapeType", "Shape", "particles_emit", { "box:Box", "ellipsoid:Ellipsoid", "cylinder-y:Cylinder", "circle:Circle", "plane:Plane", "compound:Compound Shape URL" });
        add(spec("compoundShapeURL", "Compound Shape", K::Url, "particles_emit"));
        range(add(spec("emitDimensions", "Emit Dimensions", K::Vec3, "particles_emit")), 0, 32768, 0.01, 2);
        range(add(spec("emitRadiusStart", "Emit Radius Start", K::Float, "particles_emit")), 0, 1, 0.001, 3);
        stepping(add(spec("emitOrientation", "Emit Orientation", K::Euler, "particles_emit")), 1, 1, "°");
        add(spec("emitterShouldTrail", "Trails", K::Bool, "particles_emit"));
        range(add(spec("radiusStart", "Size Start", K::Float, "particles_size")), 0, 32768, 0.01, 2);
        range(add(spec("particleRadius", "Size Middle", K::Float, "particles_size")), 0, 32768, 0.01, 2);
        range(add(spec("radiusFinish", "Size Finish", K::Float, "particles_size")), 0, 32768, 0.01, 2);
        range(add(spec("radiusSpread", "Size Spread", K::Float, "particles_size")), 0, 32768, 0.01, 2);
        add(spec("colorStart", "Color Start", K::FloatColor, "particles_color"));
        add(spec("color", "Color Middle", K::Color, "particles_color"));
        add(spec("colorFinish", "Color Finish", K::FloatColor, "particles_color"));
        add(spec("colorSpread", "Color Spread", K::Color, "particles_color"));
        range(add(spec("alphaStart", "Alpha Start", K::Float, "particles_alpha")), 0, 1, 0.001, 3);
        range(add(spec("alpha", "Alpha Middle", K::Float, "particles_alpha")), 0, 1, 0.001, 3);
        range(add(spec("alphaFinish", "Alpha Finish", K::Float, "particles_alpha")), 0, 1, 0.001, 3);
        range(add(spec("alphaSpread", "Alpha Spread", K::Float, "particles_alpha")), 0, 1, 0.001, 3);
        range(add(spec("emitAcceleration", "Acceleration", K::Vec3, "particles_acceleration")), -100, 100, 0.01, 2);
        range(add(spec("accelerationSpread", "Acceleration Spread", K::Vec3, "particles_acceleration")), 0, 100, 0.01, 2);
        range(add(spec("spinStart", "Spin Start", K::Degrees, "particles_spin")), -360, 360, 1, 1, "°");
        range(add(spec("particleSpin", "Spin Middle", K::Degrees, "particles_spin")), -360, 360, 1, 1, "°");
        range(add(spec("spinFinish", "Spin Finish", K::Degrees, "particles_spin")), -360, 360, 1, 1, "°");
        range(add(spec("spinSpread", "Spin Spread", K::Degrees, "particles_spin")), -360, 360, 1, 1, "°");
        add(spec("rotateWithEntity", "Rotate with Entity", K::Bool, "particles_spin"));
        range(add(spec("polarStart", "Polar Start", K::Degrees, "particles_constraints")), 0, 180, 1, 1, "°");
        range(add(spec("polarFinish", "Polar Finish", K::Degrees, "particles_constraints")), 0, 180, 1, 1, "°");
        range(add(spec("azimuthStart", "Azimuth Start", K::Degrees, "particles_constraints")), -180, 180, 1, 1, "°");
        range(add(spec("azimuthFinish", "Azimuth Finish", K::Degrees, "particles_constraints")), -180, 180, 1, 1, "°");

        // behavior
        add(spec("grab.grabbable", "Grabbable", K::Bool, "behavior"));
        add(spec("grab.triggerable", "Triggerable", K::Bool, "behavior"));
        add(spec("grab.grabFollowsController", "Follows Controller", K::Bool, "behavior"));
        add(spec("cloneable", "Cloneable", K::Bool, "behavior"));
        with(range(add(spec("cloneLifetime", "Clone Lifetime", K::Float, "behavior")), -1, DBL_MAX, 1, 0, "s"), "cloneable=true");
        with(range(add(spec("cloneLimit", "Clone Limit", K::Float, "behavior")), 0, DBL_MAX, 1, 0), "cloneable=true");
        with(add(spec("cloneDynamic", "Clone Dynamic", K::Bool, "behavior")), "cloneable=true");
        with(add(spec("cloneAvatarEntity", "Clone Avatar Entity", K::Bool, "behavior")), "cloneable=true");
        with(add(spec("cloneGrabbable", "Clone Grabbable", K::Bool, "behavior")), "cloneable=true");
        // whether the object casts a shadow from the sun and from spot and point lights; a light's own shadows are
        // its Shadows field, and a zone or a material casts none, so on those the box would only mislead
        with(add(spec("canCastShadow", "Cast Shadows", K::Bool, "behavior")), "type not in Light,Zone,Material");
        add(spec("href", "Link", K::Url, "behavior"));
        add(spec("ignorePickIntersection", "Ignore Picking", K::Bool, "behavior"));
        add(spec("script", "Script", K::Url, "behavior"));
        add(spec("serverScripts", "Server Script", K::Url, "behavior"));
        stepping(add(spec("lifetime", "Lifetime", K::Float, "behavior")), 1, 0, "s");
        add(spec("userData", "User Data", K::UserData, "userData"));

        // collision
        add(spec("collisionless", "Collisionless", K::Bool, "collision"));
        with(add(spec("collisionMask", "Collides With", K::CollisionMask, "collision")), "collisionless=false");
        with(add(spec("collisionSoundURL", "Collision Sound", K::Url, "collision")), "collisionless=false");
        add(spec("dynamic", "Dynamic", K::Bool, "collision"));

        // physics
        stepping(add(spec("velocity", "Velocity", K::Vec3, "physics")), 0.01, 3, "m/s");
        range(add(spec("damping", "Damping", K::Float, "physics")), 0, 1, 0.001, 3);
        stepping(add(spec("angularVelocity", "Angular Velocity", K::Vec3, "physics")), 1, 2, "°/s");
        range(add(spec("angularDamping", "Angular Damping", K::Float, "physics")), 0, 1, 0.001, 3);
        range(add(spec("restitution", "Bounciness", K::Float, "physics")), 0, 0.99, 0.01, 2);
        range(add(spec("friction", "Friction", K::Float, "physics")), 0, 10, 0.01, 2);
        range(add(spec("density", "Density", K::Float, "physics")), 100, 10000, 1, 0);
        stepping(add(spec("gravity", "Gravity", K::Vec3, "physics")), 0.1, 2, "m/s²");
        stepping(add(spec("acceleration", "Acceleration", K::Vec3, "physics")), 0.1, 2, "m/s²");

        return t;
    }

    const QVector<PropertySection> SECTIONS = {
        { "base", "", false },
        { "spatial", "Transform", false },
        { "shape", "Shape", false },
        { "reflectionProbe", "Reflection Probe", true },
        { "text", "Text", false },
        { "zone", "Zone", false },
        { "model", "Model", false },
        { "lightmap", "Lightmap", true },
        { "impostor", "Impostor", true },
        { "image", "Image", false },
        { "web", "Web", false },
        { "light", "Light", false },
        { "material", "Material", false },
        { "material_surface", "Surface Options", true },
        { "material_inputs", "Surface Inputs", true },
        { "material_data", "Material Data", true },
        { "grid", "Grid", false },
        { "terrain", "Terrain", false },
        { "particles", "Particles", false },
        { "particles_emit", "Emit", true },
        { "particles_size", "Size", true },
        { "particles_color", "Color", true },
        { "particles_alpha", "Alpha", true },
        { "particles_acceleration", "Acceleration", true },
        { "particles_spin", "Spin", true },
        { "particles_constraints", "Constraints", true },
        { "behavior", "Behavior", false },
        { "collision", "Collision", false },
        { "physics", "Physics", false },
        { "vehicle", "Vehicle", false },
        { "vehicle_state", "Vehicle State", true },
        { "userData", "User Data", false }
    };
}

namespace PropertySpecs {

const QVector<PropertySpec>& all() {
    static const QVector<PropertySpec> table = buildTable();
    return table;
}

QVector<const PropertySpec*> forSection(const QString& section) {
    QVector<const PropertySpec*> result;
    for (const auto& s : all()) {
        if (s.section == section) {
            result << &s;
        }
    }
    return result;
}

const PropertySpec* find(const QString& key) {
    for (const auto& s : all()) {
        if (s.key == key) {
            return &s;
        }
    }
    return nullptr;
}

const PropertySection* section(const QString& id) {
    for (const auto& s : SECTIONS) {
        if (s.id == id) {
            return &s;
        }
    }
    return nullptr;
}

// the sections an entity type shows, User Data aside
static QStringList typeSections(EntityTypes::EntityType type, bool multiple) {
    if (multiple) {
        return { "base", "spatial", "behavior", "collision", "physics" };
    }
    switch (type) {
        case EntityTypes::Box:
        case EntityTypes::Sphere:
        case EntityTypes::Shape:
            return { "base", "spatial", "shape", "reflectionProbe", "impostor", "behavior", "collision", "physics", "vehicle", "vehicle_state" };
        case EntityTypes::Text:
            return { "base", "spatial", "text", "behavior", "collision", "physics" };
        case EntityTypes::Zone:
            return { "base", "spatial", "zone", "behavior", "physics" };
        case EntityTypes::Model:
            return { "base", "spatial", "model", "lightmap", "impostor", "behavior", "collision", "physics", "vehicle", "vehicle_state" };
        case EntityTypes::Image:
            return { "base", "spatial", "image", "behavior", "collision", "physics" };
        case EntityTypes::Web:
            return { "base", "spatial", "web", "behavior", "collision", "physics" };
        case EntityTypes::Light:
            return { "base", "spatial", "light", "behavior", "collision", "physics" };
        case EntityTypes::Material:
            return { "base", "spatial", "material", "material_surface", "material_inputs", "material_data", "behavior" };
        case EntityTypes::ParticleEffect:
            return { "base", "spatial", "particles", "particles_emit", "particles_size", "particles_color", "particles_alpha",
                     "particles_acceleration", "particles_spin", "particles_constraints", "behavior", "physics" };
        case EntityTypes::Grid:
            return { "base", "spatial", "grid", "behavior", "physics" };
        case EntityTypes::Terrain:
            return { "base", "spatial", "terrain", "behavior", "collision" };
        default:
            return { "base", "spatial", "behavior", "collision", "physics" };
    }
}

QStringList sectionsForType(EntityTypes::EntityType type, bool multiple) {
    // every entity has User Data, last, as Unity puts what a project adds below an object's own components
    return typeSections(type, multiple) << QStringLiteral("userData");
}

QVariant get(const QString& key, const EntityItemProperties& properties) {
    auto it = accessors().constFind(key);
    return it == accessors().constEnd() ? QVariant() : it->get(properties);
}

bool set(const QString& key, EntityItemProperties& properties, const QVariant& value) {
    auto it = accessors().constFind(key);
    if (it == accessors().constEnd()) {
        return false;
    }
    it->set(properties, value);
    return true;
}

bool isEntry(const QString& key) {
    return key.contains(':');
}

bool isUserDataEntry(const QString& key) {
    return key.startsWith(QStringLiteral("userData:"));
}

bool takesEntry(const QString& key, const EntityItemProperties& properties) {
    if (isUserDataEntry(key)) {
        QString text = properties.getUserData();
        return text.trimmed().isEmpty() || QJsonDocument::fromJson(text.toUtf8()).isObject();
    }
    if (key.startsWith(QStringLiteral("materialData:"))) {
        QString text = properties.getMaterialData();
        QJsonValue materials = QJsonDocument::fromJson(text.toUtf8()).object().value("materials");
        return text.trimmed().isEmpty() || materials.isObject() || materials.isArray();
    }
    return true;
}

QStringList storedKeys(const QString& key) {
    if (key == QStringLiteral("scale")) {
        return { QStringLiteral("dimensions") };
    }
    if (key == QLatin1String(SHADOWS_KEY)) {
        return { QStringLiteral("userData"), QStringLiteral("canCastShadow") };
    }
    return { isEntry(key) ? key.left(key.indexOf(':')) : key };
}


glm::vec3 naturalSize(const EntityItemProperties& properties) {
    if (properties.getType() != EntityTypes::Model) {
        return glm::vec3(1.0f);
    }
    glm::vec3 natural = properties.getNaturalDimensions();
    if (!glm::any(glm::greaterThan(natural, glm::vec3(1.0e-6f)))) {
        return glm::vec3(0.0f);   // not loaded
    }
    // a flat model (a plane) has no thickness of its own: that axis counts in metres
    for (int i = 0; i < 3; ++i) {
        if (natural[i] <= 1.0e-6f) {
            natural[i] = 1.0f;
        }
    }
    return natural;
}

}
