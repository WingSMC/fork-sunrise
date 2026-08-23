#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace sunrise::state::build_data::entities {

/**
 * Installed entity tags the catalog can hold.
 * The sweep reports one row per installed entity of the whole package set, so this is the ceiling
 * of the spawnable list. A pass that reaches it keeps the rows it has and says how many it dropped.
 */
inline constexpr std::size_t kEntityCapacity = 131'072;
/** Package families the catalog can hold. The live count is below 200. */
inline constexpr std::size_t kFamilyCapacity = 512;
/** The longest installed package family measured is below 48 bytes. */
inline constexpr std::size_t kFamilyNameLength = 64;

/** One package family and the number of entities it installs. */
struct Family {
    /** Leaf package name without the patch suffix or the extension, with no null. */
    std::array<char, kFamilyNameLength> name{};
    /** Entity rows that name this family. */
    std::uint32_t entityCount{};
    std::uint8_t nameLength{};
};

/**
 * One installed entity tag.
 * The row holds only what the packages declare. Whether the tag is loaded, and which object type it
 * carries, is a property of the running game, so it is asked for at use time and never stored.
 */
struct Entity {
    std::uint32_t tag{};
    /** Row of the family bank that installs the tag. */
    std::uint16_t familyIndex{};
};

/**
 * Tests whether one byte may appear in a package family name.
 * The families are lowercase identifiers, so any other byte fails the row instead of joining it.
 * @param value Candidate byte.
 * @return True for a lowercase letter, a digit, an underscore, or a period.
 */
[[nodiscard]] constexpr bool family_character(char value) noexcept {
    return (value >= 'a' && value <= 'z') || (value >= '0' && value <= '9') || value == '_'
           || value == '.';
}

} // namespace sunrise::state::build_data::entities
