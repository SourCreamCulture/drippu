// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace suyu::recomp {

inline constexpr uint32_t kRecompImageAbiVersion = 1;
inline constexpr uint32_t kRecompRegsPrefixSize = 872;
inline constexpr uint32_t kRecompMaxModules = 16;
inline constexpr uint32_t kRecompBuildIdSize = 32;
inline constexpr uint32_t kRecompModuleNameSize = 32;

struct RecompImageAbi {
    uint32_t abi_version;
    uint32_t abi_size;
    uint32_t context_size;
    uint32_t regs_prefix_size;
    uint32_t module_index;
    uint8_t build_id[kRecompBuildIdSize];
    char module_name[kRecompModuleNameSize];
};

using RecompImageBlockFn = void (*)(void*);
using RecompImageLookupFn = RecompImageBlockFn (*)(uint64_t);
using RecompImageSetBaseFn = void (*)(uint64_t);
using RecompImageAbiFn = const RecompImageAbi* (*)();

struct RecompImageExports {
    RecompImageLookupFn lookup = nullptr;
    RecompImageSetBaseFn set_base = nullptr;
    RecompImageAbiFn abi = nullptr;
};

struct ImageExpect {
    const uint8_t* build_id = nullptr;
};

enum class ImageReject {
    Ok = 0,
    MissingLookup,
    MissingSetBase,
    MissingAbi,
    AbiVersion,
    AbiSize,
    RegsPrefix,
    EmptyName,
    BuildId,
    DuplicateIndex,
    IndexRange,
};

struct RecompModuleSlot {
    RecompImageSetBaseFn set_base = nullptr;
    uint64_t base = 0;
    const RecompImageAbi* abi = nullptr;
};

struct RecompModuleMap {
    RecompModuleSlot slots[kRecompMaxModules]{};
    uint32_t count = 0;
};

// Matches src/suyu/main.cpp LoadRecompiledImagesFrom and src/suyu_cmd/suyu.cpp
// today: a lookup export is enough to accept the image. ABI version, context
// size, content hash, and set_base are unresolved and not required.
inline ImageReject ValidateImageExports(const RecompImageExports& ex,
                                        const ImageExpect& expect = {}) {
    (void)expect;
    if (!ex.lookup) {
        return ImageReject::MissingLookup;
    }
    return ImageReject::Ok;
}

// Matches the suyu-cmd DLL path: missing files are skipped and survivors are
// packed into a vector. Bases are then applied by that vector's index.
inline ImageReject PlaceLoadedModule(RecompModuleMap& map, const RecompImageExports& ex) {
    if (!ex.lookup) {
        return ImageReject::MissingLookup;
    }
    if (map.count >= kRecompMaxModules) {
        return ImageReject::IndexRange;
    }
    const RecompImageAbi* abi = ex.abi ? ex.abi() : nullptr;
    map.slots[map.count++] = RecompModuleSlot{ex.set_base, 0, abi};
    return ImageReject::Ok;
}

inline void ApplyModuleBase(RecompModuleMap& map, size_t index, const char* name, uint64_t base) {
    (void)name;
    if (index < map.count && map.slots[index].set_base) {
        map.slots[index].base = base;
        map.slots[index].set_base(base);
    }
}

inline const RecompModuleSlot* SlotByName(const RecompModuleMap& map, const char* name) {
    if (!name) {
        return nullptr;
    }
    for (uint32_t i = 0; i < map.count; ++i) {
        const RecompImageAbi* abi = map.slots[i].abi;
        if (abi && std::strncmp(abi->module_name, name, kRecompModuleNameSize) == 0) {
            return &map.slots[i];
        }
    }
    return nullptr;
}

} // namespace suyu::recomp
