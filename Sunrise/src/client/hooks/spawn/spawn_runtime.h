#pragma once

#include <array>
#include <cstdint>

namespace sunrise::client::hooks::spawn {

/** Invalid native object datum. */
inline constexpr std::uint32_t kInvalidDatum = 0xFFFFFFFFU;

/** Resolves the native entity factory and attaches its game-thread request service. */
[[nodiscard]] bool install() noexcept;

/** Detaches the request service and drops unresolved native targets. */
void uninstall() noexcept;

/** @return True when every native target needed to instantiate an entity is ready. */
[[nodiscard]] bool ready() noexcept;

/** @return True when the current destination has this entity definition resident. */
[[nodiscard]] bool is_tag_resident(std::uint32_t tag) noexcept;

/**
 * Reads the native object type of one resident entity definition.
 * @param tag Runtime-extracted entity tag.
 * @param type Receives the native object type.
 * @return True when the definition is resident and its type was readable.
 */
[[nodiscard]] bool object_type(std::uint32_t tag, std::uint8_t& type) noexcept;

/**
 * Queues one entity for creation on the local-player update thread.
 * Combatants receive a second native transform application after entering the object datum table;
 * this completes their authored activation without waiting for damage to wake their behavior.
 * @param tag Runtime-extracted resident entity tag.
 * @param position World-space creation position.
 * @return True when the one-entry request slot accepted the spawn.
 */
[[nodiscard]] bool request_at(std::uint32_t tag,
                              const std::array<float, 3>& position) noexcept;

} // namespace sunrise::client::hooks::spawn
