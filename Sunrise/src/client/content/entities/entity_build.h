#pragma once

#include "../../../middleware/content/packages/reader/reader.h"

namespace sunrise::client::content::entities {

/**
 * Extracts every installed entity tag from the packages, once.
 * The rows are what the packages declare: the tag and the package family that installs it. Whether
 * a tag is loaded, and which object type it carries, belongs to the running game and is asked for
 * where it is used, so nothing here reads game memory.
 * @param source Package directory and borrowed block keys. Entry tables are plain file data, so
 * the sweep reads no block and needs no scratch.
 * @return True when State already holds the catalog or a full pass publishes it.
 */
[[nodiscard]] bool build(const middleware::content::packages::reader::Source& source) noexcept;

} // namespace sunrise::client::content::entities
