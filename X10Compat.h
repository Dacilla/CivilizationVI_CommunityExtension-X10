// GameCore compatibility profile: ALL build-specific data in one place.
// Production startup selects a known profile or fails closed (no hooks).
// Current profiles target the validated user install; reference RVAs are the
// `cur` column (build 15038592) of civ6-gamecore-reference.
#pragma once
#include <cstdint>

struct GameCoreCompatibilityProfile {
    const char* name;
    uint32_t peTimestamp;
    uint32_t imageSize;
    // Hook RVAs.
    uintptr_t populateRva;
    uintptr_t addRva;
    uintptr_t instanceCtorA;
    uintptr_t instanceCtorB;
    // Engine getter (CALLED, never hooked): ModifierSystem::
    // GetModifierDefinition(system, out16, idString). Post-Add witness
    // resolves the registered definition through it; validated like hooks.
    uintptr_t getDefRva;
    // Config reader RVAs.
    uintptr_t getInstanceRva;
    uintptr_t hashRva;
    // Instance variant-manager member offsets (game first, then root).
    uintptr_t gameVariantsOff;
    uintptr_t rootVariantsOff;
    // Manager virtual lookup slot, in bytes.
    uintptr_t lookupVtableOff;
    // Variant layout facts.
    uint16_t intMaskIds[8]; // engine int-mask ids (GetGameSpeedType pattern)
    uint16_t floatId;       // FLOAT32 scalar id: checked BEFORE the mask,
                            // since the engine mask also contains it but the
                            // engine's own callers only pass int-typed keys
    // Variant scalar layout (validated against the typed-reader evidence).
    uintptr_t variantTypeOff;    // u16 type id
    uintptr_t variantFlagsOff;   // u32 flags (int path needs bit 0x1d clear)
    uintptr_t variantPayloadOff; // scalar payload (float32 / int32)
    // SimpleModifierDefinition layout.
    uintptr_t defIdOff;   // id string (SSO shape)
    uintptr_t argVecOff;  // argument vector {begin, end, cap}
    uintptr_t argStride;  // sizeof(ArgumentDefinition)
    uintptr_t argNameOff; // name SSO string within an element
    uintptr_t argValueOff;// value SSO string within an element
    // SSO string layout (MSVC std::string): inline bytes @+0 iff
    // capacity < ssoInlineThreshold; size @+ssoSizeOff, cap @+ssoCapOff.
    uintptr_t ssoSizeOff;
    uintptr_t ssoCapOff;
    uintptr_t ssoInlineThreshold;
    uintptr_t maxArgs; // sanity cap on argument-vector element count
}; 

static const GameCoreCompatibilityProfile kGameCoreProfiles[] = {
    {
        "user-validated-15038592",
        0x667c6f5b, 0xc60000,
        0x96f6c0, 0x943110, 0x92a4f0, 0x92b220,
        0x951d90, // ModifierSystem::GetModifierDefinition (call-only)
        0x164c20, 0x606270,
        0x88, 0x80, 0x68,
        {1, 2, 3, 4, 5, 17, 18, 0xFFFF}, // engine mask, verbatim
        4,
        0x0, 0x14, 0x8,          // variant type/flags/payload
        0x18, 0x40, 0xA0, 0x0, 0x20, // def id/vec/stride/name/value
        0x10, 0x18, 0x10, 64,    // SSO size/cap/inline-threshold, max args
    },
};

static const int kGameCoreProfileCount = 1;
