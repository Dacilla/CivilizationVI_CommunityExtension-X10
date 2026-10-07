// X10 native write test: lifecycle instrumentation + definition overrides.
// Hooks GameEffects definition pipeline to establish ordering, then rewrites
// SSO-inline modifier arguments pre-attach (fail-closed throughout).
// No RegisterProcessor use.
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
    int LuaPing(struct hks::lua_State* L);
    int Register(struct hks::lua_State* L);
    // Tiny file-log helper for probe Lua: X10Lifecycle.LogMsg(str) appends
    // one line to %TEMP%\X10Probe.log. No game state touched.
    int LuaLogMsg(struct hks::lua_State* L);
    // Shared monotonic log (%TEMP%\X10Lifecycle.log), usable from X10Write.
    void X10Log(const char* fmt, ...);
}
