/******************************************************************************
 *                    _   _____   __________                                  *
 *                   | | / / _ | / __/_  __/     Visibility                   *
 *                   | |/ / __ |_\ \  / /          Across                     *
 *                   |___/_/ |_/___/ /_/       Space and Time                 *
 *                                                                            *
 * This file is part of VAST. It is subject to the license terms in the       *
 * LICENSE file found in the top-level directory of this distribution and at  *
 * http://vast.io/license. No part of VAST, including this file, may be       *
 * copied, modified, propagated, or distributed except according to the terms *
 * contained in the LICENSE file.                                             *
 ******************************************************************************/

#pragma once

#include "vast/fwd.hpp"

#include "vast/detail/cache.hpp"
#include "vast/detail/range_map.hpp"
#include "vast/segment.hpp"
#include "vast/segment_builder.hpp"
#include "vast/store.hpp"
#include "vast/uuid.hpp"

#include <filesystem>

namespace vast {

/// @relates segment_store
using segment_store_ptr = std::unique_ptr<segment_store>;

/// A store that keeps its data in terms of segments.
class segment_store : public store {
public:
  // -- constructors, destructors, and assignment operators --------------------

  /// Constructs a segment store.
  /// @param dir The directory where to store state.
  /// @param max_segment_size The maximum segment size in bytes.
  /// @param in_memory_segments The number of semgents to cache in memory.
  /// @pre `max_segment_size > 0`
  static segment_store_ptr
  make(std::filesystem::path dir, size_t max_segment_size,
       size_t in_memory_segments);

  ~segment_store();

  // -- properties -------------------------------------------------------------

  /// @returns the path for storing the segments.
  std::filesystem::path segment_path() const {
    return dir_ / "segments";
  }

  /// @returns whether the store has no unwritten data pending.
  bool dirty() const noexcept {
    return builder_.table_slice_bytes() != 0;
  }

  /// @returns the ID of the active segment.
  const uuid& active_id() const noexcept {
    return builder_.id();
  }

  /// @returns whether `x` is currently a cached segment.
  bool cached(const uuid& x) const noexcept {
    return cache_.count(x) != 0;
  }

  // -- cache management -------------------------------------------------------

  /// Evicts all segments from the cache.
  void clear_cache() {
    cache_.clear();
  }

  // -- implementation of store ------------------------------------------------

  caf::error put(table_slice xs) override;

  std::unique_ptr<store::lookup> extract(const ids& xs) const override;

  caf::error erase(const ids& xs) override;

  caf::expected<std::vector<table_slice>> get(const ids& xs) override;

  caf::error flush() override;

  void inspect_status(caf::settings& xs, system::status_verbosity v) override;

private:
  segment_store(std::filesystem::path dir, uint64_t max_segment_size,
                size_t in_memory_segments);

  // -- utility functions ------------------------------------------------------

  caf::error register_segments();

  caf::error register_segment(const std::filesystem::path& filename);

  caf::expected<segment> load_segment(uuid id) const;

  /// Fills `candidates` with all segments that qualify for `selection`.
  caf::error select_segments(const ids& selection,
                             std::vector<uuid>& candidates) const;

  /// Drops an entire segment and erases its content from disk.
  /// @param x The segment to drop.
  /// @returns The number of events in `x`.
  uint64_t drop(segment& x);

  /// Drops a segment-under-construction by resetting the builder and forcing
  /// it to generate a new segment ID.
  /// @param x The segment-under-construction to drop.
  /// @returns The number of events in `x`.
  uint64_t drop(segment_builder& x);

  // -- member variables -------------------------------------------------------

  /// Identifies the base directory for segments.
  std::filesystem::path dir_;

  /// Configures the limit each segment until we seal and flush it.
  uint64_t max_segment_size_;

  uint64_t num_events_ = 0;

  /// Maps event IDs to candidate segments.
  detail::range_map<id, uuid> segments_;

  /// Optimizes access times into segments by keeping some segments in memory.
  mutable detail::cache<uuid, segment> cache_;

  /// Serializes table slices into contiguous chunks of memory.
  segment_builder builder_;
};

} // namespace vast
