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
#include <cstring>
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
/** Bytes of one object record the unit view shows. It covers a whole datum-array record. */
constexpr std::size_t kObjectViewBytes = 224;
/** Height of the object record view, in authored pixels. */
constexpr float kObjectViewHeight = 180.0F;
/** Columns of the nearby-body table, in draw order. */
constexpr int kNearbyColumnCount = 8;
/** Nearby-body table flags. The table scrolls on its own so a crowded radius stays readable. */
constexpr ImGuiTableFlags kNearbyTableFlags = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY
                                              | ImGuiTableFlags_SizingStretchProp;
/** Height of the nearby-body table, in authored pixels. It holds about eight rows. */
constexpr float kNearbyTableHeight = 200.0F;

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
    draw_value("Object handle", "index %u", target.handleIndex);
    if (target.objectResolved) {
        draw_value("Full handle", "0x%08X", target.handle);
        draw_address("Object record", target.object);
    } else {
        draw_row("Object record", "The datum array holds no live object under that index.");
    }
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

/** Draws the surface the game's own world trace reports under the crosshair. */
void draw_surface(const world::Report& report) noexcept {
    ImGui::TextUnformatted("Surface at crosshair");
    ImGui::Separator();
    if (!report.surface.present) {
        ImGui::TextDisabled("The world trace reported no hit inside the probe distance, or the "
                            "spawn hooks are not installed.");
        return;
    }
    const world::SurfaceReport& surface = report.surface;
    draw_vector("Hit position", surface.point, true);
    draw_value("Distance", "%.2f units", static_cast<double>(surface.distance));
    draw_value("Segment covered", "%.3f", static_cast<double>(surface.fraction));
    if (surface.codeResolved) {
        draw_value("Trace code", "%d  (a live object handle)", surface.code);
        draw_address("Object record", surface.codeObject);
    } else {
        draw_value("Trace code", "%d  (no object under it, so a material index)", surface.code);
    }
}

/**
 * Draws every body inside the nearby radius, nearest first.
 *
 * It lists the same bodies the crosshair pick chooses from, so map geometry is absent here too.
 */
void draw_nearby(const world::Report& report,
                 world::PickSettings& limits,
                 bool& changed) noexcept {
    ImGui::TextUnformatted("Bodies in radius");
    ImGui::Separator();
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::SliderFloat("##nearby_radius",
                           &limits.nearbyRadius,
                           world::kMinimumNearbyRadius,
                           world::kMaximumNearbyRadius,
                           "radius %.0f units")) {
        changed = true;
    }
    if (report.nearbyFound == 0) {
        ImGui::TextDisabled("No moving body is inside the radius. Only bodies the physics tick "
                            "reports are listed, so map geometry never appears here.");
        return;
    }
    if (report.nearbyFound > report.nearbyCount) {
        ImGui::TextDisabled("%u bodies inside the radius. The nearest %u are listed.",
                            report.nearbyFound,
                            report.nearbyCount);
    } else {
        ImGui::TextDisabled("%u bodies inside the radius.", report.nearbyFound);
    }
    if (!ImGui::BeginTable("##debug_nearby_table",
                           kNearbyColumnCount,
                           kNearbyTableFlags,
                           ImVec2(0.0F, kNearbyTableHeight))) {
        return;
    }
    ImGui::TableSetupColumn("#");
    ImGui::TableSetupColumn("index");
    ImGui::TableSetupColumn("handle");
    ImGui::TableSetupColumn("distance");
    ImGui::TableSetupColumn("position");
    ImGui::TableSetupColumn("speed");
    ImGui::TableSetupColumn("component");
    ImGui::TableSetupColumn("record");
    ImGui::TableHeadersRow();
    for (std::uint32_t index = 0; index < report.nearbyCount; ++index) {
        const world::NearbyBody& body = report.nearby[index];
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        // The picked body is marked so the list and the crosshair section can be lined up.
        if (body.isTarget) {
            ImGui::Text("%u *", index);
        } else {
            ImGui::Text("%u", index);
        }
        ImGui::TableNextColumn();
        ImGui::Text("%u", body.handleIndex);
        ImGui::TableNextColumn();
        if (body.objectResolved) {
            ImGui::Text("0x%08X", body.handle);
        } else {
            ImGui::TextDisabled("%s", kUnread);
        }
        ImGui::TableNextColumn();
        ImGui::Text("%.1f", static_cast<double>(body.distance));
        ImGui::TableNextColumn();
        ImGui::Text("%.1f  %.1f  %.1f",
                    static_cast<double>(body.position[0]),
                    static_cast<double>(body.position[1]),
                    static_cast<double>(body.position[2]));
        ImGui::TableNextColumn();
        if (body.velocityValid) {
            ImGui::Text("%.1f", static_cast<double>(body.speed));
        } else {
            ImGui::TextDisabled("%s", kUnread);
        }
        ImGui::TableNextColumn();
        ImGui::Text("0x%llX",
                    static_cast<unsigned long long>(
                        reinterpret_cast<std::uintptr_t>(body.component)));
        ImGui::TableNextColumn();
        if (body.objectResolved) {
            ImGui::Text("0x%llX",
                        static_cast<unsigned long long>(
                            reinterpret_cast<std::uintptr_t>(body.object)));
        } else {
            ImGui::TextDisabled("%s", kUnread);
        }
    }
    ImGui::EndTable();
    ImGui::TextDisabled("A row marked with * is the body the crosshair pick chose.");
}

/** Draws what the client cannot report yet, so the gap is stated instead of being left blank. */
void draw_unit_state(const world::Report& report) noexcept {
    ImGui::TextUnformatted("Unit state");
    ImGui::Separator();
    ImGui::TextWrapped("Health, combatant state and AI state have no known offset yet. The game "
                       "executable is packed, so they cannot be found by reading the file. They "
                       "have to be found in the running process, which is what this view is for.");
    if (!report.target.objectResolved) {
        ImGui::TextDisabled("No object record to read. Pick a unit with the crosshair first.");
        return;
    }
    std::array<std::byte, kObjectViewBytes> bytes{};
    const std::size_t read = world::read_object(report.target.handle, bytes);
    if (read == 0) {
        ImGui::TextDisabled("The record read failed. The object may have been released.");
        return;
    }
    ImGui::Text("Object record, %zu bytes", read);
    if (ImGui::BeginChild(
            "object_record", ImVec2(0.0F, kObjectViewHeight), ImGuiChildFlags_Borders)) {
        for (std::size_t row = 0; row < read; row += kViewColumns) {
            std::array<char, 160> line{};
            int written = std::snprintf(line.data(), line.size(), "%04zu ", row);
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
    }
    ImGui::EndChild();
    // Health in this engine is most often a float that falls as the unit is hurt, so the record
    // is also offered as floats. A lane that only falls under fire is the one to look at.
    if (ImGui::TreeNodeEx("Record as floats", ImGuiTreeNodeFlags_SpanAvailWidth)) {
        for (std::size_t offset = 0; offset + sizeof(float) * 4 <= read;
             offset += sizeof(float) * 4) {
            std::array<float, 4> lanes{};
            std::memcpy(lanes.data(), bytes.data() + offset, sizeof lanes);
            ImGui::Text("%04zu  %12.3f  %12.3f  %12.3f  %12.3f",
                        offset,
                        static_cast<double>(lanes[0]),
                        static_cast<double>(lanes[1]),
                        static_cast<double>(lanes[2]),
                        static_cast<double>(lanes[3]));
        }
        ImGui::TreePop();
    }
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
    draw_surface(report);
    ImGui::Spacing();
    draw_nearby(report, limits, changed);
    ImGui::Spacing();
    draw_unit_state(report);
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
