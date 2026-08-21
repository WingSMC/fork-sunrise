#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "../hooks/teleport/runtime.h"

namespace sunrise::client::world {

/** Bodies one probe pass keeps. The physics tick lists far fewer than this in one destination. */
inline constexpr std::size_t kBodyCapacity = 512;
/** Milliseconds a body stays in the table after the last tick that reported it. */
inline constexpr std::uint64_t kBodyLifetimeMs = 500;
/** Milliseconds one scan request keeps the probe reading. The interface renews it per frame. */
inline constexpr std::uint64_t kScanRequestLifetimeMs = 500;
/** Bytes one raw read returns. It bounds the cost of the interface's memory view. */
inline constexpr std::size_t kRawReadCapacity = 256;

/** Characters one dump line holds, including the null. */
inline constexpr std::size_t kDumpLineCapacity = 192;
/** Lines one dump holds. It covers the player, the camera window and the listed bodies. */
inline constexpr std::size_t kDumpLineCount = 24;

/** One held dump line, stored with no dynamic memory. */
using DumpLine = std::array<char, kDumpLineCapacity>;

/** Smallest and largest pick radius, in world units. */
inline constexpr float kMinimumPickRadius = 0.25F;
inline constexpr float kMaximumPickRadius = 20.0F;
/** Smallest and largest pick range, in world units. */
inline constexpr float kMinimumPickRange = 5.0F;
inline constexpr float kMaximumPickRange = 500.0F;

/** Default pick radius. A body this far off the ray still counts as being at the crosshair. */
inline constexpr float kDefaultPickRadius = 2.0F;
/** Default pick range. Bodies past this distance are not picked. */
inline constexpr float kDefaultPickRange = 120.0F;

using Vector = hooks::teleport::Vector;
using CameraPose = hooks::teleport::CameraPose;

/** What the probe knows about the local player, read in one pass. */
struct LocalReport {
    /** World position of the body the player's physics component drives. */
    Vector position{};
    /** Linear velocity of that body. */
    Vector velocity{};
    /** Length of that velocity, in world units per second. */
    float speed{};
    /** Camera pose published for this frame, with its own per-part flags. */
    CameraPose pose{};
    /** Heading of the camera forward vector, in degrees. */
    float yawDegrees{};
    /** Elevation of the camera forward vector, in degrees. */
    float pitchDegrees{};
    /** Index bits of the object the player controls. */
    std::uint32_t handleIndex{};
    /** Physics component driving that object, for the memory view. */
    const void* component{};
    /** True when a position was read this pass. */
    bool present{};
    /** True when a velocity was read this pass. */
    bool velocityValid{};
    /** True while the player controls an object. */
    bool controlled{};
};

/**
 * What the probe found along the camera ray.
 *
 * The game's own collision query is not reachable, so this is a pick over the bodies the physics
 * tick reports, not a world trace. Static geometry has no body here and is never picked.
 */
struct TargetReport {
    /** World position of the picked body. */
    Vector position{};
    /** Linear velocity of that body. */
    Vector velocity{};
    /** Point on the ray closest to that body. It is not a surface hit. */
    Vector rayPoint{};
    /** Length of the picked body's velocity. */
    float speed{};
    /** Distance from the ray origin to the picked body. */
    float distance{};
    /** Distance from the picked body to the ray. */
    float offset{};
    /** Angle between the ray and the picked body, in degrees. */
    float angleDegrees{};
    /** Index bits of the object the picked body drives. */
    std::uint32_t handleIndex{};
    /** Physics component of the picked body, for the memory view. */
    const void* component{};
    /** True when one body was picked this pass. */
    bool present{};
};

/** The whole probe result, published as one value so its parts cannot disagree. */
struct Report {
    LocalReport local{};
    TargetReport target{};
    /** Point the camera ray reaches at the configured probe distance. It is not a surface hit. */
    Vector rayPoint{};
    /** Origin the ray was cast from. */
    Vector rayOrigin{};
    /** Bodies held in the table when the pass ran. */
    std::uint32_t trackedBodies{};
    /** Bodies the last pass considered, which is the table minus the stale and the player. */
    std::uint32_t consideredBodies{};
    /** True when the probe was reading when the pass ran. */
    bool scanning{};
    /** True when the ray had an origin and a direction. */
    bool rayValid{};
    /** True when the ray started at the camera. False means it started at the player body. */
    bool rayFromCamera{};
};

/** Pick limits, held in memory only. They tune one debug view and are not part of the settings. */
struct PickSettings {
    /** Bodies further than this from the ray are not picked. */
    float radius{kDefaultPickRadius};
    /** Bodies further than this from the origin are not picked. */
    float range{kDefaultPickRange};
    /** Distance along the ray the reported ray point is taken at. */
    float probeDistance{kDefaultPickRange};
};

/**
 * Asks the probe to read for the next interval. The interface calls it per frame while its page
 * is open, so a closed page costs one atomic read on the physics tick and nothing else.
 */
void request_scan() noexcept;

/**
 * Records one physics component. Call it from the physics sync, which is the only tick that sees
 * every component.
 * @param component Physics component being synced.
 */
void observe(void* component) noexcept;

/** Ages the table and publishes one report. Call it per frame, on a game thread. */
void poll() noexcept;

/** Drops the table and the published report. */
void reset() noexcept;

/** @return The last published report, which any thread may read. */
[[nodiscard]] Report snapshot() noexcept;

/** @return The pick limits in force. */
[[nodiscard]] PickSettings settings() noexcept;

/**
 * Stores new pick limits.
 * @param value Limits, clamped to the supported range.
 */
void publish_settings(const PickSettings& value) noexcept;

/**
 * Copies bytes out of game memory for the interface's memory view.
 * @param address Address to read. Nothing is read when it is null.
 * @param output Receives the bytes. At most `kRawReadCapacity` are copied.
 * @return Bytes copied, which is zero when the read fails.
 */
[[nodiscard]] std::size_t read_raw(const void* address, std::span<std::byte> output) noexcept;

/**
 * Asks for one diagnostic dump. The dump runs on the next frame poll, which is on the game thread
 * that owns the body table, so it never reads a table another thread is writing.
 *
 * The dump holds the camera pose block window, the player values beside it, and the nearest
 * tracked bodies. The camera window is what confirms which offset in the block holds the eye
 * position, which cannot be found in the executable because its code sections are packed.
 *
 * Every line goes to the client log channel and to the dump buffer. The buffer exists because the
 * shipped log level for that channel is `warn`, so an ordinary run drops the log copy.
 */
void request_dump() noexcept;

/** @return True while a dump is asked for and has not run yet. */
[[nodiscard]] bool dump_pending() noexcept;

/**
 * Copies the last dump out for display.
 * @param output Receives the lines, oldest first. Only the held lines are written.
 * @return Lines written.
 */
[[nodiscard]] std::size_t dump_lines(std::span<DumpLine> output) noexcept;

} // namespace sunrise::client::world
