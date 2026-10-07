// X10 native config reader + production definition writer.
// Reads arbitrary GameConfiguration keys via Configuration::Game::GetInstance
// + TypedVariantMap lookup; rewrites SSO-inline argument strings at
// AddModifierDefinition from the GENERATED production registry.
// FAIL-CLOSED throughout: any validation failure disables writes for the
// session and logs the reason. No Lua, no RegisterProcessor, no Mem pokes.
// No hand-maintained modifier IDs: see X10ProductionRegistry.inc.
#pragma once
#include <cstdint>

namespace X10Config {
    // Generic validated config-double reader. Logs CONFIG lines either way.
    bool TryGetConfigDouble(const char* key, double& out);
    // Multiplier read with source quantization: kErrOut is the FLOAT32
    // half-ULP of the stored k (0 for INT32 multipliers).
    bool TryGetMultiplier(const char* key, double& out, double& kErrOut);
    // Probe compat (X10_PROBE_K).
    bool TryGetProbeK(double& out);
    bool TryGetProbeKEx(double& out, double& kErrOut);
    // Module toggle: numeric 1/0; absent key means default-ON (matching the
    // mod's declared defaults) and is logged.
    bool ModuleEnabled(const char* moduleKey, bool defaultOn);
}

namespace X10Write {
    void InitBase(uintptr_t base);
    // A touched argument, reported for post-Add verification.
    struct Touched {
        void* element;
        char expected[32];
        const char* id;
        const char* arg;
    };
    // Called from the AddModifierDefinition hook with the raw reference
    // slots (d0 = shared_ptr object). Resolves, writes BEFORE orig_Add, and
    // reports all touched elements for post-Add verification.
    void OnAddModifierDefinition(void* d0, void* d1,
                                 Touched* touched, int maxTouched, int* outCount);
    // Post-Add witness: re-read every touched element from the live definition.
    void VerifyStoredAfterAdd(Touched* touched, int count);
    // Must be called after config lookup succeeds; enables the write path.
    // kErr carries the multiplier's source quantization (FLOAT32 half-ULP,
    // 0 for INT32) for the count-like exactness rule.
    void Arm(double k);
    void Arm(double k, const bool* mods);
    void Arm(double k, const bool* mods, double kErr);
    // Fail-closed reset at the start of EVERY PopulateModifierDefinitions:
    // clears armed state, k, module flags, and per-population counters.
    void Disarm();
    // Per-population diagnostics (preserved for exit logging).
    long WritesThisPopulate();
    long MismatchesThisPopulate();
    long SkippedThisPopulate();
    bool IsArmed();
}
