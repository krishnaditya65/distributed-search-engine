// segment_merger.h — background LSM-style compaction: merges several
// immutable segments into one, and this is the ONLY point at which deleted
// documents' postings/stored data are physically reclaimed (segments
// themselves are never edited in place; see segment.h).
#pragma once

#include <string>
#include <vector>

#include "bitmap/roaring_bitmap.h"

namespace dse::segment {

class SegmentMerger {
 public:
  // Reads every segment in `input_paths`, drops any document whose internal
  // id is present in `deleted`, and writes a single combined segment to
  // `output_path`. Per-field average-length statistics are recomputed from
  // the surviving documents only. Throws std::runtime_error if any input
  // segment fails to open, or if `input_paths` is empty.
  static void Merge(const std::vector<std::string>& input_paths, const std::string& output_path,
                     const dse::bitmap::RoaringBitmap& deleted);
};

}  // namespace dse::segment
