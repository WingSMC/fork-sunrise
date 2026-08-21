/**
 * The world probe. The physics sync reports every component it runs for, so the probe keeps what
 * it saw in a fixed table and picks the body nearest the camera ray once a frame.
 *
 * The pick is over reported bodies only, so static geometry is never picked. Beside it the probe
 * runs the game's own world trace, which the entity spawner exposes, and that one does hit static
 * geometry. The frame poll publishes one report under a sequence guard, so the interface reads a
 * whole pass rather than parts of two.
 */

#include "world_probe.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string_view>

#include "../../core/logging/log.h"
#include "../hooks/spawn/spawn_runtime.h"
#include "../hooks/teleport/runtime.h"
#include "../player/player_position.h"

namespace sunrise::client::world {
namespace {

namespace teleport = hooks::teleport;
namespace spawn = hooks::spawn;

/** Slots one insert probes before it takes the oldest of them. It bounds the insert cost. */
constexpr std::size_t kProbeLength = 8;
/** Shift that drops the alignment bits of a component pointer before it is hashed. */
constexpr std::size_t kPointerShift = 4;
/** Degrees in one radian, for the reported angles. */
constexpr float kDegreesPerRadian = 57.29577951F;
/** Shortest ray direction the pick accepts. A shorter one has no usable heading. */
constexpr float kMinimumDirectionLength = 0.001F;

/** Floats the dump reads from the camera block. */
constexpr std::size_t kDumpFloatCount = 32;
/** Floats the window starts before the confirmed forward vector, so its neighbours are seen. */
constexpr std::size_t kDumpFloatsBeforeForward = 4;
/** Floats one dump line carries. Four keep every line inside the log's line length. */
constexpr std::size_t kDumpFloatsPerLine = 4;
/** Bodies one dump lists. The nearest few are enough to name what is in front of the player. */
constexpr std::size_t kDumpBodyCount = 8;

/** One tracked body, written by the physics tick and read by the frame poll. */
struct Slot {
    void* component{};
    Vector position{};
    Vector velocity{};
    std::uint64_t seenTick{};
    std::uint32_t handleIndex{};
    bool velocityValid{};
};

/**
 * The table. Both the physics sync and the frame poll run on the game thread that owns the
 * player, so the table itself needs no lock; only the published report crosses to the interface.
 */
std::array<Slot, kBodyCapacity> g_slots{};

/** Odd while the report is being written, so a reader that sees one retries. */
std::atomic_uint32_t g_sequence{0};
Report g_report{};

/** Milliseconds the probe reads until. The interface pushes it forward while its page is open. */
std::atomic_uint64_t g_scanDeadline{0};

std::atomic<float> g_radius{kDefaultPickRadius};
std::atomic<float> g_range{kDefaultPickRange};
std::atomic<float> g_probeDistance{kDefaultPickRange};

/** Set by the interface and cleared by the frame poll that runs the dump. */
std::atomic_bool g_dumpRequested{false};
/** Odd while the dump buffer is being written, so a reader that sees one retries. */
std::atomic_uint32_t g_dumpSequence{0};
/** The last dump, held because the shipped client log level drops the log copy. */
std::array<DumpLine, kDumpLineCount> g_dumpLines{};
/** Lines the last dump wrote, at most the buffer's capacity. */
std::size_t g_dumpHeld{};

/** @return True while a scan request is still in force. */
[[nodiscard]] bool scanning(std::uint64_t now) noexcept {
    return now < g_scanDeadline.load(std::memory_order_relaxed);
}

/** @param component Component to place. @return Its first candidate slot. */
[[nodiscard]] std::size_t slot_of(const void* component) noexcept {
    const auto value = reinterpret_cast<std::uintptr_t>(component);
    return static_cast<std::size_t>((value >> kPointerShift) % kBodyCapacity);
}

/** @return The difference of two vectors. */
[[nodiscard]] Vector subtract(const Vector& left, const Vector& right) noexcept {
    return Vector{left[0] - right[0], left[1] - right[1], left[2] - right[2]};
}

/** @return The dot product of two vectors. */
[[nodiscard]] float dot(const Vector& left, const Vector& right) noexcept {
    return (left[0] * right[0]) + (left[1] * right[1]) + (left[2] * right[2]);
}

/** @return The length of one vector. */
[[nodiscard]] float length(const Vector& value) noexcept {
    return std::sqrt(dot(value, value));
}

/** @param origin Ray origin. @param direction Unit heading. @param distance Units to travel. */
[[nodiscard]] Vector along(const Vector& origin, const Vector& direction, float distance) noexcept {
    return Vector{origin[0] + (direction[0] * distance),
                  origin[1] + (direction[1] * distance),
                  origin[2] + (direction[2] * distance)};
}

/**
 * Finds or takes a slot for one component.
 * @param component Component to record.
 * @param now Current tick, used to age the probed slots.
 * @return The slot to write.
 */
[[nodiscard]] Slot& reserve(void* component, std::uint64_t now) noexcept {
    const std::size_t first = slot_of(component);
    std::size_t oldest = first;
    for (std::size_t step = 0; step < kProbeLength; ++step) {
        const std::size_t index = (first + step) % kBodyCapacity;
        Slot& slot = g_slots[index];
        if (slot.component == component || slot.component == nullptr) {
            return slot;
        }
        // A stale slot is free in every way that matters, so it is taken before a live one.
        if (now - slot.seenTick > now - g_slots[oldest].seenTick) {
            oldest = index;
        }
    }
    return g_slots[oldest];
}

/** @return The local player's part of the report, read in one pass. */
[[nodiscard]] LocalReport read_local() noexcept {
    LocalReport local{};
    void* const component = teleport::local_player_component();
    const player::position::Snapshot published = player::position::snapshot();
    local.component = component;
    local.controlled = teleport::controlled_object(local.handleIndex);
    if (published.present) {
        local.position = published.position;
        local.present = true;
    }
    if (component != nullptr) {
        Vector position{};
        if (teleport::read_position(component, position)) {
            local.position = position;
            local.present = true;
        }
        Vector velocity{};
        if (teleport::read_velocity(component, velocity)) {
            local.velocity = velocity;
            local.velocityValid = true;
            local.speed = length(velocity);
        }
    }
    if (teleport::camera_pose(local.pose)) {
        const Vector& forward = local.pose.forward;
        local.yawDegrees = std::atan2(forward[1], forward[0]) * kDegreesPerRadian;
        const float flat = std::sqrt((forward[0] * forward[0]) + (forward[1] * forward[1]));
        local.pitchDegrees = std::atan2(forward[teleport::kVerticalLane], flat) * kDegreesPerRadian;
    }
    return local;
}

/**
 * Picks the body nearest the ray.
 * @param report Report being built. Its local part is already filled.
 * @param now Current tick, used to skip stale slots.
 */
void pick(Report& report, std::uint64_t now) noexcept {
    const LocalReport& local = report.local;
    if (!local.pose.forwardValid) {
        return;
    }
    Vector direction = local.pose.forward;
    const float directionLength = length(direction);
    if (directionLength < kMinimumDirectionLength) {
        return;
    }
    for (float& lane : direction) {
        lane /= directionLength;
    }
    // The eye position is a hypothesis, so the player body is the origin whenever it is unproved.
    // Both origins sit on the same ray heading, so only the reported distances differ.
    if (local.pose.positionValid) {
        report.rayOrigin = local.pose.position;
        report.rayFromCamera = true;
    } else if (local.present) {
        report.rayOrigin = local.position;
    } else {
        return;
    }
    report.rayValid = true;
    const float probeDistance = g_probeDistance.load(std::memory_order_relaxed);
    report.rayPoint = along(report.rayOrigin, direction, probeDistance);

    const float radius = g_radius.load(std::memory_order_relaxed);
    const float range = g_range.load(std::memory_order_relaxed);
    for (const Slot& slot : g_slots) {
        if (slot.component == nullptr) {
            continue;
        }
        ++report.trackedBodies;
        if (now - slot.seenTick > kBodyLifetimeMs) {
            continue;
        }
        // The player's own body sits on the ray origin, so picking it would hide everything else.
        if (slot.component == local.component
            || (local.controlled && slot.handleIndex == local.handleIndex)) {
            continue;
        }
        const Vector offsetVector = subtract(slot.position, report.rayOrigin);
        const float forwardDistance = dot(offsetVector, direction);
        if (forwardDistance <= 0.0F || forwardDistance > range) {
            continue;
        }
        ++report.consideredBodies;
        const Vector rayPoint = along(report.rayOrigin, direction, forwardDistance);
        const float offset = length(subtract(slot.position, rayPoint));
        if (offset > radius) {
            continue;
        }
        // Nearest along the ray wins, which is what stands in front of the crosshair.
        if (report.target.present && forwardDistance >= report.target.distance) {
            continue;
        }
        report.target.present = true;
        report.target.position = slot.position;
        report.target.velocity = slot.velocity;
        report.target.speed = slot.velocityValid ? length(slot.velocity) : 0.0F;
        report.target.rayPoint = rayPoint;
        report.target.distance = forwardDistance;
        report.target.offset = offset;
        report.target.angleDegrees = std::atan2(offset, forwardDistance) * kDegreesPerRadian;
        report.target.handleIndex = slot.handleIndex;
        report.target.component = slot.component;
    }
    if (report.target.present) {
        // The physics component reports index bits only, so the record itself supplies the
        // generation that completes the handle.
        std::uint32_t handle = 0;
        report.target.object = spawn::object_record_by_index(report.target.handleIndex, handle);
        report.target.objectResolved = report.target.object != nullptr;
        report.target.handle = handle;
    }
    // The trace is the game's own query, so it reports the surface the crosshair rests on even
    // where no body is reported. The pick above cannot see that surface at all.
    spawn::SurfaceHit hit{};
    if (spawn::trace(report.rayOrigin, direction, probeDistance, hit)) {
        report.surface.present = true;
        report.surface.point = hit.point;
        report.surface.distance = hit.distance;
        report.surface.fraction = hit.fraction;
        report.surface.code = hit.code;
        if (hit.code >= 0) {
            report.surface.codeObject = spawn::object_record(static_cast<std::uint32_t>(hit.code));
            report.surface.codeResolved = report.surface.codeObject != nullptr;
        }
    }
}

/** @param report Report to publish under the sequence guard. */
void publish(const Report& report) noexcept {
    g_sequence.fetch_add(1, std::memory_order_acq_rel);
    g_report = report;
    g_sequence.fetch_add(1, std::memory_order_release);
}

/** Writes one log line on the client channel. @param text Line to write. */
void write_line(std::string_view text) noexcept {
    // The log copy is dropped on a normal run, because the shipped level for this channel is
    // `warn`. The dump buffer is what the interface shows, so it is always written.
    core::log::write(core::log::Channel::client, core::log::Level::info, text);
    if (g_dumpHeld >= kDumpLineCount) {
        return;
    }
    DumpLine& line = g_dumpLines[g_dumpHeld];
    line = {};
    const std::size_t length = std::min(text.size(), kDumpLineCapacity - 1);
    std::memcpy(line.data(), text.data(), length);
    ++g_dumpHeld;
}

/** Logs the camera pose block window, which is what confirms the eye-position offset. */
void log_camera_window() noexcept {
    const std::size_t start =
        teleport::camera_forward_offset() - (sizeof(float) * kDumpFloatsBeforeForward);
    std::array<float, kDumpFloatCount> window{};
    if (!teleport::read_camera_block(start, window)) {
        write_line("ev=world_probe stage=dump part=camera result=unavailable");
        return;
    }
    for (std::size_t index = 0; index < kDumpFloatCount; index += kDumpFloatsPerLine) {
        std::array<char, 160> line{};
        const std::size_t offset = start + (index * sizeof(float));
        const int written = std::snprintf(line.data(),
                                          line.size(),
                                          "ev=world_probe stage=dump part=camera off=%zu "
                                          "v0=%.3f v1=%.3f v2=%.3f v3=%.3f",
                                          offset,
                                          static_cast<double>(window[index]),
                                          static_cast<double>(window[index + 1]),
                                          static_cast<double>(window[index + 2]),
                                          static_cast<double>(window[index + 3]));
        if (written > 0) {
            write_line({line.data(), static_cast<std::size_t>(written)});
        }
    }
}

/** Logs the local player's own values, which the camera window is compared against. */
void log_local(const Report& report) noexcept {
    std::array<char, 224> line{};
    const LocalReport& local = report.local;
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=world_probe stage=dump part=local present=%u handle=%u "
                                      "pos=%.2f,%.2f,%.2f vel=%.2f,%.2f,%.2f fwd=%.3f,%.3f,%.3f "
                                      "basis=%u eye=%u eyepos=%.2f,%.2f,%.2f tracked=%u",
                                      local.present ? 1U : 0U,
                                      local.handleIndex,
                                      static_cast<double>(local.position[0]),
                                      static_cast<double>(local.position[1]),
                                      static_cast<double>(local.position[2]),
                                      static_cast<double>(local.velocity[0]),
                                      static_cast<double>(local.velocity[1]),
                                      static_cast<double>(local.velocity[2]),
                                      static_cast<double>(local.pose.forward[0]),
                                      static_cast<double>(local.pose.forward[1]),
                                      static_cast<double>(local.pose.forward[2]),
                                      local.pose.basisValid ? 1U : 0U,
                                      local.pose.positionValid ? 1U : 0U,
                                      static_cast<double>(local.pose.position[0]),
                                      static_cast<double>(local.pose.position[1]),
                                      static_cast<double>(local.pose.position[2]),
                                      report.trackedBodies);
    if (written > 0) {
        write_line({line.data(), static_cast<std::size_t>(written)});
    }
}

/** Logs the crosshair result, which is the body pick beside the game's own world trace. */
void log_crosshair(const Report& report) noexcept {
    std::array<char, 192> line{};
    int written = std::snprintf(
        line.data(),
        line.size(),
        "ev=world_probe stage=dump part=target present=%u index=%u handle=0x%08X "
        "object=0x%llX dist=%.2f pos=%.2f,%.2f,%.2f",
        report.target.present ? 1U : 0U,
        report.target.handleIndex,
        report.target.handle,
        static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(report.target.object)),
        static_cast<double>(report.target.distance),
        static_cast<double>(report.target.position[0]),
        static_cast<double>(report.target.position[1]),
        static_cast<double>(report.target.position[2]));
    if (written > 0) {
        write_line({line.data(), static_cast<std::size_t>(written)});
    }
    line = {};
    written = std::snprintf(line.data(),
                            line.size(),
                            "ev=world_probe stage=dump part=surface present=%u code=%d resolved=%u "
                            "frac=%.3f dist=%.2f pos=%.2f,%.2f,%.2f",
                            report.surface.present ? 1U : 0U,
                            report.surface.code,
                            report.surface.codeResolved ? 1U : 0U,
                            static_cast<double>(report.surface.fraction),
                            static_cast<double>(report.surface.distance),
                            static_cast<double>(report.surface.point[0]),
                            static_cast<double>(report.surface.point[1]),
                            static_cast<double>(report.surface.point[2]));
    if (written > 0) {
        write_line({line.data(), static_cast<std::size_t>(written)});
    }
}

/** Logs the bodies nearest the player. @param report Report the local position comes from. */
void log_bodies(const Report& report) noexcept {
    const std::uint64_t now = GetTickCount64();
    std::array<const Slot*, kDumpBodyCount> nearest{};
    std::array<float, kDumpBodyCount> distances{};
    std::size_t held = 0;
    for (const Slot& slot : g_slots) {
        if (slot.component == nullptr || now - slot.seenTick > kBodyLifetimeMs) {
            continue;
        }
        const float distance = length(subtract(slot.position, report.local.position));
        if (held == kDumpBodyCount && distance >= distances[kDumpBodyCount - 1]) {
            continue;
        }
        std::size_t place = std::min(held, kDumpBodyCount - 1);
        while (place > 0 && distances[place - 1] > distance) {
            --place;
        }
        for (std::size_t shift = std::min(held, kDumpBodyCount - 1); shift > place; --shift) {
            distances[shift] = distances[shift - 1];
            nearest[shift] = nearest[shift - 1];
        }
        distances[place] = distance;
        nearest[place] = &slot;
        if (held < kDumpBodyCount) {
            ++held;
        }
    }
    for (std::size_t index = 0; index < held; ++index) {
        const Slot& slot = *nearest[index];
        std::array<char, 192> line{};
        const int written = std::snprintf(
            line.data(),
            line.size(),
            "ev=world_probe stage=dump part=body rank=%zu "
            "handle=%u addr=0x%llX dist=%.2f pos=%.2f,%.2f,%.2f "
            "vel=%.2f,%.2f,%.2f",
            index,
            slot.handleIndex,
            static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(slot.component)),
            static_cast<double>(distances[index]),
            static_cast<double>(slot.position[0]),
            static_cast<double>(slot.position[1]),
            static_cast<double>(slot.position[2]),
            static_cast<double>(slot.velocity[0]),
            static_cast<double>(slot.velocity[1]),
            static_cast<double>(slot.velocity[2]));
        if (written > 0) {
            write_line({line.data(), static_cast<std::size_t>(written)});
        }
    }
}

/** Runs the dump the interface asked for, on the thread that owns the body table. */
void run_dump(const Report& report) noexcept {
    g_dumpSequence.fetch_add(1, std::memory_order_acq_rel);
    g_dumpHeld = 0;
    write_line("ev=world_probe stage=dump result=begin");
    log_local(report);
    log_camera_window();
    log_crosshair(report);
    log_bodies(report);
    write_line("ev=world_probe stage=dump result=end");
    g_dumpSequence.fetch_add(1, std::memory_order_release);
}

} // namespace

/** Asks the probe to read for the next interval. */
void request_scan() noexcept {
    g_scanDeadline.store(GetTickCount64() + kScanRequestLifetimeMs, std::memory_order_relaxed);
}

/** Records one physics component. */
void observe(void* component) noexcept {
    const std::uint64_t now = GetTickCount64();
    // A closed page costs this one atomic read per component, and nothing else.
    if (component == nullptr || !scanning(now)) {
        return;
    }
    Vector position{};
    if (!teleport::read_position(component, position)) {
        return;
    }
    Slot& slot = reserve(component, now);
    if (slot.component != component) {
        slot = {};
        slot.component = component;
        (void)teleport::object_handle(component, slot.handleIndex);
    }
    slot.position = position;
    slot.seenTick = now;
    Vector velocity{};
    slot.velocityValid = teleport::read_velocity(component, velocity);
    slot.velocity = slot.velocityValid ? velocity : Vector{};
}

/** Ages the table and publishes one report. */
void poll() noexcept {
    const std::uint64_t now = GetTickCount64();
    if (!scanning(now)) {
        // Nothing is reading, so the table is dropped rather than left to go stale in place.
        if (g_report.scanning || g_report.trackedBodies != 0) {
            reset();
        }
        return;
    }
    Report report{};
    report.scanning = true;
    report.local = read_local();
    pick(report, now);
    publish(report);
    // The dump runs here, after the pass it describes, so it reports the values the page shows.
    if (g_dumpRequested.exchange(false, std::memory_order_acq_rel)) {
        run_dump(report);
    }
}

/** Drops the table and the published report. */
void reset() noexcept {
    g_slots = {};
    publish(Report{});
}

/** @return The last published report. */
Report snapshot() noexcept {
    Report value{};
    for (;;) {
        const std::uint32_t before = g_sequence.load(std::memory_order_acquire);
        if ((before & 1U) != 0U) {
            continue;
        }
        value = g_report;
        if (g_sequence.load(std::memory_order_acquire) == before) {
            break;
        }
    }
    return value;
}

/** @return The pick limits in force. */
PickSettings settings() noexcept {
    PickSettings value{};
    value.radius = g_radius.load(std::memory_order_relaxed);
    value.range = g_range.load(std::memory_order_relaxed);
    value.probeDistance = g_probeDistance.load(std::memory_order_relaxed);
    return value;
}

/** Stores new pick limits. */
void publish_settings(const PickSettings& value) noexcept {
    const float range = std::clamp(value.range, kMinimumPickRange, kMaximumPickRange);
    g_radius.store(std::clamp(value.radius, kMinimumPickRadius, kMaximumPickRadius),
                   std::memory_order_relaxed);
    g_range.store(range, std::memory_order_relaxed);
    g_probeDistance.store(std::clamp(value.probeDistance, kMinimumPickRange, range),
                          std::memory_order_relaxed);
}

/** Copies bytes out of game memory for the interface's memory view. */
std::size_t read_object(std::uint32_t handle, std::span<std::byte> output) noexcept {
    const void* const record = spawn::object_record(handle);
    if (record == nullptr) {
        return 0;
    }
    const std::size_t wanted = (std::min)(output.size(), spawn::object_record_bytes());
    return read_raw(record, output.first(wanted));
}

std::size_t object_record_bytes() noexcept {
    return spawn::object_record_bytes();
}

std::size_t read_raw(const void* address, std::span<std::byte> output) noexcept {
    if (address == nullptr || output.empty()) {
        return 0;
    }
    const SIZE_T size = std::min<std::size_t>(output.size(), kRawReadCapacity);
    SIZE_T read = 0;
    if (ReadProcessMemory(GetCurrentProcess(), address, output.data(), size, &read) == FALSE) {
        return 0;
    }
    return static_cast<std::size_t>(read);
}

/** Asks for one diagnostic dump. */
void request_dump() noexcept {
    // The scan is renewed here too, so the dump still runs when the request lapsed this frame.
    request_scan();
    g_dumpRequested.store(true, std::memory_order_release);
}

/** @return True while a dump is asked for and has not run yet. */
bool dump_pending() noexcept {
    return g_dumpRequested.load(std::memory_order_acquire);
}

/** Copies the last dump out for display. */
std::size_t dump_lines(std::span<DumpLine> output) noexcept {
    if (output.empty()) {
        return 0;
    }
    std::size_t held = 0;
    for (;;) {
        const std::uint32_t before = g_dumpSequence.load(std::memory_order_acquire);
        if ((before & 1U) != 0U) {
            continue;
        }
        held = std::min(g_dumpHeld, output.size());
        for (std::size_t index = 0; index < held; ++index) {
            output[index] = g_dumpLines[index];
        }
        if (g_dumpSequence.load(std::memory_order_acquire) == before) {
            break;
        }
    }
    return held;
}

} // namespace sunrise::client::world
