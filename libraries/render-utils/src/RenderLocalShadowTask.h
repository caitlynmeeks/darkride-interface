//
//  RenderLocalShadowTask.h
//  render-utils/src/
//
//  Shadows and cookies from spot and point lights. Each frame the lights that matter most to the view, of those that
//  cast shadows, get shadow maps drawn into tiles of one depth atlas: a spot light one tile, a point light six smaller
//  ones, a 90-degree face each along the world's +X, -X, +Y, -Y, +Z and -Z axes (a cube map laid flat, as Unity's URP
//  draws point-light shadows). Lights with a cookie (a picture the light shines through) get it: a spot light's is
//  projected from the light down its cone, a point light's hangs around the light as a panorama (the equirectangular
//  picture a skybox takes), turned with the light; the picture is copied into a layer of a texture array. The light
//  loop the deferred and transparent shading share (LightLocal.slh) finds a light's tiles and cookie through a small
//  table (LocalShadow.slh). A tile's map is cached: it is drawn again only when the light's view changes or a caster
//  in that view appears, leaves, changes or fades (render::Item::getChangeFrame). Casters that haven't changed for a
//  while are static: they are kept in a map of their own, in a second atlas, which is copied into the tile before the
//  dynamic casters are drawn over it (as HDRP's cached shadow maps with "always draw dynamic"), so that an avatar
//  moving in a light's view costs the avatar's drawing, not the whole scene's.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#ifndef hifi_RenderLocalShadowTask_h
#define hifi_RenderLocalShadowTask_h

#include <vector>

#include <gpu/Framebuffer.h>
#include <gpu/Pipeline.h>

#include <render/CullTask.h>

#include "LightingModel.h"
#include "LightStage.h"
#include "RenderShadowTask.h"

class ViewFrustum;

// The atlas the shadow maps are drawn into, the array the cookies are copied into, the table the shaders read both
// by, and which light has which tiles and cookie
class LocalShadows {
public:
    static const int ATLAS_SIZE;            // texels a side
    static const int SPOT_TILE_SIZE;        // a spot light's shadow map
    static const int SPOT_TILE_COUNT;       // spot lights with a shadow map at once
    static const int FACE_TILE_SIZE;        // one face of a point light's six
    static const int FACE_COUNT;            // the faces of a point light: +X, -X, +Y, -Y, +Z, -Z
    static const int POINT_LIGHT_COUNT;     // point lights with shadow maps at once
    static const int TILE_COUNT;            // tiles in the atlas: the spot tiles, then each point light's faces
    static const int LIGHT_COUNT;           // lights the table covers: the size of the shaders' light array
    static const int COOKIE_WIDTH;          // texels across a layer of the cookie array: a panorama's 2:1 shape, a
    static const int COOKIE_HEIGHT;         // spot light's picture stretched over it
    static const int COOKIE_LAYERS;         // pictures the array holds
    static const int COOKIE_LIGHT_COUNT;    // lights with a cookie in a frame

    // a tile's light this frame, the view from it (a spot light's cone, or one face of a point light), and what the
    // maps drawn in the tile show and when, so that they are drawn again only when that would differ: the working
    // map the shaders read, and, with static caching, the static casters' map it is copied from
    struct Tile {
        graphics::LightPointer light;
        LightStage::Index lightIndex { LightStage::INVALID_INDEX };
        std::shared_ptr<ViewFrustum> frustum;

        bool drawn { false };
        glm::mat4 drawnView;                        // the light's transform the map was drawn from
        glm::mat4 drawnProjection;
        std::vector<render::ItemID> drawnCasters;   // sorted: the dynamic casters, or every caster without static caching
        uint64_t drawnFrame { 0 };                  // the scene frame it was drawn in (render::Scene::getFrame)
        uint64_t drawnStaticFrame { 0 };            // the static map under them (its staticFrame), 0 for none

        bool staticDrawn { false };
        glm::mat4 staticView;
        glm::mat4 staticProjection;
        std::vector<render::ItemID> staticCasters;  // sorted
        uint64_t staticFrame { 0 };

        // whether the working map was drawn over a copy of the static map (layered) or with every caster at once;
        // the frames in a row the tile has been drawn, and kept; and whether it is busy, drawn every frame for a
        // while (something in the light's view keeps moving), until it has been kept for a while
        bool drawnLayered { false };
        int drawnStreak { 0 };
        int keptStreak { 0 };
        bool busy { false };
    };

    // what drawing a tile takes this frame
    struct TilePlan {
        bool redrawStatic { false };          // the static casters' map in the static atlas is drawn again first
        bool copyStatic { false };            // the working tile takes that map (else it is cleared)
        render::ShapeBounds staticCasters;    // those drawn into the static map, when it is drawn again
        render::ShapeBounds dynamicCasters;   // those drawn into the working tile over it: with static caching the
                                              // casters that changed lately, else every caster
    };

    LocalShadows();

    const gpu::TexturePointer& getAtlas() const { return _atlas; }
    const gpu::FramebufferPointer& getFramebuffer() const { return _framebuffer; }
    const gpu::BufferPointer& getTable() const { return _table; }
    // the cookies' array; none until a light has a cookie
    const gpu::TexturePointer& getCookies() const { return _cookies; }

    // a table in which no light has a shadow or a cookie, for when there is no atlas
    static const gpu::BufferPointer& getEmptyTable();

    int getTileCount() const { return (int)_tiles.size(); }
    glm::ivec4 getTileViewport(int tile) const;
    const Tile& getTile(int tile) const { return _tiles[tile]; }
    // the tile of a point light's first face; the other five follow it
    static int firstFaceTile(int point) { return SPOT_TILE_COUNT + point * FACE_COUNT; }
    // the frustum of a point light's face, looking out from the light along the face's axis: 90 degrees plus a guard
    // angle, so that the filtering at a face's edge reads this face's map rather than stopping at its border
    static void faceFrustum(const graphics::Light& light, int face, ViewFrustum& frustum);
    bool hasLights() const;
    bool hasCookies() const { return _cookieCount > 0; }

    // a new frame: no light has a tile or a cookie (the maps drawn in the tiles stay, and their records)
    void clear();
    // gives the tile to the spot light, seen through the frustum, and enters it in the table
    void setSpotTile(int tile, const graphics::LightPointer& light, LightStage::Index lightIndex, const ViewFrustum& frustum);
    // gives the point light its six faces (the tiles from firstFaceTile), the face frustums in face order, and enters
    // the light in the table
    void setPointFaces(int point, const graphics::LightPointer& light, LightStage::Index lightIndex, const ViewFrustum* faces);
    // the tile's plan, or null when the map in the tile still shows what its light sees: the casters in the light's
    // view this frame are those drawn, none has changed or is fading since, and the view itself hasn't moved. With
    // static caching a caster unchanged for a while counts as static and goes in the static map, drawn again only
    // when those casters or the view change, so that a moving avatar costs its own drawing over a copy of that map.
    // The records are brought up to date for the maps about to be drawn; caching off, every tile is drawn every frame.
    // A busy tile, one drawn every frame anyway, draws every caster at once rather than over a copy of the static map
    // when it has no more static casters than a copy costs draws (setBlitCostInDraws)
    const TilePlan* planTile(int tile, const render::ShapeBounds& casters, const render::Scene& scene);
    void setCaching(bool caching) { _caching = caching; }
    void setStaticCaching(bool staticCaching) { _staticCaching = staticCaching; }
    void setBlitCostInDraws(int draws) { _blitCostInDraws = draws; }
    // the static atlas's framebuffer; none until static caching has been used
    const gpu::FramebufferPointer& getStaticFramebuffer() const { return _staticFramebuffer; }
    // copies the static atlas's tile into the working atlas's tile, texel for texel (a depth blit)
    void copyStaticMap(gpu::Batch& batch, const glm::ivec4& viewport);
    // gives the spot light its cookie, the picture projected through the frustum, in the layer that holds the picture
    // (kept from earlier frames); false when every layer holds another light's picture this frame
    bool setSpotCookie(LightStage::Index lightIndex, const gpu::TexturePointer& picture, const ViewFrustum& frustum);
    // gives the point light its cookie, the picture hung around the light as a panorama in the light's frame (the
    // matrix takes world space into it); false as above
    bool setPointCookie(LightStage::Index lightIndex, const gpu::TexturePointer& picture, const glm::mat4& worldToLight);
    // some picture in use this frame is new to its layer, or due a refresh
    bool cookiesDue() const;
    // copies those pictures into their layers, then makes the array's mips
    void drawCookies(gpu::Batch& batch);
    // the table as this frame has filled it, for the shaders
    void uploadTable();

private:
    // a layer of the cookie array, and the picture in it
    struct CookieLayer {
        std::weak_ptr<gpu::Texture> picture;
        uint64_t lastUsed { 0 };   // the frame
        bool due { false };        // to be copied in
    };
    int cookieLayer(const gpu::TexturePointer& picture);
    // the light's cookie entry in the table: the number of a cookie whose entry holds the matrix, the picture's layer
    // and whether it is a panorama
    bool addCookie(LightStage::Index lightIndex, const gpu::TexturePointer& picture, const glm::mat4& matrix, bool panorama);
    // the tile's light and view, and the tile's entry in the table (not the light's)
    void fillTile(int tile, const graphics::LightPointer& light, LightStage::Index lightIndex, const ViewFrustum& frustum);
    // the static atlas, made when static caching is first used
    void ensureStaticAtlas();

    gpu::TexturePointer _atlas;
    gpu::FramebufferPointer _framebuffer;
    gpu::BufferPointer _table;
    std::vector<glm::vec4> _tableData;
    std::vector<Tile> _tiles;
    std::vector<render::ItemID> _casterIds;   // scratch for planTile
    std::vector<render::ItemID> _staticIds;
    TilePlan _plan;
    bool _caching { true };
    bool _staticCaching { true };
    int _blitCostInDraws { 0 };

    gpu::TexturePointer _staticAtlas;
    gpu::FramebufferPointer _staticFramebuffer;

    gpu::TexturePointer _cookies;
    std::vector<gpu::FramebufferPointer> _cookieFramebuffers;   // one per layer
    std::vector<CookieLayer> _cookieLayers;
    gpu::PipelinePointer _cookiePipeline;
    int _cookieCount { 0 };   // lights with a cookie this frame
    uint64_t _frame { 0 };
};

class RenderLocalShadowTask {
public:
    using Input = render::VaryingSet3<LightStage::FramePointer, LightingModelPointer, LightStage::ShadowFramePointer>;
    using JobModel = render::Task::ModelI<RenderLocalShadowTask, Input>;

    void build(JobModel& task, const render::Varying& input, render::Varying& output, uint8_t tagBits = 0x00, uint8_t tagMask = 0x00);
};

class RenderLocalShadowSetupConfig : public render::Job::Config {
    Q_OBJECT
    // whether a tile's map is kept until its light or a caster in its view changes (the default), or drawn every frame
    Q_PROPERTY(bool caching READ isCaching WRITE setCaching NOTIFY dirty)
    // whether the casters that haven't changed for a while are kept in a map of their own, copied under the others
    // each time the tile is drawn, so that a moving avatar doesn't cost the whole map (the default)
    Q_PROPERTY(bool staticCaching READ isStaticCaching WRITE setStaticCaching NOTIFY dirty)
    // whether this view draws local light shadows at all (a mirror's view does without: an atlas each would cost
    // too much)
    Q_PROPERTY(bool localLights READ isLocalLights WRITE setLocalLights NOTIFY dirty)
    // whether every light with Cast Shadows on casts, as before a light's shadows had to be turned on (its Shadows
    // field On or Important): for comparing a world's cost both ways, off by default
    Q_PROPERTY(bool everyLightCasts READ isEveryLightCasts WRITE setEveryLightCasts NOTIFY dirty)
    // what copying a static map into the working atlas costs, in draw calls: a busy tile with no more static casters
    // than this draws them all itself instead of copying their map in. macOS's OpenGL copies slowly (Madders Place,
    // 2026-10-21: 29 tiles drawn whole every frame cost less than 29 copies and their moving casters, about 1,900
    // draws more); elsewhere 0, the copy always, until measured
    Q_PROPERTY(int blitCostInDraws READ getBlitCostInDraws WRITE setBlitCostInDraws NOTIFY dirty)
    // last frame's tiles: busy ones, and those drawn over a copy of their static map
    Q_PROPERTY(int busyTiles READ getBusyTiles NOTIFY dirty)
    Q_PROPERTY(int layeredTiles READ getLayeredTiles NOTIFY dirty)
public:
    bool isCaching() const { return _caching; }
    void setCaching(bool caching) { if (_caching != caching) { _caching = caching; emit dirty(); } }
    bool isStaticCaching() const { return _staticCaching; }
    void setStaticCaching(bool staticCaching) { if (_staticCaching != staticCaching) { _staticCaching = staticCaching; emit dirty(); } }
    bool isLocalLights() const { return _localLights; }
    void setLocalLights(bool localLights) { if (_localLights != localLights) { _localLights = localLights; emit dirty(); } }
    bool isEveryLightCasts() const { return _everyLightCasts; }
    void setEveryLightCasts(bool every) { if (_everyLightCasts != every) { _everyLightCasts = every; emit dirty(); } }
    int getBlitCostInDraws() const { return _blitCostInDraws; }
    void setBlitCostInDraws(int draws) { if (_blitCostInDraws != draws) { _blitCostInDraws = draws; emit dirty(); } }
    int getBusyTiles() const { return _busyTiles; }
    int getLayeredTiles() const { return _layeredTiles; }
    // set by the job each frame, without a dirty signal (which would configure the job again)
    void setTileCounts(int busy, int layered) { _busyTiles = busy; _layeredTiles = layered; }

#if defined(Q_OS_MACOS)
    static const int DEFAULT_BLIT_COST_IN_DRAWS = 128;
#else
    static const int DEFAULT_BLIT_COST_IN_DRAWS = 0;
#endif

signals:
    void dirty();

private:
    bool _caching { true };
    bool _staticCaching { true };
    bool _localLights { true };
    bool _everyLightCasts { false };
    int _blitCostInDraws { DEFAULT_BLIT_COST_IN_DRAWS };
    int _busyTiles { 0 };
    int _layeredTiles { 0 };
};

// Picks the lights and gives each its tiles, and takes in a view wide enough for all of them, for fetching what they
// might shine on; gives the lights with a cookie theirs; stops the task when no light needs a shadow
class RenderLocalShadowSetup {
public:
    using Input = RenderLocalShadowTask::Input;
    using Output = render::VaryingSet3<RenderArgs::RenderMode, glm::ivec2, LocalShadowsPointer>;
    using Config = RenderLocalShadowSetupConfig;
    using JobModel = render::Job::ModelIO<RenderLocalShadowSetup, Input, Output, Config>;

    RenderLocalShadowSetup();
    void configure(const Config& config) {
        _caching = config.isCaching();
        _staticCaching = config.isStaticCaching();
        _localLights = config.isLocalLights();
        _everyLightCasts = config.isEveryLightCasts();
        _blitCostInDraws = config.getBlitCostInDraws();
    }
    void run(const render::RenderContextPointer& renderContext, const Input& input, Output& output);

private:
    std::shared_ptr<ViewFrustum> _coarseFrustum;
    LocalShadowsPointer _localShadows;
    bool _caching { true };
    bool _staticCaching { true };
    bool _localLights { true };
    bool _everyLightCasts { false };
    int _blitCostInDraws { Config::DEFAULT_BLIT_COST_IN_DRAWS };
};

// One tile's turn: takes the view from its light, or selects nothing when the tile has no light this frame. For a
// point light, a turn for the whole light comes before its faces: a view taking in everything the light reaches, so
// that the six faces are culled from the light's casters rather than from every light's.
class RenderLocalShadowSlotSetup {
public:
    using Input = LocalShadowsPointer;
    using Output = render::VaryingSet3<render::ItemFilter, graphics::LightPointer, RenderShadowTask::CullFunctor>;
    using JobModel = render::Job::ModelIO<RenderLocalShadowSlotSetup, Input, Output>;

    RenderLocalShadowSlotSetup(int tile, render::ItemFilter filter, bool wholeLight = false);
    void run(const render::RenderContextPointer& renderContext, const Input& input, Output& output);

private:
    int _tile;
    render::ItemFilter _filter;
    bool _wholeLight;
    std::shared_ptr<ViewFrustum> _reach;   // the whole light's view
};

class RenderLocalShadowMap {
public:
    using Inputs = render::VaryingSet2<render::ShapeBounds, LocalShadowsPointer>;
    using JobModel = render::Job::ModelI<RenderLocalShadowMap, Inputs>;

    RenderLocalShadowMap(render::ShapePlumberPointer shapePlumber, int tile) : _shapePlumber{ shapePlumber }, _tile{ tile } {}
    void run(const render::RenderContextPointer& renderContext, const Inputs& inputs);

private:
    render::ShapePlumberPointer _shapePlumber;
    int _tile;
};

class RenderLocalShadowTeardown {
public:
    using Input = RenderLocalShadowSetup::Output;
    using JobModel = render::Job::ModelI<RenderLocalShadowTeardown, Input>;
    void run(const render::RenderContextPointer& renderContext, const Input& input);
};

#endif // hifi_RenderLocalShadowTask_h
