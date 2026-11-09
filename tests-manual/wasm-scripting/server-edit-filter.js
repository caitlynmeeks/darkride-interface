//
//  server-edit-filter.js
//  tests-manual/wasm-scripting
//
//  The entity edit filter of server-check.sh: an edit that names an entity "forbidden" is refused, and an entity is
//  kept below y = 5 (near enough for the editing client to be sent the result). The entity server runs it.
//
//  Distributed under the Apache License, Version 2.0.
//

function filter(properties, type) {
    if (properties.name === "forbidden") {
        return false;
    }
    if (properties.position && properties.position.y > 5) {
        properties.position.y = 5;
    }
    return properties;
}
