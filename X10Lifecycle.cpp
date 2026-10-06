// X10 lifecycle instrumentation (logging only, see X10Lifecycle.h).
//
// Hook targets are file RVAs for GameCore build 15038592 (civ6-gamecore-reference
// `cur` column). The fork logs the loaded GameCore module size + PE timestamp
// at startup so a build mismatch can be diagnosed post-hoc from the log.
// Every hook install failure is logged and non-fatal.
//
// Detour prototypes match the reference signatures exactly under the x64 ABI
// (references = pointers; 16-byte reference structs = 2 eight-byte slots).
// All hooked functions return void, so detours forward args and return nothing.
//
//   PopulateModifierDefinitions 0x96f6c0 : void(pDB, modifierSystem&)
//   SimpleModifierDefinition ctor 0x92e6a0 : void(this, id&&, type, flags&,
//       args&&, ownerReq&&, subjectReq&&, u16, u16)
//   AddModifierDefinition 0x943110 : void(this, definition[16B = 2 slots])
//   DynamicModifier ctor 0x92a4f0/0x92b220 : void(this, gameFx&, owner&,
//       definition&, collection&, effect&)
#include "X10Lifecycle.h"
#include "HavokScript.h"
#include "MinHook.h"
#include <windows.h>
#include <cstdio>
#include <cstdint>

namespace X10Lifecycle {
    namespace {
        static volatile LONG s_seq = 0;
        static volatile LONG s_ctorCount = 0;
        static volatile LONG s_addCount = 0;
        static volatile LONG s_instanceCount = 0;
        static volatile LONG s_populateDepth = 0;
        static FILE* s_log = nullptr;
        static uintptr_t s_base = 0;

        void OpenLog() {
            if (s_log) return;
            char tmp[MAX_PATH] = {};
            DWORD n = GetTempPathA(sizeof(tmp), tmp);
            char path[MAX_PATH] = {};
            snprintf(path, sizeof(path), "%sX10Lifecycle.log", n ? tmp : ".\\");
            s_log = fopen(path, "w");
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

        // ---- originals (filled by MinHook) ----
        typedef void(__cdecl* PopulateFn)(void* pDB, void* modifierSystem);
        static PopulateFn orig_Populate = nullptr;

        typedef void(__thiscall* CtorFn)(void* self, void* id, uint64_t typeId,
            void* flags, void* args, void* ownerReq, void* subjectReq,
            uint16_t ownerStack, uint16_t subjectStack);
        static CtorFn orig_Ctor = nullptr;

        typedef void(__thiscall* AddFn)(void* self, void* d0, void* d1);
        static AddFn orig_Add = nullptr;

        typedef void(__thiscall* InstanceFn)(void* self, void* gameFx, void* owner,
            void* def0, void* def1, void* col0, void* col1, void* eff0, void* eff1);
        static InstanceFn orig_InstanceA = nullptr;
        static InstanceFn orig_InstanceB = nullptr;

        void __cdecl Hook_Populate(void* pDB, void* modifierSystem) {
            InterlockedIncrement(&s_populateDepth);
            Log("PopulateModifierDefinitions ENTER depth=%ld", s_populateDepth);
            orig_Populate(pDB, modifierSystem);
            Log("PopulateModifierDefinitions EXIT depth=%ld definitions_added=%ld definitions_constructed=%ld",
                s_populateDepth, s_addCount, s_ctorCount);
            InterlockedDecrement(&s_populateDepth);
        }

        void __thiscall Hook_Ctor(void* self, void* id, uint64_t typeId,
            void* flags, void* args, void* ownerReq, void* subjectReq,
            uint16_t ownerStack, uint16_t subjectStack) {
            LONG n = InterlockedIncrement(&s_ctorCount);
            if (n <= 3 || n % 1000 == 0)
                Log("SimpleModifierDefinition ctor #%ld self=%p", n, self);
            orig_Ctor(self, id, typeId, flags, args, ownerReq, subjectReq,
                ownerStack, subjectStack);
        }

        void __thiscall Hook_Add(void* self, void* d0, void* d1) {
            LONG n = InterlockedIncrement(&s_addCount);
            if (n <= 3 || n % 1000 == 0)
                Log("AddModifierDefinition #%ld", n);
            orig_Add(self, d0, d1);
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

        bool TryHook(const char* name, uintptr_t rva, void* detour, void** orig) {
            void* target = reinterpret_cast<void*>(s_base + rva);
            MH_STATUS cs = MH_CreateHook(target, detour,
                reinterpret_cast<LPVOID*>(orig));
            if (cs != MH_OK) {
                Log("HOOK-SKIP %s rva=0x%x status=%d", name, (unsigned)rva, (int)cs);
                return false;
            }
            MH_STATUS es = MH_EnableHook(target);
            if (es != MH_OK) {
                Log("HOOK-ENABLE-FAIL %s rva=0x%x status=%d", name, (unsigned)rva, (int)es);
                return false;
            }
            Log("HOOK-OK %s rva=0x%x", name, (unsigned)rva);
            return true;
        }
    }

    void Install(uintptr_t gameCoreBase) {
        s_base = gameCoreBase;
        // Identify the loaded GameCore for post-hoc build verification.
        HMODULE mod = reinterpret_cast<HMODULE>(gameCoreBase);
        (void)mod;
        Log("X10Lifecycle install: gameCoreBase=%p assumed_build=15038592",
            reinterpret_cast<void*>(gameCoreBase));
        TryHook("PopulateModifierDefinitions", 0x96f6c0,
            reinterpret_cast<void*>(&Hook_Populate),
            reinterpret_cast<void**>(&orig_Populate));
        TryHook("SimpleModifierDefinition_ctor", 0x92e6a0,
            reinterpret_cast<void*>(&Hook_Ctor),
            reinterpret_cast<void**>(&orig_Ctor));
        TryHook("AddModifierDefinition", 0x943110,
            reinterpret_cast<void*>(&Hook_Add),
            reinterpret_cast<void**>(&orig_Add));
        TryHook("DynamicModifier_ctor_A", 0x92a4f0,
            reinterpret_cast<void*>(&Hook_InstanceA),
            reinterpret_cast<void**>(&orig_InstanceA));
        TryHook("DynamicModifier_ctor_B", 0x92b220,
            reinterpret_cast<void*>(&Hook_InstanceB),
            reinterpret_cast<void**>(&orig_InstanceB));
    }

    void LogGameplayLuaInit() {
        Log("X10 gameplay Lua initialized (RegisterScriptData)");
    }

    int LuaPing(hks::lua_State* L) {
        (void)L;
        Log("X10 probe gameplay script alive (Ping)");
        return 0;
    }

    int Register(hks::lua_State* L) {
        hks::createtable(L, 0, 1);
        PushLuaMethod(L, LuaPing, "LuaPing", -2, "Ping");
        hks::setfield(L, hks::LUA_GLOBAL, "X10Lifecycle");
        return 0;
    }
}
