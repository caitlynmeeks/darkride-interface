//
//  ShapePlumberTests.h
//  tests/render/src
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//  See the accompanying file LICENSE-MIT or https://opensource.org/licenses/MIT
//

#ifndef hifi_ShapePlumberTests_h
#define hifi_ShapePlumberTests_h

#include <QtTest/QtTest>

// ShapePlumber looks a key's pipeline up the first time it's drawn. These check that every key gets the pipeline the
// old way gave it (each filter expanded into a map entry per key it matches, in the order the filters were added, the
// first entry kept), over the whole key space, for the engine's own plumbers and for overlapping filters.
class ShapePlumberTests : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void zPassPlumberMatchesExpansion();
    void deferredPlumberMatchesExpansion();
    void forwardPlumberMatchesExpansion();
    void customKeyWithoutFactoryTakesBuiltInPipeline();
    void firstAddedFilterWins();
    void laterFilterServesUnmatchedKey();
    void mapHoldsOnlyKeysLookedUp();
};

#endif // hifi_ShapePlumberTests_h
