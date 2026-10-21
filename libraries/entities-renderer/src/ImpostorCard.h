//
//  ImpostorCard.h
//  libraries/entities-renderer/src
//
//  An entity's impostor card (IMPOSTORS.md 2.3): the render item that stands in for the entity's models at a
//  distance, a square of side two radii facing the camera, drawn by impostor.slf from the atlases the bake wrote
//  (ImpostorBaker, the "impostor" entry of the entity's userData). The base EntityRenderer owns one for every entity
//  with an entry and keeps it a second item in the scene beside the entity's own; the card's ImpostorSwitch decides,
//  per view, whether the card or the models draw (or both, dissolving into each other), and the models the card
//  stands for (the entity's own and its descendants') hold the same switch (Model::setImpostorSwitch). The item
//  draws through pipelines of its own (deferred, forward, and the shadow pass, where the card casts the shadow the
//  model would), so its shape key says so.
//
//  Created by the editor session on 2026-10-18.
//

#ifndef hifi_ImpostorCard_h
#define hifi_ImpostorCard_h

#include <memory>
#include <mutex>

#include <QtCore/QString>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <gpu/Batch.h>
#include <render/Item.h>
#include <render/Args.h>
#include <RenderHifi.h>
#include <TextureCache.h>
#include <ImpostorSwitch.h>

// The "impostor" entry of an entity's userData (IMPOSTORS.md 2.1), as the baker writes it.
struct ImpostorEntry {
    bool valid { false };
    QString spec;           // the entry's own JSON, compact: equal specs are the same entry
    QString albedoUrl;
    QString normalUrl;
    QString materialUrl;    // roughness and metallic per texel (a bake since step 4); empty: the entry's values
    int size { 256 };       // a frame's side, texels
    float radius { 0.0f };  // the bounding sphere in the entity's frame at bake time
    glm::vec3 center { 0.0f };
    float roughness { 0.7f };
    float metallic { 0.0f };

    static ImpostorEntry parse(const QString& userData);
};

class ImpostorCard : public render::PayloadProxyInterface {
public:
    ImpostorCard();

    // main thread (the entity renderer's synchronous update): what the card is and where it stands
    void setEntry(const ImpostorEntry& entry);
    void setFrame(const glm::vec3& positionWS, const glm::quat& rotationWS);
    void setVisible(bool visible);
    void setKeyBits(render::hifi::Tag tagMask, render::hifi::Layer layer, bool subMetaCulled, bool castShadow);
    // the card stands in for its models, which are not all here (ImpostorSwitch::setStandIn, ENTITY-STREAMING.md 3.2)
    void setStandIn(bool standIn);
    // the atlases are still on their way (the renderer asks for another update until they are here or have failed)
    bool waitingForTextures() const;
    // the albedo and normal atlases are loaded, or one of them failed (main thread)
    bool atlasesLoaded() const;
    bool atlasesFailed() const;
    // the sphere the card stands in, in world space, as last set (main thread): false without a valid entry
    bool getSphere(glm::vec3& centerWS, float& radius) const;
    const ImpostorSwitchPointer& getSwitch() const { return _switch; }

    // render thread, from the item's update: takes what the main thread set, readies the switch and the parameters
    void update();

    // render::PayloadProxyInterface
    render::ItemKey getKey() override;
    render::ShapeKey getShapeKey() override;
    render::Item::Bound getBound() override;
    void render(RenderArgs* args) override;
    uint32_t metaFetchMetaSubItems(render::ItemIDs& subItems) const override { return 0; }

private:
    struct State {
        ImpostorEntry entry;
        glm::vec3 position { 0.0f };
        glm::quat rotation;
        bool visible { false };
        render::hifi::Tag tagMask { render::hifi::TAG_MAIN_VIEW };
        render::hifi::Layer layer { render::hifi::LAYER_3D };
        bool subMetaCulled { false };
        bool castShadow { false };
        bool standIn { false };
    };

    mutable std::mutex _mutex;     // over _pending and the network textures
    State _pending;
    NetworkTexturePointer _albedo;
    NetworkTexturePointer _normal;
    NetworkTexturePointer _material;

    // the render thread's
    State _live;
    gpu::TexturePointer _albedoTexture;
    gpu::TexturePointer _normalTexture;
    gpu::TexturePointer _materialTexture;
    bool _ready { false };
    glm::vec3 _centerWS { 0.0f };
    render::Item::Bound _bound;
    gpu::BufferPointer _params;
    int _debugMode { 0 };
    int _marchMode { 2 };
    float _marchFootprints { 0.25f };
    ImpostorSwitchPointer _switch;
};

#endif // hifi_ImpostorCard_h
