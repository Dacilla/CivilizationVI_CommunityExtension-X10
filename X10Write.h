// X10 native config reader + definition override writer (test proof).
// Reads X10_PROBE_K via Configuration::Game::GetInstance + TypedVariantMap
// lookup; rewrites SSO-inline argument strings at AddModifierDefinition.
// FAIL-CLOSED throughout: any validation failure disables writes for the
// session and logs the reason. No Lua, no RegisterProcessor, no Mem pokes.
#pragma once
#include <cstdint>

namespace X10Config {
    // Returns true and sets `out` when X10_PROBE_K resolves to a finite
    // double in range. Logs CONFIG lines either way.
    bool TryGetProbeK(double& out);
}

namespace X10Write {
    void InitBase(uintptr_t base);
    struct Override {        const char* modifierId;
        const char* argument;
        const char* official;
        int family; // 0 = additive (*k, %.6g), 1 = combat (%.2f canonical)
    };
    // Test-only table (official baseline values; replaced by manifest
    // generation once the path is proven).
    extern const Override kOverrides[3];
    // Called from the AddModifierDefinition hook with the definition pointer.
    // Performs validated SSO-inline rewrites; logs [X10WRITE] per target.
    void OnAddModifierDefinition(void* definition);
    // Must be called after TryGetProbeK succeeds; enables the write path.
    void Arm(double k);
    bool IsArmed();
}
