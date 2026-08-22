#pragma once

namespace sunrise::core::ui::hud::overlays::probe {

/** Draws the player readings inside the overlay window the stack has already started. */
void draw_player() noexcept;

/** Draws the crosshair readings inside the overlay window the stack has already started. */
void draw_crosshair() noexcept;

/** Draws the bodies in radius inside the overlay window the stack has already started. */
void draw_nearby() noexcept;

} // namespace sunrise::core::ui::hud::overlays::probe
