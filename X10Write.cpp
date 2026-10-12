// X10 native config reader + definition override writer (see X10Write.h).
//
// All build/ABI-specific numbers live in the active GameCoreCompatibilityProfile
// (X10Compat.h); this TU holds no literal target RVAs, timestamps, sizes, or
// layout offsets. Reference basis is documented per field in X10Compat.h and in
// civ6-x10 docs/NATIVE_HOOK_VALIDATION.md.
//   Config reader: global GetInstance + string hash from the profile; variant
//     lookup is the virtual manager method at the profile's vtable slot,
//     exactly as GetGameSpeedType/GetGameMode/GetStartEra call it. The game /
//     root manager members are MANAGER OBJECTS (first qword is a vtable), NOT
//     raw maps: calling TypedVariantMap::FindVariant on them walks the wrong
//     object (regression-covered).
//   Variant decoding: typed switch over scalar representations (INT32 mask
//     branch, FLOAT32 branch checked first since the engine mask also contains
//     the float id while the engine's own callers only pass int-typed keys;
//     fractional Lua numbers commit as float32). Unknown types fail closed,
//     never reaching a string reader.
//   Definition writer: id string + argument vector from the profile;
//     ArgumentDefinition elements strided per profile with SSO name/value;
//     only SSO-inline values whose replacement fits are rewritten, else skip
//     + log. Every shape is re-validated per element before any write.
#include "X10Write.h"
#include "X10Transforms.h"
#include "X10/X10ProductionRegistry.inc"
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cctype>
#include <cmath>
#include "X10Lifecycle.h"

namespace {
    uintptr_t g_base = 0;

    template <typename Fn>
    Fn At(uintptr_t rva) {
        return reinterpret_cast<Fn>(g_base + rva);
    }

    // ---- config reader ----
    typedef void* (__cdecl* GetInstanceFn)();
    typedef uint32_t(__cdecl* HashFn)(const char* s);
    // Virtual variant lookup, replicating GetGameSpeedType exactly:
    // manager = *(inst+off); variant = manager->vtable[slot](manager, key),
    // with instance offsets + vtable slot from the active profile.
    typedef void* (__cdecl* LookupFn)(void* manager, uint32_t key);

    static void* LookupVariant(void* manager, uint32_t key) {
        auto* prof = X10Lifecycle::ActiveProfile();
        if (!prof) return nullptr;
        __try {
            if (!manager) return nullptr;
            void* vt = *reinterpret_cast<void**>(manager);
            if (!vt) return nullptr;
            void* fn = reinterpret_cast<void**>(vt)[prof->lookupVtableOff / 8];
            if (!fn) return nullptr;
            return reinterpret_cast<LookupFn>(fn)(manager, key);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return nullptr;
        }
    }

    // NOTE: there is deliberately no generic string-object reader on the
    // config path. Unknown variant types fail closed (see default branch).

    // Typed switch over the engine's scalar variant representations.
    // Provenance:
    //  INT32: GetGameSpeedType/GetGameMode mask (ids<=0x15 in 0x30002e,
    //    flags bit 0x1d clear), payload dword@+8. NOTE id 4 is EXCLUDED
    //    below even though the mask contains it: the engine's own callers
    //    only pass int-typed keys there, while fractional Lua numbers are
    //    stored as float32 (cvtsd2ss + virtual store, TableToTypedVariantMap
    //    0x9af376), so id 4 must decode as float, never as int bits.
    //  FLOAT32 id=4: fractional setup values are committed as float32
    //    (live: 7.3000001907349 == float32(7.3) widened; no cvtss2sd-free
    //    alternative exists; type-name table order Bool,Int,UInt,Float,...).
    //    Payload float@+8, widened to double, finite + range checked.
    //  Anything else (incl. strings — no string-typed setup value is in
    //  play for this key): fail closed with exact type id / flags / payload
    //  logged. Unknown types are NEVER passed through a string reader.
    static bool VariantToDouble(void* variant, double& out,
                                unsigned& typeOut, uint32_t& flagsOut,
                                const char** kindOut, bool* isFloatOut = nullptr) {
        typeOut = 0xFFFF;
        flagsOut = 0;
        *kindOut = "unknown";
        if (isFloatOut) *isFloatOut = false;
        typeOut = 0xFFFF;
        flagsOut = 0;
        *kindOut = "unknown";
        auto* prof = X10Lifecycle::ActiveProfile();
        if (!prof) return false;
        __try {
            uint8_t* v = reinterpret_cast<uint8_t*>(variant);
            uint16_t type = *reinterpret_cast<uint16_t*>(v + prof->variantTypeOff);
            uint32_t flags = *reinterpret_cast<uint32_t*>(v + prof->variantFlagsOff);
            typeOut = type;
            flagsOut = flags;
            if (type == prof->floatId) {
                float f = 0;
                memcpy(&f, v + prof->variantPayloadOff, sizeof(f));
                if (f == f && f >= 0 && f <= 100) {
                    out = (double)f;
                    *kindOut = "FLOAT32";
                    if (isFloatOut) *isFloatOut = true;
                    return true;
                }
                return false; // non-finite/out-of-range float: fail closed
            }
            bool inMask = false;
            for (int i = 0; i < 8 && prof->intMaskIds[i] != 0xFFFF; i++) {
                if (prof->intMaskIds[i] == type) {
                    inMask = true;
                    break;
                }
            }
            if (!((flags >> 0x1d) & 1) && inMask) {
                out = (double)*reinterpret_cast<int32_t*>(v + prof->variantPayloadOff);
                *kindOut = "INT32";
                return true; // INT32
            }
            return false;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }
}

namespace X10Config {
    static void* FindConfigVariant(const char* key);
    bool TryGetConfigDouble(const char* key, double& out) {
        double err = 0;
        return TryGetMultiplier(key, out, err);
    }

    // ---- Phase 7C.2 configuration-state model (declared in X10Write.h;
    // ABSENT = no variant, UNSET = type-0/flags-0/zero-payload null shape,
    // VALID, INVALID). Type 0 is absent from the verbatim engine int mask,
    // so it can never decode as INT32 0.
    static bool IsUnsetConfigVariant(unsigned type, uint32_t flags,
                                     bool payloadZero) {
        return type == 0 && flags == 0 && payloadZero;
    }

    ConfigState FindConfigState(const char* key, unsigned* typeOut,
                                uint32_t* flagsOut) {
        if (typeOut) *typeOut = 0xFFFF;
        if (flagsOut) *flagsOut = 0;
        void* variant = FindConfigVariant(key);
        if (!variant) return ConfigState::ABSENT;
        auto* prof = X10Lifecycle::ActiveProfile();
        if (!prof) return ConfigState::ABSENT;
        uint16_t type = 0xFFFF;
        uint32_t flags = 0;
        uint32_t payload = 0;
        __try {
            uint8_t* v = reinterpret_cast<uint8_t*>(variant);
            type = *reinterpret_cast<uint16_t*>(v + prof->variantTypeOff);
            flags = *reinterpret_cast<uint32_t*>(v + prof->variantFlagsOff);
            payload = *reinterpret_cast<uint32_t*>(v + prof->variantPayloadOff);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return ConfigState::INVALID;
        }
        if (typeOut) *typeOut = type;
        if (flagsOut) *flagsOut = flags;
        if (type == 0 && flags == 0)
            return IsUnsetConfigVariant(type, flags, payload == 0) ?
                ConfigState::UNSET : ConfigState::INVALID;
        return ConfigState::VALID;
    }

    // Strict boolean reader over the PROVEN integer scalar path (native
    // bool SetupParameters arrive as integer variants; no raw BOOL payload
    // decoding is invented). Exact 0 -> false, exact 1 -> true; anything
    // else numeric is INVALID. ABSENT/UNSET are reported (not defaulted)
    // so callers apply declared-mod-default policy explicitly.
    bool TryGetConfigBool(const char* key, bool& out, ConfigState& stOut) {
        out = false;
        stOut = FindConfigState(key, nullptr, nullptr);
        if (stOut != ConfigState::VALID) return false;
        double d = 0;
        unsigned vtype = 0xFFFF;
        uint32_t vflags = 0;
        const char* kind = "unknown";
        if (!VariantToDouble(
                FindConfigVariant(key), d, vtype, vflags, &kind)) {
            stOut = ConfigState::INVALID;
            return false;
        }
        if (d == 0.0) { out = false; return true; }
        if (d == 1.0) { out = true; return true; }
        X10Lifecycle::X10Log("CONFIG key=%s non-boolean numeric %g: rejected",
               key, d);
        stOut = ConfigState::INVALID;
        return false;
    }

    // Strict preset reader: exactly the shipped dropdown values. 101 is
    // Custom (not a multiplier) and never passes through the 0-100
    // multiplier range check. Any other integer is INVALID.
    bool TryGetMultiplierPreset(const char* key, int& presetOut,
                                ConfigState& stOut) {
        presetOut = -1;
        stOut = FindConfigState(key, nullptr, nullptr);
        if (stOut != ConfigState::VALID) return false;
        double d = 0;
        unsigned vtype = 0xFFFF;
        uint32_t vflags = 0;
        const char* kind = "unknown";
        if (!VariantToDouble(
                FindConfigVariant(key), d, vtype, vflags, &kind)) {
            stOut = ConfigState::INVALID;
            return false;
        }
        static const int kAllowed[] = {0, 2, 3, 5, 10, 20, 50, 100, 101};
        for (size_t i = 0; i < sizeof(kAllowed) / sizeof(kAllowed[0]); i++) {
            if (d == (double)kAllowed[i]) {
                presetOut = kAllowed[i];
                return true;
            }
        }
        X10Lifecycle::X10Log("CONFIG preset %s non-preset value %g: rejected",
               key, d);
        stOut = ConfigState::INVALID;
        return false;
    }

    // Effective-multiplier resolution (Phase 7C.2, specified order):
    // 1. new preset VALID standard value -> use it (Off included);
    // 2. preset 101 (Custom) -> require valid custom numeric 0-100;
    // 3. preset ABSENT/UNSET -> legacy X10_MULTIPLIER path (INT32/FLOAT32
    //    as proven, plus the X10_PROBE_K experiment fallback);
    // 4. preset present-but-INVALID, or Custom blank/malformed/out-of-range
    //    -> fail closed with an explicit reason (never silent fallback,
    //    never clamp/round, never garbage-as-zero). Returns true iff X10
    //    should arm with *kOut (k==0 is normalized to disarmed by the
    //    caller, matching the long-standing k==0 convention).
    bool ResolveEffectiveMultiplier(double& kOut, double& kErrOut) {
        kOut = 0;
        kErrOut = 0;
        int preset = -1;
        ConfigState pst = ConfigState::ABSENT;
        if (TryGetMultiplierPreset("X10_MULTIPLIER_PRESET", preset, pst)) {
            if (preset == 101) {
                double ck = 0;
                double ckErr = 0;
                if (TryGetMultiplier("X10_MULTIPLIER", ck, ckErr)) {
                    kOut = ck;
                    kErrOut = ckErr;
                    X10Lifecycle::X10Log("X10 effective multiplier: custom %g",
                           ck);
                    return true;
                }
                X10Lifecycle::X10Log("X10 Custom selected but X10_MULTIPLIER "
                       "is blank, malformed, or outside 0-100 "
                       "(decimals need a period, e.g. 7.3): "
                       "X10 remains disabled for safety");
                return false;
            }
            if (preset == 0) {
                X10Lifecycle::X10Log("X10 multiplier preset OFF; "
                       "definition transforms disabled");
                return false;
            }
            kOut = (double)preset;
            kErrOut = 0;
            X10Lifecycle::X10Log("X10 effective multiplier: preset %d",
                   preset);
            return true;
        }
        if (pst == ConfigState::ABSENT || pst == ConfigState::UNSET) {
            if (TryGetMultiplier("X10_MULTIPLIER", kOut, kErrOut))
                return true;
            if (TryGetProbeKEx(kOut, kErrOut))
                return true;
            return false;
        }
        X10Lifecycle::X10Log("X10 preset present but invalid: no legacy "
               "fallback; X10 remains disabled for safety");
        return false;
    }

    // Multiplier read with source quantization: kErr is the FLOAT32
    // half-ULP of the stored k (0 for INT32 multipliers), consumed by the
    // count-like exactness rule so stored-float k still accepts exact
    // integer results without a coarse epsilon.
    bool TryGetMultiplier(const char* key, double& out, double& kErrOut) {
        out = 0;
        kErrOut = 0;
        void* variant = FindConfigVariant(key);
        if (!variant) {
            X10Lifecycle::X10Log("CONFIG key=%s found=false reason=absent-or-unreachable", key);
            return false;
        }
            double d = 0;
            unsigned vtype = 0xFFFF;
            uint32_t vflags = 0;
            const char* kind = "unknown";
            bool isFloat = false;
            if (VariantToDouble(variant, d, vtype, vflags, &kind, &isFloat)) {
                if (d >= 0 && d <= 100 && d == d) {
                    out = d;
                    if (isFloat) {
                        float kf = (float)d;
                        float hi = nextafterf(kf, INFINITY);
                        float lo = nextafterf(kf, -INFINITY);
                        double hiErr = (double)(hi - kf);
                        double loErr = (double)(kf - lo);
                        kErrOut = (hiErr > loErr ? hiErr : loErr) / 2.0;
                    }
                    X10Lifecycle::X10Log("CONFIG key=%s found=true type=%s id=%u flags=0x%x numeric=%.15g kerr=%.3g", key, kind, vtype, vflags, d, kErrOut);
                    return true;
                }
                X10Lifecycle::X10Log("CONFIG key=%s found=true type=%s id=%u raw=%f reason=out-of-range", key, kind, vtype, d);
                return false;
            }
            X10Lifecycle::X10Log("CONFIG key=%s found=true reason=undecodable-variant id=%u flags=0x%x", key, vtype, vflags);
            __try {
                uint8_t* v = reinterpret_cast<uint8_t*>(variant);
                uint8_t b[16] = {};
                memcpy(b, v + 8, sizeof(b));
                X10Lifecycle::X10Log("CONFIG key=%s variant type=%u flags=0x%x payload=%02x%02x%02x%02x%02x%02x%02x%02x...",
                       key, vtype, vflags, b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
            }
            return false;
    }

    // Raw variant lookup shared by value reads and presence probes.
    // Returns the variant pointer, or null when absent/unreachable.
    // All addresses come from the active compatibility profile.
    static void* FindConfigVariant(const char* key) {
        auto* prof = X10Lifecycle::ActiveProfile();
        if (!prof) return nullptr;
        auto getInstance = At<GetInstanceFn>(prof->getInstanceRva);
        auto hashFn = At<HashFn>(prof->hashRva);
        void* inst = nullptr;
        __try {
            inst = getInstance();
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return nullptr;
        }
        if (!inst) return nullptr;
        uint32_t h = 0;
        __try {
            h = hashFn(key);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return nullptr;
        }
        const uintptr_t mgrs[2] = {prof->gameVariantsOff, prof->rootVariantsOff};
        for (int m = 0; m < 2; m++) {
            void* mgr = nullptr;
            __try {
                mgr = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(inst) + mgrs[m]);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                continue;
            }
            if (!mgr) continue;
            __try {
                void* variant = LookupVariant(mgr, h);
                if (variant) return variant;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                continue;
            }
        }
        return nullptr;
    }

    bool TryGetProbeK(double& out) {
        return TryGetConfigDouble("X10_PROBE_K", out);
    }

    bool TryGetProbeKEx(double& out, double& kErrOut) {
        return TryGetMultiplier("X10_PROBE_K", out, kErrOut);
    }

    bool ModuleEnabled(const char* moduleKey, bool defaultOn) {
        char full[96] = {};
        snprintf(full, sizeof(full), "X10_MODULE_%s", moduleKey);
        for (char* p = full; *p; p++) {
            if (*p >= 'a' && *p <= 'z') *p -= 32;
        }
        // Absent or UNSET key: default per declared mod defaults, loudly.
        // A present-but-unreadable key fails closed (disabled). Strict bool
        // decoding also accepts the legacy INT32 0/1 module parameters.
        bool on = false;
        ConfigState st = ConfigState::ABSENT;
        if (TryGetConfigBool(full, on, st)) {
            X10Lifecycle::X10Log("CONFIG module %s %s", moduleKey, on ? "ON" : "OFF");
            return on;
        }
        if (st == ConfigState::ABSENT || st == ConfigState::UNSET) {
            X10Lifecycle::X10Log("CONFIG module %s absent/unset: default %s", moduleKey,
                   defaultOn ? "ON" : "OFF");
            return defaultOn;
        }
        X10Lifecycle::X10Log("CONFIG module %s unreadable: disabled (fail-closed)",
               moduleKey);
        return false;
    }
}


namespace X10Write {
    // Production override registry: GENERATED from reviewed manifests
    // (civ6x10.production, X10/X10ProductionRegistry.inc). No hand-maintained
    // modifier IDs.

    static double g_k = 0;
    static double g_kErr = 0; // source quantization half-ULP of g_k (0 = INT32)
    static bool g_armed = false;
    // 13 supported modules (Phase 8B): traits, policies, governments,
    // pantheons, wonders, governors, suzerain, promotions, dedications,
    // modes, religion, greatpeople, projects.
    static bool s_modEnabled[13] = {true, true, true, true, true, true, true,
                                    true, true, true, true, true, true};
    static volatile LONG s_writesSession = 0;
    static volatile LONG s_writesPopulate = 0;
    static volatile LONG s_mismatchesPopulate = 0;
    static volatile LONG s_skippedPopulate = 0;
    static volatile LONG s_transformRefused = 0;
    static volatile LONG s_postAddMatch = 0;
    static volatile LONG s_postAddMismatch = 0;
    static volatile LONG s_postAddUnreadable = 0;
    // REPEAT copies left inert by design (copy index above integral k):
    // neither writes nor failures, counted separately for validation.
    static volatile LONG s_repeatIdle = 0;
    // Store-lookup path proven at Install (getDefRva validated). When false,
    // Verify degrades loudly: every touched entry counts unreadable, never
    // MATCH. The writer path is unaffected.
    static bool s_storeLookupProven = false;

    void Arm(double k, const bool* mods, double kErr) {
        g_k = k;
        g_kErr = kErr;
        g_armed = true;
        for (int i = 0; i < 13; i++) s_modEnabled[i] = mods[i];
    }

    void Arm(double k, const bool* mods) {
        Arm(k, mods, 0.0);
    }

    // Legacy single-arg arm (probe compat): all supported modules on,
    // zero quantization (strict integer exactness).
    void Arm(double k) {
        static const bool all[13] = {true, true, true, true, true, true, true,
            true, true, true, true, true, true};
        Arm(k, all, 0.0);
    }

    void Disarm() {
        g_armed = false;
        g_k = 0;
        g_kErr = 0;
        s_writesPopulate = 0;
        s_mismatchesPopulate = 0;
        s_skippedPopulate = 0;
        s_transformRefused = 0;
        s_postAddMatch = 0;
        s_postAddMismatch = 0;
        s_postAddUnreadable = 0;
        s_repeatIdle = 0;
    }

    bool IsArmed() { return g_armed; }

    void SetStoreLookupProven(bool proven) { s_storeLookupProven = proven; }

    long WritesThisPopulate() { return (long)s_writesPopulate; }
    long MismatchesThisPopulate() { return (long)s_mismatchesPopulate; }
    long SkippedThisPopulate() { return (long)s_skippedPopulate; }
    long TransformRefusedThisPopulate() { return (long)s_transformRefused; }
    long PostAddMatchThisPopulate() { return (long)s_postAddMatch; }
    long PostAddMismatchThisPopulate() { return (long)s_postAddMismatch; }
    long PostAddUnreadableThisPopulate() { return (long)s_postAddUnreadable; }
    long RepeatIdleThisPopulate() { return (long)s_repeatIdle; }

    static bool BoundedStringRead(void* s, char* out, size_t cap,
                                  size_t& lenOut);

    static bool SsoRead(void* s, char* out, size_t cap, size_t& lenOut) {
        return BoundedStringRead(s, out, cap, lenOut);
    }

    // Bounded READ-ONLY string reader: SSO-inline and heap-backed (reads only;
    // heap WRITES remain refused — see SsoWrite). Layout from the profile.
    static bool BoundedStringRead(void* s, char* out, size_t cap,
                                  size_t& lenOut) {
        auto* prof = X10Lifecycle::ActiveProfile();
        if (!prof) return false;
        __try {
            uint8_t* p = reinterpret_cast<uint8_t*>(s);
            uint64_t size = *reinterpret_cast<uint64_t*>(p + prof->ssoSizeOff);
            uint64_t capa = *reinterpret_cast<uint64_t*>(p + prof->ssoCapOff);
            if (size > 256) return false;
            const char* data = nullptr;
            if (capa < prof->ssoInlineThreshold) {
                data = reinterpret_cast<const char*>(p);
            } else {
                if (capa < size) return false;
                data = *reinterpret_cast<const char* const*>(p);
                if (!data) return false;
            }
            for (uint64_t i = 0; i < size; i++) {
                char c = data[i];
                if (c < 32 || c > 126) return false;
            }
            if (data[size] != '\0') return false;
            if (size + 1 > cap) return false;
            memcpy(out, data, (size_t)size + 1);
            lenOut = (size_t)size;
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    static bool SsoWrite(void* s, const char* text) {
        auto* prof = X10Lifecycle::ActiveProfile();
        if (!prof) return false;
        __try {
            size_t n = strlen(text);
            if (n >= prof->ssoInlineThreshold) return false;
            uint8_t* p = reinterpret_cast<uint8_t*>(s);
            uint64_t capa = *reinterpret_cast<uint64_t*>(p + prof->ssoCapOff);
            if (capa >= prof->ssoInlineThreshold) return false; // heap: never touch
            for (size_t i = 0; i < n; i++) {
                if (text[i] < 32 || text[i] > 126) return false;
            }
            memcpy(p, text, n + 1);
            *reinterpret_cast<uint64_t*>(p + prof->ssoSizeOff) = (uint64_t)n;
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    // Resolves a ModifierDefinitionReference (shared_ptr-like {_Ptr, _Rep})
    // to the raw SimpleModifierDefinition pointer. Fails closed (null).
    // Evidence: AddModifierDefinition dereferences RDX+0 then virtual+0x10,
    // and ref-counts via lock xadd on the second qword (shared_ptr control
    // block uses@+8/weaks@+0xc) — see docs/NATIVE_HOOK_VALIDATION.md.
    static void* ResolveDefinitionReference(void* d0, void* d1) {
        (void)d1;
        __try {
            if (!d0) return nullptr;
            void* def = *reinterpret_cast<void**>(d0);
            if (!def) return nullptr;
            // The definition must itself be a polymorphic object.
            void* vt = *reinterpret_cast<void**>(def);
            if (!vt) return nullptr;
            return def;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return nullptr;
        }
    }

    static int EnabledOwnerMask() {
        // Loop-built: proves bits >= 0x80 and >= 0x100 behave identically
        // to the original seven (no silent truncation; int holds to 2^30).
        int mask = 0;
        for (int i = 0; i < 13; i++) {
            if (s_modEnabled[i]) mask |= (1 << i);
        }
        return mask;
    }

    // Writes ALL matching production entries of one definition (multi-arg
    // capable: refusal of one argument never blocks another eligible one).
    // Shared entries apply only when ALL owning modules are enabled.
    // Returns the write count; fills touched[] up to maxTouched.
    static int WriteDefinition(void* definition, Touched* touched, int maxTouched) {
        int n = 0;
        auto* prof = X10Lifecycle::ActiveProfile();
        if (!prof) return 0;
        __try {
            uint8_t* def = reinterpret_cast<uint8_t*>(definition);
            static char id[256];
            size_t idLen = 0;
            if (!SsoRead(def + prof->defIdOff, id, sizeof(id), idLen)) return 0; // id unreadable: skip
            // Argument vector {begin, end, cap} at the profile offset.
            void** vec = reinterpret_cast<void**>(def + prof->argVecOff);
            uint8_t* begin = reinterpret_cast<uint8_t*>(vec[0]);
            uint8_t* end = reinterpret_cast<uint8_t*>(vec[1]);
            if (!begin || !end || end < begin) return 0;
            size_t count = (size_t)(end - begin) / prof->argStride;
            if (count == 0 || count > prof->maxArgs) return 0; // insane: skip
            int enabled = EnabledOwnerMask();
            for (int t = 0; t < kX10ProductionRegistryCount; t++) {
                const X10RegistryEntry& e = kX10ProductionRegistry[t];
                if (strcmp(id, e.modifierId) != 0) continue;
                if ((e.owners & enabled) != e.owners) {
                    InterlockedIncrement(&s_skippedPopulate);
                    X10Lifecycle::X10Log("[X10WRITE] modifier=%s arg=%s owners=0x%x not-all-enabled skipped",
                           e.modifierId, e.argument, e.owners);
                    continue;
                }
                for (size_t i = 0; i < count; i++) {
                    uint8_t* el = begin + i * prof->argStride;
                    char name[64] = {};
                    size_t nameLen = 0;
                    if (!SsoRead(el + prof->argNameOff, name, sizeof(name), nameLen)) continue;
                    if (strcmp(name, e.argument) != 0) continue;
                    char before[64] = {};
                    size_t beforeLen = 0;
                    if (!SsoRead(el + prof->argValueOff, before, sizeof(before), beforeLen)) continue;
                    if (e.kind == 6 /*REPEAT*/) {
                        // Implicit-+1 slot grants: copies are stored inert
                        // (SLOT_NONE). Copy N activates iff N <= integral k
                        // by rewriting to the real slot type; otherwise it
                        // stays inert (vanilla single slot). Fail closed on
                        // any drift, non-integral k, or bad aux/target.
                        static const char* kInert = "SLOT_NONE";
                        if (strcmp(before, e.official) != 0 ||
                                strcmp(before, kInert) != 0) {
                            InterlockedIncrement(&s_mismatchesPopulate);
                            X10Lifecycle::X10Log("[X10WRITE] modifier=%s arg=%s repeat-not-inert(saw %s) skipped",
                                   e.modifierId, e.argument, before);
                            continue;
                        }
                        int copies = X10Transforms::RepeatCopies(
                            g_k, g_kErr, 99);
                        if (copies < 0) {
                            InterlockedIncrement(&s_transformRefused);
                            X10Lifecycle::X10Log("[X10WRITE] modifier=%s arg=%s repeat-refused (non-integral k=%.6g) skipped",
                                   e.modifierId, e.argument, g_k);
                            continue;
                        }
                        if (e.aux < 2 || e.aux > 100 || !e.target ||
                                !*e.target) {
                            InterlockedIncrement(&s_skippedPopulate);
                            X10Lifecycle::X10Log("[X10WRITE] modifier=%s arg=%s repeat-bad-aux skipped",
                                   e.modifierId, e.argument);
                            continue;
                        }
                        if (e.aux > copies + 1) {
                            InterlockedIncrement(&s_repeatIdle);
                            continue;  // stays inert by design, no log spam
                        }
                        if (!SsoWrite(el + prof->argValueOff, e.target)) {
                            InterlockedIncrement(&s_skippedPopulate);
                            X10Lifecycle::X10Log("[X10WRITE] modifier=%s arg=%s repeat-write-refused (not SSO-safe)", e.modifierId, e.argument);
                            continue;
                        }
                        char after[64] = {};
                        size_t afterLen = 0;
                        SsoRead(el + prof->argValueOff, after, sizeof(after), afterLen);
                        InterlockedIncrement(&s_writesSession);
                        InterlockedIncrement(&s_writesPopulate);
                        X10Lifecycle::X10Log("[X10WRITE] modifier=%s arg=%s official=%s k=%.6g transform=REPEAT requested=%s before=%s after=%s phase=definition-population",
                               e.modifierId, e.argument, e.official, g_k, e.target, before, after);
                        if (n < maxTouched) {
                            strncpy(touched[n].id, id, sizeof(touched[n].id) - 1);
                            touched[n].id[sizeof(touched[n].id) - 1] = '\0';
                            strncpy(touched[n].arg, e.argument, sizeof(touched[n].arg) - 1);
                            touched[n].arg[sizeof(touched[n].arg) - 1] = '\0';
                            strncpy(touched[n].expected, e.target, sizeof(touched[n].expected) - 1);
                            touched[n].expected[sizeof(touched[n].expected) - 1] = '\0';
                        }
                        n++;
                        continue;
                    }
                    if (e.kind == 5 /*DELTA*/) {
                        // Non-destructive bridge helper: stored inert ("0"),
                        // official base in e.official. Anything else stored
                        // is drift (mismatch), never recomputed.
                        if (strcmp(before, "0") != 0) {
                            InterlockedIncrement(&s_mismatchesPopulate);
                            X10Lifecycle::X10Log("[X10WRITE] modifier=%s arg=%s delta-not-inert(expected 0, saw %s) skipped",
                                   e.modifierId, e.argument, before);
                            continue;
                        }
                    } else if (strcmp(before, e.official) != 0) {
                        InterlockedIncrement(&s_mismatchesPopulate);
                        X10Lifecycle::X10Log("[X10WRITE] modifier=%s arg=%s official-mismatch(expected %s, saw %s) skipped",
                               e.modifierId, e.argument, e.official, before);
                        continue;
                    }
                    double official = strtod(
                        (e.kind == 5) ? e.official : before, nullptr);
                    char replacement[32] = {};
                    static const char* kNames[] = {"ADDITIVE", "COMBAT", "PROBABILITY", "DISCOUNT",
                        "VECTOR", "DELTA", "REPEAT"};
                    const char* tname = (e.kind >= 0 && e.kind <= 6) ? kNames[e.kind] : "?";
                    bool applied;
                    if (e.kind == 4 /*VECTOR*/) {
                        applied = X10Transforms::ApplyVector(
                            before, g_k, replacement, sizeof(replacement));
                    } else {
                        applied = X10Transforms::Apply(e.kind, official, g_k, g_kErr,
                                                  e.countLike != 0,
                                                  replacement, sizeof(replacement));
                    }
                    if (!applied) {
                        InterlockedIncrement(&s_transformRefused);
                        X10Lifecycle::X10Log("[X10WRITE] modifier=%s arg=%s transform-refused kind=%s skipped",
                               e.modifierId, e.argument, tname);
                        continue;
                    }
                    if (!SsoWrite(el + prof->argValueOff, replacement)) {
                        InterlockedIncrement(&s_skippedPopulate);
                        X10Lifecycle::X10Log("[X10WRITE] modifier=%s arg=%s write-refused (not SSO-safe)", e.modifierId, e.argument);
                        continue;
                    }
                    char after[64] = {};
                    size_t afterLen = 0;
                    SsoRead(el + prof->argValueOff, after, sizeof(after), afterLen);
                    InterlockedIncrement(&s_writesSession);
                    InterlockedIncrement(&s_writesPopulate);
                    X10Lifecycle::X10Log("[X10WRITE] modifier=%s arg=%s official=%s k=%.6g transform=%s requested=%s before=%s after=%s phase=definition-population",
                           e.modifierId, e.argument, e.official, g_k, tname, replacement, before, after);
                    if (n < maxTouched) {
                        strncpy(touched[n].id, id, sizeof(touched[n].id) - 1);
                        touched[n].id[sizeof(touched[n].id) - 1] = '\0';
                        strncpy(touched[n].arg, e.argument, sizeof(touched[n].arg) - 1);
                        touched[n].arg[sizeof(touched[n].arg) - 1] = '\0';
                        strncpy(touched[n].expected, replacement, sizeof(touched[n].expected) - 1);
                        touched[n].expected[sizeof(touched[n].expected) - 1] = '\0';
                    }
                    n++;
                }
            }
            return n;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            X10Lifecycle::X10Log("[X10WRITE] fault during definition scan (skipped)");
            return n;
        }
    }

    void OnAddModifierDefinition(void* d0, void* d1,
                                 Touched* touched, int maxTouched, int* outCount) {
        *outCount = 0;
        if (!g_armed) return;
        void* definition = ResolveDefinitionReference(d0, d1);
        if (!definition) {
            X10Lifecycle::X10Log("[X10WRITE] unresolvable definition reference (skipped)");
            return;
        }
        *outCount = WriteDefinition(definition, touched, maxTouched);
    }

    // Post-Add witness: resolve every touched entry through the REGISTERED
    // ModifierSystem store (engine GetModifierDefinition by ID string) and
    // compare against the expected value. Tiers:
    //   textual definition retained -> MATCH / MISMATCH;
    //   definition found + ID cross-checked but value blank/absent ->
    //     typed-or-consumed (unreadable, never MATCH);
    //   lookup miss / fault / degraded path -> unreadable, never MATCH.
    // The engine key is the ID string itself (MSVC std::string layout,
    // heap form over caller-owned storage; proven by Get's find helper,
    // which reads key bytes + size from the string object).
    struct EngineKey {
        const char* ptr; // +0x0 (heap form)
        char pad[8];     // +0x8 (unused)
        uint64_t size;   // +0x10
        uint64_t capa;   // +0x18 (must be >= ssoInlineThreshold)
    };
    struct EngineRef {
        void* ptr; // shared_ptr _Ptr: the definition object
        void* rep; // shared_ptr _Rep: control block (we AddRef'd; release)
    };
    typedef void (__thiscall* GetDefFn)(void* self, void* outRef, void* idKey);

    // Release our Get-acquired reference (mirrors Add's tail: decref uses;
    // the store always holds its own ref, so destroy is unreachable — if it
    // ever were last, log + leak rather than run destroy paths).
    static void ReleaseEngineRef(void* rep) {
        if (!rep) return;
        __try {
            LONG* uses = reinterpret_cast<LONG*>(reinterpret_cast<uint8_t*>(rep) + 8);
            LONG remaining = InterlockedDecrement(uses);
            if (remaining == 0) {
                X10Lifecycle::X10Log("[X10WRITE] ref-release was last (leaked, not destroyed)");
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            X10Lifecycle::X10Log("[X10WRITE] ref-release fault (leaked)");
        }
    }

    static void VerifyOne(void* system, Touched* t) {
        auto* prof = X10Lifecycle::ActiveProfile();
        if (!prof || !s_storeLookupProven || !system) {
            InterlockedIncrement(&s_postAddUnreadable);
            X10Lifecycle::X10Log("[X10WRITE] id=%s arg=%s stored_after_add=<lookup-unavailable> expected=%s",
                   t->id, t->arg, t->expected);
            return;
        }
        __try {
            size_t len = strlen(t->id);
            if (len == 0 || len > sizeof(t->id) - 1) {
                InterlockedIncrement(&s_postAddUnreadable);
                X10Lifecycle::X10Log("[X10WRITE] id=%s arg=%s stored_after_add=<verifier-key-too-long> expected=%s",
                       t->id, t->arg, t->expected);
                return;
            }
            EngineKey key;
            key.ptr = t->id;
            memset(key.pad, 0, sizeof(key.pad));
            key.size = (uint64_t)len;
            key.capa = sizeof(t->id); // heap form over our stable Touched storage
            EngineRef out{ nullptr, nullptr };
            auto getFn = reinterpret_cast<GetDefFn>(
                reinterpret_cast<uint8_t*>(g_base) + prof->getDefRva);
            getFn(system, &out, &key);
            if (!out.ptr || !out.rep) {
                InterlockedIncrement(&s_postAddUnreadable);
                X10Lifecycle::X10Log("[X10WRITE] id=%s arg=%s stored_after_add=<lookup-miss> expected=%s",
                       t->id, t->arg, t->expected);
                return;
            }
            // ID cross-check: proves the lookup returned OUR definition.
            // A wrong definition here refutes the key derivation, loudly.
            uint8_t* def = reinterpret_cast<uint8_t*>(out.ptr);
            bool idOk = false;
            char storedId[256] = {};
            size_t idLen = 0;
            if (*reinterpret_cast<void**>(def) &&
                SsoRead(def + prof->defIdOff, storedId, sizeof(storedId), idLen) &&
                strcmp(storedId, t->id) == 0) {
                idOk = true;
            }
            if (!idOk) {
                InterlockedIncrement(&s_postAddMismatch);
                X10Lifecycle::X10Log("[X10WRITE] id=%s arg=%s stored_id_mismatch(saw %s) expected-id=%s MISMATCH",
                       t->id, t->arg, storedId[0] ? storedId : "<unreadable>", t->id);
                ReleaseEngineRef(out.rep);
                return;
            }
            // Walk the REGISTERED definition's argument vector for our arg.
            void** vec = reinterpret_cast<void**>(def + prof->argVecOff);
            uint8_t* begin = reinterpret_cast<uint8_t*>(vec[0]);
            uint8_t* end = reinterpret_cast<uint8_t*>(vec[1]);
            if (begin && end && end >= begin) {
                size_t count = (size_t)(end - begin) / prof->argStride;
                if (count > 0 && count <= prof->maxArgs) {
                    for (size_t i = 0; i < count; i++) {
                        uint8_t* el = begin + i * prof->argStride;
                        char name[64] = {};
                        size_t nameLen = 0;
                        if (!SsoRead(el + prof->argNameOff, name, sizeof(name), nameLen))
                            continue;
                        if (strcmp(name, t->arg) != 0)
                            continue;
                        char stored[64] = {};
                        size_t storedLen = 0;
                        if (!SsoRead(el + prof->argValueOff, stored, sizeof(stored), storedLen)) {
                            InterlockedIncrement(&s_postAddUnreadable);
                            X10Lifecycle::X10Log("[X10WRITE] id=%s arg=%s stored_after_add=<typed-or-consumed> expected=%s",
                                   t->id, t->arg, t->expected);
                        } else if (strcmp(stored, t->expected) == 0) {
                            InterlockedIncrement(&s_postAddMatch);
                            X10Lifecycle::X10Log("[X10WRITE] id=%s arg=%s stored_after_add=%s expected=%s MATCH via=store-lookup",
                                   t->id, t->arg, stored, t->expected);
                        } else {
                            InterlockedIncrement(&s_postAddMismatch);
                            X10Lifecycle::X10Log("[X10WRITE] id=%s arg=%s stored_after_add=%s expected=%s MISMATCH via=store-lookup",
                                   t->id, t->arg, stored[0] ? stored : "<empty>", t->expected);
                        }
                        ReleaseEngineRef(out.rep);
                        return;
                    }
                }
            }
            InterlockedIncrement(&s_postAddUnreadable);
            X10Lifecycle::X10Log("[X10WRITE] id=%s arg=%s stored_after_add=<arg-absent> expected=%s",
                   t->id, t->arg, t->expected);
            ReleaseEngineRef(out.rep);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            InterlockedIncrement(&s_postAddUnreadable);
            X10Lifecycle::X10Log("[X10WRITE] id=%s arg=%s stored_after_add=<verify-fault> expected=%s",
                   t->id, t->arg, t->expected);
        }
    }

    void VerifyStoredAfterAdd(void* system, Touched* touched, int count) {
        for (int i = 0; i < count; i++) {
            VerifyOne(system, &touched[i]);
        }
    }

    void InitBase(uintptr_t base) { g_base = base; }
}
