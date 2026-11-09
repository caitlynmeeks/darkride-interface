//
//  EntityScriptRights.h
//  libraries/entities/src
//
//  Whether this client's own rights let a change to an entity's scripts reach everyone in its world, by the entity
//  server's rule (EntityTree::senderMayChange, and the keepScriptsOut it applies to people who may only make temporary
//  entities): the entity unlocked, and the person one of the world's editors (who may change locks) or allowed to make
//  permanent entities. The Code panel's Save and its server-script reading, and Make's opening of scripts, ask this one
//  helper (security's should-fix, 11-06); the server applies its own rule whatever the client thinks.
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//  See the accompanying file LICENSE-MIT or https://opensource.org/licenses/MIT
//

#ifndef hifi_EntityScriptRights_h
#define hifi_EntityScriptRights_h

namespace entity_script_rights {

// this client, in the world it's in now: may it change scripts at all (an editor, or permanent entities allowed)
bool mayChangeScripts();

// and this entity's: unlocked too
bool mayChangeScriptsOf(bool entityLocked);

}

#endif // hifi_EntityScriptRights_h
