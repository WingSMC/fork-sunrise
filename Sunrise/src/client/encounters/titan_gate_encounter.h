#pragma once

namespace sunrise::client::encounters::titan_gate {

/**
 * Advances the Saturn-moon gate encounter from the live destination and player position.
 * The playable Titan character class is deliberately not an input to this service.
 */
void poll() noexcept;

/** Drops destination-owned trigger edges and runtime-extracted encounter data. */
void reset() noexcept;

} // namespace sunrise::client::encounters::titan_gate
