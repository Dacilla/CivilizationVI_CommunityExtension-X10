// X10 native config reader + definition override writer (see X10Write.h).
//
// Reference basis (civ6-gamecore-reference, `cur` column, build 15038592):
//   Configuration::Game::GetInstance ............ 0x164c20 (global read)
//   string hash ................................. 0x606270 (rcx=str -> eax)
//   Variant lookup: virtual method at vtable+0x68(manager, keyHash), exactly
//     as GetGameSpeedType/GetGameMode/GetStartEra call it. The qwords at
//     inst+0x88 (game variants) / inst+0x80 (root variants) are MANAGER
//     OBJECTS (their first qword is a vtable), NOT raw maps: calling
//     TypedVariantMap::FindVariant on them walks the wrong object.
// Instance: gameMgr=[inst+0x88], rootMgr=[inst+0x80] (Windows-validated via
// the getters above). Variant: u16 type@+0x0, payload@+0x8;
// int pattern validated against the GetGameSpeedType mask (ids<=0x15 in
// 0x30002e). String payloads are read as objects ONLY after strict layout
// validation (printable, NUL-terminated <=64, strtod-clean, finite, in
// range); anything else fails closed with type/flags/payload logged.
//
// ArgumentDefinition (cross-checked: 2 ctors, 2 find loops, TryGetValue):
//   sizeof = 0xA0; name = SSO string @+0x00; value = SSO string @+0x20.
//   SSO string: inline bytes @+0x0 (cap@+0x18 < 0x10), size@+0x10,
//   capacity@+0x18; heap ptr @+0x0 otherwise (never touched: we only rewrite
//   SSO-inline values whose replacement fits, else skip + log).
// SimpleModifierDefinition: id string @+0x18 (SSO shape), argument vector
//   @+0x40 ({begin,end,cap}); elements strided 0xA0. Every shape is
//   re-validated per element before any byte is written.
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
    // manager = *(inst+off); variant = manager->vtable[0x68/8](manager, key).
    typedef void* (__cdecl* LookupFn)(void* manager, uint32_t key);

    static void* LookupVariant(void* manager, uint32_t key) {
        __try {
            if (!manager) return nullptr;
            void* vt = *reinterpret_cast<void**>(manager);
            if (!vt) return nullptr;
            void* fn = reinterpret_cast<void**>(vt)[0x68 / 8];
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
                                const char** kindOut) {
        typeOut = 0xFFFF;
        flagsOut = 0;
        *kindOut = "unknown";
        __try {
            uint8_t* v = reinterpret_cast<uint8_t*>(variant);
            uint16_t type = *reinterpret_cast<uint16_t*>(v);
            uint32_t flags = *reinterpret_cast<uint32_t*>(v + 0x14);
            typeOut = type;
            flagsOut = flags;
            if (type == 4) {
                float f = 0;
                memcpy(&f, v + 8, sizeof(f));
                if (f == f && f >= 0 && f <= 100) {
                    out = (double)f;
                    *kindOut = "FLOAT32";
                    return true;
                }
                return false; // non-finite/out-of-range float: fail closed
            }
            if (!((flags >> 0x1d) & 1) && type <= 0x15 && ((0x30002e >> type) & 1)) {
                out = (double)*reinterpret_cast<int32_t*>(v + 8);
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
    bool TryGetConfigDouble(const char* key, double& out) {
        out = 0;
        auto getInstance = At<GetInstanceFn>(0x164c20);
        auto hashFn = At<HashFn>(0x606270);
        void* inst = nullptr;
        __try {
            inst = getInstance();
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            X10Lifecycle::X10Log("CONFIG key=%s found=false reason=getinstance-fault", key);
            return false;
        }
        if (!inst) {
            X10Lifecycle::X10Log("CONFIG key=%s found=false reason=null-instance", key);
            return false;
        }
        uint32_t h = 0;
        __try {
            h = hashFn(key);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            X10Lifecycle::X10Log("CONFIG key=%s found=false reason=hash-fault", key);
            return false;
        }
        const uintptr_t mgrs[2] = {0x88, 0x80}; // game variants first, then root
        for (int m = 0; m < 2; m++) {
            void* mgr = nullptr;
            __try {
                mgr = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(inst) + mgrs[m]);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                continue;
            }
            if (!mgr) continue;
            void* variant = nullptr;
            __try {
                variant = LookupVariant(mgr, h);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                continue;
            }
            if (!variant) continue;
            double d = 0;
            unsigned vtype = 0xFFFF;
            uint32_t vflags = 0;
            const char* kind = "unknown";
            if (VariantToDouble(variant, d, vtype, vflags, &kind)) {
                if (d >= 0 && d <= 100 && d == d) {
                    out = d;
                    X10Lifecycle::X10Log("CONFIG key=%s found=true type=%s id=%u flags=0x%x numeric=%.15g", key, kind, vtype, vflags, d);
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
        X10Lifecycle::X10Log("CONFIG key=%s found=false reason=absent-from-both-managers", key);
        return false;
    }

    // Raw variant lookup shared by value reads and presence probes.
    // Returns the variant pointer, or null when absent/unreachable.
    static void* FindConfigVariant(const char* key) {
        auto getInstance = At<GetInstanceFn>(0x164c20);
        auto hashFn = At<HashFn>(0x606270);
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
        const uintptr_t mgrs[2] = {0x88, 0x80};
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

    bool ModuleEnabled(const char* moduleKey, bool defaultOn) {
        char full[96] = {};
        snprintf(full, sizeof(full), "X10_MODULE_%s", moduleKey);
        for (char* p = full; *p; p++) {
            if (*p >= 'a' && *p <= 'z') *p -= 32;
        }
        // Absent key: default per declared mod defaults, loudly.
        // A present-but-unreadable key fails closed (disabled).
        if (!FindConfigVariant(full)) {
            X10Lifecycle::X10Log("CONFIG module %s absent: default %s", moduleKey,
                   defaultOn ? "ON" : "OFF");
            return defaultOn;
        }
        double d = 0;
        if (TryGetConfigDouble(full, d)) {
            bool on = (d != 0);
            X10Lifecycle::X10Log("CONFIG module %s %s", moduleKey, on ? "ON" : "OFF");
            return on;
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
    static bool g_armed = false;
    static bool s_modEnabled[3] = {true, true, true}; // traits, policies, governments
    static volatile LONG s_writesSession = 0;
    static volatile LONG s_writesPopulate = 0;
    static volatile LONG s_mismatchesPopulate = 0;
    static volatile LONG s_skippedPopulate = 0;

    static int ModuleIndex(const char* module) {
        if (strcmp(module, "traits") == 0) return 0;
        if (strcmp(module, "policies") == 0) return 1;
        if (strcmp(module, "governments") == 0) return 2;
        return -1;
    }

    void Arm(double k, const bool* mods) {
        g_k = k;
        g_armed = true;
        for (int i = 0; i < 3; i++) s_modEnabled[i] = mods[i];
    }

    // Legacy single-arg arm (probe compat): all supported modules on.
    void Arm(double k) {
        static const bool all[3] = {true, true, true};
        Arm(k, all);
    }

    void Disarm() {
        g_armed = false;
        g_k = 0;
        s_writesPopulate = 0;
        s_mismatchesPopulate = 0;
        s_skippedPopulate = 0;
    }

    bool IsArmed() { return g_armed; }

    long WritesThisPopulate() { return (long)s_writesPopulate; }
    long MismatchesThisPopulate() { return (long)s_mismatchesPopulate; }
    long SkippedThisPopulate() { return (long)s_skippedPopulate; }

    static bool BoundedStringRead(void* s, char* out, size_t cap,
                                  size_t& lenOut);

    static bool SsoRead(void* s, char* out, size_t cap, size_t& lenOut) {
        return BoundedStringRead(s, out, cap, lenOut);
    }

    // Bounded READ-ONLY string reader: SSO-inline and heap-backed (reads only;
    // heap WRITES remain refused — see SsoWrite).
    static bool BoundedStringRead(void* s, char* out, size_t cap,
                                  size_t& lenOut) {
        __try {
            uint8_t* p = reinterpret_cast<uint8_t*>(s);
            uint64_t size = *reinterpret_cast<uint64_t*>(p + 0x10);
            uint64_t capa = *reinterpret_cast<uint64_t*>(p + 0x18);
            if (size > 256) return false;
            const char* data = nullptr;
            if (capa < 0x10) {
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
        __try {
            size_t n = strlen(text);
            if (n > 15) return false;
            uint8_t* p = reinterpret_cast<uint8_t*>(s);
            uint64_t capa = *reinterpret_cast<uint64_t*>(p + 0x18);
            if (capa >= 0x10) return false; // heap: never touch
            for (size_t i = 0; i < n; i++) {
                if (text[i] < 32 || text[i] > 126) return false;
            }
            memcpy(p, text, n + 1);
            *reinterpret_cast<uint64_t*>(p + 0x10) = (uint64_t)n;
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

    // Writes the first matching production entry of one definition.
    // Every entry derives from official baseline + runtime k (never from
    // stored values). Official mismatch skips THAT entry and counts it.
    // Returns true and fills outEl/outExpected when a write happened.
    static bool WriteDefinition(void* definition, void** outEl,
                                char* outExpected, size_t expCap,
                                const char** outId, const char** outArg) {
        *outEl = nullptr;
        __try {
            uint8_t* def = reinterpret_cast<uint8_t*>(definition);
            static char id[256];
            size_t idLen = 0;
            if (!SsoRead(def + 0x18, id, sizeof(id), idLen)) return false; // id unreadable: skip
            // Argument vector at +0x40: {begin, end, cap}.
            void** vec = reinterpret_cast<void**>(def + 0x40);
            uint8_t* begin = reinterpret_cast<uint8_t*>(vec[0]);
            uint8_t* end = reinterpret_cast<uint8_t*>(vec[1]);
            if (!begin || !end || end < begin) return false;
            size_t count = (size_t)(end - begin) / 0xA0;
            if (count == 0 || count > 64) return false; // insane: skip
            for (int t = 0; t < kX10ProductionRegistryCount; t++) {
                const X10RegistryEntry& e = kX10ProductionRegistry[t];
                if (strcmp(id, e.modifierId) != 0) continue;
                int mi = ModuleIndex(e.module);
                if (mi < 0 || !s_modEnabled[mi]) {
                    InterlockedIncrement(&s_skippedPopulate);
                    continue;
                }
                for (size_t i = 0; i < count; i++) {
                    uint8_t* el = begin + i * 0xA0;
                    char name[64] = {};
                    size_t nameLen = 0;
                    if (!SsoRead(el, name, sizeof(name), nameLen)) continue;
                    if (strcmp(name, e.argument) != 0) continue;
                    char before[64] = {};
                    size_t beforeLen = 0;
                    if (!SsoRead(el + 0x20, before, sizeof(before), beforeLen)) continue;
                    if (strcmp(before, e.official) != 0) {
                        InterlockedIncrement(&s_mismatchesPopulate);
                        X10Lifecycle::X10Log("[X10WRITE] modifier=%s arg=%s official-mismatch(expected %s, saw %s) skipped",
                               e.modifierId, e.argument, e.official, before);
                        continue;
                    }
                    double official = strtod(before, nullptr);
                    char replacement[32] = {};
                    static const char* kNames[] = {"ADDITIVE", "COMBAT", "PROBABILITY", "DISCOUNT"};
                    if (!X10Transforms::Apply(e.kind, official, g_k, e.countLike != 0,
                                              replacement, sizeof(replacement))) {
                        InterlockedIncrement(&s_skippedPopulate);
                        X10Lifecycle::X10Log("[X10WRITE] modifier=%s arg=%s transform-refused kind=%s skipped",
                               e.modifierId, e.argument, kNames[e.kind]);
                        continue;
                    }
                    if (!SsoWrite(el + 0x20, replacement)) {
                        InterlockedIncrement(&s_skippedPopulate);
                        X10Lifecycle::X10Log("[X10WRITE] modifier=%s arg=%s write-refused (not SSO-safe)", e.modifierId, e.argument);
                        continue;
                    }
                    char after[64] = {};
                    size_t afterLen = 0;
                    SsoRead(el + 0x20, after, sizeof(after), afterLen);
                    InterlockedIncrement(&s_writesSession);
                    InterlockedIncrement(&s_writesPopulate);
                    X10Lifecycle::X10Log("[X10WRITE] modifier=%s arg=%s official=%s k=%.6g transform=%s requested=%s before=%s after=%s phase=definition-population",
                           e.modifierId, e.argument, e.official, g_k, kNames[e.kind], replacement, before, after);
                    *outEl = el + 0x20;
                    strncpy(outExpected, replacement, expCap - 1);
                    *outId = e.modifierId;
                    *outArg = e.argument;
                    return true;
                }
                return false; // id matched: done with this definition
            }
            return false;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            X10Lifecycle::X10Log("[X10WRITE] fault during definition scan (skipped)");
            return false;
        }
    }

    void OnAddModifierDefinition(void* d0, void* d1,
                                 void** outEl, char* outExpected, size_t expCap,
                                 const char** outId, const char** outArg) {
        *outEl = nullptr;
        if (outExpected && expCap) outExpected[0] = '\0';
        if (!g_armed) return;
        void* definition = ResolveDefinitionReference(d0, d1);
        if (!definition) {
            X10Lifecycle::X10Log("[X10WRITE] unresolvable definition reference (skipped)");
            return;
        }
        WriteDefinition(definition, outEl, outExpected, expCap, outId, outArg);
    }

    // Post-Add witness: re-read the touched element from the live definition.
    void VerifyStoredAfterAdd(void* el, const char* expected,
                              const char* id, const char* arg) {
        char stored[64] = {};
        size_t storedLen = 0;
        if (el && SsoRead(el, stored, sizeof(stored), storedLen)) {
            X10Lifecycle::X10Log("[X10WRITE] id=%s arg=%s stored_after_add=%s expected=%s %s",
                   id, arg, stored, expected,
                   strcmp(stored, expected) == 0 ? "MATCH" : "MISMATCH");
        } else {
            X10Lifecycle::X10Log("[X10WRITE] id=%s arg=%s stored_after_add=<unreadable>", id, arg);
        }
    }

    void InitBase(uintptr_t base) { g_base = base; }
}
