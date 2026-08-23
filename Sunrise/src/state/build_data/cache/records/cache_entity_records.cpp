#include <algorithm>
#include <span>

#include "codec.h"

namespace sunrise::state::build_data::cache::records {
namespace {

/** @return True when the row carries a lowercase family name and no byte past its length. */
[[nodiscard]] bool canonical_family(std::span<const char> name, std::uint8_t length) noexcept {
    if (length == 0 || length > name.size()) {
        return false;
    }
    for (std::size_t index = 0; index < name.size(); ++index) {
        const char value = name[index];
        if (index < length ? !entities::family_character(value) : value != '\0') {
            return false;
        }
    }
    return true;
}

} // namespace

bool encode(const entities::Family& value, EntityFamilyRecord& record) noexcept {
    record = {};
    if (!canonical_family(value.name, value.nameLength) || value.entityCount == 0) {
        return false;
    }
    record.name = value.name;
    record.entityCount = value.entityCount;
    record.nameLength = value.nameLength;
    return true;
}

bool decode(const EntityFamilyRecord& record, entities::Family& value) noexcept {
    value = {};
    if (!canonical_family(record.name, record.nameLength) || record.entityCount == 0
        || std::any_of(record.reserved.begin(), record.reserved.end(), [](std::uint8_t byte) {
               return byte != 0;
           })) {
        return false;
    }
    value.name = record.name;
    value.entityCount = record.entityCount;
    value.nameLength = record.nameLength;
    return true;
}

bool encode(const entities::Entity& value, EntityRecord& record) noexcept {
    record = {};
    if (value.tag == 0 || value.tag == 0xFFFFFFFFU) {
        return false;
    }
    record.tag = value.tag;
    record.familyIndex = value.familyIndex;
    return true;
}

bool decode(const EntityRecord& record, entities::Entity& value) noexcept {
    value = {};
    if (record.tag == 0 || record.tag == 0xFFFFFFFFU
        || std::any_of(record.reserved.begin(), record.reserved.end(), [](std::uint8_t byte) {
               return byte != 0;
           })) {
        return false;
    }
    value.tag = record.tag;
    value.familyIndex = record.familyIndex;
    return true;
}

} // namespace sunrise::state::build_data::cache::records
