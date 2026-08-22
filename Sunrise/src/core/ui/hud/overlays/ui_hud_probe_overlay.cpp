/**
 * The world-probe overlays. They report the same readings as the Debug page, but as fixed corner
 * panes that take no input, so the game can be played while they are on screen.
 *
 * Every value comes from the world probe (`client/world/world_probe.cpp`). The probe reads only
 * while something asks it to, so each pane renews a scan request on the frame it draws.
 */

#include "ui_hud_probe_overlay.h"

#include <cstdint>
#include <imgui.h>
#include <string_view>

#include "../../../../client/world/world_probe.h"

namespace sunrise::core::ui::hud::overlays::probe {
namespace {

namespace world = client::world;

/** Widest label of these panes. It sets the value column for every row they draw. */
constexpr char kWidestLabel[] = "Camera angles";
/** Shown for a value the client cannot read yet. */
constexpr char kUnread[] = "-";
/** Shown while the probe has published nothing. */
constexpr char kNoReading[] = "waiting for a game tick";
/** Bodies the nearby pane lists. The rest are counted only, to keep the pane small. */
constexpr std::uint32_t kListedBodies = 10;
/** Columns of the nearby pane's table, in draw order. */
constexpr int kColumnCount = 5;

/** @return The value column, measured from the widest label these panes draw. */
[[nodiscard]] float value_column() noexcept {
    return ImGui::CalcTextSize(kWidestLabel).x + (ImGui::GetStyle().ItemSpacing.x * 2.0F);
}

/** Draws one label and its text. @param label Row label. @param value Text to show. */
void draw_row(const char* label, std::string_view value) noexcept {
    ImGui::TextDisabled("%s", label);
    ImGui::SameLine(value_column());
    ImGui::TextUnformatted(value.data(), value.data() + value.size());
}

/** Draws one label and a vector. @param known False when the value was not read. */
void draw_vector(const char* label, const world::Vector& value, bool known) noexcept {
    if (!known) {
        draw_row(label, kUnread);
        return;
    }
    ImGui::TextDisabled("%s", label);
    ImGui::SameLine(value_column());
    ImGui::Text("%.2f  %.2f  %.2f",
                static_cast<double>(value[0]),
                static_cast<double>(value[1]),
                static_cast<double>(value[2]));
}

/**
 * Reads one pass of the probe and renews the scan request behind it.
 * @return The last published report.
 */
[[nodiscard]] world::Report read_probe() noexcept {
    world::request_scan();
    return world::snapshot();
}

} // namespace

/** Draws where the player is and where the camera looks. */
void draw_player() noexcept {
    const world::Report report = read_probe();
    const world::LocalReport& local = report.local;
    if (!local.present && !local.pose.forwardValid) {
        ImGui::TextDisabled("%s", kNoReading);
        return;
    }
    draw_vector("Position", local.position, local.present);
    draw_vector("Velocity", local.velocity, local.velocityValid);
    if (local.velocityValid) {
        ImGui::TextDisabled("Speed");
        ImGui::SameLine(value_column());
        ImGui::Text("%.2f units/s", static_cast<double>(local.speed));
    } else {
        draw_row("Speed", kUnread);
    }
    if (local.pose.forwardValid) {
        ImGui::TextDisabled("Camera angles");
        ImGui::SameLine(value_column());
        ImGui::Text("yaw %.1f  pitch %.1f",
                    static_cast<double>(local.yawDegrees),
                    static_cast<double>(local.pitchDegrees));
    } else {
        draw_row("Camera angles", kUnread);
    }
    // The eye position is a hypothesis about the pose block, so its state is named, not assumed.
    draw_vector("Eye position", local.pose.position, local.pose.positionValid);
    if (local.controlled) {
        ImGui::TextDisabled("Object");
        ImGui::SameLine(value_column());
        ImGui::Text("index %u", local.handleIndex);
    } else {
        draw_row("Object", "no controlled object");
    }
}

/** Draws the body pick and the world trace under the crosshair. */
void draw_crosshair() noexcept {
    const world::Report report = read_probe();
    if (!report.rayValid) {
        ImGui::TextDisabled("no camera pose yet");
        return;
    }
    draw_row("Ray origin", report.rayFromCamera ? "camera eye" : "player body");
    if (report.target.present) {
        const world::TargetReport& target = report.target;
        ImGui::TextDisabled("Target");
        ImGui::SameLine(value_column());
        if (target.objectResolved) {
            ImGui::Text("index %u  handle 0x%08X", target.handleIndex, target.handle);
        } else {
            ImGui::Text("index %u  no live record", target.handleIndex);
        }
        ImGui::TextDisabled("Target range");
        ImGui::SameLine(value_column());
        ImGui::Text("%.2f units  off %.2f  %.2f units/s",
                    static_cast<double>(target.distance),
                    static_cast<double>(target.offset),
                    static_cast<double>(target.speed));
        draw_vector("Target position", target.position, true);
    } else {
        // The pick is over reported bodies only, so an empty result is expected on map geometry.
        draw_row("Target", "no body inside the pick radius");
    }
    if (report.surface.present) {
        const world::SurfaceReport& surface = report.surface;
        draw_vector("Surface", surface.point, true);
        ImGui::TextDisabled("Surface range");
        ImGui::SameLine(value_column());
        ImGui::Text("%.2f units  code %d%s",
                    static_cast<double>(surface.distance),
                    surface.code,
                    surface.codeResolved ? "  (object)" : "");
    } else {
        draw_row("Surface", "no trace hit");
    }
}

/** Draws the bodies inside the nearby radius, nearest first. */
void draw_nearby() noexcept {
    const world::Report report = read_probe();
    const world::PickSettings limits = world::settings();
    ImGui::TextDisabled("Bodies inside %.0f units: %u",
                        static_cast<double>(limits.nearbyRadius),
                        report.nearbyFound);
    if (report.nearbyCount == 0) {
        return;
    }
    if (!ImGui::BeginTable("##sunrise_hud_probe_nearby", kColumnCount)) {
        return;
    }
    ImGui::TableSetupColumn("index");
    ImGui::TableSetupColumn("handle");
    ImGui::TableSetupColumn("distance");
    ImGui::TableSetupColumn("position");
    ImGui::TableSetupColumn("speed");
    ImGui::TableHeadersRow();
    const std::uint32_t listed =
        report.nearbyCount < kListedBodies ? report.nearbyCount : kListedBodies;
    for (std::uint32_t index = 0; index < listed; ++index) {
        const world::NearbyBody& body = report.nearby[index];
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        // The picked body is marked so this pane and the crosshair pane can be lined up.
        if (body.isTarget) {
            ImGui::Text("%u *", body.handleIndex);
        } else {
            ImGui::Text("%u", body.handleIndex);
        }
        ImGui::TableNextColumn();
        if (body.objectResolved) {
            ImGui::Text("0x%08X", body.handle);
        } else {
            ImGui::TextDisabled("%s", kUnread);
        }
        ImGui::TableNextColumn();
        ImGui::Text("%.1f", static_cast<double>(body.distance));
        ImGui::TableNextColumn();
        ImGui::Text("%.1f %.1f %.1f",
                    static_cast<double>(body.position[0]),
                    static_cast<double>(body.position[1]),
                    static_cast<double>(body.position[2]));
        ImGui::TableNextColumn();
        if (body.velocityValid) {
            ImGui::Text("%.1f", static_cast<double>(body.speed));
        } else {
            ImGui::TextDisabled("%s", kUnread);
        }
    }
    ImGui::EndTable();
    if (report.nearbyFound > listed) {
        ImGui::TextDisabled("%u more, not listed here", report.nearbyFound - listed);
    }
}

} // namespace sunrise::core::ui::hud::overlays::probe
