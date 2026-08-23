#include "entity_catalog.h"

#include <algorithm>
#include <cstring>
#include <string_view>

#include "../table.h"

namespace sunrise::state::build_data::entities {
namespace {

Lock g_lock;
Table<Family, kFamilyCapacity> g_families;
Table<Entity, kEntityCapacity> g_entities;

[[nodiscard]] std::string_view name_of(const Family& family) noexcept {
    return {family.name.data(), family.nameLength};
}

[[nodiscard]] bool canonical(const Family& family) noexcept {
    if (family.nameLength == 0 || family.nameLength > family.name.size()) {
        return false;
    }
    for (std::size_t index = 0; index < family.name.size(); ++index) {
        const char value = family.name[index];
        if (index < family.nameLength ? !family_character(value) : value != '\0') {
            return false;
        }
    }
    return true;
}

} // namespace

void clear() noexcept {
    const Lock::Exclusive guard(g_lock);
    g_entities.clear();
    g_families.clear();
}

bool valid(std::span<const Family> families, std::span<const Entity> installed) noexcept {
    if (families.size() > kFamilyCapacity || installed.size() > kEntityCapacity) {
        return false;
    }
    // An empty catalog is complete: a package set with no installed entity publishes no row.
    if (families.empty() != installed.empty()) {
        return false;
    }
    std::size_t counted = 0;
    for (std::size_t index = 0; index < families.size(); ++index) {
        if (!canonical(families[index])
            || (index != 0 && !(name_of(families[index - 1]) < name_of(families[index])))
            || families[index].entityCount == 0
            || families[index].entityCount > installed.size() - counted) {
            return false;
        }
        counted += families[index].entityCount;
    }
    if (counted != installed.size()) {
        return false;
    }
    for (std::size_t index = 0; index < installed.size(); ++index) {
        if (installed[index].tag == 0 || installed[index].tag == 0xFFFFFFFFU
            || installed[index].familyIndex >= families.size()
            || (index != 0 && installed[index - 1].tag >= installed[index].tag)) {
            return false;
        }
    }
    return true;
}

bool replace(std::span<const Family> families, std::span<const Entity> installed) noexcept {
    if (!valid(families, installed)) {
        return false;
    }
    const Lock::Exclusive guard(g_lock);
    if (!g_families.replace(families)) {
        return false;
    }
    if (!g_entities.replace(installed)) {
        // The family bank is already replaced, so a failed second array leaves no half catalog.
        g_families.clear();
        return false;
    }
    return true;
}

bool find(std::uint32_t tag, Entity& installed) noexcept {
    installed = {};
    const Lock::Shared guard(g_lock);
    const std::span<const Entity> rows = g_entities.rows();
    const auto found = std::lower_bound(
        rows.begin(), rows.end(), tag, [](const Entity& row, std::uint32_t wanted) {
            return row.tag < wanted;
        });
    if (found == rows.end() || found->tag != tag) {
        return false;
    }
    installed = *found;
    return true;
}

bool snapshot_families(std::span<Family> output, std::size_t& count) noexcept {
    const Lock::Shared guard(g_lock);
    return g_families.snapshot(output, count);
}

bool snapshot(std::span<Entity> output, std::size_t& count) noexcept {
    const Lock::Shared guard(g_lock);
    return g_entities.snapshot(output, count);
}

std::size_t count() noexcept {
    const Lock::Shared guard(g_lock);
    return g_entities.count();
}

std::size_t family_count() noexcept {
    const Lock::Shared guard(g_lock);
    return g_families.count();
}

} // namespace sunrise::state::build_data::entities
