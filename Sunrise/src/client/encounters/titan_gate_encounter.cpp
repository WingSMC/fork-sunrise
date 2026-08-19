#include "titan_gate_encounter.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <string_view>
#include <vector>

#include "../../core/filesystem/path.h"
#include "../../core/logging/log.h"
#include "../../middleware/content/packages/named_tags.h"
#include "../../middleware/content/packages/reader/reader.h"
#include "../../middleware/content/packages/tables/scenario_reader.h"
#include "../../state/activity/definition.h"
#include "../../state/activity/destination/activity_destination_snapshot.h"
#include "../../state/activity/membership/activity_membership_query.h"
#include "../../state/build_data/runtime.h"
#include "../content/items/packages/internal.h"
#include "../hooks/spawn/spawn_runtime.h"
#include "../player/player_position.h"

namespace sunrise::client::encounters::titan_gate {
namespace {

namespace named_tags = middleware::content::packages::named_tags;
namespace reader = middleware::content::packages::reader;
namespace tables = middleware::content::packages::tables;
namespace destination = state::activity::destination;
namespace membership = state::activity::membership;
namespace scenarios = state::build_data::scenarios;
namespace spawn = hooks::spawn;

/** Titan's runtime-extracted map-package stem; unrelated to `CharacterClass::titan`. */
constexpr std::string_view kTitanMapStem = "planet";
/** Native entity object type used by hostile combatants. */
constexpr std::uint8_t kCombatantObjectType = 12;
/** One world unit is one metre for placement and player transforms in this build. */
constexpr float kTriggerRadius = 50.0F;
constexpr float kTriggerRadiusSquared = kTriggerRadius * kTriggerRadius;
/** Keeps a newly instantiated combatant above the authored gate's collision floor. */
constexpr float kSpawnLift = 1.0F;
/** Bounded gates retained for one destination arrival. */
constexpr std::size_t kGateCapacity = 32;
/** Named gate entity definitions retained before the placement walk. */
constexpr std::size_t kGateTagCapacity = 128;
/** Class of an authored placement blob. */
constexpr std::uint32_t kPlacementClass = 0x808099D6U;
/** Class of an entity definition. */
constexpr std::uint32_t kEntityClass = 0x80809C0FU;
constexpr std::size_t kRecordBase = 0x30;
constexpr std::size_t kRecordStride = 0x90;
constexpr std::size_t kRecordTagOffset = 0x00;
constexpr std::size_t kRecordPositionOffset = 0x20;
constexpr float kCoordinateLimit = 20'000.0F;
constexpr std::size_t kMaximumDepth = 2;
constexpr std::size_t kReadBudget = 120'000;
constexpr std::size_t kVisitedCapacity = 1U << 17U;

struct Gate final {
    std::array<float, 3> position{};
    bool inside{};
    bool fired{};
};

struct NameCollection final {
    std::array<std::uint32_t, kGateTagCapacity> gateTags{};
    std::size_t gateTagCount{};
    std::uint32_t acolyteTag{spawn::kInvalidDatum};
};

struct ExtractPass final {
    reader::Source source{};
    reader::Scratch* scratch{};
    const NameCollection* names{};
    std::array<Gate, kGateCapacity>* gates{};
    std::size_t* gateCount{};
    std::vector<std::uint32_t> visited{};
    std::size_t reads{};
};

std::uint64_t g_sessionId{state::activity::kAbsentSessionId};
std::array<char, destination::kPackageNameCapacity> g_destination{};
std::size_t g_destinationLength{};
std::array<Gate, kGateCapacity> g_gates{};
std::size_t g_gateCount{};
std::uint32_t g_acolyteTag{spawn::kInvalidDatum};
bool g_prepared{};

/** Large package-reader state lives off the game thread stack. */
reader::Scratch g_scratch{};
ExtractPass g_pass{};

/** @return True when ASCII text contains a marker without case. */
[[nodiscard]] bool contains_insensitive(std::string_view text, std::string_view marker) noexcept {
    if (marker.empty() || marker.size() > text.size()) {
        return false;
    }
    for (std::size_t start = 0; start <= text.size() - marker.size(); ++start) {
        std::size_t index = 0;
        while (index < marker.size()) {
            const char value = text[start + index];
            const char lowered = value >= 'A' && value <= 'Z'
                                     ? static_cast<char>(value - 'A' + 'a')
                                     : value;
            if (lowered != marker[index]) {
                break;
            }
            ++index;
        }
        if (index == marker.size()) {
            return true;
        }
    }
    return false;
}

/** @return True for a named Hive spawn-door or portal definition. */
[[nodiscard]] bool gate_name(std::string_view name) noexcept {
    const bool aperture = contains_insensitive(name, "door")
                       || contains_insensitive(name, "gate")
                       || contains_insensitive(name, "portal");
    const bool spawnDoor = contains_insensitive(name, "spawn") && aperture;
    const bool hiveDoor = contains_insensitive(name, "hive") && aperture;
    return spawnDoor || hiveDoor;
}

/** @return True for an ordinary Hive Acolyte rather than a named variant. */
[[nodiscard]] bool acolyte_name(std::string_view name) noexcept {
    return contains_insensitive(name, "acolyte") && !contains_insensitive(name, "taken")
        && !contains_insensitive(name, "boss") && !contains_insensitive(name, "major")
        && !contains_insensitive(name, "champion") && !contains_insensitive(name, "nightmare");
}

/** Collects runtime named entity tags needed by this encounter. */
[[nodiscard]] bool collect_name(void* context, const named_tags::Entry& entry) noexcept {
    auto& output = *static_cast<NameCollection*>(context);
    if (entry.classId != kEntityClass || entry.nameLength == 0) {
        return true;
    }
    const std::string_view name(entry.name.data(), entry.nameLength);
    if (gate_name(name) && output.gateTagCount < output.gateTags.size()) {
        output.gateTags[output.gateTagCount++] = entry.tag;
    }
    if (output.acolyteTag == spawn::kInvalidDatum && acolyte_name(name)
        && spawn::is_tag_resident(entry.tag)) {
        std::uint8_t type = 0;
        if (spawn::object_type(entry.tag, type) && type == kCombatantObjectType) {
            output.acolyteTag = entry.tag;
        }
    }
    return true;
}

/** @return True when a word has the package-tag shape used by placement chains. */
[[nodiscard]] bool tag_shaped(std::uint32_t value) noexcept {
    return (value >> 24U) == 0x80U && (value >> 16U) != 0x8080U;
}

/** Marks one package tag visited without unbounded allocation. */
[[nodiscard]] bool mark_visited(ExtractPass& pass, std::uint32_t tag) noexcept {
    if (pass.visited.size() != kVisitedCapacity) {
        pass.visited.assign(kVisitedCapacity, 0U);
    }
    std::size_t slot = (tag * 2654435761U) % kVisitedCapacity;
    for (std::size_t probe = 0; probe < kVisitedCapacity; ++probe) {
        std::uint32_t& held = pass.visited[slot];
        if (held == 0U) {
            held = tag;
            return true;
        }
        if (held == tag) {
            return false;
        }
        slot = slot + 1 == kVisitedCapacity ? 0 : slot + 1;
    }
    return false;
}

/** @return True when the extracted tag names one of the discovered Hive gates. */
[[nodiscard]] bool wanted_gate(const NameCollection& names, std::uint32_t tag) noexcept {
    return std::find(names.gateTags.begin(),
                     names.gateTags.begin() + static_cast<std::ptrdiff_t>(names.gateTagCount),
                     tag)
        != names.gateTags.begin() + static_cast<std::ptrdiff_t>(names.gateTagCount);
}

/** Adds one unique gate transform to bounded encounter storage. */
void append_gate(ExtractPass& pass, const std::array<float, 3>& position) noexcept {
    for (std::size_t index = 0; index < *pass.gateCount; ++index) {
        const auto& existing = (*pass.gates)[index].position;
        const float x = existing[0] - position[0];
        const float y = existing[1] - position[1];
        const float z = existing[2] - position[2];
        if (x * x + y * y + z * z < 1.0F) {
            return;
        }
    }
    if (*pass.gateCount < pass.gates->size()) {
        (*pass.gates)[(*pass.gateCount)++].position = position;
    }
}

/** Reads matching entity records from one authored placement blob. */
void read_placements(ExtractPass& pass, std::span<const std::byte> blob) noexcept {
    for (std::size_t offset = kRecordBase; offset + kRecordStride <= blob.size();
         offset += kRecordStride) {
        std::uint32_t tag = 0;
        std::array<float, 4> position{};
        std::memcpy(&tag, blob.data() + offset + kRecordTagOffset, sizeof tag);
        std::memcpy(position.data(), blob.data() + offset + kRecordPositionOffset, sizeof position);
        if (!tag_shaped(tag) || !wanted_gate(*pass.names, tag) || position[3] != 1.0F) {
            continue;
        }
        bool valid = true;
        for (std::size_t lane = 0; lane < 3; ++lane) {
            valid = valid && std::isfinite(position[lane])
                 && std::fabs(position[lane]) <= kCoordinateLimit;
        }
        if (valid) {
            append_gate(pass, {position[0], position[1], position[2]});
        }
    }
}

/** Follows one placed-handle tag to its placement blob. */
void extract_tag(ExtractPass& pass, std::uint32_t tag, std::size_t depth) noexcept {
    if (depth > kMaximumDepth || pass.reads >= kReadBudget || !mark_visited(pass, tag)) {
        return;
    }
    std::vector<std::byte> blob{};
    std::uint32_t classId = 0;
    if (!reader::read_tag(pass.source, *pass.scratch, tag, blob, classId)) {
        return;
    }
    ++pass.reads;
    if (classId == kPlacementClass) {
        read_placements(pass, blob);
        return;
    }
    if (classId == kEntityClass) {
        return;
    }
    for (std::size_t offset = 0; offset + sizeof(std::uint32_t) <= blob.size(); offset += 4) {
        std::uint32_t child = 0;
        std::memcpy(&child, blob.data() + offset, sizeof child);
        if (tag_shaped(child)) {
            extract_tag(pass, child, depth + 1);
        }
    }
}

/** Extracts gate placements from one object registry. */
void extract_registry(ExtractPass& pass, std::uint32_t registryTag) noexcept {
    std::vector<std::byte> registry{};
    if (!reader::read_tag(pass.source, *pass.scratch, registryTag, registry)) {
        return;
    }
    constexpr std::array<std::size_t, 3> descriptors{
        tables::kRegistryFirstDescriptor,
        tables::kRegistrySecondDescriptor,
        tables::kRegistryThirdDescriptor,
    };
    for (const std::size_t descriptor : descriptors) {
        tables::Array objects{};
        if (!tables::registry_objects(registry, descriptor, objects)) {
            continue;
        }
        for (std::uint64_t objectIndex = 0; objectIndex < objects.count; ++objectIndex) {
            std::uint32_t objectTag = 0;
            std::vector<std::byte> object{};
            if (!tables::registry_object_at(registry, objects, objectIndex, objectTag)
                || !reader::read_tag(pass.source, *pass.scratch, objectTag, object)) {
                continue;
            }
            tables::Array bubbles{};
            if (!tables::object_bubbles(object, bubbles)) {
                continue;
            }
            for (std::uint64_t bubbleIndex = 0; bubbleIndex < bubbles.count; ++bubbleIndex) {
                tables::ObjectBubble bubble{};
                if (!tables::object_bubble_at(object, bubbles, bubbleIndex, bubble)) {
                    continue;
                }
                for (std::uint64_t slot = 0; slot < bubble.handleCount; ++slot) {
                    std::uint32_t handle = 0;
                    if (tables::object_placed_handle_at(object, bubble, slot, handle) && handle != 0) {
                        extract_tag(pass, handle, 0);
                    }
                }
            }
        }
    }
}

/** Walks public Titan slice sets to every authored gate placement. */
[[nodiscard]] bool extract_gates(const scenarios::Definition& layout,
                                 const NameCollection& names,
                                 core::path::Buffer& directory,
                                 reader::BlockKeys& keys) noexcept {
    std::vector<std::byte> scenario{};
    g_pass = {};
    g_pass.source = {directory.chars.data(), &keys};
    g_pass.scratch = &g_scratch;
    g_pass.names = &names;
    g_pass.gates = &g_gates;
    g_pass.gateCount = &g_gateCount;
    g_gateCount = 0;
    g_gates = {};
    if (!reader::read_tag(g_pass.source, g_scratch, layout.tag, scenario)) {
        return false;
    }
    tables::Array bubbles{};
    if (!tables::scenario_bubbles(scenario, bubbles)) {
        return false;
    }
    for (std::uint64_t bubbleIndex = 0; bubbleIndex < bubbles.count; ++bubbleIndex) {
        tables::Bubble bubble{};
        if (!tables::bubble_at(scenario, bubbles, bubbleIndex, bubble)) {
            continue;
        }
        for (std::uint64_t stateIndex = 0; stateIndex < bubble.stateCount; ++stateIndex) {
            tables::SliceState state{};
            if (!tables::slice_state_at(scenario, bubble, stateIndex, state) || !state.isPublic
                || state.entryTag == 0) {
                continue;
            }
            std::vector<std::byte> entry{};
            tables::SliceEntry parsed{};
            if (!reader::read_tag(g_pass.source, g_scratch, state.entryTag, entry)
                || !tables::slice_entry(entry, parsed) || parsed.registryTag == 0) {
                continue;
            }
            extract_registry(g_pass, parsed.registryTag);
        }
    }
    reader::close_files(g_scratch);
    return true;
}

/** Reports one preparation outcome without leaking authored coordinates. */
void report_prepare(const char* result, const char* reason) noexcept {
    std::array<char, 192> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=titan_gate stage=prepare gates=%zu acolyte=0x%08X "
                                      "result=%s reason=%s",
                                      g_gateCount,
                                      g_acolyteTag,
                                      result,
                                      reason);
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         std::string_view(result) == "ok" ? core::log::Level::info
                                                          : core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/** Builds all encounter data from the installed package corpus once per Titan arrival. */
[[nodiscard]] bool prepare(std::string_view destinationName) noexcept {
    scenarios::Definition layout{};
    if (!state::build_data::find_scenario_layout(destinationName, layout)) {
        report_prepare("fail", "scenario");
        return false;
    }
    const std::string_view stem(layout.spawnStem.data(), layout.spawnStemLength);
    if (stem != kTitanMapStem) {
        return false;
    }
    core::path::Buffer directory{};
    reader::BlockKeys keys{};
    if (!content::items::packages::package_directory(directory)
        || !content::items::packages::collect_keys(keys)) {
        SecureZeroMemory(&keys, sizeof keys);
        report_prepare("fail", "packages");
        return false;
    }
    NameCollection names{};
    named_tags::DirectoryResult namedResult{};
    const bool named = named_tags::extract_directory(
        directory.chars.data(), &collect_name, &names, namedResult);
    std::sort(names.gateTags.begin(),
              names.gateTags.begin() + static_cast<std::ptrdiff_t>(names.gateTagCount));
    names.gateTagCount = static_cast<std::size_t>(std::unique(
                             names.gateTags.begin(),
                             names.gateTags.begin()
                                 + static_cast<std::ptrdiff_t>(names.gateTagCount))
                         - names.gateTags.begin());
    g_acolyteTag = names.acolyteTag;
    const bool extracted = named && names.gateTagCount != 0
                        && g_acolyteTag != spawn::kInvalidDatum
                        && extract_gates(layout, names, directory, keys);
    SecureZeroMemory(&keys, sizeof keys);
    if (!extracted || g_gateCount == 0) {
        report_prepare("fail", !named ? "names" : names.gateTagCount == 0 ? "gate_tags"
                                                                            : "placements");
        return false;
    }
    report_prepare("ok", "ready");
    return true;
}

/** @return Squared three-dimensional world distance. */
[[nodiscard]] float distance_squared(const std::array<float, 3>& first,
                                     const std::array<float, 3>& second) noexcept {
    const float x = first[0] - second[0];
    const float y = first[1] - second[1];
    const float z = first[2] - second[2];
    return x * x + y * y + z * z;
}

/** Rebinds one new activity arrival and returns its destination name. */
[[nodiscard]] bool current_destination(std::string_view& name) noexcept {
    const std::uint64_t sessionId =
        membership::live_region_session(state::activity::kAbsentSessionId);
    if (sessionId == state::activity::kAbsentSessionId) {
        reset();
        return false;
    }
    destination::DestinationSelection selection{};
    if (!destination::snapshot(sessionId, selection) || selection.packageNameLength == 0) {
        return false;
    }
    name = {reinterpret_cast<const char*>(selection.packageName.data()),
            selection.packageNameLength};
    if (sessionId != g_sessionId || name != std::string_view(g_destination.data(), g_destinationLength)) {
        reset();
        g_sessionId = sessionId;
        g_destinationLength = name.size();
        std::copy(name.begin(), name.end(), g_destination.begin());
    }
    return true;
}

} // namespace

void poll() noexcept {
    if (!spawn::ready()) {
        return;
    }
    std::string_view destinationName{};
    if (!current_destination(destinationName)) {
        return;
    }
    if (!g_prepared) {
        g_prepared = true;
        if (!prepare(destinationName)) {
            return;
        }
    }
    if (g_gateCount == 0 || g_acolyteTag == spawn::kInvalidDatum) {
        return;
    }
    const player::position::Snapshot player = player::position::snapshot();
    if (!player.present) {
        return;
    }
    for (std::size_t index = 0; index < g_gateCount; ++index) {
        Gate& gate = g_gates[index];
        const bool inside = distance_squared(player.position, gate.position) <= kTriggerRadiusSquared;
        const bool entered = inside && !gate.inside;
        gate.inside = inside;
        if (!entered || gate.fired) {
            continue;
        }
        std::array<float, 3> spawnPosition = gate.position;
        spawnPosition[2] += kSpawnLift;
        if (spawn::request_at(g_acolyteTag, spawnPosition)) {
            gate.fired = true;
        }
    }
}

void reset() noexcept {
    g_sessionId = state::activity::kAbsentSessionId;
    g_destination = {};
    g_destinationLength = 0;
    g_gates = {};
    g_gateCount = 0;
    g_acolyteTag = spawn::kInvalidDatum;
    g_prepared = false;
    g_pass = {};
}

} // namespace sunrise::client::encounters::titan_gate
