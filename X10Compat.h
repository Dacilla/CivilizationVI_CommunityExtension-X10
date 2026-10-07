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
    // Config reader RVAs.
    uintptr_t getInstanceRva;
    uintptr_t hashRva;
    // Variant layout facts.
    uint16_t intMaskIds[8]; // engine int-mask ids (GetGameSpeedType pattern)
    uint16_t floatId;       // FLOAT32 scalar id: checked BEFORE the mask,
                            // since the engine mask also contains it but the
                            // engine's own callers only pass int-typed keys
}; 

static const GameCoreCompatibilityProfile kGameCoreProfiles[] = {
    {
        "user-validated-15038592",
        0x667c6f5b, 0xc60000,
        0x96f6c0, 0x943110, 0x92a4f0, 0x92b220,
        0x164c20, 0x606270,
        {1, 2, 3, 4, 5, 17, 18, 0xFFFF}, // engine mask, verbatim
        4,
    },
};

static const int kGameCoreProfileCount = 1;
