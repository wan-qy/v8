// Copyright 2016 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_SLOT_SET_H
#define V8_SLOT_SET_H

#include "src/allocation.h"
#include "src/base/bits.h"

namespace v8 {
namespace internal {

// Data structure for maintaining a set of slots in a standard (non-large)
// page. The base address of the page must be set with SetPageStart before any
// operation.
// The data structure assumes that the slots are pointer size aligned and
// splits the valid slot offset range into kBuckets buckets.
// Each bucket is a bitmap with a bit corresponding to a single slot offset.
class SlotSet : public Malloced {
 public:
  enum CallbackResult { KEEP_SLOT, REMOVE_SLOT };

  SlotSet() {
    for (int i = 0; i < kBuckets; i++) {
      bucket[i] = nullptr;
    }
  }

  ~SlotSet() {
    for (int i = 0; i < kBuckets; i++) {
      ReleaseBucket(i);
    }
  }

  void SetPageStart(Address page_start) { page_start_ = page_start; }

  // The slot offset specifies a slot at address page_start_ + slot_offset.
  void Insert(int slot_offset) {
    int bucket_index, cell_index, bit_index;
    SlotToIndices(slot_offset, &bucket_index, &cell_index, &bit_index);
    if (bucket[bucket_index] == nullptr) {
      bucket[bucket_index] = AllocateBucket();
    }
    bucket[bucket_index][cell_index] |= 1u << bit_index;
  }

  // The slot offset specifies a slot at address page_start_ + slot_offset.
  void Remove(int slot_offset) {
    int bucket_index, cell_index, bit_index;
    SlotToIndices(slot_offset, &bucket_index, &cell_index, &bit_index);
    if (bucket[bucket_index] != nullptr) {
      uint32_t cell = bucket[bucket_index][cell_index];
      if (cell) {
        uint32_t bit_mask = 1u << bit_index;
        if (cell & bit_mask) {
          bucket[bucket_index][cell_index] ^= bit_mask;
        }
      }
    }
  }

  // The slot offsets specify a range of slots at addresses:
  // [page_start_ + start_offset ... page_start_ + end_offset).
  void RemoveRange(int start_offset, int end_offset) {
    DCHECK_LE(start_offset, end_offset);
    int start_bucket, start_cell, start_bit;
    SlotToIndices(start_offset, &start_bucket, &start_cell, &start_bit);
    int end_bucket, end_cell, end_bit;
    SlotToIndices(end_offset, &end_bucket, &end_cell, &end_bit);
    uint32_t start_mask = (1u << start_bit) - 1;
    uint32_t end_mask = ~((1u << end_bit) - 1);
    if (start_bucket == end_bucket && start_cell == end_cell) {
      MaskCell(start_bucket, start_cell, start_mask | end_mask);
      return;
    }
    MaskCell(start_bucket, start_cell, start_mask);
    start_cell++;
    if (bucket[start_bucket] != nullptr && start_bucket < end_bucket) {
      while (start_cell < kCellsPerBucket) {
        bucket[start_bucket][start_cell] = 0;
        start_cell++;
      }
    }
    while (start_bucket < end_bucket) {
      delete[] bucket[start_bucket];
      bucket[start_bucket] = nullptr;
      start_bucket++;
    }
    if (start_bucket < kBuckets && bucket[start_bucket] != nullptr) {
      while (start_cell < end_cell) {
        bucket[start_bucket][start_cell] = 0;
        start_cell++;
      }
    }
    if (end_bucket < kBuckets) {
      MaskCell(end_bucket, end_cell, end_mask);
    }
  }

  // The slot offset specifies a slot at address page_start_ + slot_offset.
  bool Lookup(int slot_offset) {
    int bucket_index, cell_index, bit_index;
    SlotToIndices(slot_offset, &bucket_index, &cell_index, &bit_index);
    if (bucket[bucket_index] != nullptr) {
      uint32_t cell = bucket[bucket_index][cell_index];
      return (cell & (1u << bit_index)) != 0;
    }
    return false;
  }

  // Iterate over all slots in the set and for each slot invoke the callback.
  // If the callback returns REMOVE_SLOT then the slot is removed from the set.
  //
  // Sample usage:
  // Iterate([](Address slot_address) {
  //    if (good(slot_address)) return KEEP_SLOT;
  //    else return REMOVE_SLOT;
  // });
  template <typename Callback>
  void Iterate(Callback callback) {
    for (int bucket_index = 0; bucket_index < kBuckets; bucket_index++) {
      if (bucket[bucket_index] != nullptr) {
        bool bucket_is_empty = true;
        uint32_t* current_bucket = bucket[bucket_index];
        int cell_offset = bucket_index * kBitsPerBucket;
        for (int i = 0; i < kCellsPerBucket; i++, cell_offset += kBitsPerCell) {
          if (current_bucket[i]) {
            uint32_t cell = current_bucket[i];
            uint32_t old_cell = cell;
            uint32_t new_cell = cell;
            while (cell) {
              int bit_offset = base::bits::CountTrailingZeros32(cell);
              uint32_t bit_mask = 1u << bit_offset;
              uint32_t slot = (cell_offset + bit_offset) << kPointerSizeLog2;
              if (callback(page_start_ + slot) == KEEP_SLOT) {
                bucket_is_empty = false;
              } else {
                new_cell ^= bit_mask;
              }
              cell ^= bit_mask;
            }
            if (old_cell != new_cell) {
              current_bucket[i] = new_cell;
            }
          }
        }
        if (bucket_is_empty) {
          ReleaseBucket(bucket_index);
        }
      }
    }
  }

 private:
  static const int kMaxSlots = (1 << kPageSizeBits) / kPointerSize;
  static const int kCellsPerBucket = 32;
  static const int kCellsPerBucketLog2 = 5;
  static const int kBitsPerCell = 32;
  static const int kBitsPerCellLog2 = 5;
  static const int kBitsPerBucket = kCellsPerBucket * kBitsPerCell;
  static const int kBitsPerBucketLog2 = kCellsPerBucketLog2 + kBitsPerCellLog2;
  static const int kBuckets = kMaxSlots / kCellsPerBucket / kBitsPerCell;

  uint32_t* AllocateBucket() {
    uint32_t* result = NewArray<uint32_t>(kCellsPerBucket);
    for (int i = 0; i < kCellsPerBucket; i++) {
      result[i] = 0;
    }
    return result;
  }

  void ReleaseBucket(int bucket_index) {
    DeleteArray<uint32_t>(bucket[bucket_index]);
    bucket[bucket_index] = nullptr;
  }

  void MaskCell(int bucket_index, int cell_index, uint32_t mask) {
    uint32_t* cells = bucket[bucket_index];
    if (cells != nullptr && cells[cell_index] != 0) {
      cells[cell_index] &= mask;
    }
  }

  // Converts the slot offset into bucket/cell/bit index.
  void SlotToIndices(int slot_offset, int* bucket_index, int* cell_index,
                     int* bit_index) {
    DCHECK_EQ(slot_offset % kPointerSize, 0);
    int slot = slot_offset >> kPointerSizeLog2;
    DCHECK(slot >= 0 && slot <= kMaxSlots);
    *bucket_index = slot >> kBitsPerBucketLog2;
    *cell_index = (slot >> kBitsPerCellLog2) & (kCellsPerBucket - 1);
    *bit_index = slot & (kBitsPerCell - 1);
  }

  uint32_t* bucket[kBuckets];
  Address page_start_;
};

}  // namespace internal
}  // namespace v8

#endif  // V8_SLOT_SET_H
