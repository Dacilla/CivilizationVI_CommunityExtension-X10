// X10 lifecycle instrumentation + production definition overrides (see
// X10Lifecycle.h). Build/ABI constants live ONLY in the active
// GameCoreCompatibilityProfile (X10Compat.h); this TU holds no literal
// target RVAs, timestamps, or image sizes.
//
// FAIL-CLOSED VALIDATION (see docs/NATIVE_HOOK_VALIDATION.md in civ6-x10):
// the fork installs ZERO hooks unless all of these hold:
//   1. loaded module filename is GameCore_XP2_FinalRelease.dll,
//   2. PE timestamp + SizeOfImage match a known compatibility profile,
//   3. every profile target RVA lies in executable .text,
//   4. every target starts with a masked function-prologue class
//      (48 89 ?? 24 ??  |  40 5?  |  48 83 EC ??).
// The prologue classes are generic x64 code-shape checks, NOT copies of
// observed bytes: they confirm "function entry", never a specific function.
// Function identity comes from the GameCore reference (names, sizes,
// adjacency, signatures) documented per hook in NATIVE_HOOK_VALIDATION.md.
//
// Detour prototypes match the reference signatures under the MSVC x64 ABI
// (first 4 args in RCX/RDX/R8/R9, rest on stack; references are pointers).
// ModifierDefinitionReference arrives as a single pointer (in RDX) to a
// 16-byte shared_ptr-like {_Ptr, _Rep} object (resolved 2026-10-07: the
// callee dereferences [RDX+0] then virtual +0x10 with shared_ptr control
// block ref-counting — see doc); the hook forwards both reference slots to
// orig_Add for call fidelity and resolves the raw definition via *(void**)d0
// (X10Write::ResolveDefinitionReference, fail-closed).
// All hooked functions return void.
//
//   PopulateModifierDefinitions : void(pDB, modifierSystem&)
//   AddModifierDefinition : void(this, ModifierDefinitionReference)
//   DynamicModifier ctors A/B : void(this, gameFx&, owner&,
//       definition&, collection&, effect&)
// (Hook RVAs: see the active profile in X10Compat.h.)
#include "X10Lifecycle.h"
#include "X10Write.h"
#include "X10Compat.h"
#include "HavokScript.h"
#include "MinHook.h"
#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdarg>

#define X10_VERSION "x10-write-test 2 (definition overrides, 2026-10-07)"

namespace X10Lifecycle {
    namespace {
        static volatile LONG s_seq = 0;
        static volatile LONG s_addCount = 0;
        static volatile LONG s_instanceCount = 0;
        static volatile LONG s_populateDepth = 0;
        static FILE* s_log = nullptr;
        static uintptr_t s_base = 0;
        // .text range filled by validation.
        static uintptr_t s_textStart = 0;
        static uintptr_t s_textEnd = 0;

        void OpenLog() {
            if (s_log) return;
            char tmp[MAX_PATH] = {};
            DWORD n = GetTempPathA(sizeof(tmp), tmp);
            char path[MAX_PATH] = {};
            snprintf(path, sizeof(path), "%sX10Lifecycle.log", n ? tmp : ".\\");
            // Append, never truncate: a save/reload cycle must preserve both
            // population sequences. Each Install writes a session header.
            s_log = fopen(path, "a");
        }

        void Log(const char* fmt, ...) {
            OpenLog();
            if (!s_log) return;
            LONG id = InterlockedIncrement(&s_seq);
            fprintf(s_log, "%06ld ", id);
            va_list ap;
            va_start(ap, fmt);
            vfprintf(s_log, fmt, ap);
            va_end(ap);
            fprintf(s_log, "\n");
            fflush(s_log);
        }

        struct PeInfo {
            bool ok = false;
            uint32_t timestamp = 0;
            uint32_t imageSize = 0;
            uintptr_t textStart = 0;
            uintptr_t textEnd = 0;
            char filename[MAX_PATH] = {};
        };

        PeInfo ReadPe(uintptr_t base) {
            PeInfo pi;
            __try {
                uint8_t* b = reinterpret_cast<uint8_t*>(base);
                if (b[0] != 'M' || b[1] != 'Z') return pi;
                uint32_t pe = *reinterpret_cast<uint32_t*>(b + 0x3C);
                if (*reinterpret_cast<uint32_t*>(b + pe) != 0x00004550) return pi;
                pi.timestamp = *reinterpret_cast<uint32_t*>(b + pe + 8);
                uint16_t nsec = *reinterpret_cast<uint16_t*>(b + pe + 6);
                uint32_t opt = pe + 24;
                uint16_t magic = *reinterpret_cast<uint16_t*>(b + opt);
                pi.imageSize = *reinterpret_cast<uint32_t*>(b + opt + 56);
                uint32_t sec = opt + (magic == 0x20b ? 240 : 224);
                for (int i = 0; i < nsec && i < 64; i++) {
                    uint8_t* s = b + sec + i * 40;
                    uint32_t vaddr = *reinterpret_cast<uint32_t*>(s + 12);
                    uint32_t vsize = *reinterpret_cast<uint32_t*>(s + 8);
                    uint32_t chars = *reinterpret_cast<uint32_t*>(s + 36);
                    if (memcmp(s, ".text", 5) == 0 && (chars & 0x20000000)) {
                        pi.textStart = base + vaddr;
                        pi.textEnd = base + vaddr + vsize;
                    }
                }
                GetModuleFileNameA(reinterpret_cast<HMODULE>(base),
                    pi.filename, sizeof(pi.filename));
                pi.ok = (pi.textStart != 0);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                pi.ok = false;
            }
            return pi;
        }

        const char* BaseName(const char* p) {
            const char* s = strrchr(p, '\\');
            const char* f = strrchr(s ? s : p, '/');
            return f ? f + 1 : (s ? s + 1 : p);
        }

        bool SameCi(const char* a, const char* b) {
            while (*a && *b) {
                char ca = (*a >= 'A' && *a <= 'Z') ? *a + 32 : *a;
                char cb = (*b >= 'A' && *b <= 'Z') ? *b + 32 : *b;
                if (ca != cb) return false;
                a++; b++;
            }
            return *a == *b;
        }

        // Masked prologue classes (generic x64 shapes, see header comment).
        bool IsPrologue(uintptr_t addr) {
            __try {
                uint8_t* p = reinterpret_cast<uint8_t*>(addr);
                if (p[0] == 0x48 && p[1] == 0x89 &&
                    (p[2] == 0x54 || p[2] == 0x5C || p[2] == 0x6C || p[2] == 0x74) &&
                    p[3] == 0x24)
                    return true;                       // mov [rsp+disp8], reg
                if (p[0] == 0x40 && (p[1] & 0xF8) == 0x50)
                    return true;                       // push rbx/rbp/rsi/rdi
                if (p[0] == 0x48 && p[1] == 0x83 && p[2] == 0xEC)
                    return true;                       // sub rsp, imm8
                return false;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        // ---- originals (filled by MinHook) ----
        typedef void(__cdecl* PopulateFn)(void* pDB, void* modifierSystem);
        static PopulateFn orig_Populate = nullptr;

        typedef void(__thiscall* AddFn)(void* self, void* d0, void* d1);
        static AddFn orig_Add = nullptr;

        typedef void(__thiscall* InstanceFn)(void* self, void* gameFx, void* owner,
            void* def0, void* def1, void* col0, void* col1, void* eff0, void* eff1);
        static InstanceFn orig_InstanceA = nullptr;
        static InstanceFn orig_InstanceB = nullptr;

        void __cdecl Hook_Populate(void* pDB, void* modifierSystem) {
            InterlockedIncrement(&s_populateDepth);
            Log("PopulateModifierDefinitions ENTER depth=%ld", s_populateDepth);
            // Fail-closed reset FIRST: a failed lookup in any later game/load
            // leaves writes=0 even if an earlier game in this process armed.
            X10Write::Disarm();
            double k = 0;
            // Production multiplier (probe key still honored for experiments).
            bool haveK = X10Config::TryGetConfigDouble("X10_MULTIPLIER", k);
            if (!haveK)
                haveK = X10Config::TryGetProbeK(k);
            if (haveK && k == 0) {
                Log("X10 multiplier 0: controller OFF (definitions untouched)");
                haveK = false;
            }
            if (haveK) {
                bool mods[3] = {
                    X10Config::ModuleEnabled("traits", true),
                    X10Config::ModuleEnabled("policies", true),
                    X10Config::ModuleEnabled("governments", true),
                };
                for (const char* m : {"pantheons", "governors", "wonders", "suzerain"}) {
                    // Declared but unsupported: never silently applied.
                    char full[96] = {};
                    snprintf(full, sizeof(full), "X10_MODULE_%s", m);
                    for (char* p = full; *p; p++) {
                        if (*p >= 'a' && *p <= 'z') *p -= 32;
                    }
                    double dummy = 0;
                    if (X10Config::TryGetConfigDouble(full, dummy) && dummy != 0) {
                        Log("X10_MODULE_%s requested ON but unsupported in this build; ignored", m);
                    }
                }
                X10Write::Arm(k, mods);
            } else {
                Log("X10 writes DISABLED for this session (no native k)");
            }
            orig_Populate(pDB, modifierSystem);
            Log("PopulateModifierDefinitions EXIT depth=%ld definitions_added=%ld writes=%ld mismatches=%ld skipped=%ld",
                s_populateDepth, s_addCount, X10Write::WritesThisPopulate(),
                X10Write::MismatchesThisPopulate(), X10Write::SkippedThisPopulate());
            // Never remain armed outside the population window.
            X10Write::Disarm();
            InterlockedDecrement(&s_populateDepth);
        }

        void __thiscall Hook_Add(void* self, void* d0, void* d1) {
            LONG n = InterlockedIncrement(&s_addCount);
            if (n <= 3 || n % 1000 == 0)
                Log("AddModifierDefinition #%ld", n);
            // Write BEFORE orig_Add: the definition is fully constructed by
            // the caller and not yet visible to the system, so ownership is
            // clean and no shared-reference aliasing is relied upon.
            X10Write::Touched touched[8];
            int ntouched = 0;
            X10Write::OnAddModifierDefinition(d0, d1, touched, 8, &ntouched);
            orig_Add(self, d0, d1);
            if (ntouched > 0)
                X10Write::VerifyStoredAfterAdd(touched, ntouched);
        }

        void __thiscall Hook_InstanceA(void* self, void* gameFx, void* owner,
            void* def0, void* def1, void* col0, void* col1, void* eff0, void* eff1) {
            LONG n = InterlockedIncrement(&s_instanceCount);
            if (n <= 3)
                Log("FIRST DynamicModifier instance #%ld (attach underway)", n);
            orig_InstanceA(self, gameFx, owner, def0, def1, col0, col1, eff0, eff1);
        }

        void __thiscall Hook_InstanceB(void* self, void* gameFx, void* owner,
            void* def0, void* def1, void* col0, void* col1, void* eff0, void* eff1) {
            LONG n = InterlockedIncrement(&s_instanceCount);
            if (n <= 3)
                Log("FIRST DynamicModifier instance #%ld (attach underway)", n);
            orig_InstanceB(self, gameFx, owner, def0, def1, col0, col1, eff0, eff1);
        }

        struct Target {
            const char* name;
            uintptr_t rva;
            void* detour;
            void** orig;
        };
    }

    // The active compatibility profile, selected at Install time. All
    // build-specific constants flow from here; implementation code holds no
    // literal target RVAs, timestamps, or sizes.
    static const GameCoreCompatibilityProfile* s_profile = nullptr;

    const GameCoreCompatibilityProfile* ActiveProfile() { return s_profile; }

    bool Install(uintptr_t gameCoreBase) {
        s_base = gameCoreBase;
        X10Write::InitBase(gameCoreBase);
        Log("===== X10 native session =====");
        Log("X10 CE native write test " X10_VERSION);

        PeInfo pi = ReadPe(gameCoreBase);
        const char* base = BaseName(pi.filename);
        Log("GameCore:");
        Log("  module = %s", base);
        Log("  image size = 0x%x", pi.imageSize);
        Log("  timestamp = 0x%x", pi.timestamp);
        Log("  .text = [0x%p, 0x%p)",
            reinterpret_cast<void*>(pi.textStart),
            reinterpret_cast<void*>(pi.textEnd));
        s_profile = nullptr;
        if (pi.ok && SameCi(base, "GameCore_XP2_FinalRelease.dll")) {
            for (int i = 0; i < kGameCoreProfileCount; i++) {
                const auto& p = kGameCoreProfiles[i];
                if (pi.timestamp == p.peTimestamp && pi.imageSize == p.imageSize) {
                    s_profile = &p;
                    break;
                }
            }
        }
        if (!s_profile) {
            Log("HOOK VALIDATION FAILED (no known compatibility profile)");
            Log("NO X10 LIFECYCLE HOOKS INSTALLED");
            return false;
        }
        Log("compatibility profile: %s", s_profile->name);
        s_textStart = pi.textStart;
        s_textEnd = pi.textEnd;

        Target targets[] = {
            {"PopulateModifierDefinitions", s_profile->populateRva,
             reinterpret_cast<void*>(&Hook_Populate),
             reinterpret_cast<void**>(&orig_Populate)},
            {"AddModifierDefinition", s_profile->addRva,
             reinterpret_cast<void*>(&Hook_Add),
             reinterpret_cast<void**>(&orig_Add)},
            {"DynamicModifier_ctor_A", s_profile->instanceCtorA,
             reinterpret_cast<void*>(&Hook_InstanceA),
             reinterpret_cast<void**>(&orig_InstanceA)},
            {"DynamicModifier_ctor_B", s_profile->instanceCtorB,
             reinterpret_cast<void*>(&Hook_InstanceB),
             reinterpret_cast<void**>(&orig_InstanceB)},
        };
        Log("Hook validation:");
        for (auto& t : targets) {
            uintptr_t addr = gameCoreBase + t.rva;
            bool inText = (addr >= s_textStart && addr < s_textEnd);
            bool prologue = inText && IsPrologue(addr);
            Log("  %s %s (in.text=%d prologue=%d)",
                t.name, (inText && prologue) ? "PASS" : "FAIL",
                (int)inText, (int)prologue);
            if (!inText || !prologue) {
                Log("HOOK VALIDATION FAILED (%s)", t.name);
                Log("NO X10 LIFECYCLE HOOKS INSTALLED");
                return false;
            }
        }
        Log("ALL_REQUIRED_SIGNATURES_VALID");
        // Transactional X10-only installation: track exactly the hooks WE
        // created/enabled, and unwind only those on failure. Unrelated
        // Community Extension hooks are never disabled or removed here.
        void* created[4] = {};
        int ncreated = 0;
        for (auto& t : targets) {
            void* target = reinterpret_cast<void*>(gameCoreBase + t.rva);
            if (MH_CreateHook(target, t.detour, reinterpret_cast<LPVOID*>(t.orig)) != MH_OK) {
                Log("HOOK CREATE FAILED (%s)", t.name);
                for (int i = 0; i < ncreated; i++)
                    MH_RemoveHook(created[i]);
                Log("NO X10 LIFECYCLE HOOKS INSTALLED");
                return false;
            }
            created[ncreated++] = target;
        }
        for (auto& t : targets) {
            void* target = reinterpret_cast<void*>(gameCoreBase + t.rva);
            if (MH_EnableHook(target) != MH_OK) {
                Log("HOOK ENABLE FAILED (%s)", t.name);
                for (int i = 0; i < ncreated; i++) {
                    MH_DisableHook(created[i]);
                    MH_RemoveHook(created[i]);
                }
                Log("NO X10 LIFECYCLE HOOKS INSTALLED");
                return false;
            }
        }
        Log("4 lifecycle hooks installed (Populate/Add/InstanceA/InstanceB)");
        return true;
    }

    void LogGameplayLuaInit() {
        Log("X10 gameplay Lua initialized (RegisterScriptData)");
    }

    void X10Log(const char* fmt, ...) {
        OpenLog();
        if (!s_log) return;
        LONG id = InterlockedIncrement(&s_seq);
        fprintf(s_log, "%06ld ", id);
        va_list ap;
        va_start(ap, fmt);
        vfprintf(s_log, fmt, ap);
        va_end(ap);
        fprintf(s_log, "\n");
        fflush(s_log);
    }

    int LuaPing(hks::lua_State* L) {
        (void)L;
        Log("X10 probe gameplay script alive (Ping)");
        return 0;
    }

    int LuaLogMsg(hks::lua_State* L) {
        const char* s = hks::tolstring(L, 1, nullptr);
        FILE* f = nullptr;
        char tmp[MAX_PATH] = {};
        DWORD n = GetTempPathA(sizeof(tmp), tmp);
        char path[MAX_PATH] = {};
        snprintf(path, sizeof(path), "%sX10Probe.log", n ? tmp : ".\\");
        f = fopen(path, "a");
        if (f) {
            fprintf(f, "%s\n", s ? s : "(nil)");
            fclose(f);
        }
        return 0;
    }

    int Register(hks::lua_State* L) {
        hks::createtable(L, 0, 2);
        PushLuaMethod(L, LuaPing, "LuaPing", -2, "Ping");
        PushLuaMethod(L, LuaLogMsg, "LuaLogMsg", -2, "LogMsg");
        hks::setfield(L, hks::LUA_GLOBAL, "X10Lifecycle");
        return 0;
    }
}
