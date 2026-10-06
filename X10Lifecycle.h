// X10 lifecycle instrumentation (logging only).
// Hooks GameEffects definition/attach pipeline entry points to establish the
// relative order of: PopulateModifierDefinitions -> AddModifierDefinition ->
// gameplay Lua init -> first modifier instantiation.
// No writes, no gameplay changes, no RegisterProcessor use.
//
// Hook set is minimal: Populate (phase boundaries), Add (definitions entering
// the system), DynamicModifier ctors A/B (first instantiation). The
// SimpleModifierDefinition ctor is deliberately NOT hooked: Add covers the
// ordering evidence with a simpler prototype.
#pragma once
#include <cstdint>
#include "HavokScript.h"

namespace X10Lifecycle {
    // Install validated hooks. Fail-closed: unless the loaded GameCore
    // passes identity + per-target signature validation, ZERO hooks are
    // installed (returns false).
    bool Install(uintptr_t gameCoreBase);
    // Called from Hook_RegisterScriptData (gameplay Lua state creation).
    void LogGameplayLuaInit();
    // Lua-callable liveness ping from the probe gameplay script.
    int LuaPing(struct hks::lua_State* L);
    int Register(struct hks::lua_State* L);
}
