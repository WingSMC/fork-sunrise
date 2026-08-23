#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "definition.h"

namespace sunrise::state::build_data::entities {

/** Clears every extracted family and entity row. */
void clear() noexcept;

/**
 * Checks one complete entity catalog in canonical order.
 * Both arrays are checked together. An entity names its family by row index, so a family bank short
 * by one row silently renames every entity above it.
 * @param families Candidate family rows in ascending name order.
 * @param installed Candidate entity rows in ascending tag order.
 * @return True when every name, index, count, and ordering rule holds.
 */
[[nodiscard]] bool valid(std::span<const Family> families,
                         std::span<const Entity> installed) noexcept;

/**
 * Replaces the complete entity catalog in one step.
 * @param families Complete family rows in ascending name order.
 * @param installed Complete entity rows in ascending tag order.
 * @return True when the catalog passes validation and fits fixed State storage.
 */
[[nodiscard]] bool replace(std::span<const Family> families,
                           std::span<const Entity> installed) noexcept;

/**
 * Finds one installed entity by its tag.
 * @param tag Entity tag.
 * @param installed Receives the matching row.
 * @return True when the catalog carries that exact tag.
 */
[[nodiscard]] bool find(std::uint32_t tag, Entity& installed) noexcept;

/**
 * Copies every family row in ascending name order.
 * @param output Caller-owned fixed row storage.
 * @param count Receives the copied row count, or zero when output is too small.
 * @return True when output can hold every row.
 */
[[nodiscard]] bool snapshot_families(std::span<Family> output, std::size_t& count) noexcept;

/**
 * Copies every entity row in ascending tag order.
 * @param output Caller-owned fixed row storage.
 * @param count Receives the copied row count, or zero when output is too small.
 * @return True when output can hold every row.
 */
[[nodiscard]] bool snapshot(std::span<Entity> output, std::size_t& count) noexcept;

/** @return The entity row count, read under the lock. */
[[nodiscard]] std::size_t count() noexcept;

/** @return The family row count, read under the lock. */
[[nodiscard]] std::size_t family_count() noexcept;

} // namespace sunrise::state::build_data::entities
