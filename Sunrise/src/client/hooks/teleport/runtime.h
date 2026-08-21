#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace sunrise::client::hooks::teleport {

/** Three floats make one position or velocity vector. */
inline constexpr std::size_t kVectorLanes = 3;
/** The vertical lane. The camera basis is X forward, Z up, so the third lane is up. */
inline constexpr std::size_t kVerticalLane = 2;

/** One world-space vector as the game stores it. */
using Vector = std::array<float, kVectorLanes>;

/**
 * One camera pose, read from the pose block of the player the camera transform ran for.
 *
 * Only `forward` is confirmed. The three vectors that follow it are read as a hypothesis: a pose
 * block that stores a basis and then a translation. Each flag says what the read proved, so a
 * caller never presents an unproved field as a fact.
 */
struct CameraPose {
    /** Confirmed. Its default is (1,0,0), so the basis is X forward, Z up. */
    Vector forward{};
    /** Second basis vector. Valid only with `basisValid`. */
    Vector left{};
    /** Third basis vector. Valid only with `basisValid`. */
    Vector up{};
    /** Candidate eye position. Valid only with `positionValid`. */
    Vector position{};
    /** True when the camera transform has published a forward vector. */
    bool forwardValid{};
    /** True when all three vectors are unit length and mutually perpendicular. */
    bool basisValid{};
    /** True when the fourth vector is finite and inside the world bound. */
    bool positionValid{};
};

/** Writes the local player's controlled-object handle, or the invalid sentinel. */
using ControlledHandle = std::uint32_t* (*)(std::uint32_t*);
/** Returns the camera pose block array. The pointer in its global is obfuscated, so we call it. */
using CameraSingleton = std::byte* (*)();

/**
 * Publishes the two functions the hooks call.
 * @param controlled Writes the local player's object handle.
 * @param singleton Returns the camera pose block array.
 */
void publish_targets(ControlledHandle controlled, CameraSingleton singleton) noexcept;

/** Drops those functions and every latched request. */
void clear_targets() noexcept;

/**
 * Attaches the camera and physics hooks that carry the teleport.
 * @return True when all three targets were found and both detours attached.
 */
[[nodiscard]] bool install() noexcept;

/** Detaches both teleport hooks. */
void uninstall() noexcept;

/**
 * Publishes the camera forward vector for the physics tick that follows.
 * @param playerIndex Player the camera pose block belongs to.
 */
void capture_forward(std::uint32_t playerIndex) noexcept;

/** Latches one teleport request if the bound key went down this frame. */
void poll_request() noexcept;

/** Runs the move for a request no physics tick collected, and drives the sync for it. */
void force_pending() noexcept;

/**
 * Finds both key tables from the polled keyboard scan.
 * @return True when the scan and both of its table loads were found.
 */
[[nodiscard]] bool resolve_action_keys() noexcept;

/** Drops both key tables. */
void clear_action_keys() noexcept;

/**
 * Turns one authored binding into the virtual key the scan will read.
 * @param binding Input code taken from an authored binding half.
 * @return The virtual key, or 0 when there is none.
 */
[[nodiscard]] std::uint32_t action_key(std::uint16_t binding) noexcept;

/**
 * Calls the physics sync for one component through the installed trampoline.
 * @param component Physics component to sync.
 */
void invoke_sync(void* component) noexcept;

/**
 * Moves the local player if a request is pending and this component owns them.
 * @param component Physics component about to be synced.
 */
void apply_pending(void* component) noexcept;

/**
 * @param component Candidate physics component.
 * @return True when it drives the object the local player controls.
 *
 * Exposed because the physics sync is the only tick that sees every component, and a feature that
 * has to act on the player's own tick needs the same test this module already performs.
 */
[[nodiscard]] bool owns_local_player(void* component) noexcept;

/**
 * Reads the world position of the body a physics component drives.
 * @param component Physics component.
 * @param position Receives the three lanes.
 * @return True when the body was found and read.
 */
[[nodiscard]] bool read_position(void* component, Vector& position) noexcept;

/**
 * Reports the physics component the local player was last seen driving.
 * The sync stops for a player at rest, so a frame poll has no other way back to them.
 * @return That component, or null before the player has been seen. Prove it before use.
 */
[[nodiscard]] void* local_player_component() noexcept;

/**
 * Writes the world position of the body a physics component drives.
 * @param component Physics component.
 * @param position Three lanes to store.
 * @return True when the body was found and written.
 */
[[nodiscard]] bool write_position(void* component, const Vector& position) noexcept;

/**
 * Reads the linear velocity of the body a physics component drives.
 * @param component Physics component.
 * @param velocity Receives the three lanes.
 * @return True when the body was found and read.
 */
[[nodiscard]] bool read_velocity(void* component, Vector& velocity) noexcept;

/**
 * Writes the linear velocity of the body a physics component drives.
 * @param component Physics component.
 * @param velocity Three lanes to store.
 * @return True when the body was found and written.
 */
[[nodiscard]] bool write_velocity(void* component, const Vector& velocity) noexcept;

/**
 * The camera hook is the only site that sees the pose block, so it publishes the vector here.
 * @param forward Receives the camera forward vector published this frame.
 * @return True once the camera hook has published one.
 */
[[nodiscard]] bool camera_forward(Vector& forward) noexcept;

/**
 * Reports the whole camera pose published this frame, with one flag per proved part.
 * @param pose Receives the vectors and their flags. Cleared first.
 * @return True once the camera hook has published a forward vector.
 */
[[nodiscard]] bool camera_pose(CameraPose& pose) noexcept;

/**
 * Reads the object handle a physics component drives.
 * @param component Physics component.
 * @param handleIndex Receives the index bits, which name one object.
 * @return True when the handle was read.
 */
[[nodiscard]] bool object_handle(void* component, std::uint32_t& handleIndex) noexcept;

/**
 * Reports the object the local player controls.
 * @param handleIndex Receives the index bits of that object.
 * @return True while the player controls one and the getter is published.
 */
[[nodiscard]] bool controlled_object(std::uint32_t& handleIndex) noexcept;

/** @return The offset of the confirmed forward vector inside the camera pose block. */
[[nodiscard]] std::size_t camera_forward_offset() noexcept;

/**
 * Reads floats out of the camera pose block of the player the camera transform last ran for.
 * It exists for the diagnostic dump that confirms which offset holds which vector.
 * @param byteOffset Offset inside that block.
 * @param output Receives the floats. Nothing is written when the read fails.
 * @return True when the whole span was copied.
 */
[[nodiscard]] bool read_camera_block(std::size_t byteOffset, std::span<float> output) noexcept;

} // namespace sunrise::client::hooks::teleport
