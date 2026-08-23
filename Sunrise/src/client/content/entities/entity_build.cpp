#include "entity_build.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "../../../core/logging/log.h"
#include "../../../state/build_data/runtime.h"

namespace sunrise::client::content::entities {
namespace {

namespace reader = middleware::content::packages::reader;
namespace entities_state = state::build_data::entities;

/** Tag class of an installed entity, which is what a placement names. */
constexpr std::uint32_t kEntityClass = 0x80809C0FU;

/** One pass of the sweep. Nothing here survives the pass. */
struct Collection {
    std::vector<entities_state::Family> families{};
    std::vector<entities_state::Entity> installed{};
    /** Family name to its row in the list above, so a repeated family costs one lookup. */
    std::unordered_map<std::string, std::uint16_t> familyRows{};
    std::size_t packages{};
    std::size_t entries{};
    /** Rows the fixed catalog storage had no room for. */
    std::size_t dropped{};
    /** Entries whose package family holds a byte a family name may not carry. */
    std::size_t unnamed{};
};

/**
 * Folds one package family into the lowercase form the catalog stores.
 * @param family Leaf package name the sweep reports.
 * @param output Receives the folded name.
 * @return True when every byte fits a family name.
 */
[[nodiscard]] bool family_text(std::wstring_view family, std::string& output) noexcept {
    output.clear();
    if (family.empty() || family.size() > entities_state::kFamilyNameLength) {
        return false;
    }
    for (const wchar_t wide : family) {
        if (wide > 0x7F) {
            return false;
        }
        char value = static_cast<char>(wide);
        if (value >= 'A' && value <= 'Z') {
            value = static_cast<char>(value - 'A' + 'a');
        }
        if (!entities_state::family_character(value)) {
            return false;
        }
        output.push_back(value);
    }
    return true;
}

[[nodiscard]] bool collect_entity(void* context, const reader::ClassEntry& entry) noexcept {
    Collection& output = *static_cast<Collection*>(context);
    ++output.entries;
    if (entry.tag == 0 || entry.tag == 0xFFFFFFFFU) {
        return true;
    }
    if (output.installed.size() >= entities_state::kEntityCapacity) {
        // A full catalog costs the rows past it, nothing else. So they are counted and dropped.
        ++output.dropped;
        return true;
    }
    std::string name{};
    if (!family_text(entry.packageFamily, name)) {
        ++output.unnamed;
        return true;
    }
    const auto known = output.familyRows.find(name);
    std::uint16_t row = 0;
    if (known != output.familyRows.end()) {
        row = known->second;
    } else {
        if (output.families.size() >= entities_state::kFamilyCapacity) {
            ++output.dropped;
            return true;
        }
        entities_state::Family family{};
        family.nameLength = static_cast<std::uint8_t>(name.size());
        std::memcpy(family.name.data(), name.data(), name.size());
        row = static_cast<std::uint16_t>(output.families.size());
        output.families.push_back(family);
        output.familyRows.emplace(name, row);
    }
    output.installed.push_back({entry.tag, row});
    return true;
}

/** @return The family name of one row, for the ordering the catalog wants. */
[[nodiscard]] std::string_view name_of(const entities_state::Family& family) noexcept {
    return {family.name.data(), family.nameLength};
}

/**
 * Puts the pass into the canonical form the catalog accepts.
 * Entities rise by tag, families rise by name, and every family names at least one entity.
 * @param output Pass storage, rewritten in place.
 */
void canonicalize(Collection& output) noexcept {
    std::sort(output.installed.begin(),
              output.installed.end(),
              [](const entities_state::Entity& left, const entities_state::Entity& right) {
                  return left.tag < right.tag;
              });
    output.installed.erase(
        std::unique(output.installed.begin(),
                    output.installed.end(),
                    [](const entities_state::Entity& left, const entities_state::Entity& right) {
                        return left.tag == right.tag;
                    }),
        output.installed.end());

    // The rows are ordered by name, so the old row of every family has to be remapped once.
    std::vector<std::uint16_t> order(output.families.size());
    for (std::size_t index = 0; index < order.size(); ++index) {
        order[index] = static_cast<std::uint16_t>(index);
    }
    std::sort(order.begin(), order.end(), [&output](std::uint16_t left, std::uint16_t right) {
        return name_of(output.families[left]) < name_of(output.families[right]);
    });
    std::vector<std::uint32_t> counts(output.families.size(), 0);
    for (const entities_state::Entity& installed : output.installed) {
        ++counts[installed.familyIndex];
    }
    std::vector<std::uint16_t> remap(output.families.size(), 0);
    std::vector<entities_state::Family> sorted{};
    sorted.reserve(output.families.size());
    for (const std::uint16_t source : order) {
        if (counts[source] == 0) {
            // A family every entity of which was dropped names nothing, so it is not published.
            continue;
        }
        remap[source] = static_cast<std::uint16_t>(sorted.size());
        entities_state::Family family = output.families[source];
        family.entityCount = counts[source];
        sorted.push_back(family);
    }
    for (entities_state::Entity& installed : output.installed) {
        installed.familyIndex = remap[installed.familyIndex];
    }
    output.families = std::move(sorted);
}

void report(const Collection& output, const char* result) noexcept {
    std::array<char, 256> line{};
    const int length = std::snprintf(line.data(),
                                     line.size(),
                                     "ev=build_data stage=entities packages=%zu entries=%zu "
                                     "families=%zu entities=%zu unnamed=%zu dropped=%zu result=%s",
                                     output.packages,
                                     output.entries,
                                     output.families.size(),
                                     output.installed.size(),
                                     output.unnamed,
                                     output.dropped,
                                     result);
    if (length > 0) {
        core::log::write(core::log::Channel::client,
                         result[0] == 'o' ? core::log::Level::info : core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(length)});
    }
}

} // namespace

bool build(const reader::Source& source) noexcept {
    if (state::build_data::entities_ready()) {
        return true;
    }
    Collection output{};
    reader::ScanResult scan{};
    const bool swept =
        reader::scan_class_entries(source.directory, kEntityClass, &collect_entity, &output, scan);
    output.packages = scan.packages;
    if (!swept) {
        report(output, "sweep");
        return false;
    }
    canonicalize(output);
    const bool published = state::build_data::publish_entities(output.families, output.installed);
    report(output, published ? "ok" : "publish");
    return published;
}

} // namespace sunrise::client::content::entities
