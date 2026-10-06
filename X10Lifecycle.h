// X10 lifecycle instrumentation (logging only).
// Hooks GameEffects definition/attach pipeline entry points to establish the
// relative order of: PopulateModifierDefinitions -> definition construction ->
// AddModifierDefinition -> gameplay Lua init -> first modifier instantiation.
// No writes, no gameplay changes, no RegisterProcessor use.
#pragma once
#include <cstdint>
#include "HavokScript.h"

namespace X10Lifecycle {
    // Install MinHook hooks. Safe to call once; each install failure is
    // logged and non-fatal (that hook is simply skipped).
    void Install(uintptr_t gameCoreBase);
    // Called from Hook_RegisterScriptData (gameplay Lua state creation).
    void LogGameplayLuaInit();
    // Lua-callable liveness ping from the probe gameplay script.
    int LuaPing(struct hks::lua_State* L);
    int Register(struct hks::lua_State* L);
}
