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
    extern const Override kOverrides[4];
    static const int kOverrideCount = 4;
    // Called from the AddModifierDefinition hook with the raw reference
    // slots (d0 = shared_ptr object). Resolves, writes BEFORE orig_Add, and
    // reports the touched element for post-Add verification.
    void OnAddModifierDefinition(void* d0, void* d1,
                                 void** outEl, char* outExpected, size_t expCap,
                                 const char** outId, const char** outArg);
    // Post-Add witness: re-read the touched element from the live definition.
    void VerifyStoredAfterAdd(void* el, const char* expected,
                              const char* id, const char* arg);
    // Must be called after TryGetProbeK succeeds; enables the write path.
    void Arm(double k);
    // Fail-closed reset at the start of EVERY PopulateModifierDefinitions:
    // clears armed state, k, and the per-population write count.
    void Disarm();
    // Per-population successful writes (preserved for exit diagnostics).
    long WritesThisPopulate();
    bool IsArmed();
}
