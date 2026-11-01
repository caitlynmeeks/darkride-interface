//
//  ShapePlumberTests.cpp
//  tests/render/src
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//  See the accompanying file LICENSE-MIT or https://opensource.org/licenses/MIT
//

#include "ShapePlumberTests.h"

#include <vector>

#include <DependencyManager.h>
#include <Trace.h>
#include <gpu/Shader.h>
#include <gpu/State.h>
#include <render/ShapePipeline.h>
#include <shaders/Shaders.h>

using namespace render;

// render-utils fills its plumbers with these (RenderPipelines.cpp)
void initDeferredPipelines(ShapePlumber& plumber, const ShapePipeline::BatchSetter& batchSetter, const ShapePipeline::ItemSetter& itemSetter);
void initForwardPipelines(ShapePlumber& plumber, const ShapePipeline::BatchSetter& batchSetter, const ShapePipeline::ItemSetter& itemSetter);
void initZPassPipelines(ShapePlumber& plumber, gpu::StatePointer state, const ShapePipeline::BatchSetter& batchSetter, const ShapePipeline::ItemSetter& itemSetter);

QTEST_MAIN(ShapePlumberTests)

namespace {

const unsigned long NUM_KEYS = 1UL << ShapeKey::NUM_FLAGS;

// The table the old ShapePlumber::addPipelineHelper built: every filter, in the order added, expanded into each key it
// matches by flipping the bits outside its mask one at a time; a key's first entry was kept (unordered_map::insert)
using Table = std::vector<const ShapePipeline*>;

void expand(Table& table, const ShapeKey::Filter& filter, ShapeKey key, int bit, const ShapePipeline* pipeline) {
    if (bit < (int)ShapeKey::NUM_FLAGS) {
        expand(table, filter, key, bit + 1, pipeline);
        if (!filter.getMask()[bit]) {
            key._flags.flip(bit);
            expand(table, filter, key, bit + 1, pipeline);
        }
    } else if (!table[key._flags.to_ulong()]) {
        table[key._flags.to_ulong()] = pipeline;
    }
}

Table oldTable(const ShapePlumber& plumber) {
    Table table(NUM_KEYS, nullptr);
    for (const auto& filtered : plumber.getFilteredPipelines()) {
        expand(table, filtered.filter, ShapeKey(filtered.filter.getFlags()), 0, filtered.pipeline.get());
    }
    return table;
}

// Every key in the key space looked up, against the old table: the number of keys that differ (the first few printed)
int compareAllKeys(const ShapePlumber& plumber) {
    const Table table = oldTable(plumber);
    int differences = 0;
    for (unsigned long flags = 0; flags < NUM_KEYS; flags++) {
        const ShapeKey key { ShapeKey::Flags(flags) };
        if (plumber.findPipeline(key).get() != table[flags]) {
            if (differences++ < 5) {
                qWarning() << "key" << key << "looks up a different pipeline than the old table had";
            }
        }
    }
    return differences;
}

ShapeKey::Filter::Builder anyKey() {
    return ShapeKey::Filter::Builder();
}

void addTestPipeline(ShapePlumber& plumber, const ShapeKey::Filter& filter) {
    plumber.addPipeline(filter, gpu::Shader::createProgram(shader::render_utils::program::model_shadow), std::make_shared<gpu::State>());
}

}

void ShapePlumberTests::initTestCase() {
    DependencyManager::set<tracing::Tracer>();  // addPipeline profiles itself
    QCOMPARE((int)ShapeKey::NUM_FLAGS, 22);  // the whole key space, 4 M keys, is walked below
}

void ShapePlumberTests::zPassPlumberMatchesExpansion() {
    // the shadow, highlight and local shadow passes' plumbers: a filter leaves 20 of the 22 bits free (1 M keys)
    ShapePlumber plumber;
    initZPassPipelines(plumber, std::make_shared<gpu::State>(), nullptr, nullptr);
    QVERIFY(!plumber.getFilteredPipelines().empty());
    QCOMPARE(plumber.getPipelineMapSize(), (size_t)0);
    QCOMPARE(compareAllKeys(plumber), 0);
}

void ShapePlumberTests::deferredPlumberMatchesExpansion() {
    ShapePlumber plumber;
    initDeferredPipelines(plumber, nullptr, nullptr);
    QVERIFY(!plumber.getFilteredPipelines().empty());
    QCOMPARE(compareAllKeys(plumber), 0);
}

void ShapePlumberTests::forwardPlumberMatchesExpansion() {
    ShapePlumber plumber;
    initForwardPipelines(plumber, nullptr, nullptr);
    QVERIFY(!plumber.getFilteredPipelines().empty());
    QCOMPARE(compareAllKeys(plumber), 0);
}

void ShapePlumberTests::customKeyWithoutFactoryTakesBuiltInPipeline() {
    // a particle or terrain key in the shadow pass, which has no custom factory, takes the plain shadow pipeline
    ShapePlumber plumber;
    initZPassPipelines(plumber, std::make_shared<gpu::State>(), nullptr, nullptr);
    const ShapeKey customKey { ShapeKey::Builder().withCustom(1) };
    QVERIFY(customKey.isCustom());
    QVERIFY(plumber.findPipeline(customKey) != nullptr);
    QVERIFY(plumber.findPipeline(customKey) == plumber.getFilteredPipelines().front().pipeline);
    QVERIFY(plumber.findPipeline(customKey).get() == oldTable(plumber)[customKey._flags.to_ulong()]);
}

void ShapePlumberTests::firstAddedFilterWins() {
    // a narrow filter then a wide one: the narrow one's keys keep it, the rest take the wide one
    ShapePlumber narrowFirst;
    addTestPipeline(narrowFirst, anyKey().withoutFade());
    addTestPipeline(narrowFirst, anyKey());
    const auto& narrow = narrowFirst.getFilteredPipelines()[0].pipeline;
    const auto& wide = narrowFirst.getFilteredPipelines()[1].pipeline;
    QVERIFY(narrowFirst.findPipeline(ShapeKey::Builder()) == narrow);
    QVERIFY(narrowFirst.findPipeline(ShapeKey::Builder().withFade()) == wide);
    QCOMPARE(compareAllKeys(narrowFirst), 0);

    // the wide one first: it takes every key
    ShapePlumber wideFirst;
    addTestPipeline(wideFirst, anyKey());
    addTestPipeline(wideFirst, anyKey().withoutFade());
    QVERIFY(wideFirst.findPipeline(ShapeKey::Builder()) == wideFirst.getFilteredPipelines()[0].pipeline);
    QVERIFY(wideFirst.findPipeline(ShapeKey::Builder().withFade()) == wideFirst.getFilteredPipelines()[0].pipeline);
    QCOMPARE(compareAllKeys(wideFirst), 0);
}

void ShapePlumberTests::laterFilterServesUnmatchedKey() {
    // a key no filter matches has no pipeline, and gets one once a matching filter is added
    ShapePlumber plumber;
    addTestPipeline(plumber, anyKey().withoutFade());
    const ShapeKey fadeKey { ShapeKey::Builder().withFade() };
    QVERIFY(plumber.findPipeline(fadeKey) == nullptr);
    addTestPipeline(plumber, anyKey().withFade());
    QVERIFY(plumber.findPipeline(fadeKey) == plumber.getFilteredPipelines()[1].pipeline);
}

void ShapePlumberTests::mapHoldsOnlyKeysLookedUp() {
    ShapePlumber plumber;
    initZPassPipelines(plumber, std::make_shared<gpu::State>(), nullptr, nullptr);
    QCOMPARE(plumber.getPipelineMapSize(), (size_t)0);
    plumber.findPipeline(ShapeKey::Builder());
    plumber.findPipeline(ShapeKey::Builder());
    plumber.findPipeline(ShapeKey::Builder().withFade());
    QCOMPARE(plumber.getPipelineMapSize(), (size_t)2);
}
