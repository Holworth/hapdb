//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.
#pragma once

#include <atomic>

#include "table/internal_iterator.h"
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-parameter"
#endif
#ifdef __clang__
#pragma clang diagnostic pop
#endif
#include <deque>
#include <functional>
#include <limits>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "db/column_family.h"
#include "db/compaction_iterator.h"
#include "db/dbformat.h"
#include "db/flush_scheduler.h"
#include "db/internal_stats.h"
#include "db/job_context.h"
#include "db/log_writer.h"
#include "db/memtable_list.h"
#include "db/range_del_aggregator.h"
#include "db/version_edit.h"
#include "db/write_controller.h"
#include "db/write_thread.h"
#include "options/cf_options.h"
#include "options/db_options.h"
#include "port/port.h"
#include "rocksdb/compaction_filter.h"
#include "rocksdb/compaction_job_stats.h"
#include "rocksdb/db.h"
#include "rocksdb/env.h"
#include "rocksdb/memtablerep.h"
#include "rocksdb/terark_namespace.h"
#include "rocksdb/transaction_log.h"
#include "table/scoped_arena_iterator.h"
#include "util/autovector.h"
#include "util/event_logger.h"
#include "util/stop_watch.h"
#include "util/thread_local.h"

namespace TERARKDB_NAMESPACE {

class Arena;
class ErrorHandler;
class MemTable;
class SnapshotChecker;
class TableCache;
class Version;
class VersionEdit;
class VersionSet;

class CompactionJob {
 public:
  CompactionJob(int job_id, Compaction* compaction,
                const ImmutableDBOptions& db_options,
                const EnvOptions env_options, VersionSet* versions,
                const std::atomic<bool>* shutting_down,
                const SequenceNumber preserve_deletes_seqnum,
                LogBuffer* log_buffer, Directory* db_directory,
                Directory* output_directory, Statistics* stats,
                InstrumentedMutex* db_mutex, ErrorHandler* db_error_handler,
                std::vector<SequenceNumber> existing_snapshots,
                SequenceNumber earliest_write_conflict_snapshot,
                const SnapshotChecker* snapshot_checker,
                std::shared_ptr<Cache> table_cache, EventLogger* event_logger,
                bool paranoid_file_checks, bool measure_io_stats,
                const std::string& dbname,
                CompactionJobStats* compaction_job_stats);

  ~CompactionJob();

  // no copy/move
  CompactionJob(CompactionJob&& job) = delete;
  CompactionJob(const CompactionJob& job) = delete;
  CompactionJob& operator=(const CompactionJob& job) = delete;

  // REQUIRED: mutex held
  int Prepare(int sub_compaction_slots);
  // REQUIRED mutex not held
  Status Run();
  Status RunSelf();

  Status VerifyFiles();

  // REQUIRED: mutex held
  Status Install(const MutableCFOptions& mutable_cf_options);

  SeparationType separation_type() const;

  struct ProcessArg {
    CompactionJob* job;
    int task_id;
    std::promise<bool> finished;
    std::future<bool> future;
  };

  static void CallProcessCompaction(void* arg);

 private:
  struct SubcompactionState;

  void AggregateStatistics();
  void GenSubcompactionBoundaries(int max_usable_threads);

  // update the thread status for starting a compaction.
  void ReportStartedCompaction(Compaction* compaction);
  void AllocateCompactionOutputFileNumbers();
  // Call compaction filter. Then iterate through input and compact the
  // kv-pairs
  void ProcessCompaction(SubcompactionState* sub_compact);
  void ProcessKeyValueCompaction(SubcompactionState* sub_compact);
  void ProcessGarbageCollection(SubcompactionState* sub_compact);

  // For ZNS
  Status ProcessZNSNonPartitionGarbageCollection(
      SubcompactionState* sub_compact, ColumnFamilyData* cfd,
      std::unique_ptr<InternalIterator> input);
  Status ProcessZNSNonPartitionGarbageCollectionWithNoLookback(
      SubcompactionState* sub_compact, ColumnFamilyData* cfd,
      std::unique_ptr<InternalIterator> input);
  Status ProcessZNSPartitionGarbageCollection(
      SubcompactionState* sub_compact, ColumnFamilyData* cfd,
      std::unique_ptr<InternalIterator> input);
  Status ProcessZNSPartitionGarbageCollectionWithNoTriaging(
      SubcompactionState* sub_compact, ColumnFamilyData* cfd,
      std::unique_ptr<InternalIterator> input);
  void ProcessZNSGarbageCollection(SubcompactionState* sub_compact,
                                   bool look_back);

  Status FinishCompactionOutputFile(
      const Status& input_status, SubcompactionState* sub_compact,
      CompactionRangeDelAggregator* range_del_agg,
      CompactionIterationStats* range_del_out_stats,
      const std::unordered_map<uint64_t, uint64_t>& dependence,
      const Slice* next_table_min_key = nullptr);
  Status FinishCompactionOutputBlob(
      const Status& input_status, SubcompactionState* sub_compact,
      const std::vector<uint64_t>& inheritance_tree);
  Status FinishSpecialCompactionOutputBlob(
      const Status& input_status, SubcompactionState* sub_compact,
      const std::vector<uint64_t>& inheritance_tree, PlacementFileType type);
  Status InstallCompactionResults(const MutableCFOptions& mutable_cf_options);
  void RecordCompactionIOStats();
  Status OpenCompactionOutputFile(SubcompactionState* sub_compact);

  // OpenCompactionOutputBlob and OpenSpecialCompactionOutputBlob are
  // specialized for ZNS. In OpenCompactionOutputBlob, we reuse the blob_builder
  // and related fields in SubcompactionState but assigns the output files
  // PlacementFileType to be either Hot or Warm.
  //
  // In OpenCompactionOutputBlob, we use the hot_blob_builder,
  // warm_blob_builder and partition_blob_builder accordingly.
  Status OpenCompactionOutputBlob(SubcompactionState* sub_compact);
  Status OpenCompactionOutputBlob(SubcompactionState* subcompact,
                                  PlacementFileType type);

  // This helper initializes the blob_outfile and blob_builder fields in the
  // subcompact. The default parameter use_default_blob will decide whether
  // to use the blob_outputs unconditionally, or use the hot_blob_outputs,
  // warm_blob_outputs and cold_blob_outputs accordingly.
  Status OpenCompactinOutputBlobHelper(
      SubcompactionState* sub_compact,
      std::unique_ptr<WritableFileWriter>& blob_outfile,
      std::unique_ptr<TableBuilder>& blob_builder, PlacementFileType type,
      bool use_default_blob = true);
  void CleanupCompaction();
  void UpdateCompactionJobStats(
      const InternalStats::CompactionStats& stats) const;
  void RecordDroppedKeys(const CompactionIterationStats& c_iter_stats,
                         CompactionJobStats* compaction_job_stats = nullptr);

  void UpdateCompactionStats();
  void UpdateCompactionInputStatsHelper(int* num_files, uint64_t* bytes_read,
                                        int input_level);

  void LogCompaction();

  int job_id_;

  // CompactionJob state
  struct CompactionState;
  CompactionState* compact_;
  CompactionJobStats* compaction_job_stats_;
  InternalStats::CompactionStats compaction_stats_;

  // DBImpl state
  const std::string& dbname_;
  const ImmutableDBOptions& db_options_;
  const EnvOptions env_options_;

  Env* env_;
  // env_option optimized for compaction table reads
  EnvOptions env_options_for_read_;
  VersionSet* versions_;
  const std::atomic<bool>* shutting_down_;
  const SequenceNumber preserve_deletes_seqnum_;
  LogBuffer* log_buffer_;
  Directory* db_directory_;
  Directory* output_directory_;
  Statistics* stats_;
  InstrumentedMutex* db_mutex_;
  ErrorHandler* db_error_handler_;
  // If there were two snapshots with seq numbers s1 and
  // s2 and s1 < s2, and if we find two instances of a key k1 then lies
  // entirely within s1 and s2, then the earlier version of k1 can be safely
  // deleted because that version is not visible in any snapshot.
  std::vector<SequenceNumber> existing_snapshots_;

  // This is the earliest snapshot that could be used for write-conflict
  // checking by a transaction.  For any user-key newer than this snapshot, we
  // should make sure not to remove evidence that a write occurred.
  SequenceNumber earliest_write_conflict_snapshot_;

  const SnapshotChecker* const snapshot_checker_;

  std::shared_ptr<Cache> table_cache_;

  EventLogger* event_logger_;

  bool bottommost_level_;
  bool paranoid_file_checks_;
  bool measure_io_stats_;
  // Stores the Slices that designate the boundaries for each subcompaction
  std::vector<Slice> boundaries_;
  // Stores the approx size of keys covered in the range of each subcompaction
  std::vector<uint64_t> sizes_;
  Env::WriteLifeTimeHint write_hint_;
};

}  // namespace TERARKDB_NAMESPACE
