/**
 * The debug module's interface. It reports what the client can prove about the local player and
 * about what the camera points at, and it says so when a value is not reachable.
 *
 * Every reading comes from the world probe, which is a pick over the bodies the physics tick
 * reports. It is not the game's own collision query, so static geometry is never named here.
 */

#include "debug_panel.h"

#include <array>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <imgui.h>
#include <span>
#include <string_view>

#include "../../../client/hooks/bootflow/bootflow_hook_lifecycle.h"
#include "../../../middleware/content/packages/tables/region_reader.h"
#include "../../../state/activity/destination/activity_destination_snapshot.h"
#include "../../../state/activity/membership/activity_membership_query.h"
#include "../../../state/activity/runtime.h"
#include "../../world/world_probe.h"

namespace sunrise::client::ui::debug {
namespace {

namespace activity = state::activity;
namespace tables = middleware::content::packages::tables;

/** Left edge of the value column, in authored pixels before the label is measured. */
constexpr char kWidestLabel[] = "Physics component";
/** Shown for a value the client cannot read yet. */
constexpr char kUnread[] = "-";
/** Bytes the memory view shows at once. */
constexpr std::size_t kViewBytes = 128;
/** Bytes one memory-view row holds. */
constexpr std::size_t kViewColumns = 16;
/** Largest offset the memory view starts at. It keeps the read inside one object. */
constexpr int kMaximumViewOffset = 4096;
/** Height of the dump view, in authored pixels. It holds about ten lines. */
constexpr float kDumpViewHeight = 200.0F;

/** Offset the memory view starts at, held for the session only. */
int g_viewOffset = 0;
/** True while the memory view reads the picked target instead of the player. */
bool g_viewTarget = true;

/** @return The value column, measured from the widest label this page draws. */
[[nodiscard]] float value_column() noexcept {
    return ImGui::CalcTextSize(kWidestLabel).x + (ImGui::GetStyle().ItemSpacing.x * 2.0F);
}

/** Draws one label and its value. @param label Row label. @param value Formatted value. */
void draw_row(const char* label, std::string_view value) noexcept {
    ImGui::TextDisabled("%s", label);
    ImGui::SameLine(value_column());
    ImGui::TextUnformatted(value.data(), value.data() + value.size());
}

/** Draws one label and a formatted value. */
void draw_value(const char* label, const char* format, ...) noexcept {
    ImGui::TextDisabled("%s", label);
    ImGui::SameLine(value_column());
    va_list arguments;
    va_start(arguments, format);
    ImGui::TextV(format, arguments);
    va_end(arguments);
}

/**
 * Draws one vector row. @param label Row label. @param value Three lanes. @param known Validity.
 */
void draw_vector(const char* label, const world::Vector& value, bool known) noexcept {
    if (!known) {
        draw_row(label, kUnread);
        return;
    }
    draw_value(label,
               "%.2f  %.2f  %.2f",
               static_cast<double>(value[0]),
               static_cast<double>(value[1]),
               static_cast<double>(value[2]));
}

/** Draws one address row. @param label Row label. @param address Pointer to report. */
void draw_address(const char* label, const void* address) noexcept {
    if (address == nullptr) {
        draw_row(label, kUnread);
        return;
    }
    draw_value(label,
               "0x%llX",
               static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(address)));
}

/** Draws where the player is, from the client's own published position and camera pose. */
void draw_player_position(const world::Report& report) noexcept {
    const world::LocalReport& local = report.local;
    ImGui::TextUnformatted("Player position");
    ImGui::Separator();
    draw_vector("World position", local.position, local.present);
    draw_vector("Velocity", local.velocity, local.velocityValid);
    if (local.velocityValid) {
        draw_value("Speed", "%.2f units/s", static_cast<double>(local.speed));
    } else {
        draw_row("Speed", kUnread);
    }
    draw_vector("Camera forward", local.pose.forward, local.pose.forwardValid);
    if (local.pose.forwardValid) {
        draw_value("Camera angles",
                   "yaw %.1f deg  pitch %.1f deg",
                   static_cast<double>(local.yawDegrees),
                   static_cast<double>(local.pitchDegrees));
    } else {
        draw_row("Camera angles", kUnread);
    }
    // The eye position is a hypothesis about the pose block, so its state is named, not assumed.
    draw_vector("Eye position", local.pose.position, local.pose.positionValid);
    if (!local.pose.positionValid) {
        ImGui::TextDisabled("The eye position offset is unconfirmed, so the ray starts at the "
                            "player body instead.");
    }
}

/** Draws what the player is, which is the object and the physics component behind them. */
void draw_player_state(const world::Report& report) noexcept {
    const world::LocalReport& local = report.local;
    const std::uint64_t sessionId =
        activity::membership::live_region_session(activity::kAbsentSessionId);
    const bool joined = sessionId != activity::kAbsentSessionId;
    const bool inWorld = client::hooks::bootflow::in_world() && joined;

    ImGui::TextUnformatted("Player state");
    ImGui::Separator();
    draw_row("In world", inWorld ? "yes" : "no");
    if (local.controlled) {
        draw_value("Object handle", "%u", local.handleIndex);
    } else {
        draw_row("Object handle", "no controlled object");
    }
    draw_address("Physics component", local.component);
    if (!joined) {
        draw_row("Activity", "not in an activity");
        return;
    }
    activity::destination::DestinationSelection selection{};
    (void)activity::destination::snapshot(sessionId, selection);
    const std::string_view name{reinterpret_cast<const char*>(selection.packageName.data()),
                                selection.packageNameLength};
    draw_row("Activity", name.empty() ? std::string_view("unknown") : name);
    draw_value("Session", "%llu", static_cast<unsigned long long>(sessionId));
    const std::int32_t region = activity::membership::reported_region(sessionId);
    if (region < 0) {
        draw_row("Region", "unknown");
        return;
    }
    const auto index = static_cast<std::uint32_t>(region);
    draw_value("Region",
               "%u  bubble %u  state %u",
               index,
               index / tables::kSliceSetIndexFactor,
               index % tables::kSliceSetIndexFactor);
}

/** Draws the ray the pick was made along. */
void draw_aim(const world::Report& report, world::PickSettings& limits, bool& changed) noexcept {
    ImGui::TextUnformatted("Crosshair ray");
    ImGui::Separator();
    if (!report.rayValid) {
        ImGui::TextDisabled("No camera pose has been published yet.");
        return;
    }
    draw_row("Ray origin", report.rayFromCamera ? "camera eye" : "player body");
    draw_vector("Origin", report.rayOrigin, true);
    // The client cannot ask the game where the ray meets the world, so the reported point is the
    // ray at a set distance and not a surface hit.
    draw_vector("Point at probe distance", report.rayPoint, true);
    ImGui::TextDisabled("This point is the ray at the probe distance. It is not a surface hit: "
                        "the game's collision query is not reachable from the client.");
    ImGui::SetNextItemWidth(-FLT_MIN);
    float probeDistance = limits.probeDistance;
    if (ImGui::SliderFloat("##probe_distance",
                           &probeDistance,
                           world::kMinimumPickRange,
                           limits.range,
                           "probe distance %.0f units")) {
        limits.probeDistance = probeDistance;
        changed = true;
    }
}

/** Draws the body the pick found in front of the crosshair. */
void draw_target(const world::Report& report) noexcept {
    ImGui::TextUnformatted("Target at crosshair");
    ImGui::Separator();
    if (!report.target.present) {
        ImGui::TextDisabled("No moving body is inside the pick radius. Only bodies the physics "
                            "tick reports can be picked, so map geometry never appears here.");
        return;
    }
    const world::TargetReport& target = report.target;
    draw_value("Object handle", "%u", target.handleIndex);
    draw_vector("World position", target.position, true);
    draw_vector("Closest ray point", target.rayPoint, true);
    draw_value("Distance", "%.2f units", static_cast<double>(target.distance));
    draw_value("Off the ray",
               "%.2f units  (%.1f deg)",
               static_cast<double>(target.offset),
               static_cast<double>(target.angleDegrees));
    draw_vector("Velocity", target.velocity, true);
    draw_value("Speed", "%.2f units/s", static_cast<double>(target.speed));
    draw_address("Physics component", target.component);
}

/** Draws what the client cannot report yet, so the gap is stated instead of being left blank. */
void draw_unit_state() noexcept {
    ImGui::TextUnformatted("Unit state");
    ImGui::Separator();
    ImGui::TextWrapped("Health, combatant state and AI state are not reported yet. The client "
                       "reaches a body through the physics component, and no field on that path "
                       "is known to hold them.");
    ImGui::TextWrapped("The game executable is packed, so these offsets cannot be found by "
                       "reading the file. They have to be found in the running process. The "
                       "memory view below and the log dump exist for that work.");
}

/** Draws the raw bytes of one tracked component, which is how new offsets are found. */
void draw_memory_view(const world::Report& report) noexcept {
    ImGui::TextUnformatted("Memory view");
    ImGui::Separator();
    ImGui::Checkbox("Read the target instead of the player", &g_viewTarget);
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::SliderInt("##view_offset", &g_viewOffset, 0, kMaximumViewOffset, "offset %d bytes");

    const void* base = g_viewTarget ? report.target.component : report.local.component;
    if (base == nullptr) {
        ImGui::TextDisabled("No component to read.");
        return;
    }
    const auto* const address =
        reinterpret_cast<const std::byte*>(base) + static_cast<std::size_t>(g_viewOffset);
    std::array<std::byte, kViewBytes> bytes{};
    const std::size_t read = world::read_raw(address, bytes);
    if (read == 0) {
        ImGui::TextDisabled("The read failed. The component may have been released.");
        return;
    }
    for (std::size_t row = 0; row < read; row += kViewColumns) {
        std::array<char, 128> line{};
        int written = std::snprintf(
            line.data(), line.size(), "%04zu ", static_cast<std::size_t>(g_viewOffset) + row);
        for (std::size_t column = 0; column < kViewColumns && row + column < read; ++column) {
            if (written <= 0 || static_cast<std::size_t>(written) >= line.size()) {
                break;
            }
            written += std::snprintf(line.data() + written,
                                     line.size() - static_cast<std::size_t>(written),
                                     " %02X",
                                     static_cast<unsigned>(bytes[row + column]));
        }
        ImGui::TextUnformatted(line.data());
    }
    // The same bytes read as floats, which is what a position, a rotation or a health value in
    // this engine most often is.
    std::array<float, 4> floats{};
    if (world::read_raw(address, std::as_writable_bytes(std::span{floats})) == sizeof floats) {
        ImGui::Text("as floats  %.3f  %.3f  %.3f  %.3f",
                    static_cast<double>(floats[0]),
                    static_cast<double>(floats[1]),
                    static_cast<double>(floats[2]),
                    static_cast<double>(floats[3]));
    }
}

/**
 * Draws the last dump, and offers it as one block of text for the clipboard.
 *
 * The dump is shown here because the shipped log level for the client channel is `warn`, which
 * drops the log copy. This view does not depend on that level.
 */
void draw_dump() noexcept {
    std::array<world::DumpLine, world::kDumpLineCount> lines{};
    const std::size_t held = world::dump_lines(lines);
    if (held == 0) {
        return;
    }
    ImGui::Spacing();
    if (ImGui::Button("Copy the dump")) {
        std::array<char, world::kDumpLineCount * world::kDumpLineCapacity> block{};
        std::size_t length = 0;
        for (std::size_t index = 0; index < held; ++index) {
            const int written = std::snprintf(
                block.data() + length, block.size() - length, "%s\n", lines[index].data());
            if (written <= 0) {
                break;
            }
            length += static_cast<std::size_t>(written);
        }
        ImGui::SetClipboardText(block.data());
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%zu lines", held);
    if (ImGui::BeginChild("dump", ImVec2(0.0F, kDumpViewHeight), ImGuiChildFlags_Borders)) {
        for (std::size_t index = 0; index < held; ++index) {
            ImGui::TextUnformatted(lines[index].data());
        }
    }
    ImGui::EndChild();
}

/** Draws the pick limits and the probe's own counters. */
void draw_probe(const world::Report& report, world::PickSettings& limits, bool& changed) noexcept {
    ImGui::TextUnformatted("Probe");
    ImGui::Separator();
    draw_value("Tracked bodies", "%u", report.trackedBodies);
    draw_value("Considered this pass", "%u", report.consideredBodies);
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::SliderFloat("##pick_radius",
                           &limits.radius,
                           world::kMinimumPickRadius,
                           world::kMaximumPickRadius,
                           "pick radius %.2f units")) {
        changed = true;
    }
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::SliderFloat("##pick_range",
                           &limits.range,
                           world::kMinimumPickRange,
                           world::kMaximumPickRange,
                           "pick range %.0f units")) {
        changed = true;
    }
    if (ImGui::Button("Write a log dump")) {
        world::request_dump();
    }
    ImGui::SameLine();
    if (world::dump_pending()) {
        ImGui::TextDisabled("Waiting for the next game tick.");
    } else {
        ImGui::TextDisabled("Reads the camera block, the player and the nearest bodies.");
    }
    draw_dump();
}

} // namespace

/** Draws the debug module inside the active Core UI frame. */
void draw() noexcept {
    // The probe reads only while this page asks it to, so the request is renewed every frame.
    world::request_scan();
    const world::Report report = world::snapshot();
    world::PickSettings limits = world::settings();
    bool changed = false;

    draw_player_position(report);
    ImGui::Spacing();
    draw_player_state(report);
    ImGui::Spacing();
    draw_aim(report, limits, changed);
    ImGui::Spacing();
    draw_target(report);
    ImGui::Spacing();
    draw_unit_state();
    ImGui::Spacing();
    if (ImGui::CollapsingHeader("Memory view##debug")) {
        draw_memory_view(report);
    }
    ImGui::Spacing();
    draw_probe(report, limits, changed);

    if (changed) {
        world::publish_settings(limits);
    }
}

} // namespace sunrise::client::ui::debug
