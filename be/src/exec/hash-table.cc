// Copyright 2012 Cloudera Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "exec/hash-table.inline.h"

#include <functional>
#include <numeric>
#include <gutil/strings/substitute.h>

#include "codegen/codegen-anyval.h"
#include "codegen/llvm-codegen.h"
#include "exprs/expr.h"
#include "exprs/expr-context.h"
#include "exprs/slot-ref.h"
#include "runtime/buffered-block-mgr.h"
#include "runtime/mem-tracker.h"
#include "runtime/raw-value.inline.h"
#include "runtime/runtime-state.h"
#include "runtime/string-value.inline.h"
#include "util/debug-util.h"
#include "util/impalad-metrics.h"

#include "common/names.h"

using namespace impala;
using namespace llvm;
using namespace strings;

DEFINE_bool(enable_quadratic_probing, true, "Enable quadratic probing hash table");

const char* HashTableCtx::LLVM_CLASS_NAME = "class.impala::HashTableCtx";

// Random primes to multiply the seed with.
static uint32_t SEED_PRIMES[] = {
  1, // First seed must be 1, level 0 is used by other operators in the fragment.
  1431655781,
  1183186591,
  622729787,
  472882027,
  338294347,
  275604541,
  41161739,
  29999999,
  27475109,
  611603,
  16313357,
  11380003,
  21261403,
  33393119,
  101,
  71043403
};

// Put a non-zero constant in the result location for NULL.
// We don't want(NULL, 1) to hash to the same as (0, 1).
// This needs to be as big as the biggest primitive type since the bytes
// get copied directly.
// TODO find a better approach, since primitives like CHAR(N) can be up to 128 bytes
static int64_t NULL_VALUE[] = { HashUtil::FNV_SEED, HashUtil::FNV_SEED,
                                HashUtil::FNV_SEED, HashUtil::FNV_SEED,
                                HashUtil::FNV_SEED, HashUtil::FNV_SEED,
                                HashUtil::FNV_SEED, HashUtil::FNV_SEED,
                                HashUtil::FNV_SEED, HashUtil::FNV_SEED,
                                HashUtil::FNV_SEED, HashUtil::FNV_SEED,
                                HashUtil::FNV_SEED, HashUtil::FNV_SEED,
                                HashUtil::FNV_SEED, HashUtil::FNV_SEED };

// The first NUM_SMALL_BLOCKS of nodes_ are made of blocks less than the IO size (of 8MB)
// to reduce the memory footprint of small queries. In particular, we always first use a
// 64KB and a 512KB block before starting using IO-sized blocks.
static const int64_t INITIAL_DATA_PAGE_SIZES[] = { 64 * 1024, 512 * 1024 };
static const int NUM_SMALL_DATA_PAGES = sizeof(INITIAL_DATA_PAGE_SIZES) / sizeof(int64_t);

HashTableCtx::HashTableCtx(const std::vector<ExprContext*>& build_expr_ctxs,
    const std::vector<ExprContext*>& probe_expr_ctxs, bool stores_nulls,
    const std::vector<bool>& finds_nulls, int32_t initial_seed,
    int max_levels, MemTracker* tracker)
    : build_expr_ctxs_(build_expr_ctxs),
      probe_expr_ctxs_(probe_expr_ctxs),
      stores_nulls_(stores_nulls),
      finds_nulls_(finds_nulls),
      finds_some_nulls_(std::accumulate(
          finds_nulls_.begin(), finds_nulls_.end(), false, std::logical_or<bool>())),
      level_(0),
      scratch_row_(NULL),
      tracker_(tracker) {
  DCHECK(!finds_some_nulls_ || stores_nulls_);
  // Compute the layout and buffer size to store the evaluated expr results
  DCHECK_EQ(build_expr_ctxs_.size(), probe_expr_ctxs_.size());
  DCHECK_EQ(build_expr_ctxs_.size(), finds_nulls_.size());
  DCHECK(!build_expr_ctxs_.empty());

  // Populate the seeds to use for all the levels. TODO: revisit how we generate these.
  DCHECK_GE(max_levels, 0);
  DCHECK_LT(max_levels, sizeof(SEED_PRIMES) / sizeof(SEED_PRIMES[0]));
  DCHECK_NE(initial_seed, 0);
  seeds_.resize(max_levels + 1);
  seeds_[0] = initial_seed;
  for (int i = 1; i <= max_levels; ++i) {
    seeds_[i] = seeds_[i - 1] * SEED_PRIMES[i];
  }
}

Status HashTableCtx::Create(RuntimeState* state,
    const std::vector<ExprContext*>& build_expr_ctxs,
    const std::vector<ExprContext*>& probe_expr_ctxs, bool stores_nulls,
    const std::vector<bool>& finds_nulls, int32_t initial_seed, int max_levels,
    int num_build_tuples, MemTracker* tracker, scoped_ptr<HashTableCtx>* ht_ctx) {
  ht_ctx->reset(new HashTableCtx(build_expr_ctxs, probe_expr_ctxs, stores_nulls,
      finds_nulls, initial_seed, max_levels, tracker));
  return ht_ctx->get()->Init(state, num_build_tuples);
}

Status HashTableCtx::Init(RuntimeState* state, int num_build_tuples) {
  int scratch_row_size = sizeof(Tuple*) * num_build_tuples;
  scratch_row_ = reinterpret_cast<TupleRow*>(malloc(scratch_row_size));
  if (UNLIKELY(scratch_row_ == NULL)) {
    return Status(Substitute("Failed to allocate $0 bytes for scratch row of "
        "HashTableCtx.", scratch_row_size));
  }
  return expr_values_cache_.Init(state, tracker_, build_expr_ctxs_);
}

void HashTableCtx::Close() {
  free(scratch_row_);
  scratch_row_ = NULL;
  expr_values_cache_.Close(tracker_);
}

uint32_t HashTableCtx::Hash(const void* input, int len, uint32_t hash) const {
  /// Use CRC hash at first level for better performance. Switch to murmur hash at
  /// subsequent levels since CRC doesn't randomize well with different seed inputs.
  if (level_ == 0) return HashUtil::Hash(input, len, hash);
  return HashUtil::MurmurHash2_64(input, len, hash);
}

uint32_t HashTableCtx::HashCurrentRow() const {
  DCHECK_LT(level_, seeds_.size());
  if (expr_values_cache_.var_result_offset() == -1) {
    /// This handles NULLs implicitly since a constant seed value was put
    /// into results buffer for nulls.
    return Hash(expr_values_cache_.cur_expr_values_,
        expr_values_cache_.expr_values_bytes_per_row(), seeds_[level_]);
  } else {
    return HashTableCtx::HashVariableLenRow();
  }
}

bool HashTableCtx::EvalRow(TupleRow* row, const vector<ExprContext*>& ctxs) {
  bool has_null = false;
  uint8_t* exprs_nullness = expr_values_cache_.ExprValueNullPtr(0);
  for (int i = 0; i < ctxs.size(); ++i) {
    void* loc = expr_values_cache_.ExprValuePtr(i);
    void* val = ctxs[i]->GetValue(row);
    if (val == NULL) {
      // If the table doesn't store nulls, no reason to keep evaluating
      if (!stores_nulls_) return true;
      exprs_nullness[i] = true;
      val = reinterpret_cast<void*>(&NULL_VALUE);
      has_null = true;
    } else {
      exprs_nullness[i] = false;
    }
    DCHECK_LE(build_expr_ctxs_[i]->root()->type().GetSlotSize(),
        sizeof(NULL_VALUE));
    RawValue::Write(val, loc, build_expr_ctxs_[i]->root()->type(), NULL);
  }
  return has_null;
}

uint32_t HashTableCtx::HashVariableLenRow() const {
  uint32_t hash = seeds_[level_];
  int var_result_offset = expr_values_cache_.var_result_offset();
  // Hash the non-var length portions (if there are any)
  if (var_result_offset != 0) {
    hash = Hash(expr_values_cache_.cur_expr_values_, var_result_offset, hash);
  }

  uint8_t* exprs_nullness = expr_values_cache_.ExprValueNullPtr(0);
  for (int i = 0; i < build_expr_ctxs_.size(); ++i) {
    // non-string and null slots are already part of cur_expr_values_
    if (build_expr_ctxs_[i]->root()->type().type != TYPE_STRING &&
        build_expr_ctxs_[i]->root()->type().type != TYPE_VARCHAR) continue;

    void* loc = expr_values_cache_.ExprValuePtr(i);
    if (exprs_nullness[i]) {
      // Hash the null random seed values at 'loc'
      hash = Hash(loc, sizeof(StringValue), hash);
    } else {
      // Hash the string
      // TODO: when using CRC hash on empty string, this only swaps bytes.
      StringValue* str = reinterpret_cast<StringValue*>(loc);
      hash = Hash(str->ptr, str->len, hash);
    }
  }
  return hash;
}

template<bool FORCE_NULL_EQUALITY>
bool HashTableCtx::Equals(TupleRow* build_row) const {
  uint8_t* exprs_nullness = expr_values_cache_.ExprValueNullPtr(0);
  for (int i = 0; i < build_expr_ctxs_.size(); ++i) {
    void* val = build_expr_ctxs_[i]->GetValue(build_row);
    if (val == NULL) {
      if (!(FORCE_NULL_EQUALITY || finds_nulls_[i])) return false;
      if (!exprs_nullness[i]) return false;
      continue;
    } else {
      if (exprs_nullness[i]) return false;
    }

    void* loc = expr_values_cache_.ExprValuePtr(i);
    if (!RawValue::Eq(loc, val, build_expr_ctxs_[i]->root()->type())) {
      return false;
    }
  }
  return true;
}

template bool HashTableCtx::Equals<true>(TupleRow* build_row) const;
template bool HashTableCtx::Equals<false>(TupleRow* build_row) const;

HashTableCtx::ExprValuesCache::ExprValuesCache()
    : capacity_(0),
      cur_expr_values_(NULL),
      cur_expr_values_null_(NULL),
      cur_expr_values_hash_(NULL),
      cur_expr_values_hash_end_(NULL),
      expr_values_array_(NULL),
      expr_values_null_array_(NULL),
      expr_values_hash_array_(NULL),
      null_bitmap_(0) { }

Status HashTableCtx::ExprValuesCache::Init(RuntimeState* state,
    MemTracker* tracker, const std::vector<ExprContext*>& build_expr_ctxs) {
  // Initialize the number of expressions.
  num_exprs_ = build_expr_ctxs.size();
  // Compute the layout of evaluated values of a row.
  expr_values_bytes_per_row_ = Expr::ComputeResultsLayout(build_expr_ctxs,
      &expr_values_offsets_, &var_result_offset_);
  if (expr_values_bytes_per_row_ == 0) {
    DCHECK_EQ(num_exprs_, 0);
    return Status::OK();
  }
  DCHECK_GT(expr_values_bytes_per_row_, 0);
  // Compute the maximum number of cached rows which can fit in the memory budget.
  // TODO: Find the optimal prefetch batch size. This may be something
  // processor dependent so we may need calibration at Impala startup time.
  capacity_ = std::max(1, std::min(state->batch_size(),
      MAX_EXPR_VALUES_ARRAY_SIZE / expr_values_bytes_per_row_));

  int mem_usage = MemUsage(capacity_, expr_values_bytes_per_row_, num_exprs_);
  if (!tracker->TryConsume(mem_usage)) {
    capacity_ = 0;
    string details = Substitute("HashTableCtx::ExprValuesCache failed to allocate $0 bytes.",
        mem_usage);
    return tracker->MemLimitExceeded(state, details, mem_usage);
  }

  int expr_values_size = expr_values_bytes_per_row_ * capacity_;
  expr_values_array_.reset(new uint8_t[expr_values_size]);
  cur_expr_values_ = expr_values_array_.get();
  memset(cur_expr_values_, 0, expr_values_size);

  int expr_values_null_size = num_exprs_ * capacity_;
  expr_values_null_array_.reset(new uint8_t[expr_values_null_size]);
  cur_expr_values_null_ = expr_values_null_array_.get();
  memset(cur_expr_values_null_, 0, expr_values_null_size);

  expr_values_hash_array_.reset(new uint32_t[capacity_]);
  cur_expr_values_hash_ = expr_values_hash_array_.get();
  cur_expr_values_hash_end_ = cur_expr_values_hash_;
  memset(cur_expr_values_hash_, 0, sizeof(uint32) * capacity_);

  null_bitmap_.Reset(capacity_);
  return Status::OK();
}

void HashTableCtx::ExprValuesCache::Close(MemTracker* tracker) {
  if (capacity_ == 0) return;
  cur_expr_values_ = NULL;
  cur_expr_values_null_ = NULL;
  cur_expr_values_hash_ = NULL;
  cur_expr_values_hash_end_ = NULL;
  expr_values_array_.reset();
  expr_values_null_array_.reset();
  expr_values_hash_array_.reset();
  null_bitmap_.Reset(0);
  int mem_usage = MemUsage(capacity_, expr_values_bytes_per_row_, num_exprs_);
  tracker->Release(mem_usage);
}

int HashTableCtx::ExprValuesCache::MemUsage(int capacity,
    int expr_values_bytes_per_row, int num_exprs) {
  return expr_values_bytes_per_row * capacity + // expr_values_array_
      num_exprs * capacity +                    // expr_values_null_array_
      sizeof(uint32) * capacity +               // expr_values_hash_array_
      Bitmap::MemUsage(capacity);               // null_bitmap_
}

uint8_t* HashTableCtx::ExprValuesCache::ExprValuePtr(int expr_idx) const {
  return cur_expr_values_ + expr_values_offsets_[expr_idx];
}

uint8_t* HashTableCtx::ExprValuesCache::ExprValueNullPtr(int expr_idx) const {
  return cur_expr_values_null_ + expr_idx;
}

void HashTableCtx::ExprValuesCache::ResetIterators() {
  cur_expr_values_ = expr_values_array_.get();
  cur_expr_values_null_ = expr_values_null_array_.get();
  cur_expr_values_hash_ = expr_values_hash_array_.get();
}

void HashTableCtx::ExprValuesCache::Reset() {
  ResetIterators();
  // Set the end pointer after resetting the other pointers so they point to
  // the same location.
  cur_expr_values_hash_end_ = cur_expr_values_hash_;
  null_bitmap_.SetAllBits(false);
}

void HashTableCtx::ExprValuesCache::ResetForRead() {
  // Record the end of hash values iterator to be used in AtEnd().
  // Do it before resetting the pointers.
  cur_expr_values_hash_end_ = cur_expr_values_hash_;
  ResetIterators();
}

const double HashTable::MAX_FILL_FACTOR = 0.75f;

HashTable* HashTable::Create(RuntimeState* state,
    BufferedBlockMgr::Client* client, bool stores_duplicates, int num_build_tuples,
    BufferedTupleStream* tuple_stream, int64_t max_num_buckets,
    int64_t initial_num_buckets) {
  return new HashTable(FLAGS_enable_quadratic_probing, state, client, stores_duplicates,
      num_build_tuples, tuple_stream, max_num_buckets, initial_num_buckets);
}

HashTable::HashTable(bool quadratic_probing, RuntimeState* state,
    BufferedBlockMgr::Client* client, bool stores_duplicates, int num_build_tuples,
    BufferedTupleStream* stream, int64_t max_num_buckets, int64_t num_buckets)
  : state_(state),
    block_mgr_client_(client),
    tuple_stream_(stream),
    stores_tuples_(num_build_tuples == 1),
    stores_duplicates_(stores_duplicates),
    quadratic_probing_(quadratic_probing),
    total_data_page_size_(0),
    next_node_(NULL),
    node_remaining_current_page_(0),
    num_duplicate_nodes_(0),
    max_num_buckets_(max_num_buckets),
    buckets_(NULL),
    num_buckets_(num_buckets),
    num_filled_buckets_(0),
    num_buckets_with_duplicates_(0),
    num_build_tuples_(num_build_tuples),
    has_matches_(false),
    num_probes_(0), num_failed_probes_(0), travel_length_(0), num_hash_collisions_(0),
    num_resizes_(0) {
  DCHECK_EQ((num_buckets & (num_buckets-1)), 0) << "num_buckets must be a power of 2";
  DCHECK_GT(num_buckets, 0) << "num_buckets must be larger than 0";
  DCHECK(stores_tuples_ || stream != NULL);
  DCHECK(client != NULL);
}

bool HashTable::Init() {
  int64_t buckets_byte_size = num_buckets_ * sizeof(Bucket);
  if (!state_->block_mgr()->ConsumeMemory(block_mgr_client_, buckets_byte_size)) {
    num_buckets_ = 0;
    return false;
  }
  buckets_ = reinterpret_cast<Bucket*>(malloc(buckets_byte_size));
  memset(buckets_, 0, buckets_byte_size);
  return true;
}

void HashTable::Close() {
  // Print statistics only for the large or heavily used hash tables.
  // TODO: Tweak these numbers/conditions, or print them always?
  const int64_t LARGE_HT = 128 * 1024;
  const int64_t HEAVILY_USED = 1024 * 1024;
  // TODO: These statistics should go to the runtime profile as well.
  if ((num_buckets_ > LARGE_HT) || (num_probes_ > HEAVILY_USED)) VLOG(2) << PrintStats();
  for (int i = 0; i < data_pages_.size(); ++i) {
    data_pages_[i]->Delete();
  }
  if (ImpaladMetrics::HASH_TABLE_TOTAL_BYTES != NULL) {
    ImpaladMetrics::HASH_TABLE_TOTAL_BYTES->Increment(-total_data_page_size_);
  }
  data_pages_.clear();
  if (buckets_ != NULL) free(buckets_);
  state_->block_mgr()->ReleaseMemory(block_mgr_client_, num_buckets_ * sizeof(Bucket));
}

bool HashTable::CheckAndResize(uint64_t buckets_to_fill, const HashTableCtx* ht_ctx) {
  uint64_t shift = 0;
  while (num_filled_buckets_ + buckets_to_fill >
         (num_buckets_ << shift) * MAX_FILL_FACTOR) {
    // TODO: next prime instead of double?
    ++shift;
  }
  if (shift > 0) return ResizeBuckets(num_buckets_ << shift, ht_ctx);
  return true;
}

bool HashTable::ResizeBuckets(int64_t num_buckets, const HashTableCtx* ht_ctx) {
  DCHECK_EQ((num_buckets & (num_buckets-1)), 0)
      << "num_buckets=" << num_buckets << " must be a power of 2";
  DCHECK_GT(num_buckets, num_filled_buckets_) << "Cannot shrink the hash table to "
      "smaller number of buckets than the number of filled buckets.";
  VLOG(2) << "Resizing hash table from "
          << num_buckets_ << " to " << num_buckets << " buckets.";
  if (max_num_buckets_ != -1 && num_buckets > max_num_buckets_) return false;
  ++num_resizes_;

  // All memory that can grow proportional to the input should come from the block mgrs
  // mem tracker.
  // Note that while we copying over the contents of the old hash table, we need to have
  // allocated both the old and the new hash table. Once we finish, we return the memory
  // of the old hash table.
  int64_t old_size = num_buckets_ * sizeof(Bucket);
  int64_t new_size = num_buckets * sizeof(Bucket);
  if (!state_->block_mgr()->ConsumeMemory(block_mgr_client_, new_size)) return false;
  Bucket* new_buckets = reinterpret_cast<Bucket*>(malloc(new_size));
  DCHECK(new_buckets != NULL);
  memset(new_buckets, 0, new_size);

  // Walk the old table and copy all the filled buckets to the new (resized) table.
  // We do not have to do anything with the duplicate nodes. This operation is expected
  // to succeed.
  for (HashTable::Iterator iter = Begin(ht_ctx); !iter.AtEnd();
       NextFilledBucket(&iter.bucket_idx_, &iter.node_)) {
    Bucket* bucket_to_copy = &buckets_[iter.bucket_idx_];
    bool found = false;
    int64_t bucket_idx =
        Probe<true>(new_buckets, num_buckets, NULL, bucket_to_copy->hash, &found);
    DCHECK(!found);
    DCHECK_NE(bucket_idx, Iterator::BUCKET_NOT_FOUND) << " Probe failed even though "
        " there are free buckets. " << num_buckets << " " << num_filled_buckets_;
    Bucket* dst_bucket = &new_buckets[bucket_idx];
    *dst_bucket = *bucket_to_copy;
  }

  num_buckets_ = num_buckets;
  free(buckets_);
  buckets_ = new_buckets;
  state_->block_mgr()->ReleaseMemory(block_mgr_client_, old_size);
  return true;
}

bool HashTable::GrowNodeArray() {
  int64_t page_size = 0;
  page_size = state_->block_mgr()->max_block_size();
  if (data_pages_.size() < NUM_SMALL_DATA_PAGES) {
    page_size = min(page_size, INITIAL_DATA_PAGE_SIZES[data_pages_.size()]);
  }
  BufferedBlockMgr::Block* block = NULL;
  Status status = state_->block_mgr()->GetNewBlock(
      block_mgr_client_, NULL, &block, page_size);
  DCHECK(status.ok() || block == NULL);
  if (block == NULL) return false;
  data_pages_.push_back(block);
  next_node_ = block->Allocate<DuplicateNode>(page_size);
  ImpaladMetrics::HASH_TABLE_TOTAL_BYTES->Increment(page_size);
  node_remaining_current_page_ = page_size / sizeof(DuplicateNode);
  total_data_page_size_ += page_size;
  return true;
}

void HashTable::DebugStringTuple(stringstream& ss, HtData& htdata,
    const RowDescriptor* desc) {
  if (stores_tuples_) {
    ss << "(" << htdata.tuple << ")";
  } else {
    ss << "(" << htdata.idx.block() << ", " << htdata.idx.idx()
       << ", " << htdata.idx.offset() << ")";
  }
  if (desc != NULL) {
    Tuple* row[num_build_tuples_];
    ss << " " << PrintRow(GetRow(htdata, reinterpret_cast<TupleRow*>(row)), *desc);
  }
}

string HashTable::DebugString(bool skip_empty, bool show_match,
    const RowDescriptor* desc) {
  stringstream ss;
  ss << endl;
  for (int i = 0; i < num_buckets_; ++i) {
    if (skip_empty && !buckets_[i].filled) continue;
    ss << i << ": ";
    if (show_match) {
      if (buckets_[i].matched) {
        ss << " [M]";
      } else {
        ss << " [U]";
      }
    }
    if (buckets_[i].hasDuplicates) {
      DuplicateNode* node = buckets_[i].bucketData.duplicates;
      bool first = true;
      ss << " [D] ";
      while (node != NULL) {
        if (!first) ss << ",";
        DebugStringTuple(ss, node->htdata, desc);
        node = node->next;
        first = false;
      }
    } else {
      ss << " [B] ";
      if (buckets_[i].filled) {
        DebugStringTuple(ss, buckets_[i].bucketData.htdata, desc);
      } else {
        ss << " - ";
      }
    }
    ss << endl;
  }
  return ss.str();
}

string HashTable::PrintStats() const {
  double curr_fill_factor = (double)num_filled_buckets_/(double)num_buckets_;
  double avg_travel = (double)travel_length_/(double)num_probes_;
  double avg_collisions = (double)num_hash_collisions_/(double)num_filled_buckets_;
  stringstream ss;
  ss << "Buckets: " << num_buckets_ << " " << num_filled_buckets_ << " "
     << curr_fill_factor << endl;
  ss << "Duplicates: " << num_buckets_with_duplicates_ << " buckets "
     << num_duplicate_nodes_ << " nodes" << endl;
  ss << "Probes: " << num_probes_ << endl;
  ss << "FailedProbes: " << num_failed_probes_ << endl;
  ss << "Travel: " << travel_length_ << " " << avg_travel << endl;
  ss << "HashCollisions: " << num_hash_collisions_ << " " << avg_collisions << endl;
  ss << "Resizes: " << num_resizes_ << endl;
  return ss.str();
}

// Helper function to store a value into the results buffer if the expr
// evaluated to NULL.  We don't want (NULL, 1) to hash to the same as (0,1) so
// we'll pick a more random value.
static void CodegenAssignNullValue(LlvmCodeGen* codegen,
    LlvmCodeGen::LlvmBuilder* builder, Value* dst, const ColumnType& type) {
  int64_t fvn_seed = HashUtil::FNV_SEED;

  if (type.type == TYPE_STRING || type.type == TYPE_VARCHAR) {
    Value* dst_ptr = builder->CreateStructGEP(NULL, dst, 0, "string_ptr");
    Value* dst_len = builder->CreateStructGEP(NULL, dst, 1, "string_len");
    Value* null_len = codegen->GetIntConstant(TYPE_INT, fvn_seed);
    Value* null_ptr = builder->CreateIntToPtr(null_len, codegen->ptr_type());
    builder->CreateStore(null_ptr, dst_ptr);
    builder->CreateStore(null_len, dst_len);
  } else {
    Value* null_value = NULL;
    // Get a type specific representation of fvn_seed
    switch (type.type) {
      case TYPE_BOOLEAN:
        // In results, booleans are stored as 1 byte
        dst = builder->CreateBitCast(dst, codegen->ptr_type());
        null_value = codegen->GetIntConstant(TYPE_TINYINT, fvn_seed);
        break;
      case TYPE_TINYINT:
      case TYPE_SMALLINT:
      case TYPE_INT:
      case TYPE_BIGINT:
      case TYPE_DECIMAL:
        null_value = codegen->GetIntConstant(type.GetByteSize(), fvn_seed);
        break;
      case TYPE_FLOAT: {
        // Don't care about the value, just the bit pattern
        float fvn_seed_float = *reinterpret_cast<float*>(&fvn_seed);
        null_value = ConstantFP::get(codegen->context(), APFloat(fvn_seed_float));
        break;
      }
      case TYPE_DOUBLE: {
        // Don't care about the value, just the bit pattern
        double fvn_seed_double = *reinterpret_cast<double*>(&fvn_seed);
        null_value = ConstantFP::get(codegen->context(), APFloat(fvn_seed_double));
        break;
      }
      default:
        DCHECK(false);
    }
    builder->CreateStore(null_value, dst);
  }
}

// Codegen for evaluating a tuple row over either build_expr_ctxs_ or probe_expr_ctxs_.
// For a group by with (big int, string) the IR looks like
// define i1 @EvalProbeRow(%"class.impala::HashTableCtx"* %this_ptr,
//                         %"class.impala::TupleRow"* %row) #33 {
// entry:
//   %0 = load i8*, i8** inttoptr (i64 230325056 to i8**)
//   %1 = load i8*, i8** inttoptr (i64 230325064 to i8**)
//   %loc_addr = getelementptr i8, i8* %0, i32 0
//   %loc = bitcast i8* %loc_addr to i32*
//   %result = call i64 @GetSlotRef.3(%"class.impala::ExprContext"*
//             inttoptr (i64 158123712 to %"class.impala::ExprContext"*),
//             %"class.impala::TupleRow"* %row)
//   %is_null = trunc i64 %result to i1
//   %2 = zext i1 %is_null to i8
//   %null_byte_loc = getelementptr i8, i8* %1, i32 0
//   store i8 %2, i8* %null_byte_loc
//   br i1 %is_null, label %null, label %not_null
//
// null:                                             ; preds = %entry
//   store i32 -2128831035, i32* %loc
//   br label %continue
//
// not_null:                                         ; preds = %entry
//   %3 = ashr i64 %result, 32
//   %4 = trunc i64 %3 to i32
//   store i32 %4, i32* %loc
//   br label %continue
//
// continue:                                         ; preds = %not_null, %null
//   %is_null_phi = phi i1 [ true, %null ], [ false, %not_null ]
//   %has_null = or i1 false, %is_null_phi
//   %loc_addr1 = getelementptr i8, i8* %0, i32 8
//   %loc2 = bitcast i8* %loc_addr1 to %"struct.impala::StringValue"*
//   %result6 = call { i64, i8* } @GetSlotRef.4(%"class.impala::ExprContext"*
//              inttoptr (i64 158123904 to %"class.impala::ExprContext"*),
//              %"class.impala::TupleRow"* %row)
//   %5 = extractvalue { i64, i8* } %result6, 0
//   %is_null7 = trunc i64 %5 to i1
//   %6 = zext i1 %is_null7 to i8
//   %null_byte_loc8 = getelementptr i8, i8* %1, i32 1
//   store i8 %6, i8* %null_byte_loc8
//   br i1 %is_null7, label %null3, label %not_null4
//
// null3:                                            ; preds = %continue
//   %string_ptr = getelementptr inbounds %"struct.impala::StringValue",
//                 %"struct.impala::StringValue"* %loc2, i32 0, i32 0
//   %string_len = getelementptr inbounds %"struct.impala::StringValue",
//                 %"struct.impala::StringValue"* %loc2, i32 0, i32 1
//   store i8* inttoptr (i32 -2128831035 to i8*), i8** %string_ptr
//   store i32 -2128831035, i32* %string_len
//   br label %continue5
//
// not_null4:                                        ; preds = %continue
//   %result9 = extractvalue { i64, i8* } %result6, 1
//   %7 = insertvalue %"struct.impala::StringValue" zeroinitializer, i8* %result9, 0
//   %8 = extractvalue { i64, i8* } %result6, 0
//   %9 = ashr i64 %8, 32
//   %10 = trunc i64 %9 to i32
//   %11 = insertvalue %"struct.impala::StringValue" %7, i32 %10, 1
//   store %"struct.impala::StringValue" %11, %"struct.impala::StringValue"* %loc2
//   br label %continue5
//
// continue5:                                        ; preds = %not_null4, %null3
//   %is_null_phi10 = phi i1 [ true, %null3 ], [ false, %not_null4 ]
//   %has_null11 = or i1 %has_null, %is_null_phi10
//   ret i1 %has_null11
// }
// For each expr, we create 3 code blocks.  The null, not null and continue blocks.
// Both the null and not null branch into the continue block.  The continue block
// becomes the start of the next block for codegen (either the next expr or just the
// end of the function).
Status HashTableCtx::CodegenEvalRow(RuntimeState* state, bool build, Function** fn) {
  // TODO: CodegenAssignNullValue() can't handle TYPE_TIMESTAMP or TYPE_DECIMAL yet
  const vector<ExprContext*>& ctxs = build ? build_expr_ctxs_ : probe_expr_ctxs_;
  for (int i = 0; i < ctxs.size(); ++i) {
    PrimitiveType type = ctxs[i]->root()->type().type;
    if (type == TYPE_TIMESTAMP || type == TYPE_CHAR) {
      return Status(Substitute("HashTableCtx::CodegenEvalRow(): type $0 NYI",
          TypeToString(type)));
    }
  }

  LlvmCodeGen* codegen;
  RETURN_IF_ERROR(state->GetCodegen(&codegen));

  // Get types to generate function prototype
  Type* tuple_row_type = codegen->GetType(TupleRow::LLVM_CLASS_NAME);
  DCHECK(tuple_row_type != NULL);
  PointerType* tuple_row_ptr_type = PointerType::get(tuple_row_type, 0);

  Type* this_type = codegen->GetType(HashTableCtx::LLVM_CLASS_NAME);
  DCHECK(this_type != NULL);
  PointerType* this_ptr_type = PointerType::get(this_type, 0);
  PointerType* buffer_ptr_type = PointerType::get(codegen->ptr_type(), 0);
  LlvmCodeGen::FnPrototype prototype(codegen, build ? "EvalBuildRow" : "EvalProbeRow",
      codegen->GetType(TYPE_BOOLEAN));
  prototype.AddArgument(LlvmCodeGen::NamedVariable("this_ptr", this_ptr_type));
  prototype.AddArgument(LlvmCodeGen::NamedVariable("row", tuple_row_ptr_type));

  LLVMContext& context = codegen->context();
  LlvmCodeGen::LlvmBuilder builder(context);
  Value* args[2];
  *fn = prototype.GeneratePrototype(&builder, args);
  Value* row = args[1];
  Value* has_null = codegen->false_value();

  // Load cur_expr_values_ into a LLVM pointer.
  Value* cur_expr_values_ptr = codegen->CastPtrToLlvmPtr(buffer_ptr_type,
      &expr_values_cache_.cur_expr_values_);
  Value* cur_expr_values = builder.CreateLoad(cur_expr_values_ptr);

  // Load cur_expr_values_null_ into a LLVM pointer.
  Value* cur_expr_values_null_ptr = codegen->CastPtrToLlvmPtr(buffer_ptr_type,
      &expr_values_cache_.cur_expr_values_null_);
  Value* cur_expr_values_null = builder.CreateLoad(cur_expr_values_null_ptr);

  for (int i = 0; i < ctxs.size(); ++i) {
    // TODO: refactor this to somewhere else?  This is not hash table specific except for
    // the null handling bit and would be used for anyone that needs to materialize a
    // vector of exprs
    // Convert result buffer to llvm ptr type
    int offset = expr_values_cache_.expr_values_offsets(i);
    Value* loc = builder.CreateGEP(NULL, cur_expr_values,
        codegen->GetIntConstant(TYPE_INT, offset), "loc_addr");
    Value* llvm_loc = builder.CreatePointerCast(loc,
        codegen->GetPtrType(ctxs[i]->root()->type()), "loc");

    BasicBlock* null_block = BasicBlock::Create(context, "null", *fn);
    BasicBlock* not_null_block = BasicBlock::Create(context, "not_null", *fn);
    BasicBlock* continue_block = BasicBlock::Create(context, "continue", *fn);

    // Call expr
    Function* expr_fn;
    Status status = ctxs[i]->root()->GetCodegendComputeFn(state, &expr_fn);
    if (!status.ok()) {
      (*fn)->eraseFromParent(); // deletes function
      *fn = NULL;
      return Status(Substitute("Problem with HashTableCtx::CodegenEvalRow(): $0",
          status.GetDetail()));
    }

    Value* ctx_arg = codegen->CastPtrToLlvmPtr(
        codegen->GetPtrType(ExprContext::LLVM_CLASS_NAME), ctxs[i]);
    Value* expr_fn_args[] = { ctx_arg, row };
    CodegenAnyVal result = CodegenAnyVal::CreateCallWrapped(
        codegen, &builder, ctxs[i]->root()->type(), expr_fn, expr_fn_args, "result");
    Value* is_null = result.GetIsNull();

    // Set null-byte result
    Value* null_byte = builder.CreateZExt(is_null, codegen->GetType(TYPE_TINYINT));
    Value* llvm_null_byte_loc = builder.CreateGEP(NULL, cur_expr_values_null,
        codegen->GetIntConstant(TYPE_INT, i), "null_byte_loc");
    builder.CreateStore(null_byte, llvm_null_byte_loc);
    builder.CreateCondBr(is_null, null_block, not_null_block);

    // Null block
    builder.SetInsertPoint(null_block);
    if (!stores_nulls_) {
      // hash table doesn't store nulls, no reason to keep evaluating exprs
      builder.CreateRet(codegen->true_value());
    } else {
      CodegenAssignNullValue(codegen, &builder, llvm_loc, ctxs[i]->root()->type());
      builder.CreateBr(continue_block);
    }

    // Not null block
    builder.SetInsertPoint(not_null_block);
    result.ToNativePtr(llvm_loc);
    builder.CreateBr(continue_block);

    // Continue block
    builder.SetInsertPoint(continue_block);
    if (stores_nulls_) {
      // Update has_null
      PHINode* is_null_phi = builder.CreatePHI(codegen->boolean_type(), 2, "is_null_phi");
      is_null_phi->addIncoming(codegen->true_value(), null_block);
      is_null_phi->addIncoming(codegen->false_value(), not_null_block);
      has_null = builder.CreateOr(has_null, is_null_phi, "has_null");
    }
  }
  builder.CreateRet(has_null);

  *fn = codegen->FinalizeFunction(*fn);
  if (*fn == NULL) {
    return Status("Codegen'd HashTableCtx::EvalRow() function failed verification, "
        "see log");
  }
  return Status::OK();
}

// Codegen for hashing the current row.  In the case with both string and non-string data
// (group by int_col, string_col), the IR looks like:
// define i32 @HashCurrentRow(%"class.impala::HashTableCtx"* %this_ptr) #33 {
// entry:
//   %0 = load i8*, i8** inttoptr (i64 230325056 to i8**)
//   %1 = load i8*, i8** inttoptr (i64 230325064 to i8**)
//   %seed = call i32 @_ZNK6impala12HashTableCtx11GetHashSeedEv(
//           %"class.impala::HashTableCtx"* %this_ptr)
//   %hash = call i32 @CrcHash8(i8* %0, i32 8, i32 %seed)
//   %loc_addr = getelementptr i8, i8* %0, i32 8
//   %null_byte_loc = getelementptr i8, i8* %1, i32 1
//   %null_byte = load i8, i8* %null_byte_loc
//   %is_null = icmp ne i8 %null_byte, 0
//   br i1 %is_null, label %null, label %not_null
//
// null:                                             ; preds = %entry
//   %str_null = call i32 @CrcHash16(i8* %loc_addr, i32 16, i32 %hash)
//   br label %continue
//
// not_null:                                         ; preds = %entry
//   %str_val = bitcast i8* %loc_addr to %"struct.impala::StringValue"*
//   %2 = getelementptr inbounds %"struct.impala::StringValue",
//        %"struct.impala::StringValue"* %str_val, i32 0, i32 0
//   %3 = getelementptr inbounds %"struct.impala::StringValue",
//        %"struct.impala::StringValue"* %str_val, i32 0, i32 1
//   %ptr = load i8*, i8** %2
//   %len = load i32, i32* %3
//   %string_hash = call i32 @IrCrcHash(i8* %ptr, i32 %len, i32 %hash)
//   br label %continue
//
// continue:                                         ; preds = %not_null, %null
//   %hash_phi = phi i32 [ %string_hash, %not_null ], [ %str_null, %null ]
//   ret i32 %hash_phi
// }
Status HashTableCtx::CodegenHashCurrentRow(RuntimeState* state, bool use_murmur,
    Function** fn) {
  for (int i = 0; i < build_expr_ctxs_.size(); ++i) {
    // Disable codegen for CHAR
    if (build_expr_ctxs_[i]->root()->type().type == TYPE_CHAR) {
      return Status("HashTableCtx::CodegenHashCurrentRow(): CHAR NYI");
    }
  }

  LlvmCodeGen* codegen;
  RETURN_IF_ERROR(state->GetCodegen(&codegen));

  // Get types to generate function prototype
  Type* this_type = codegen->GetType(HashTableCtx::LLVM_CLASS_NAME);
  DCHECK(this_type != NULL);
  PointerType* this_ptr_type = PointerType::get(this_type, 0);
  PointerType* buffer_ptr_type = PointerType::get(codegen->ptr_type(), 0);

  LlvmCodeGen::FnPrototype prototype(codegen,
      (use_murmur ? "MurmurHashCurrentRow" : "HashCurrentRow"),
      codegen->GetType(TYPE_INT));
  prototype.AddArgument(LlvmCodeGen::NamedVariable("this_ptr", this_ptr_type));

  LLVMContext& context = codegen->context();
  LlvmCodeGen::LlvmBuilder builder(context);
  Value* this_arg;
  *fn = prototype.GeneratePrototype(&builder, &this_arg);

  // Load cur_expr_values_ into a LLVM pointer.
  Value* cur_expr_values_ptr =
      codegen->CastPtrToLlvmPtr(buffer_ptr_type, &expr_values_cache_.cur_expr_values_);
  Value* cur_expr_values = builder.CreateLoad(cur_expr_values_ptr);

  // Load cur_expr_values_null_ into a LLVM pointer.
  Value* cur_expr_values_null = NULL;
  if (stores_nulls_) {
    Value* cur_expr_values_null_ptr = codegen->CastPtrToLlvmPtr(
        buffer_ptr_type, &expr_values_cache_.cur_expr_values_null_);
    cur_expr_values_null = builder.CreateLoad(cur_expr_values_null_ptr);
  }

  // Call GetHashSeed() to get seeds_[level_]
  Function* get_hash_seed_fn =
      codegen->GetFunction(IRFunction::HASH_TABLE_GET_HASH_SEED, false);
  Value* seed = builder.CreateCall(get_hash_seed_fn, ArrayRef<Value*>({this_arg}),
      "seed");

  Value* hash_result = seed;
  const int var_result_offset = expr_values_cache_.var_result_offset();
  const int expr_values_bytes_per_row = expr_values_cache_.expr_values_bytes_per_row();
  if (var_result_offset == -1) {
    // No variable length slots, just hash what is in 'expr_expr_values_cache_'
    if (expr_values_bytes_per_row > 0) {
      Function* hash_fn = use_murmur ?
                          codegen->GetMurmurHashFunction(expr_values_bytes_per_row) :
                          codegen->GetHashFunction(expr_values_bytes_per_row);
      Value* len = codegen->GetIntConstant(TYPE_INT, expr_values_bytes_per_row);
      hash_result = builder.CreateCall(hash_fn,
          ArrayRef<Value*>({cur_expr_values, len, hash_result}), "hash");
    }
  } else {
    if (var_result_offset > 0) {
      Function* hash_fn = use_murmur ?
                          codegen->GetMurmurHashFunction(var_result_offset) :
                          codegen->GetHashFunction(var_result_offset);
      Value* len = codegen->GetIntConstant(TYPE_INT, var_result_offset);
      hash_result = builder.CreateCall(hash_fn,
          ArrayRef<Value*>({cur_expr_values, len, hash_result}), "hash");
    }

    // Hash string slots
    for (int i = 0; i < build_expr_ctxs_.size(); ++i) {
      if (build_expr_ctxs_[i]->root()->type().type != TYPE_STRING
          && build_expr_ctxs_[i]->root()->type().type != TYPE_VARCHAR) continue;

      BasicBlock* null_block = NULL;
      BasicBlock* not_null_block = NULL;
      BasicBlock* continue_block = NULL;
      Value* str_null_result = NULL;

      int offset = expr_values_cache_.expr_values_offsets(i);
      Value* llvm_loc = builder.CreateGEP(NULL, cur_expr_values,
          codegen->GetIntConstant(TYPE_INT, offset), "loc_addr");

      // If the hash table stores nulls, we need to check if the stringval
      // evaluated to NULL
      if (stores_nulls_) {
        null_block = BasicBlock::Create(context, "null", *fn);
        not_null_block = BasicBlock::Create(context, "not_null", *fn);
        continue_block = BasicBlock::Create(context, "continue", *fn);

        Value* llvm_null_byte_loc = builder.CreateGEP(NULL, cur_expr_values_null,
            codegen->GetIntConstant(TYPE_INT, i), "null_byte_loc");
        Value* null_byte = builder.CreateLoad(llvm_null_byte_loc, "null_byte");
        Value* is_null = builder.CreateICmpNE(null_byte,
            codegen->GetIntConstant(TYPE_TINYINT, 0), "is_null");
        builder.CreateCondBr(is_null, null_block, not_null_block);

        // For null, we just want to call the hash function on the portion of
        // the data
        builder.SetInsertPoint(null_block);
        Function* null_hash_fn = use_murmur ?
                                 codegen->GetMurmurHashFunction(sizeof(StringValue)) :
                                 codegen->GetHashFunction(sizeof(StringValue));
        Value* len = codegen->GetIntConstant(TYPE_INT, sizeof(StringValue));
        str_null_result = builder.CreateCall(null_hash_fn,
            ArrayRef<Value*>({llvm_loc, len, hash_result}), "str_null");
        builder.CreateBr(continue_block);

        builder.SetInsertPoint(not_null_block);
      }

      // Convert expr_values_buffer_ loc to llvm value
      Value* str_val = builder.CreatePointerCast(llvm_loc,
          codegen->GetPtrType(TYPE_STRING), "str_val");

      Value* ptr = builder.CreateStructGEP(NULL, str_val, 0);
      Value* len = builder.CreateStructGEP(NULL, str_val, 1);
      ptr = builder.CreateLoad(ptr, "ptr");
      len = builder.CreateLoad(len, "len");

      // Call hash(ptr, len, hash_result);
      Function* general_hash_fn = use_murmur ? codegen->GetMurmurHashFunction() :
                                  codegen->GetHashFunction();
      Value* string_hash_result = builder.CreateCall(general_hash_fn,
          ArrayRef<Value*>({ptr, len, hash_result}), "string_hash");

      if (stores_nulls_) {
        builder.CreateBr(continue_block);
        builder.SetInsertPoint(continue_block);
        // Use phi node to reconcile that we could have come from the string-null
        // path and string not null paths.
        PHINode* phi_node = builder.CreatePHI(codegen->GetType(TYPE_INT), 2, "hash_phi");
        phi_node->addIncoming(string_hash_result, not_null_block);
        phi_node->addIncoming(str_null_result, null_block);
        hash_result = phi_node;
      } else {
        hash_result = string_hash_result;
      }
    }
  }

  builder.CreateRet(hash_result);
  *fn = codegen->FinalizeFunction(*fn);
  if (*fn == NULL) {
    return Status("Codegen'd HashTableCtx::HashCurrentRow() function failed "
        "verification, see log");
  }
  return Status::OK();
}

// Codegen for HashTableCtx::Equals.  For a group by with (bigint, string),
// the IR looks like:
//
// define i1 @Equals(%"class.impala::HashTableCtx"* %this_ptr,
//                   %"class.impala::TupleRow"* %row) #33 {
// entry:
//   %0 = alloca { i64, i8* }
//   %1 = load i8*, i8** inttoptr (i64 230325056 to i8**)
//   %2 = load i8*, i8** inttoptr (i64 230325064 to i8**)
//   %result = call i64 @GetSlotRef(%"class.impala::ExprContext"* inttoptr
//             (i64 165557504 to %"class.impala::ExprContext"*),
//             %"class.impala::TupleRow"* %row)
//   %is_null = trunc i64 %result to i1
//   %null_byte_loc = getelementptr i8, i8* %2, i32 0
//   %3 = load i8, i8* %null_byte_loc
//   %4 = icmp ne i8 %3, 0
//   %loc = getelementptr i8, i8* %1, i32 0
//   %row_val = bitcast i8* %loc to i32*
//   br i1 %is_null, label %null, label %not_null
//
// false_block:                ; preds = %cmp9, %not_null2, %null1, %cmp, %not_null, %null
//   ret i1 false
//
// null:                                             ; preds = %entry
//   br i1 %4, label %continue, label %false_block
//
// not_null:                                         ; preds = %entry
//   br i1 %4, label %false_block, label %cmp
//
// continue:                                         ; preds = %cmp, %null
//   %result4 = call { i64, i8* } @GetSlotRef.2(%"class.impala::ExprContext"*
//              inttoptr (i64 165557696 to %"class.impala::ExprContext"*),
//              %"class.impala::TupleRow"* %row)
//   %5 = extractvalue { i64, i8* } %result4, 0
//   %is_null5 = trunc i64 %5 to i1
//   %null_byte_loc6 = getelementptr i8, i8* %2, i32 1
//   %6 = load i8, i8* %null_byte_loc6
//   %7 = icmp ne i8 %6, 0
//   %loc7 = getelementptr i8, i8* %1, i32 8
//   %row_val8 = bitcast i8* %loc7 to %"struct.impala::StringValue"*
//   br i1 %is_null5, label %null1, label %not_null2
//
// cmp:                                              ; preds = %not_null
//   %8 = load i32, i32* %row_val
//   %9 = ashr i64 %result, 32
//   %10 = trunc i64 %9 to i32
//   %cmp_raw = icmp eq i32 %10, %8
//   br i1 %cmp_raw, label %continue, label %false_block
//
// null1:                                            ; preds = %continue
//   br i1 %7, label %continue3, label %false_block
//
// not_null2:                                        ; preds = %continue
//   br i1 %7, label %false_block, label %cmp9
//
// continue3:                                        ; preds = %cmp9, %null1
//   ret i1 true
//
// cmp9:                                             ; preds = %not_null2
//   store { i64, i8* } %result4, { i64, i8* }* %0
//   %11 = bitcast { i64, i8* }* %0 to %"struct.impala_udf::StringVal"*
//   %cmp_raw10 = call i1 @_Z13StringValueEqRKN10impala_udf9StringValERKN6
//                impala11StringValueE(%"struct.impala_udf::StringVal"* %11,
//                %"struct.impala::StringValue"* %row_val8)
//   br i1 %cmp_raw10, label %continue3, label %false_block
// }
Status HashTableCtx::CodegenEquals(RuntimeState* state, bool force_null_equality,
    Function** fn) {
  for (int i = 0; i < build_expr_ctxs_.size(); ++i) {
    // Disable codegen for CHAR
    if (build_expr_ctxs_[i]->root()->type().type == TYPE_CHAR) {
      return Status("HashTableCtx::CodegenEquals(): CHAR NYI");
    }
  }

  LlvmCodeGen* codegen;
  RETURN_IF_ERROR(state->GetCodegen(&codegen));
  // Get types to generate function prototype
  Type* tuple_row_type = codegen->GetType(TupleRow::LLVM_CLASS_NAME);
  DCHECK(tuple_row_type != NULL);
  PointerType* tuple_row_ptr_type = PointerType::get(tuple_row_type, 0);

  Type* this_type = codegen->GetType(HashTableCtx::LLVM_CLASS_NAME);
  DCHECK(this_type != NULL);
  PointerType* this_ptr_type = PointerType::get(this_type, 0);
  PointerType* buffer_ptr_type = PointerType::get(codegen->ptr_type(), 0);
  LlvmCodeGen::FnPrototype prototype(codegen, "Equals", codegen->GetType(TYPE_BOOLEAN));
  prototype.AddArgument(LlvmCodeGen::NamedVariable("this_ptr", this_ptr_type));
  prototype.AddArgument(LlvmCodeGen::NamedVariable("row", tuple_row_ptr_type));

  LLVMContext& context = codegen->context();
  LlvmCodeGen::LlvmBuilder builder(context);
  Value* args[2];
  *fn = prototype.GeneratePrototype(&builder, args);
  Value* row = args[1];

  // Load cur_expr_values_ into a LLVM pointer.
  Value* cur_expr_values_ptr = codegen->CastPtrToLlvmPtr(buffer_ptr_type,
      &expr_values_cache_.cur_expr_values_);
  Value* cur_expr_values = builder.CreateLoad(cur_expr_values_ptr);

  // Load cur_expr_values_null_ into a LLVM pointer.
  Value* cur_expr_values_null_ptr = codegen->CastPtrToLlvmPtr(buffer_ptr_type,
      &expr_values_cache_.cur_expr_values_null_);
  Value* cur_expr_values_null = builder.CreateLoad(cur_expr_values_null_ptr);

  BasicBlock* false_block = BasicBlock::Create(context, "false_block", *fn);
  for (int i = 0; i < build_expr_ctxs_.size(); ++i) {
    BasicBlock* null_block = BasicBlock::Create(context, "null", *fn);
    BasicBlock* not_null_block = BasicBlock::Create(context, "not_null", *fn);
    BasicBlock* continue_block = BasicBlock::Create(context, "continue", *fn);

    // call GetValue on build_exprs[i]
    Function* expr_fn;
    Status status = build_expr_ctxs_[i]->root()->GetCodegendComputeFn(state, &expr_fn);
    if (!status.ok()) {
      (*fn)->eraseFromParent(); // deletes function
      *fn = NULL;
      return Status(Substitute("Problem with HashTableCtx::CodegenEquals: $0",
          status.GetDetail()));
    }

    Value* ctx_arg = codegen->CastPtrToLlvmPtr(
        codegen->GetPtrType(ExprContext::LLVM_CLASS_NAME), build_expr_ctxs_[i]);
    Value* expr_fn_args[] = { ctx_arg, row };
    CodegenAnyVal result = CodegenAnyVal::CreateCallWrapped(codegen, &builder,
        build_expr_ctxs_[i]->root()->type(), expr_fn, expr_fn_args, "result");
    Value* is_null = result.GetIsNull();

    // Determine if row is null (i.e. cur_expr_values_null_[i] == true). In
    // the case where the hash table does not store nulls, this is always false.
    Value* row_is_null = codegen->false_value();

    // We consider null values equal if we are comparing build rows or if the join
    // predicate is <=>
    if (force_null_equality || finds_nulls_[i]) {
      Value* llvm_null_byte_loc = builder.CreateGEP(NULL, cur_expr_values_null,
          codegen->GetIntConstant(TYPE_INT, i), "null_byte_loc");
      Value* null_byte = builder.CreateLoad(llvm_null_byte_loc);
      row_is_null = builder.CreateICmpNE(null_byte,
          codegen->GetIntConstant(TYPE_TINYINT, 0));
    }

    // Get llvm value for row_val from 'cur_expr_values_'
    int offset = expr_values_cache_.expr_values_offsets(i);
    Value* loc = builder.CreateGEP(NULL, cur_expr_values,
        codegen->GetIntConstant(TYPE_INT, offset), "loc");
    Value* row_val = builder.CreatePointerCast(loc,
        codegen->GetPtrType(build_expr_ctxs_[i]->root()->type()), "row_val");

    // Branch for GetValue() returning NULL
    builder.CreateCondBr(is_null, null_block, not_null_block);

    // Null block
    builder.SetInsertPoint(null_block);
    builder.CreateCondBr(row_is_null, continue_block, false_block);

    // Not-null block
    builder.SetInsertPoint(not_null_block);
    if (stores_nulls_) {
      BasicBlock* cmp_block = BasicBlock::Create(context, "cmp", *fn);
      // First need to compare that row expr[i] is not null
      builder.CreateCondBr(row_is_null, false_block, cmp_block);
      builder.SetInsertPoint(cmp_block);
    }
    // Check result == row_val
    Value* is_equal = result.EqToNativePtr(row_val);
    builder.CreateCondBr(is_equal, continue_block, false_block);

    builder.SetInsertPoint(continue_block);
  }
  builder.CreateRet(codegen->true_value());

  builder.SetInsertPoint(false_block);
  builder.CreateRet(codegen->false_value());

  *fn = codegen->FinalizeFunction(*fn);
  if (*fn == NULL) {
    return Status("Codegen'd HashTableCtx::Equals() function failed verification, "
        "see log");
  }
  return Status::OK();
}

Status HashTableCtx::ReplaceHashTableConstants(RuntimeState* state,
    bool stores_duplicates, int num_build_tuples, Function* fn,
    HashTableReplacedConstants* replacement_counts) {
  LlvmCodeGen* codegen;
  RETURN_IF_ERROR(state->GetCodegen(&codegen));

  replacement_counts->stores_nulls = codegen->ReplaceCallSitesWithBoolConst(
      fn, stores_nulls(), "stores_nulls");
  replacement_counts->finds_some_nulls = codegen->ReplaceCallSitesWithBoolConst(
      fn, finds_some_nulls(), "finds_some_nulls");
  replacement_counts->stores_tuples = codegen->ReplaceCallSitesWithBoolConst(
      fn, num_build_tuples == 1, "stores_tuples");
  replacement_counts->stores_duplicates = codegen->ReplaceCallSitesWithBoolConst(
      fn, stores_duplicates, "stores_duplicates");
  replacement_counts->quadratic_probing = codegen->ReplaceCallSitesWithBoolConst(
      fn, FLAGS_enable_quadratic_probing, "quadratic_probing");
  return Status::OK();
}
