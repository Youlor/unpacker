/*
 * Copyright (C) 2015 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "immune_spaces.h"

#include <vector>
#include <tuple>

#include "gc/space/space-inl.h"
#include "mirror/object.h"
#include "oat_file.h"

namespace art {
namespace gc {
namespace collector {

void ImmuneSpaces::Reset() {
  spaces_.clear();
  largest_immune_region_.Reset();
}

void ImmuneSpaces::CreateLargestImmuneRegion() {
  uintptr_t best_begin = 0u;
  uintptr_t best_end = 0u;
  uintptr_t best_heap_size = 0u;
  uintptr_t cur_begin = 0u;
  uintptr_t cur_end = 0u;
  uintptr_t cur_heap_size = 0u;
  using Interval = std::tuple</*start*/uintptr_t, /*end*/uintptr_t, /*is_heap*/bool>;
  std::vector<Interval> intervals;
  for (space::ContinuousSpace* space : GetSpaces()) {
    uintptr_t space_begin = reinterpret_cast<uintptr_t>(space->Begin());
    uintptr_t space_end = reinterpret_cast<uintptr_t>(space->Limit());
    if (space->IsImageSpace()) {
      // For the boot image, the boot oat file is always directly after. For app images it may not
      // be if the app image was mapped at a random address.
      space::ImageSpace* image_space = space->AsImageSpace();
      // Update the end to include the other non-heap sections.
      space_end = RoundUp(reinterpret_cast<uintptr_t>(image_space->GetImageEnd()), kPageSize);
      // For the app image case, GetOatFileBegin is where the oat file was mapped during image
      // creation, the actual oat file could be somewhere else.
      const OatFile* const image_oat_file = image_space->GetOatFile();
      if (image_oat_file != nullptr) {
        intervals.push_back(Interval(reinterpret_cast<uintptr_t>(image_oat_file->Begin()),
                                     reinterpret_cast<uintptr_t>(image_oat_file->End()),
                                     /*image*/false));
      }
    }
    intervals.push_back(Interval(space_begin, space_end, /*is_heap*/true));
  }
  std::sort(intervals.begin(), intervals.end());
  // Intervals are already sorted by begin, if a new interval begins at the end of the current
  // region then we append, otherwise we restart the current interval. To prevent starting an
  // interval on an oat file, ignore oat files that are not extending an existing interval.
  // If the total number of image bytes in the current interval is larger than the current best
  // one, then we set the best one to be the current one.
  for (const Interval& interval : intervals) {
    const uintptr_t begin = std::get<0>(interval);
    const uintptr_t end = std::get<1>(interval);
    const bool is_heap = std::get<2>(interval);
    VLOG(collector) << "Interval " << reinterpret_cast<const void*>(begin) << "-"
                    << reinterpret_cast<const void*>(end) << " is_heap=" << is_heap;
    DCHECK_GE(end, begin);
    DCHECK_GE(begin, cur_end);
    // New interval is not at the end of the current one, start a new interval if we are a heap
    // interval. Otherwise continue since we never start a new region with non image intervals.
    if (begin != cur_end) {
      if (!is_heap) {
        continue;
      }
      // Not extending, reset the region.
      cur_begin = begin;
      cur_heap_size = 0;
    }
    cur_end = end;
    if (is_heap) {
      // Only update if the total number of image bytes is greater than the current best one.
      // We don't want to count the oat file bytes since these contain no java objects.
      cur_heap_size += end - begin;
      if (cur_heap_size > best_heap_size) {
        best_begin = cur_begin;
        best_end = cur_end;
        best_heap_size = cur_heap_size;
      }
    }
  }
  largest_immune_region_.SetBegin(reinterpret_cast<mirror::Object*>(best_begin));
  largest_immune_region_.SetEnd(reinterpret_cast<mirror::Object*>(best_end));
  VLOG(collector) << "Immune region " << largest_immune_region_.Begin() << "-"
                  << largest_immune_region_.End();
}

void ImmuneSpaces::AddSpace(space::ContinuousSpace* space) {
  DCHECK(spaces_.find(space) == spaces_.end()) << *space;
  // Bind live to mark bitmap if necessary.
  if (space->GetLiveBitmap() != space->GetMarkBitmap()) {
    CHECK(space->IsContinuousMemMapAllocSpace());
    space->AsContinuousMemMapAllocSpace()->BindLiveToMarkBitmap();
  }
  spaces_.insert(space);
  CreateLargestImmuneRegion();
}

bool ImmuneSpaces::CompareByBegin::operator()(space::ContinuousSpace* a, space::ContinuousSpace* b)
    const {
  return a->Begin() < b->Begin();
}

bool ImmuneSpaces::ContainsSpace(space::ContinuousSpace* space) const {
  return spaces_.find(space) != spaces_.end();
}

}  // namespace collector
}  // namespace gc
}  // namespace art
