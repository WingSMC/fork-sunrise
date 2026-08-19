#include "spawn_runtime.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

#include "../../../core/logging/log.h"
#include "../../hooking/detour.h"
#include "../../patterns/image_scan.h"
#include "../teleport/runtime.h"

namespace sunrise::client::hooks::spawn {
namespace {

using namespace patterns;

constexpr std::string_view kPlacementInitializeText =
    "89 54 24 10 53 48 83 EC 20 48 8B D9 83 FA FF 0F 84 ? ? ? ? 48 8D 54 24 30 "
    "48 8D 4C 24 38 E8 ? ? ? ? 8B 44 24 30 83 F8 FF 0F 84 ? ? ? ? 48 8B 15 ? ? ? ?";
constexpr auto kPlacementInitialize =
    signature<signature_length(kPlacementInitializeText)>(kPlacementInitializeText);

constexpr std::string_view kDirectInitializeText =
    "48 89 5C 24 08 57 48 83 EC 20 8B DA 48 8B F9 83 FA FF 74 33 8B CA E8 ? ? ? ? "
    "48 C7 47 30 00 00 00 00 48 8B CF 48 C7 47 10 00 00 00 00";
constexpr auto kDirectInitialize =
    signature<signature_length(kDirectInitializeText)>(kDirectInitializeText);

constexpr std::string_view kObjectFactoryText =
    "40 53 48 83 EC 20 41 83 C9 FF 41 83 C8 FF 48 8B D9 E8 ? ? ? ? 48 8B C3 "
    "48 83 C4 20 5B C3";
constexpr auto kObjectFactory = signature<signature_length(kObjectFactoryText)>(kObjectFactoryText);

constexpr std::string_view kObjectTransformText =
    "48 89 5C 24 10 57 48 83 EC 70 0F 29 74 24 60 48 8B 05 ? ? ? ? 48 33 C4 "
    "48 89 44 24 50 0F 10 02 48 8B F9 0F 11 81 A0 00 00 00 0F 10 72 10 "
    "0F 29 74 24 30 E8 ? ? ? ? 8B D8 E8 ? ? ? ?";
constexpr auto kObjectTransform =
    signature<signature_length(kObjectTransformText)>(kObjectTransformText);

constexpr std::string_view kPlayerComponentUpdateText =
    "48 89 5C 24 10 55 57 41 54 41 56 41 57 48 8B EC 48 83 EC 70 45 33 E4 "
    "48 89 B4 24 A0 00 00 00 41 8B FC 48 8D 99 FC 02 00 00 4D 8B F0 4C 8B FA";
constexpr auto kPlayerComponentUpdate =
    signature<signature_length(kPlayerComponentUpdateText)>(kPlayerComponentUpdateText);

constexpr std::size_t kResolverCallOperand = 0x17;
constexpr std::size_t kResolverCallEnd = 0x1B;
constexpr std::size_t kDefinitionObjectType = 0x96;
constexpr std::uint8_t kCombatantObjectType = 12;
constexpr std::size_t kPlacementHeaderBytes = 0x40;
constexpr std::size_t kPlacementPayloadBytes = 0x800;
constexpr std::uintptr_t kObjectDatumDescriptorRva = 0x1F93420;
constexpr std::size_t kObjectDatumBaseOffset = 0x08;
constexpr std::size_t kObjectDatumStrideOffset = 0x10;
constexpr std::size_t kObjectDatumBytes = 0xE0;
constexpr std::size_t kObjectHandleOffset = 0x0C;
constexpr std::uint8_t kActivationAttempts = 8;

using PlacementInitialize = std::uint8_t(__fastcall*)(void*, std::uint32_t);
using ObjectFactory = std::uint32_t*(__fastcall*)(std::uint32_t*, void*);
using ObjectTransform = void(__fastcall*)(void*, const float*);
using TagResolver = const std::byte*(__fastcall*)(std::uint32_t);
using PlayerComponentUpdate = void(__fastcall*)(void*, void*, void*);

struct alignas(16) PlacementStorage final {
    std::array<std::byte, kPlacementHeaderBytes + kPlacementPayloadBytes> bytes{};
};

struct Request final {
    std::uint32_t tag{kInvalidDatum};
    std::array<float, 3> position{};
    bool pending{};
};

struct Activation final {
    std::uint32_t handle{kInvalidDatum};
    std::array<float, 8> transform{};
    std::uint8_t attempts{};
    bool pending{};
};

hooking::detour::Handle g_updateHook{};
std::atomic_bool g_installed{};
PlacementInitialize g_initialize{};
PlacementInitialize g_directInitialize{};
ObjectFactory g_factory{};
ObjectTransform g_transform{};
TagResolver g_resolver{};
HMODULE g_gameModule{};
SRWLOCK g_requestLock{SRWLOCK_INIT};
Request g_request{};
Activation g_activation{};

/** Reads one value without propagating a fault from game-owned storage. */
template <typename Value> [[nodiscard]] bool safe_read(const void* source, Value& value) noexcept {
    __try {
        std::memcpy(&value, source, sizeof value);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        value = {};
        return false;
    }
}

/** Initializes the game's relative placement descriptor storage. */
void reset_storage(PlacementStorage& storage) noexcept {
    storage = {};
    constexpr std::uint64_t zero = 0;
    constexpr std::uint64_t capacity = kPlacementPayloadBytes;
    constexpr std::uint64_t alignment = 0x10;
    std::memcpy(storage.bytes.data(), &zero, sizeof zero);
    std::memcpy(storage.bytes.data() + 0x10, &zero, sizeof zero);
    std::memcpy(storage.bytes.data() + 0x18, &kInvalidDatum, sizeof kInvalidDatum);
    std::memcpy(storage.bytes.data() + 0x20, &capacity, sizeof capacity);
    std::memcpy(storage.bytes.data() + 0x28, &alignment, sizeof alignment);
    std::memcpy(storage.bytes.data() + 0x30, &zero, sizeof zero);
}

/** @return The initialized placement descriptor, or null when its relative pointer is absent. */
[[nodiscard]] void* descriptor_of(PlacementStorage& storage) noexcept {
    std::int64_t relative = 0;
    return safe_read(storage.bytes.data(), relative) && relative != 0
               ? storage.bytes.data() + relative
               : nullptr;
}

/** @return A live object datum, or null after reclamation or a layout mismatch. */
[[nodiscard]] std::byte* resolve_object(std::uint32_t handle) noexcept {
    if (handle == kInvalidDatum || g_gameModule == nullptr) {
        return nullptr;
    }
    std::byte* const descriptor =
        reinterpret_cast<std::byte*>(g_gameModule) + kObjectDatumDescriptorRva;
    std::byte* base = nullptr;
    std::uint32_t stride = 0;
    if (!safe_read(descriptor + kObjectDatumBaseOffset, base)
        || !safe_read(descriptor + kObjectDatumStrideOffset, stride) || base == nullptr
        || stride != kObjectDatumBytes) {
        return nullptr;
    }
    std::byte* const object = base + (handle & 0x1FFFU) * stride;
    std::uint32_t live = kInvalidDatum;
    return safe_read(object + kObjectHandleOffset, live) && live == handle ? object : nullptr;
}

/** Applies the authored transform once the new datum is live, which also wakes combatant logic. */
void service_activation() noexcept {
    if (!g_activation.pending || g_transform == nullptr) {
        return;
    }
    std::byte* const object = resolve_object(g_activation.handle);
    bool complete = false;
    if (object != nullptr) {
        __try {
            g_transform(object, g_activation.transform.data());
            complete = true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    if (complete || ++g_activation.attempts >= kActivationAttempts) {
        g_activation = {};
    }
}

/** Instantiates one entity with an upright world transform. */
[[nodiscard]] std::uint32_t spawn_one(std::uint32_t tag,
                                      const std::array<float, 3>& world) noexcept {
    PlacementStorage storage{};
    reset_storage(storage);
    std::uint32_t result = kInvalidDatum;
    constexpr std::array<float, 4> rotation{0.0F, 0.0F, 0.0F, 1.0F};
    const std::array<float, 4> position{world[0], world[1], world[2], 1.0F};
    __try {
        bool initialized = g_initialize(storage.bytes.data(), tag) != 0;
        if (!initialized && g_resolver(tag) != nullptr) {
            reset_storage(storage);
            initialized = g_directInitialize(storage.bytes.data(), tag) != 0;
        }
        void* const descriptor = initialized ? descriptor_of(storage) : nullptr;
        if (descriptor == nullptr) {
            return kInvalidDatum;
        }
        std::memcpy(static_cast<std::byte*>(descriptor) + 0x10, rotation.data(), sizeof rotation);
        std::memcpy(static_cast<std::byte*>(descriptor) + 0x20, position.data(), sizeof position);
        (void)g_factory(&result, descriptor);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        result = kInvalidDatum;
    }
    if (result != kInvalidDatum) {
        std::uint8_t type = 0;
        if (object_type(tag, type) && type == kCombatantObjectType) {
            g_activation = {};
            g_activation.handle = result;
            std::copy(rotation.begin(), rotation.end(), g_activation.transform.begin());
            std::copy(position.begin(), position.end(), g_activation.transform.begin() + 4);
            g_activation.pending = true;
        }
    }
    return result;
}

/** Consumes the one-entry request slot on the local player's native update. */
void service_request() noexcept {
    Request request{};
    AcquireSRWLockExclusive(&g_requestLock);
    if (g_request.pending) {
        request = g_request;
        g_request = {};
    }
    ReleaseSRWLockExclusive(&g_requestLock);
    if (!request.pending) {
        return;
    }
    const std::uint32_t handle = spawn_one(request.tag, request.position);
    std::array<char, 160> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=titan_gate stage=spawn tag=0x%08X handle=0x%08X result=%s",
                                      request.tag,
                                      handle,
                                      handle == kInvalidDatum ? "fail" : "ok");
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         handle == kInvalidDatum ? core::log::Level::warn
                                                 : core::log::Level::info,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/** Runs encounter requests after the game's component update. */
void __fastcall player_component_update(void* object, void* input, void* authored) noexcept {
    const auto next = reinterpret_cast<PlayerComponentUpdate>(g_updateHook.original);
    if (next != nullptr) {
        next(object, input, authored);
    }
    if (teleport::is_controlled_object(object)) {
        service_activation();
        service_request();
    }
}

/** Logs one optional hook installation failure. */
[[nodiscard]] bool fail(const char* reason) noexcept {
    std::array<char, 96> line{};
    const int written = std::snprintf(
        line.data(), line.size(), "ev=spawn stage=install result=fail reason=%s", reason);
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(written)});
    }
    return false;
}

} // namespace

bool install() noexcept {
    if (g_installed.load(std::memory_order_acquire)) {
        return true;
    }
    std::byte* const initialize =
        scan_main_image_unique(kPlacementInitialize, "spawn_placement_initialize");
    std::byte* const direct =
        scan_main_image_unique(kDirectInitialize, "spawn_direct_initialize");
    std::byte* const factory = scan_main_image_unique(kObjectFactory, "spawn_object_factory");
    std::byte* const transform = scan_main_image_unique(kObjectTransform, "spawn_object_transform");
    std::byte* const update =
        scan_main_image_unique(kPlayerComponentUpdate, "spawn_player_component_update");
    if (initialize == nullptr || direct == nullptr || factory == nullptr || transform == nullptr
        || update == nullptr) {
        return fail("target");
    }

    g_initialize = reinterpret_cast<PlacementInitialize>(initialize);
    g_directInitialize = reinterpret_cast<PlacementInitialize>(direct);
    g_factory = reinterpret_cast<ObjectFactory>(factory);
    g_transform = reinterpret_cast<ObjectTransform>(transform);
    g_resolver = reinterpret_cast<TagResolver>(
        resolve_relative(direct + kResolverCallOperand, direct + kResolverCallEnd));
    g_gameModule = GetModuleHandleW(nullptr);
    if (g_resolver == nullptr || g_gameModule == nullptr
        || !hooking::detour::install(
            {update, reinterpret_cast<void*>(&player_component_update)}, g_updateHook)) {
        uninstall();
        return fail("attach");
    }
    g_installed.store(true, std::memory_order_release);
    core::log::write(
        core::log::Channel::client, core::log::Level::info, "ev=spawn stage=install result=ok");
    return true;
}

void uninstall() noexcept {
    g_installed.store(false, std::memory_order_release);
    (void)hooking::detour::uninstall(g_updateHook);
    g_updateHook = {};
    g_initialize = nullptr;
    g_directInitialize = nullptr;
    g_factory = nullptr;
    g_transform = nullptr;
    g_resolver = nullptr;
    g_gameModule = nullptr;
    AcquireSRWLockExclusive(&g_requestLock);
    g_request = {};
    g_activation = {};
    ReleaseSRWLockExclusive(&g_requestLock);
}

bool ready() noexcept {
    return g_installed.load(std::memory_order_acquire);
}

bool is_tag_resident(std::uint32_t tag) noexcept {
    if (!ready() || tag == kInvalidDatum || g_resolver == nullptr) {
        return false;
    }
    __try {
        return g_resolver(tag) != nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool object_type(std::uint32_t tag, std::uint8_t& type) noexcept {
    type = 0;
    if (!is_tag_resident(tag)) {
        return false;
    }
    __try {
        const std::byte* const definition = g_resolver(tag);
        return definition != nullptr && safe_read(definition + kDefinitionObjectType, type);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool request_at(std::uint32_t tag, const std::array<float, 3>& position) noexcept {
    if (!ready() || !is_tag_resident(tag)
        || !std::all_of(position.begin(), position.end(), [](float value) {
               return std::isfinite(value);
           })) {
        return false;
    }
    AcquireSRWLockExclusive(&g_requestLock);
    if (g_request.pending) {
        ReleaseSRWLockExclusive(&g_requestLock);
        return false;
    }
    g_request.tag = tag;
    g_request.position = position;
    g_request.pending = true;
    ReleaseSRWLockExclusive(&g_requestLock);
    return true;
}

} // namespace sunrise::client::hooks::spawn
