//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
//
// Test for issue 178: a manual compaction causes deleted data to reappear.
#include <iostream>
#include <sstream>
#include <cstdlib>

#include "rocksdb/db.h"
#include "rocksdb/compaction_filter.h"
#include "rocksdb/slice.h"
#include "rocksdb/write_batch.h"
#include "util/testharness.h"
#include "port/port.h"

using namespace TERARKDB_NAMESPACE;

namespace {

// Reasoning: previously the number was 1100000. Since the keys are written to
// the batch in one write each write will result into one SST file. each write
// will result into one SST file. We reduced the write_buffer_size to 1K to
// basically have the same effect with however less number of keys, which
// results into less test runtime.
const int kNumKeys = 1100;

std::string Key1(int i) {
  char buf[100];
  snprintf(buf, sizeof(buf), "my_key_%d", i);
  return buf;
}

std::string Key2(int i) {
  return Key1(i) + "_xxx";
}

class ManualCompactionTest : public testing::Test {
 public:
  ManualCompactionTest() {
    // Get rid of any state from an old run.
    dbname_ = TERARKDB_NAMESPACE::test::PerThreadDBPath("rocksdb_cbug_test");
    DestroyDB(dbname_, TERARKDB_NAMESPACE::Options());
  }

  std::string dbname_;
};

class DestroyAllCompactionFilter : public CompactionFilter {
 public:
  DestroyAllCompactionFilter() {}

  virtual bool Filter(int /*level*/, const Slice& /*key*/,
                      const Slice& existing_value, std::string* /*new_value*/,
                      bool* /*value_changed*/) const override {
    return existing_value.ToString() == "destroy";
  }

  virtual const char* Name() const override {
    return "DestroyAllCompactionFilter";
  }
};

TEST_F(ManualCompactionTest, CompactTouchesAllKeys) {
  for (int iter = 0; iter < 2; ++iter) {
    DB* db;
    Options options;
    options.enable_lazy_compaction = false;
    if (iter == 0) { // level compaction
      options.num_levels = 3;
      options.compaction_style = kCompactionStyleLevel;
    } else { // universal compaction
      options.compaction_style = kCompactionStyleUniversal;
    }
    options.create_if_missing = true;
    options.compression = TERARKDB_NAMESPACE::kNoCompression;
    options.compaction_filter = new DestroyAllCompactionFilter();
    ASSERT_OK(DB::Open(options, dbname_, &db));

    db->Put(WriteOptions(), Slice("key1"), Slice("destroy"));
    db->Put(WriteOptions(), Slice("key2"), Slice("destroy"));
    db->Put(WriteOptions(), Slice("key3"), Slice("value3"));
    db->Put(WriteOptions(), Slice("key4"), Slice("destroy"));

    Slice key4("key4");
    db->CompactRange(CompactRangeOptions(), nullptr, &key4);
    Iterator* itr = db->NewIterator(ReadOptions());
    itr->SeekToFirst();
    ASSERT_TRUE(itr->Valid());
    ASSERT_EQ("key3", itr->key().ToString());
    itr->Next();
    ASSERT_TRUE(!itr->Valid());
    delete itr;

    delete options.compaction_filter;
    delete db;
    DestroyDB(dbname_, options);
  }
}

TEST_F(ManualCompactionTest, Test) {
  // Open database.  Disable compression since it affects the creation
  // of layers and the code below is trying to test against a very
  // specific scenario.
  TERARKDB_NAMESPACE::DB* db;
  TERARKDB_NAMESPACE::Options db_options;
  db_options.write_buffer_size = 1024;
  db_options.create_if_missing = true;
  db_options.compression = TERARKDB_NAMESPACE::kNoCompression;
  ASSERT_OK(TERARKDB_NAMESPACE::DB::Open(db_options, dbname_, &db));

  // create first key range
  TERARKDB_NAMESPACE::WriteBatch batch;
  for (int i = 0; i < kNumKeys; i++) {
    batch.Put(Key1(i), "value for range 1 key");
  }
  ASSERT_OK(db->Write(TERARKDB_NAMESPACE::WriteOptions(), &batch));

  // create second key range
  batch.Clear();
  for (int i = 0; i < kNumKeys; i++) {
    batch.Put(Key2(i), "value for range 2 key");
  }
  ASSERT_OK(db->Write(TERARKDB_NAMESPACE::WriteOptions(), &batch));

  // delete second key range
  batch.Clear();
  for (int i = 0; i < kNumKeys; i++) {
    batch.Delete(Key2(i));
  }
  ASSERT_OK(db->Write(TERARKDB_NAMESPACE::WriteOptions(), &batch));

  // compact database
  std::string start_key = Key1(0);
  std::string end_key = Key1(kNumKeys - 1);
  TERARKDB_NAMESPACE::Slice least(start_key.data(), start_key.size());
  TERARKDB_NAMESPACE::Slice greatest(end_key.data(), end_key.size());

  // commenting out the line below causes the example to work correctly
  db->CompactRange(CompactRangeOptions(), &least, &greatest);

  // count the keys
  TERARKDB_NAMESPACE::Iterator* iter = db->NewIterator(TERARKDB_NAMESPACE::ReadOptions());
  int num_keys = 0;
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    num_keys++;
  }
  delete iter;
  ASSERT_EQ(kNumKeys, num_keys) << "Bad number of keys";

  // close database
  delete db;
  DestroyDB(dbname_, TERARKDB_NAMESPACE::Options());
}

}  // anonymous namespace

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}