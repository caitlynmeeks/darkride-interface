//
//  RenderableLightEntityItem.h
//  interface/src/entities
//
//  Created by Brad Hefta-Gaub on 8/6/14.
//  Copyright 2014 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#ifndef hifi_RenderableLightEntityItem_h
#define hifi_RenderableLightEntityItem_h

#include "RenderableEntityItem.h"
#include <LightEntityItem.h>
#include <LightPayload.h>
#include <TextureCache.h>

namespace render { namespace entities { 

class LightEntityRenderer final : public TypedEntityRenderer<LightEntityItem> {
    using Parent = TypedEntityRenderer<LightEntityItem>;
    friend class EntityRenderer;

public:
    LightEntityRenderer(const EntityItemPointer& entity) : Parent(entity) { }

protected:
    virtual bool needsRenderUpdateFromTypedEntity(const TypedEntityPointer& entity) const override;
    virtual void doRenderUpdateSynchronousTyped(const ScenePointer& scene, Transaction& transaction, const TypedEntityPointer& entity) override;
    virtual void doRenderUpdateAsynchronousTyped(const TypedEntityPointer& entity) override;

    virtual ItemKey getKey() override;
    virtual Item::Bound getBound() override;
    virtual void doRender(RenderArgs* args) override;

private:
    const LightPayload::Pointer _lightPayload{ std::make_shared<LightPayload>() };

    // the cookie: the picture the userData's "cookie" entry names (the build mode Inspector's Cookie field); and
    // the userData's "shadows" entry, "on" or "important" (the Inspector's Shadows field), without which the light
    // casts no shadows
    QString _userData;
    QString _cookieURL;
    NetworkTexturePointer _cookie;
    bool _shadowsOn { false };
    bool _shadowsImportant { false };
};

} } // namespace 

#endif // hifi_RenderableLightEntityItem_h
