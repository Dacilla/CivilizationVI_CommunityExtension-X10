// X10 native config reader + definition override writer (see X10Write.h).
//
// Reference basis (civ6-gamecore-reference, `cur` column, build 15038592):
//   Configuration::Game::GetInstance ............ 0x164c20 (global read)
//   string hash ................................. 0x606270 (rcx=str -> eax)
//   Data::TypedVariantMap::FindVariant .......... 0x99f570 (map, key -> node+0x28 | NULL)
// Map: root=[map+0x40], sentinel=map+0x30; node: left+0x0, right+0x8,
// key(dword)+0x20, value=node+0x28. Instance: gameMap=[inst+0x88],
// rootMap=[inst+0x80] (Windows-validated via GetGameSpeedType/operator=).
// Variant: u16 type@+0x0, payload@+0x8; int pattern validated against the
// GetGameSpeedType mask (ids<=0x15 in 0x30002e). String payloads are read as
// C strings ONLY after strict validation (printable, NUL-terminated <=64,
// strtod-clean, finite, in range); anything else fails closed.
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
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cctype>
#include <cmath>
#include "X10Lifecycle.h"

extern void X10Lifecycle::X10Log(const char* fmt, ...);

namespace {
    uintptr_t g_base = 0;

    template <typename Fn>
    Fn At(uintptr_t rva) {
        return reinterpret_cast<Fn>(g_base + rva);
    }

    // ---- config reader ----
    typedef void* (__cdecl* GetInstanceFn)();
    typedef uint32_t(__cdecl* HashFn)(const char* s);
    typedef void* (__cdecl* FindVariantFn)(void* map, uint32_t key);

    bool ReadCString(const void* strObj, char* out, size_t cap) {
        __try {
            const uint8_t* p = reinterpret_cast<const uint8_t*>(strObj);
            uint64_t size = *reinterpret_cast<const uint64_t*>(p + 0x10);
            uint64_t capa = *reinterpret_cast<const uint64_t*>(p + 0x18);
            const char* data = (capa < 0x10) ? reinterpret_cast<const char*>(p)
                                            : *reinterpret_cast<const char* const*>(p);
            if (size > 64) return false;
            for (uint64_t i = 0; i < size; i++) {
                char c = data[i];
                if (c < 32 || c > 126) return false;
            }
            if (data[size] != '\0') return false;
            if (size + 1 > cap) return false;
            memcpy(out, data, (size_t)size + 1);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    bool VariantToDouble(void* variant, double& out) {
        // Mirrors GetGameSpeedType/GetGameMode exactly: flags@+0x14 bit 0x1d
        // set means "not a plain int"; else u16 type must be in 0x30002e.
        __try {
            uint8_t* v = reinterpret_cast<uint8_t*>(variant);
            uint32_t flags = *reinterpret_cast<uint32_t*>(v + 0x14);
            if ((flags >> 0x1d) & 1) return false;
            uint16_t type = *reinterpret_cast<uint16_t*>(v);
            if (type > 0x15 || !((0x30002e >> type) & 1)) return false;
            out = (double)*reinterpret_cast<int32_t*>(v + 8);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }
}

namespace X10Config {
    bool TryGetProbeK(double& out) {
        out = 0;
        auto getInstance = At<GetInstanceFn>(0x164c20);
        auto hashFn = At<HashFn>(0x606270);
        auto findVariant = At<FindVariantFn>(0x99f570);
        void* inst = nullptr;
        __try {
            inst = getInstance();
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            X10Lifecycle::X10Log("CONFIG key=X10_PROBE_K found=false reason=getinstance-fault");
            return false;
        }
        if (!inst) {
            X10Lifecycle::X10Log("CONFIG key=X10_PROBE_K found=false reason=null-instance");
            return false;
        }
        uint32_t key = 0;
        __try {
            key = hashFn("X10_PROBE_K");
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            X10Lifecycle::X10Log("CONFIG key=X10_PROBE_K found=false reason=hash-fault");
            return false;
        }
        const uintptr_t maps[2] = {0x88, 0x80}; // game variants first, then root
        for (int m = 0; m < 2; m++) {
            void* map = nullptr;
            __try {
                map = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(inst) + maps[m]);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                continue;
            }
            if (!map) continue;
            void* variant = nullptr;
            __try {
                variant = findVariant(map, key);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                continue;
            }
            if (!variant) continue;
            double d = 0;
            if (VariantToDouble(variant, d)) {
                if (d >= 0 && d <= 100 && d == d) {
                    out = d;
                    X10Lifecycle::X10Log("CONFIG key=X10_PROBE_K found=true type=numeric raw=%f numeric=%f", d, d);
                    return true;
                }
                X10Lifecycle::X10Log("CONFIG key=X10_PROBE_K found=true type=numeric raw=%f reason=out-of-range", d);
                return false;
            }
            char buf[72] = {};
            // String-typed variant: payload is a string object at +8.
            bool ok = false;
            __try {
                uint8_t* v = reinterpret_cast<uint8_t*>(variant);
                void* payload = *reinterpret_cast<void**>(v + 8);
                ok = payload && ReadCString(payload, buf, sizeof(buf));
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                ok = false;
            }
            if (ok) {
                char* end = nullptr;
                double dd = strtod(buf, &end);
                if (end != buf && *end == '\0' && dd == dd && dd >= 0 && dd <= 100) {
                    out = dd;
                    X10Lifecycle::X10Log("CONFIG key=X10_PROBE_K found=true type=string raw=%s numeric=%f", buf, dd);
                    return true;
                }
                X10Lifecycle::X10Log("CONFIG key=X10_PROBE_K found=true type=string raw=%s reason=unparseable", buf);
                return false;
            }
            X10Lifecycle::X10Log("CONFIG key=X10_PROBE_K found=true reason=unreadable-variant-type");
            // Report exact type/flags/payload for the next static iteration.
            __try {
                uint8_t* v = reinterpret_cast<uint8_t*>(variant);
                uint16_t type = *reinterpret_cast<uint16_t*>(v);
                uint32_t flags = *reinterpret_cast<uint32_t*>(v + 0x14);
                uint8_t b[16] = {};
                memcpy(b, v + 8, sizeof(b));
                X10Lifecycle::X10Log("CONFIG key=X10_PROBE_K variant type=%u flags=0x%x payload=%02x%02x%02x%02x%02x%02x%02x%02x...",
                       (unsigned)type, flags, b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
            }
            return false;
        }
        X10Lifecycle::X10Log("CONFIG key=X10_PROBE_K found=false reason=absent-from-both-maps");
        return false;
    }
}

namespace X10Write {
    // Test-only override table (official baseline values). The Rome entry is
    // the guaranteed-active runtime witness (human plays Rome); the other
    // three are definition-write proofs verified via stored_after_add.
    const Override kOverrides[4] = {        {"TRAIT_LINCOLN_INDUSTRIAL_ZONE_LOYALTY", "Amount", "3", 0},
        {"AGOGE_ANCIENT_MELEE_PRODUCTION", "Amount", "50", 0},
        {"ALL_PARK_COMBAT_BONUS", "Amount", "5", 1},
        {"TRAIT_GOLD_FROM_DOMESTIC_TRADING_POSTS", "Amount", "1", 0},
    };

    static double g_k = 0;
    static bool g_armed = false;
    static volatile LONG s_writesSession = 0;
    static volatile LONG s_writesPopulate = 0;

    void Arm(double k) {
        g_k = k;
        g_armed = true;
    }

    void Disarm() {
        g_armed = false;
        g_k = 0;
        s_writesPopulate = 0;
    }

    bool IsArmed() { return g_armed; }

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

    // Writes the first matching target element of one definition.
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
            for (int t = 0; t < kOverrideCount; t++) {
                const Override& o = kOverrides[t];
                if (strcmp(id, o.modifierId) != 0) continue;
                // Argument vector at +0x40: {begin, end, cap}.
                void** vec = reinterpret_cast<void**>(def + 0x40);
                uint8_t* begin = reinterpret_cast<uint8_t*>(vec[0]);
                uint8_t* end = reinterpret_cast<uint8_t*>(vec[1]);
                if (!begin || !end || end < begin) return false;
                size_t count = (size_t)(end - begin) / 0xA0;
                if (count == 0 || count > 64) return false; // insane: skip
                for (size_t i = 0; i < count; i++) {
                    uint8_t* el = begin + i * 0xA0;
                    char name[64] = {};
                    size_t nameLen = 0;
                    if (!SsoRead(el, name, sizeof(name), nameLen)) continue;
                    if (strcmp(name, o.argument) != 0) continue;
                    char before[64] = {};
                    size_t beforeLen = 0;
                    if (!SsoRead(el + 0x20, before, sizeof(before), beforeLen)) continue;
                    if (strcmp(before, o.official) != 0) {
                        X10Lifecycle::X10Log("[X10WRITE] modifier=%s arg=%s official-mismatch(expected %s, saw %s) skipped",
                               o.modifierId, o.argument, o.official, before);
                        continue;
                    }
                    double official = strtod(before, nullptr);
                    char replacement[32] = {};
                    const char* transform = "ADDITIVE";
                    if (o.family == 1) {
                        transform = "COMBAT";
                        double v = 25.0 * log(g_k * (exp(official / 25.0) - 1.0) + 1.0);
                        if (!(v == v) || v < 0 || v > 100000) {
                            X10Lifecycle::X10Log("[X10WRITE] modifier=%s arg=%s non-finite combat result skipped", o.modifierId, o.argument);
                            continue;
                        }
                        snprintf(replacement, sizeof(replacement), "%.2f", v);
                    } else {
                        double v = official * g_k;
                        if (!(v == v)) {
                            X10Lifecycle::X10Log("[X10WRITE] modifier=%s arg=%s non-finite result skipped", o.modifierId, o.argument);
                            continue;
                        }
                        snprintf(replacement, sizeof(replacement), "%.6g", v);
                    }
                    if (!SsoWrite(el + 0x20, replacement)) {
                        X10Lifecycle::X10Log("[X10WRITE] modifier=%s arg=%s write-refused (not SSO-safe)", o.modifierId, o.argument);
                        continue;
                    }
                    char after[64] = {};
                    size_t afterLen = 0;
                    SsoRead(el + 0x20, after, sizeof(after), afterLen);
                    InterlockedIncrement(&s_writesSession);
                    InterlockedIncrement(&s_writesPopulate);
                    X10Lifecycle::X10Log("[X10WRITE] modifier=%s arg=%s official=%s k=%.6g transform=%s requested=%s before=%s after=%s phase=definition-population",
                           o.modifierId, o.argument, o.official, g_k, transform, replacement, before, after);
                    *outEl = el + 0x20;
                    strncpy(outExpected, replacement, expCap - 1);
                    *outId = o.modifierId;
                    *outArg = o.argument;
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
