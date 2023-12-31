//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
#pragma once

#include <algorithm>
#include <deque>
#include <string>
#include <vector>

#include "db/compaction.h"
#include "db/compaction_iteration_stats.h"
#include "db/merge_helper.h"
#include "db/range_del_aggregator.h"
#include "db/snapshot_checker.h"
#include "options/cf_options.h"
#include "rocksdb/compaction_filter.h"
#include "rocksdb/terark_namespace.h"
#include "table/iterator_wrapper.h"
#include "util/chash_set.h"

namespace TERARKDB_NAMESPACE {

class CompactionIterator {
 public:
  friend class CompactionIteratorToInternalIterator;

  // A wrapper around Compaction. Has a much smaller interface, only what
  // CompactionIterator uses. Tests can override it.
  class CompactionProxy {
   public:
    explicit CompactionProxy(const Compaction* compaction)
        : compaction_(compaction) {}

    virtual ~CompactionProxy() = default;
    virtual SeparationType separation_type() const {
      return compaction_->separation_type();
    }
    virtual int level(size_t /*compaction_input_level*/ = 0) const {
      return compaction_->level();
    }
    virtual bool KeyNotExistsBeyondOutputLevel(
        const Slice& user_key, std::vector<size_t>* level_ptrs) const {
      return compaction_->KeyNotExistsBeyondOutputLevel(user_key, level_ptrs);
    }
    virtual bool bottommost_level() const {
      return compaction_->bottommost_level();
    }
    virtual int number_levels() const { return compaction_->number_levels(); }
    virtual Slice GetLargestUserKey() const {
      return compaction_->GetLargestUserKey();
    }
    virtual bool allow_ingest_behind() const {
      return compaction_->immutable_cf_options()->allow_ingest_behind;
    }
    virtual bool preserve_deletes() const {
      return compaction_->immutable_cf_options()->preserve_deletes;
    }

   protected:
    CompactionProxy() : compaction_(nullptr) {}

   private:
    const Compaction* compaction_;
  };

  CompactionIterator(InternalIterator* input, SeparateHelper* separate_helper,
                     const Slice* end, const Comparator* cmp,
                     MergeHelper* merge_helper, SequenceNumber last_sequence,
                     std::vector<SequenceNumber>* snapshots,
                     SequenceNumber earliest_write_conflict_snapshot,
                     const SnapshotChecker* snapshot_checker, Env* env,
                     bool report_detailed_time, bool expect_valid_internal_key,
                     CompactionRangeDelAggregator* range_del_agg,
                     const Compaction* compaction = nullptr,
                     BlobConfig blob_config = BlobConfig{size_t(-1), 0.0},
                     const CompactionFilter* compaction_filter = nullptr,
                     const std::atomic<bool>* shutting_down = nullptr,
                     const SequenceNumber preserve_deletes_seqnum = 0,
                     const chash_set<uint64_t>* b = nullptr);

  // Constructor with custom CompactionProxy, used for tests.
  CompactionIterator(InternalIterator* input, SeparateHelper* separate_helper,
                     const Slice* end, const Comparator* cmp,
                     MergeHelper* merge_helper, SequenceNumber last_sequence,
                     std::vector<SequenceNumber>* snapshots,
                     SequenceNumber earliest_write_conflict_snapshot,
                     const SnapshotChecker* snapshot_checker, Env* env,
                     bool report_detailed_time, bool expect_valid_internal_key,
                     CompactionRangeDelAggregator* range_del_agg,
                     std::unique_ptr<CompactionProxy> compaction,
                     BlobConfig blob_config,
                     const CompactionFilter* compaction_filter = nullptr,
                     const std::atomic<bool>* shutting_down = nullptr,
                     const SequenceNumber preserve_deletes_seqnum = 0,
                     const chash_set<uint64_t>* b = nullptr);

  ~CompactionIterator();

  void ResetRecordCounts();

  // Seek to the beginning of the compaction iterator output.
  //
  // REQUIRED: Call only once.
  void SeekToFirst();

  // Produces the next record in the compaction.
  //
  // REQUIRED: SeekToFirst() has been called.
  void Next();

  void SetTrackObsoleteRecordsFlag(bool flag) {
    track_obsolete_records_flag_ = flag;
  }

  // Getters
  const Slice& key() const { return key_; }
  const LazyBuffer& value() const { return value_; }
  const Status& status() const { return status_; }
  const ParsedInternalKey& ikey() const { return ikey_; }
  bool Valid() const { return valid_; }
  const Slice& user_key() const { return current_user_key_; }
  const CompactionIterationStats& iter_stats() const { return iter_stats_; }
  void SetFilterSampleInterval(size_t filter_sample_interval);
  bool IfTrackObsoleteRecords() { return track_obsolete_records_flag_; }

 private:
  // Processes the input stream to find the next output
  void NextFromInput();

  // Do last preparations before presenting the output to the callee. At this
  // point this only zeroes out the sequence number if possible for better
  // compression.
  void PrepareOutput();

  // Invoke compaction filter if needed.
  void InvokeFilterIfNeeded(bool* need_skip, Slice* skip_until);

  // Given a sequence number, return the sequence number of the
  // earliest snapshot that this sequence number is visible in.
  // The snapshots themselves are arranged in ascending order of
  // sequence numbers.
  // Employ a sequential search because the total number of
  // snapshots are typically small.
  inline SequenceNumber findEarliestVisibleSnapshot(
      SequenceNumber in, SequenceNumber* prev_snapshot);

  // Checks whether the currently seen ikey_ is needed for
  // incremental (differential) snapshot and hence can't be dropped
  // or seqnum be zero-ed out even if all other conditions for it are met.
  inline bool ikeyNotNeededForIncrementalSnapshot();

  CombinedInternalIterator input_;
  const Slice* end_;
  const Comparator* cmp_;
  MergeHelper* merge_helper_;
  const std::vector<SequenceNumber>* snapshots_;
  const SequenceNumber earliest_write_conflict_snapshot_;
  const SnapshotChecker* const snapshot_checker_;
  Env* env_;
  // bool report_detailed_time_;
  bool expect_valid_internal_key_;
  CompactionRangeDelAggregator* range_del_agg_;
  std::unique_ptr<CompactionProxy> compaction_;
  const BlobConfig blob_config_;
  const uint64_t blob_large_key_ratio_lsh16_;
  const CompactionFilter* compaction_filter_;
  const std::atomic<bool>* shutting_down_;
  const SequenceNumber preserve_deletes_seqnum_;
  bool bottommost_level_;
  bool valid_ = false;
  bool visible_at_tip_;
  SequenceNumber earliest_snapshot_;
  SequenceNumber latest_snapshot_;
  bool ignore_snapshots_;

  // State
  //
  // Points to a copy of the current compaction iterator output (current_key_)
  // if valid_.
  Slice key_;
  // Points to the value in the underlying iterator that corresponds to the
  // current output.
  LazyBuffer value_;
  std::string value_meta_;
  // The status is OK unless compaction iterator encounters a merge operand
  // while not having a merge operator defined.
  Status status_;
  // Stores the user key, sequence number and type of the current compaction
  // iterator output (or current key in the underlying iterator during
  // NextFromInput()).
  ParsedInternalKey ikey_;
  // Stores whether ikey_.user_key is valid. If set to false, the user key is
  // not compared against the current key in the underlying iterator.
  bool has_current_user_key_ = false;
  bool at_next_ = false;  // If false, the iterator
  // Holds a copy of the current compaction iterator output (or current key in
  // the underlying iterator during NextFromInput()).
  IterKey current_key_;
  Slice current_user_key_;
  SequenceNumber current_user_key_sequence_;
  SequenceNumber current_user_key_snapshot_;

  // True if the iterator has already returned a record for the current key.
  bool has_outputted_key_ = false;

  // truncated the value of the next key and output it without applying any
  // compaction rules.  This is used for outputting a put after a single delete.
  bool clear_and_output_next_key_ = false;

  MergeOutputIterator merge_out_iter_;
  LazyBuffer compaction_filter_value_;
  InternalKey compaction_filter_skip_until_;
  // "level_ptrs" holds indices that remember which file of an associated
  // level we were last checking during the last call to compaction->
  // KeyNotExistsBeyondOutputLevel(). This allows future calls to the function
  // to pick off where it left off since each subcompaction's key range is
  // increasing so a later call to the function must be looking for a key that
  // is in or beyond the last file checked during the previous call
  std::vector<size_t> level_ptrs_;
  CompactionIterationStats iter_stats_;

  // Used to avoid purging uncommitted values. The application can specify
  // uncommitted values by providing a SnapshotChecker object.
  bool current_key_committed_;

  bool do_separate_value_;  // separate big value
  bool do_rebuild_blob_;    // rebuild all blobs in need_rebuild_blobs if user
                            // force rebuild need_rebuild_blobs.empty() == true
  bool do_combine_value_;   // fetch and combine bigvalue from blobs

  size_t filter_sample_interval_ = 64;
  size_t filter_hit_count_ = 0;
  const chash_set<uint64_t>* rebuild_blob_set_;

  // (kqh):
  // Track the occurrence of each key in this process or not.
  // The tracked information will be used to generate hotness set and used
  // as a guidance of our zone gc.
  // In current design, key sst compaction generates no such extra information
  // but we shall see if adding this feature improves the efficiency of hotness
  // detection
  bool track_key_occurrence_ = false;

  // Track the number of deprecated records during this compaction or not
  bool track_obsolete_records_flag_ = false;

  // This is the file number of the value sst contains the latest value.
  // It is only updated when it encounters a new input key. 
  // For example, consider the following input sequence:
  // 
  //  <<key1, 100>, 20.sst>, <<key1, 20>, 10.sst>, <<key1, 5>, 7.sst>
  //  <<key2, 120>, 17.sst>, <<key2, 40>, 9.sst>, <<key2, 7>, 8.sst>
  // 
  // Then the latest_valid_fileno_ will be updated to be 20.sst when it 
  // first encounters the record <key1, 100>. And remain unchanged while
  // reading the next 2 records. It changes to be 17.sst when it reads 
  // <key2, 120>. 
  // 
  // Since the caller of CompactionIterator may discard the value_ field
  // during their invocation, we can't read the latest file no via value_. 
  // Thus an extra internal state are needed
  uint64_t latest_valid_fileno_ = -1;

 public:
  bool IsShuttingDown() {
    // This is a best-effort facility, so memory_order_relaxed is sufficient.
    return shutting_down_ && shutting_down_->load(std::memory_order_relaxed);
  }
};

InternalIterator* NewCompactionIterator(
    CompactionIterator* (*new_compaction_iter_callback)(void*), void* arg,
    const Slice* start_user_key = nullptr);

}  // namespace TERARKDB_NAMESPACE
