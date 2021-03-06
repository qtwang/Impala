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

#include "exec/partitioned-aggregation-node.h"

#include <algorithm>
#include <math.h>
#include <set>
#include <sstream>
#include <gutil/strings/substitute.h>
#include <thrift/protocol/TDebugProtocol.h>

#include "codegen/codegen-anyval.h"
#include "codegen/llvm-codegen.h"
#include "exec/hash-table.inline.h"
#include "exprs/agg-fn-evaluator.h"
#include "exprs/anyval-util.h"
#include "exprs/expr.h"
#include "exprs/expr-context.h"
#include "exprs/slot-ref.h"
#include "runtime/buffered-tuple-stream.inline.h"
#include "runtime/descriptors.h"
#include "runtime/mem-pool.h"
#include "runtime/raw-value.h"
#include "runtime/row-batch.h"
#include "runtime/runtime-state.h"
#include "runtime/string-value.inline.h"
#include "runtime/tuple.h"
#include "runtime/tuple-row.h"
#include "udf/udf-internal.h"
#include "util/debug-util.h"
#include "util/runtime-profile.h"

#include "gen-cpp/Exprs_types.h"
#include "gen-cpp/PlanNodes_types.h"

#include "common/names.h"

using namespace impala;
using namespace llvm;
using namespace strings;

namespace impala {

const char* PartitionedAggregationNode::LLVM_CLASS_NAME =
    "class.impala::PartitionedAggregationNode";

/// The minimum reduction factor (input rows divided by output rows) to grow hash tables
/// in a streaming preaggregation, given that the hash tables are currently the given
/// size or above. The sizes roughly correspond to hash table sizes where the bucket
/// arrays will fit in  a cache level. Intuitively, we don't want the working set of the
/// aggregation to expand to the next level of cache unless we're reducing the input
/// enough to outweigh the increased memory latency we'll incur for each hash table
/// lookup.
///
/// Note that the current reduction achieved is not always a good estimate of the
/// final reduction. It may be biased either way depending on the ordering of the
/// input. If the input order is random, we will underestimate the final reduction
/// factor because the probability of a row having the same key as a previous row
/// increases as more input is processed.  If the input order is correlated with the
/// key, skew may bias the estimate. If high cardinality keys appear first, we
/// may overestimate and if low cardinality keys appear first, we underestimate.
/// To estimate the eventual reduction achieved, we estimate the final reduction
/// using the planner's estimated input cardinality and the assumption that input
/// is in a random order. This means that we assume that the reduction factor will
/// increase over time.
struct StreamingHtMinReductionEntry {
  // Use 'streaming_ht_min_reduction' if the total size of hash table bucket directories in
  // bytes is greater than this threshold.
  int min_ht_mem;
  // The minimum reduction factor to expand the hash tables.
  double streaming_ht_min_reduction;
};

// TODO: experimentally tune these values and also programmatically get the cache size
// of the machine that we're running on.
static const StreamingHtMinReductionEntry STREAMING_HT_MIN_REDUCTION[] = {
  // Expand up to L2 cache always.
  {0, 0.0},
  // Expand into L3 cache if we look like we're getting some reduction.
  {256 * 1024, 1.1},
  // Expand into main memory if we're getting a significant reduction.
  {2 * 1024 * 1024, 2.0},
};

static const int STREAMING_HT_MIN_REDUCTION_SIZE =
    sizeof(STREAMING_HT_MIN_REDUCTION) / sizeof(STREAMING_HT_MIN_REDUCTION[0]);

PartitionedAggregationNode::PartitionedAggregationNode(
    ObjectPool* pool, const TPlanNode& tnode, const DescriptorTbl& descs)
  : ExecNode(pool, tnode, descs),
    intermediate_tuple_id_(tnode.agg_node.intermediate_tuple_id),
    intermediate_tuple_desc_(NULL),
    output_tuple_id_(tnode.agg_node.output_tuple_id),
    output_tuple_desc_(NULL),
    needs_finalize_(tnode.agg_node.need_finalize),
    is_streaming_preagg_(tnode.agg_node.use_streaming_preaggregation),
    needs_serialize_(false),
    block_mgr_client_(NULL),
    output_partition_(NULL),
    process_batch_no_grouping_fn_(NULL),
    process_batch_fn_(NULL),
    process_batch_streaming_fn_(NULL),
    build_timer_(NULL),
    ht_resize_timer_(NULL),
    get_results_timer_(NULL),
    num_hash_buckets_(NULL),
    partitions_created_(NULL),
    max_partition_level_(NULL),
    num_row_repartitioned_(NULL),
    num_repartitions_(NULL),
    num_spilled_partitions_(NULL),
    largest_partition_percent_(NULL),
    streaming_timer_(NULL),
    num_passthrough_rows_(NULL),
    preagg_estimated_reduction_(NULL),
    preagg_streaming_ht_min_reduction_(NULL),
    estimated_input_cardinality_(tnode.agg_node.estimated_input_cardinality),
    singleton_output_tuple_(NULL),
    singleton_output_tuple_returned_(true),
    partition_eos_(false),
    child_eos_(false),
    partition_pool_(new ObjectPool()) {
  DCHECK_EQ(PARTITION_FANOUT, 1 << NUM_PARTITIONING_BITS);
  if (is_streaming_preagg_) {
    DCHECK(conjunct_ctxs_.empty()) << "Preaggs have no conjuncts";
    DCHECK(!tnode.agg_node.grouping_exprs.empty()) << "Streaming preaggs do grouping";
    DCHECK(limit_ == -1) << "Preaggs have no limits";
  }
}

Status PartitionedAggregationNode::Init(const TPlanNode& tnode, RuntimeState* state) {
  RETURN_IF_ERROR(ExecNode::Init(tnode, state));
  RETURN_IF_ERROR(
      Expr::CreateExprTrees(pool_, tnode.agg_node.grouping_exprs, &grouping_expr_ctxs_));
  for (int i = 0; i < tnode.agg_node.aggregate_functions.size(); ++i) {
    AggFnEvaluator* evaluator;
    RETURN_IF_ERROR(AggFnEvaluator::Create(
        pool_, tnode.agg_node.aggregate_functions[i], &evaluator));
    aggregate_evaluators_.push_back(evaluator);
  }
  return Status::OK();
}

Status PartitionedAggregationNode::Prepare(RuntimeState* state) {
  SCOPED_TIMER(runtime_profile_->total_time_counter());

  // Create the codegen object before preparing conjunct_ctxs_ and children_, so that any
  // ScalarFnCalls will use codegen.
  // TODO: this is brittle and hard to reason about, revisit
  if (state->codegen_enabled()) {
    LlvmCodeGen* codegen;
    RETURN_IF_ERROR(state->GetCodegen(&codegen));
  }

  RETURN_IF_ERROR(ExecNode::Prepare(state));
  state_ = state;

  mem_pool_.reset(new MemPool(mem_tracker()));
  agg_fn_pool_.reset(new MemPool(expr_mem_tracker()));

  ht_resize_timer_ = ADD_TIMER(runtime_profile(), "HTResizeTime");
  get_results_timer_ = ADD_TIMER(runtime_profile(), "GetResultsTime");
  num_hash_buckets_ =
      ADD_COUNTER(runtime_profile(), "HashBuckets", TUnit::UNIT);
  partitions_created_ =
      ADD_COUNTER(runtime_profile(), "PartitionsCreated", TUnit::UNIT);
  largest_partition_percent_ = runtime_profile()->AddHighWaterMarkCounter(
      "LargestPartitionPercent", TUnit::UNIT);
  if (is_streaming_preagg_) {
    AddRuntimeExecOption("Streaming Preaggregation");
    streaming_timer_ = ADD_TIMER(runtime_profile(), "StreamingTime");
    num_passthrough_rows_ =
        ADD_COUNTER(runtime_profile(), "RowsPassedThrough", TUnit::UNIT);
    preagg_estimated_reduction_ = ADD_COUNTER(
        runtime_profile(), "ReductionFactorEstimate", TUnit::DOUBLE_VALUE);
    preagg_streaming_ht_min_reduction_ = ADD_COUNTER(
        runtime_profile(), "ReductionFactorThresholdToExpand", TUnit::DOUBLE_VALUE);
  } else {
    build_timer_ = ADD_TIMER(runtime_profile(), "BuildTime");
    num_row_repartitioned_ =
        ADD_COUNTER(runtime_profile(), "RowsRepartitioned", TUnit::UNIT);
    num_repartitions_ =
        ADD_COUNTER(runtime_profile(), "NumRepartitions", TUnit::UNIT);
    num_spilled_partitions_ =
        ADD_COUNTER(runtime_profile(), "SpilledPartitions", TUnit::UNIT);
    max_partition_level_ = runtime_profile()->AddHighWaterMarkCounter(
        "MaxPartitionLevel", TUnit::UNIT);
  }

  intermediate_tuple_desc_ =
      state->desc_tbl().GetTupleDescriptor(intermediate_tuple_id_);
  output_tuple_desc_ = state->desc_tbl().GetTupleDescriptor(output_tuple_id_);
  DCHECK_EQ(intermediate_tuple_desc_->slots().size(),
        output_tuple_desc_->slots().size());

  RETURN_IF_ERROR(Expr::Prepare(grouping_expr_ctxs_, state, child(0)->row_desc(),
      expr_mem_tracker()));
  AddExprCtxsToFree(grouping_expr_ctxs_);

  // Construct build exprs from intermediate_agg_tuple_desc_
  for (int i = 0; i < grouping_expr_ctxs_.size(); ++i) {
    SlotDescriptor* desc = intermediate_tuple_desc_->slots()[i];
    DCHECK(desc->type().type == TYPE_NULL ||
        desc->type() == grouping_expr_ctxs_[i]->root()->type());
    // Hack to avoid TYPE_NULL SlotRefs.
    Expr* expr = desc->type().type != TYPE_NULL ?
        new SlotRef(desc) : new SlotRef(desc, TYPE_BOOLEAN);
    state->obj_pool()->Add(expr);
    build_expr_ctxs_.push_back(new ExprContext(expr));
    state->obj_pool()->Add(build_expr_ctxs_.back());
    if (expr->type().IsVarLenStringType()) {
      string_grouping_exprs_.push_back(i);
    }
  }
  // Construct a new row desc for preparing the build exprs because neither the child's
  // nor this node's output row desc may contain the intermediate tuple, e.g.,
  // in a single-node plan with an intermediate tuple different from the output tuple.
  intermediate_row_desc_.reset(new RowDescriptor(intermediate_tuple_desc_, false));
  RETURN_IF_ERROR(
      Expr::Prepare(build_expr_ctxs_, state, *intermediate_row_desc_,
                    expr_mem_tracker()));
  AddExprCtxsToFree(build_expr_ctxs_);

  int j = grouping_expr_ctxs_.size();
  for (int i = 0; i < aggregate_evaluators_.size(); ++i, ++j) {
    SlotDescriptor* intermediate_slot_desc = intermediate_tuple_desc_->slots()[j];
    SlotDescriptor* output_slot_desc = output_tuple_desc_->slots()[j];
    FunctionContext* agg_fn_ctx = NULL;
    RETURN_IF_ERROR(aggregate_evaluators_[i]->Prepare(state, child(0)->row_desc(),
        intermediate_slot_desc, output_slot_desc, agg_fn_pool_.get(), &agg_fn_ctx));
    agg_fn_ctxs_.push_back(agg_fn_ctx);
    state->obj_pool()->Add(agg_fn_ctx);
    needs_serialize_ |= aggregate_evaluators_[i]->SupportsSerialize();
  }

  if (grouping_expr_ctxs_.empty()) {
    // Create single output tuple; we need to output something even if our input is empty.
    singleton_output_tuple_ =
        ConstructSingletonOutputTuple(agg_fn_ctxs_, mem_pool_.get());
    // Check for failures during AggFnEvaluator::Init().
    RETURN_IF_ERROR(state_->GetQueryStatus());
    singleton_output_tuple_returned_ = false;
  } else {
    RETURN_IF_ERROR(HashTableCtx::Create(state, build_expr_ctxs_, grouping_expr_ctxs_,
        true, vector<bool>(build_expr_ctxs_.size(), true), state->fragment_hash_seed(),
        MAX_PARTITION_DEPTH, 1, mem_tracker(), &ht_ctx_));
    RETURN_IF_ERROR(state_->block_mgr()->RegisterClient(
        Substitute("PartitionedAggregationNode id=$0 ptr=$1", id_, this),
        MinRequiredBuffers(), true, mem_tracker(), state, &block_mgr_client_));
    RETURN_IF_ERROR(CreateHashPartitions(0));
  }

  // TODO: Is there a need to create the stream here? If memory reservations work we may
  // be able to create this stream lazily and only whenever we need to spill.
  if (!is_streaming_preagg_ && needs_serialize_ && block_mgr_client_ != NULL) {
    serialize_stream_.reset(new BufferedTupleStream(state, *intermediate_row_desc_,
        state->block_mgr(), block_mgr_client_, false /* use_initial_small_buffers */,
        false /* read_write */));
    RETURN_IF_ERROR(serialize_stream_->Init(id(), runtime_profile(), false));
    DCHECK(serialize_stream_->has_write_block());
  }

  bool codegen_enabled = false;
  Status codegen_status;
  if (state->codegen_enabled()) {
    codegen_status = is_streaming_preagg_ ? CodegenProcessBatchStreaming()
                                          : CodegenProcessBatch();
    codegen_enabled = codegen_status.ok();
  }
  AddCodegenExecOption(codegen_enabled, codegen_status);
  return Status::OK();
}

Status PartitionedAggregationNode::Open(RuntimeState* state) {
  SCOPED_TIMER(runtime_profile_->total_time_counter());
  RETURN_IF_ERROR(ExecNode::Open(state));

  RETURN_IF_ERROR(Expr::Open(grouping_expr_ctxs_, state));
  RETURN_IF_ERROR(Expr::Open(build_expr_ctxs_, state));

  DCHECK_EQ(aggregate_evaluators_.size(), agg_fn_ctxs_.size());
  for (int i = 0; i < aggregate_evaluators_.size(); ++i) {
    RETURN_IF_ERROR(aggregate_evaluators_[i]->Open(state, agg_fn_ctxs_[i]));
  }

  RETURN_IF_ERROR(children_[0]->Open(state));

  // Streaming preaggregations do all processing in GetNext().
  if (is_streaming_preagg_) return Status::OK();

  RowBatch batch(child(0)->row_desc(), state->batch_size(), mem_tracker());
  // Read all the rows from the child and process them.
  bool eos = false;
  do {
    RETURN_IF_CANCELLED(state);
    RETURN_IF_ERROR(QueryMaintenance(state));
    RETURN_IF_ERROR(children_[0]->GetNext(state, &batch, &eos));

    if (UNLIKELY(VLOG_ROW_IS_ON)) {
      for (int i = 0; i < batch.num_rows(); ++i) {
        TupleRow* row = batch.GetRow(i);
        VLOG_ROW << "input row: " << PrintRow(row, children_[0]->row_desc());
      }
    }

    TPrefetchMode::type prefetch_mode = state->query_options().prefetch_mode;
    SCOPED_TIMER(build_timer_);
    if (grouping_expr_ctxs_.empty()) {
      if (process_batch_no_grouping_fn_ != NULL) {
        RETURN_IF_ERROR(process_batch_no_grouping_fn_(this, &batch));
      } else {
        RETURN_IF_ERROR(ProcessBatchNoGrouping(&batch));
      }
    } else {
      // There is grouping, so we will do partitioned aggregation.
      if (process_batch_fn_ != NULL) {
        RETURN_IF_ERROR(process_batch_fn_(this, &batch, prefetch_mode, ht_ctx_.get()));
      } else {
        RETURN_IF_ERROR(ProcessBatch<false>(&batch, prefetch_mode, ht_ctx_.get()));
      }
    }
    batch.Reset();
  } while (!eos);

  // The child can be closed at this point in most cases because we have consumed all of
  // the input from the child and transfered ownership of the resources we need. The
  // exception is if we are inside a subplan expecting to call Open()/GetNext() on the
  // child again,
  if (!IsInSubplan()) child(0)->Close(state);
  child_eos_ = true;

  // Done consuming child(0)'s input. Move all the partitions in hash_partitions_
  // to spilled_partitions_ or aggregated_partitions_. We'll finish the processing in
  // GetNext().
  if (!grouping_expr_ctxs_.empty()) {
    RETURN_IF_ERROR(MoveHashPartitions(child(0)->rows_returned()));
  }
  return Status::OK();
}

Status PartitionedAggregationNode::GetNext(RuntimeState* state, RowBatch* row_batch,
    bool* eos) {
  int first_row_idx = row_batch->num_rows();
  RETURN_IF_ERROR(GetNextInternal(state, row_batch, eos));
  RETURN_IF_ERROR(HandleOutputStrings(row_batch, first_row_idx));
  return Status::OK();
}

Status PartitionedAggregationNode::HandleOutputStrings(RowBatch* row_batch,
    int first_row_idx) {
  if (!needs_finalize_ && !needs_serialize_) return Status::OK();
  // String data returned by Serialize() or Finalize() is from local expr allocations in
  // the agg function contexts, and will be freed on the next GetNext() call by
  // FreeLocalAllocations(). The data either needs to be copied out or sent up the plan
  // tree via MarkNeedToReturn(). (See IMPALA-3311)
  for (int i = 0; i < aggregate_evaluators_.size(); ++i) {
    const SlotDescriptor* slot_desc = aggregate_evaluators_[i]->output_slot_desc();
    DCHECK(!slot_desc->type().IsCollectionType()) << "producing collections NYI";
    if (!slot_desc->type().IsVarLenStringType()) continue;
    if (IsInSubplan()) {
      // Copy string data to the row batch's pool. This is more efficient than
      // MarkNeedToReturn() in a subplan since we are likely producing many small batches.
      RETURN_IF_ERROR(CopyStringData(slot_desc, row_batch, first_row_idx,
              row_batch->tuple_data_pool()));
    } else {
      row_batch->MarkNeedToReturn();
      break;
    }
  }
  return Status::OK();
}

Status PartitionedAggregationNode::CopyStringData(const SlotDescriptor* slot_desc,
    RowBatch* row_batch, int first_row_idx, MemPool* pool) {
  DCHECK(slot_desc->type().IsVarLenStringType());
  DCHECK_EQ(row_batch->row_desc().tuple_descriptors().size(), 1);
  FOREACH_ROW(row_batch, first_row_idx, batch_iter) {
    Tuple* tuple = batch_iter.Get()->GetTuple(0);
    StringValue* sv = reinterpret_cast<StringValue*>(
        tuple->GetSlot(slot_desc->tuple_offset()));
    if (sv == NULL || sv->len == 0) continue;
    char* new_ptr = reinterpret_cast<char*>(pool->TryAllocate(sv->len));
    if (new_ptr == NULL) {
      Status s = Status::MemLimitExceeded();
      s.AddDetail(Substitute("Cannot perform aggregation at node with id $0."
              " Failed to allocate $1 output bytes.", id_, sv->len));
      state_->SetMemLimitExceeded();
      return s;
    }
    memcpy(new_ptr, sv->ptr, sv->len);
    sv->ptr = new_ptr;
  }
  return Status::OK();
}

Status PartitionedAggregationNode::GetNextInternal(RuntimeState* state,
    RowBatch* row_batch, bool* eos) {
  SCOPED_TIMER(runtime_profile_->total_time_counter());
  RETURN_IF_ERROR(ExecDebugAction(TExecNodePhase::GETNEXT, state));
  RETURN_IF_CANCELLED(state);
  RETURN_IF_ERROR(QueryMaintenance(state));

  if (ReachedLimit()) {
    *eos = true;
    return Status::OK();
  }

  if (grouping_expr_ctxs_.empty()) {
    // There was no grouping, so evaluate the conjuncts and return the single result row.
    // We allow calling GetNext() after eos, so don't return this row again.
    if (!singleton_output_tuple_returned_) GetSingletonOutput(row_batch);
    singleton_output_tuple_returned_ = true;
    *eos = true;
    return Status::OK();
  }

  if (!child_eos_) {
    // For streaming preaggregations, we process rows from the child as we go.
    DCHECK(is_streaming_preagg_);
    RETURN_IF_ERROR(GetRowsStreaming(state, row_batch));
  } else if (!partition_eos_) {
    RETURN_IF_ERROR(GetRowsFromPartition(state, row_batch));
  }

  *eos = partition_eos_ && child_eos_;
  COUNTER_SET(rows_returned_counter_, num_rows_returned_);
  return Status::OK();
}

void PartitionedAggregationNode::GetSingletonOutput(RowBatch* row_batch) {
  DCHECK(grouping_expr_ctxs_.empty());
  int row_idx = row_batch->AddRow();
  TupleRow* row = row_batch->GetRow(row_idx);
  Tuple* output_tuple = GetOutputTuple(
      agg_fn_ctxs_, singleton_output_tuple_, row_batch->tuple_data_pool());
  row->SetTuple(0, output_tuple);
  if (ExecNode::EvalConjuncts(&conjunct_ctxs_[0], conjunct_ctxs_.size(), row)) {
    row_batch->CommitLastRow();
    ++num_rows_returned_;
    COUNTER_SET(rows_returned_counter_, num_rows_returned_);
  }
  // Keep the current chunk to amortize the memory allocation over a series
  // of Reset()/Open()/GetNext()* calls.
  row_batch->tuple_data_pool()->AcquireData(mem_pool_.get(), true);
  // This node no longer owns the memory for singleton_output_tuple_.
  singleton_output_tuple_ = NULL;
}

Status PartitionedAggregationNode::GetRowsFromPartition(RuntimeState* state,
    RowBatch* row_batch) {
  DCHECK(!row_batch->AtCapacity());
  if (output_iterator_.AtEnd()) {
    // Done with this partition, move onto the next one.
    if (output_partition_ != NULL) {
      output_partition_->Close(false);
      output_partition_ = NULL;
    }
    if (aggregated_partitions_.empty() && spilled_partitions_.empty()) {
      // No more partitions, all done.
      partition_eos_ = true;
      return Status::OK();
    }
    // Process next partition.
    RETURN_IF_ERROR(NextPartition());
    DCHECK(output_partition_ != NULL);
  }

  SCOPED_TIMER(get_results_timer_);
  int count = 0;
  const int N = BitUtil::RoundUpToPowerOfTwo(state->batch_size());
  // Keeping returning rows from the current partition.
  while (!output_iterator_.AtEnd()) {
    // This loop can go on for a long time if the conjuncts are very selective. Do query
    // maintenance every N iterations.
    if ((count++ & (N - 1)) == 0) {
      RETURN_IF_CANCELLED(state);
      RETURN_IF_ERROR(QueryMaintenance(state));
    }

    int row_idx = row_batch->AddRow();
    TupleRow* row = row_batch->GetRow(row_idx);
    Tuple* intermediate_tuple = output_iterator_.GetTuple();
    Tuple* output_tuple = GetOutputTuple(
        output_partition_->agg_fn_ctxs, intermediate_tuple, row_batch->tuple_data_pool());
    output_iterator_.Next();
    row->SetTuple(0, output_tuple);
    if (ExecNode::EvalConjuncts(&conjunct_ctxs_[0], conjunct_ctxs_.size(), row)) {
      row_batch->CommitLastRow();
      ++num_rows_returned_;
      if (ReachedLimit() || row_batch->AtCapacity()) {
        break;
      }
    }
  }

  COUNTER_SET(rows_returned_counter_, num_rows_returned_);
  partition_eos_ = ReachedLimit();
  if (output_iterator_.AtEnd()) row_batch->MarkNeedToReturn();

  return Status::OK();
}

Status PartitionedAggregationNode::GetRowsStreaming(RuntimeState* state,
    RowBatch* out_batch) {
  DCHECK(!child_eos_);
  DCHECK(is_streaming_preagg_);

  if (child_batch_ == NULL) {
    child_batch_.reset(new RowBatch(child(0)->row_desc(), state->batch_size(),
        mem_tracker()));
  }

  do {
    DCHECK_EQ(out_batch->num_rows(), 0);
    RETURN_IF_CANCELLED(state);
    RETURN_IF_ERROR(QueryMaintenance(state));

    RETURN_IF_ERROR(child(0)->GetNext(state, child_batch_.get(), &child_eos_));

    SCOPED_TIMER(streaming_timer_);

    int remaining_capacity[PARTITION_FANOUT];
    bool ht_needs_expansion = false;
    for (int i = 0; i < PARTITION_FANOUT; ++i) {
      HashTable* hash_tbl = GetHashTable(i);
      DCHECK(hash_tbl != NULL);
      remaining_capacity[i] = hash_tbl->NumInsertsBeforeResize();
      ht_needs_expansion |= remaining_capacity[i] < child_batch_->num_rows();
    }

    // Stop expanding hash tables if we're not reducing the input sufficiently. As our
    // hash tables expand out of each level of cache hierarchy, every hash table lookup
    // will take longer. We also may not be able to expand hash tables because of memory
    // pressure. In this case HashTable::CheckAndResize() will fail. In either case we
    // should always use the remaining space in the hash table to avoid wasting memory.
    if (ht_needs_expansion && ShouldExpandPreaggHashTables()) {
      for (int i = 0; i < PARTITION_FANOUT; ++i) {
        HashTable* ht = GetHashTable(i);
        if (remaining_capacity[i] < child_batch_->num_rows()) {
          SCOPED_TIMER(ht_resize_timer_);
          if (ht->CheckAndResize(child_batch_->num_rows(), ht_ctx_.get())) {
            remaining_capacity[i] = ht->NumInsertsBeforeResize();
          }
        }
      }
    }

    TPrefetchMode::type prefetch_mode = state->query_options().prefetch_mode;
    if (process_batch_streaming_fn_ != NULL) {
      RETURN_IF_ERROR(process_batch_streaming_fn_(this, needs_serialize_, prefetch_mode,
          child_batch_.get(), out_batch, ht_ctx_.get(), remaining_capacity));
    } else {
      RETURN_IF_ERROR(ProcessBatchStreaming(needs_serialize_, prefetch_mode,
          child_batch_.get(), out_batch, ht_ctx_.get(), remaining_capacity ));
    }

    child_batch_->Reset(); // All rows from child_batch_ were processed.
  } while (out_batch->num_rows() == 0 && !child_eos_);

  if (child_eos_) {
    child(0)->Close(state);
    child_batch_.reset();
    MoveHashPartitions(child(0)->rows_returned());
  }

  num_rows_returned_ += out_batch->num_rows();
  COUNTER_SET(num_passthrough_rows_, num_rows_returned_);
  return Status::OK();
}

bool PartitionedAggregationNode::ShouldExpandPreaggHashTables() const {
  int64_t ht_mem = 0;
  int64_t ht_rows = 0;
  for (int i = 0; i < PARTITION_FANOUT; ++i) {
    HashTable* ht = hash_partitions_[i]->hash_tbl.get();
    ht_mem += ht->CurrentMemSize();
    ht_rows += ht->size();
  }

  // Need some rows in tables to have valid statistics.
  if (ht_rows == 0) return true;

  // Find the appropriate reduction factor in our table for the current hash table sizes.
  int cache_level = 0;
  while (cache_level + 1 < STREAMING_HT_MIN_REDUCTION_SIZE &&
      ht_mem >= STREAMING_HT_MIN_REDUCTION[cache_level + 1].min_ht_mem) {
    ++cache_level;
  }

  // Compare the number of rows in the hash table with the number of input rows that
  // were aggregated into it. Exclude passed through rows from this calculation since
  // they were not in hash tables.
  const int64_t input_rows = children_[0]->rows_returned();
  const int64_t aggregated_input_rows = input_rows - num_rows_returned_;
  const int64_t expected_input_rows = estimated_input_cardinality_ - num_rows_returned_;
  double current_reduction = static_cast<double>(aggregated_input_rows) / ht_rows;

  // TODO: workaround for IMPALA-2490: subplan node rows_returned counter may be
  // inaccurate, which could lead to a divide by zero below.
  if (aggregated_input_rows <= 0) return true;

  // Extrapolate the current reduction factor (r) using the formula
  // R = 1 + (N / n) * (r - 1), where R is the reduction factor over the full input data
  // set, N is the number of input rows, excluding passed-through rows, and n is the
  // number of rows inserted or merged into the hash tables. This is a very rough
  // approximation but is good enough to be useful.
  // TODO: consider collecting more statistics to better estimate reduction.
  double estimated_reduction = aggregated_input_rows >= expected_input_rows
      ? current_reduction
      : 1 + (expected_input_rows / aggregated_input_rows) * (current_reduction - 1);
  double min_reduction =
    STREAMING_HT_MIN_REDUCTION[cache_level].streaming_ht_min_reduction;

  COUNTER_SET(preagg_estimated_reduction_, estimated_reduction);
  COUNTER_SET(preagg_streaming_ht_min_reduction_, min_reduction);
  return estimated_reduction > min_reduction;
}

void PartitionedAggregationNode::CleanupHashTbl(
    const vector<FunctionContext*>& agg_fn_ctxs, HashTable::Iterator it) {
  if (!needs_finalize_ && !needs_serialize_) return;

  // Iterate through the remaining rows in the hash table and call Serialize/Finalize on
  // them in order to free any memory allocated by UDAs.
  if (needs_finalize_) {
    // Finalize() requires a dst tuple but we don't actually need the result,
    // so allocate a single dummy tuple to avoid accumulating memory.
    Tuple* dummy_dst = NULL;
    dummy_dst = Tuple::Create(output_tuple_desc_->byte_size(), mem_pool_.get());
    while (!it.AtEnd()) {
      Tuple* tuple = it.GetTuple();
      AggFnEvaluator::Finalize(aggregate_evaluators_, agg_fn_ctxs, tuple, dummy_dst);
      it.Next();
    }
  } else {
    while (!it.AtEnd()) {
      Tuple* tuple = it.GetTuple();
      AggFnEvaluator::Serialize(aggregate_evaluators_, agg_fn_ctxs, tuple);
      it.Next();
    }
  }
}

Status PartitionedAggregationNode::Reset(RuntimeState* state) {
  DCHECK(!is_streaming_preagg_) << "Cannot reset preaggregation";
  if (grouping_expr_ctxs_.empty()) {
    // Re-create the single output tuple for this non-grouping agg.
    singleton_output_tuple_ =
        ConstructSingletonOutputTuple(agg_fn_ctxs_, mem_pool_.get());
    // Check for failures during AggFnEvaluator::Init().
    RETURN_IF_ERROR(state_->GetQueryStatus());
    singleton_output_tuple_returned_ = false;
  } else {
    child_eos_ = false;
    partition_eos_ = false;
    // Reset the HT and the partitions for this grouping agg.
    ht_ctx_->set_level(0);
    ClosePartitions();
    RETURN_IF_ERROR(CreateHashPartitions(0));
  }
  return ExecNode::Reset(state);
}

void PartitionedAggregationNode::Close(RuntimeState* state) {
  if (is_closed()) return;

  if (!singleton_output_tuple_returned_) {
    DCHECK_EQ(agg_fn_ctxs_.size(), aggregate_evaluators_.size());
    GetOutputTuple(agg_fn_ctxs_, singleton_output_tuple_, mem_pool_.get());
  }

  // Iterate through the remaining rows in the hash table and call Serialize/Finalize on
  // them in order to free any memory allocated by UDAs
  if (output_partition_ != NULL) {
    CleanupHashTbl(output_partition_->agg_fn_ctxs, output_iterator_);
    output_partition_->Close(false);
  }

  ClosePartitions();

  child_batch_.reset();
  for (int i = 0; i < aggregate_evaluators_.size(); ++i) {
    aggregate_evaluators_[i]->Close(state);
  }
  for (int i = 0; i < agg_fn_ctxs_.size(); ++i) {
    agg_fn_ctxs_[i]->impl()->Close();
  }
  if (agg_fn_pool_.get() != NULL) agg_fn_pool_->FreeAll();
  if (mem_pool_.get() != NULL) mem_pool_->FreeAll();
  if (ht_ctx_.get() != NULL) ht_ctx_->Close();
  if (serialize_stream_.get() != NULL) serialize_stream_->Close();

  if (block_mgr_client_ != NULL) {
    state->block_mgr()->ClearReservations(block_mgr_client_);
  }

  Expr::Close(grouping_expr_ctxs_, state);
  Expr::Close(build_expr_ctxs_, state);
  ExecNode::Close(state);
}

PartitionedAggregationNode::Partition::~Partition() {
  DCHECK(is_closed);
}

Status PartitionedAggregationNode::Partition::InitStreams() {
  agg_fn_pool.reset(new MemPool(parent->expr_mem_tracker()));
  DCHECK_EQ(agg_fn_ctxs.size(), 0);
  for (int i = 0; i < parent->agg_fn_ctxs_.size(); ++i) {
    agg_fn_ctxs.push_back(parent->agg_fn_ctxs_[i]->impl()->Clone(agg_fn_pool.get()));
    parent->partition_pool_->Add(agg_fn_ctxs[i]);
  }

  // Varlen aggregate function results are stored outside of aggregated_row_stream because
  // BufferedTupleStream doesn't support relocating varlen data stored in the stream.
  auto agg_slot = parent->intermediate_tuple_desc_->slots().begin() +
      parent->grouping_expr_ctxs_.size();
  set<SlotId> external_varlen_slots;
  for (; agg_slot != parent->intermediate_tuple_desc_->slots().end(); ++agg_slot) {
    if ((*agg_slot)->type().IsVarLenStringType()) {
      external_varlen_slots.insert((*agg_slot)->id());
    }
  }

  aggregated_row_stream.reset(new BufferedTupleStream(parent->state_,
      *parent->intermediate_row_desc_, parent->state_->block_mgr(),
      parent->block_mgr_client_, true /* use_initial_small_buffers */,
      false /* read_write */, external_varlen_slots));
  RETURN_IF_ERROR(
      aggregated_row_stream->Init(parent->id(), parent->runtime_profile(), true));

  if (!parent->is_streaming_preagg_) {
    unaggregated_row_stream.reset(new BufferedTupleStream(parent->state_,
        parent->child(0)->row_desc(), parent->state_->block_mgr(),
      parent->block_mgr_client_, true /* use_initial_small_buffers */,
        false /* read_write */));
    // This stream is only used to spill, no need to ever have this pinned.
    RETURN_IF_ERROR(unaggregated_row_stream->Init(parent->id(), parent->runtime_profile(),
        false));
    DCHECK(unaggregated_row_stream->has_write_block());
  }
  return Status::OK();
}

bool PartitionedAggregationNode::Partition::InitHashTable() {
  DCHECK(hash_tbl.get() == NULL);
  // We use the upper PARTITION_FANOUT num bits to pick the partition so only the
  // remaining bits can be used for the hash table.
  // TODO: we could switch to 64 bit hashes and then we don't need a max size.
  // It might be reasonable to limit individual hash table size for other reasons
  // though. Always start with small buffers.
  // TODO: How many buckets? We currently use a default value, 1024.
  static const int64_t PAGG_DEFAULT_HASH_TABLE_SZ = 1024;
  hash_tbl.reset(HashTable::Create(parent->state_, parent->block_mgr_client_,
      false, 1, NULL, 1L << (32 - NUM_PARTITIONING_BITS),
      PAGG_DEFAULT_HASH_TABLE_SZ));
  return hash_tbl->Init();
}

Status PartitionedAggregationNode::Partition::SerializeStreamForSpilling() {
  DCHECK(!parent->is_streaming_preagg_);
  if (parent->needs_serialize_ && aggregated_row_stream->num_rows() != 0) {
    // We need to do a lot more work in this case. This step effectively does a merge
    // aggregation in this node. We need to serialize the intermediates, spill the
    // intermediates and then feed them into the aggregate function's merge step.
    // This is often used when the intermediate is a string type, meaning the current
    // (before serialization) in-memory layout is not the on-disk block layout.
    // The disk layout does not support mutable rows. We need to rewrite the stream
    // into the on disk format.
    // TODO: if it happens to not be a string, we could serialize in place. This is
    // a future optimization since it is very unlikely to have a serialize phase
    // for those UDAs.
    DCHECK(parent->serialize_stream_.get() != NULL);
    DCHECK(!parent->serialize_stream_->is_pinned());
    DCHECK(parent->serialize_stream_->has_write_block());

    const vector<AggFnEvaluator*>& evaluators = parent->aggregate_evaluators_;

    // Serialize and copy the spilled partition's stream into the new stream.
    Status status = Status::OK();
    bool failed_to_add = false;
    BufferedTupleStream* new_stream = parent->serialize_stream_.get();
    HashTable::Iterator it = hash_tbl->Begin(parent->ht_ctx_.get());
    while (!it.AtEnd()) {
      Tuple* tuple = it.GetTuple();
      it.Next();
      AggFnEvaluator::Serialize(evaluators, agg_fn_ctxs, tuple);
      if (UNLIKELY(!new_stream->AddRow(reinterpret_cast<TupleRow*>(&tuple), &status))) {
        failed_to_add = true;
        break;
      }
    }

    // Even if we can't add to new_stream, finish up processing this agg stream to make
    // clean up easier (someone has to finalize this stream and we don't want to remember
    // where we are).
    if (failed_to_add) {
      parent->CleanupHashTbl(agg_fn_ctxs, it);
      hash_tbl->Close();
      hash_tbl.reset();
      aggregated_row_stream->Close();
      RETURN_IF_ERROR(status);
      return parent->state_->block_mgr()->MemLimitTooLowError(parent->block_mgr_client_,
          parent->id());
    }
    DCHECK(status.ok());

    aggregated_row_stream->Close();
    aggregated_row_stream.swap(parent->serialize_stream_);
    // Recreate the serialize_stream (and reserve 1 buffer) now in preparation for
    // when we need to spill again. We need to have this available before we need
    // to spill to make sure it is available. This should be acquirable since we just
    // freed at least one buffer from this partition's (old) aggregated_row_stream.
    parent->serialize_stream_.reset(new BufferedTupleStream(parent->state_,
        *parent->intermediate_row_desc_, parent->state_->block_mgr(),
        parent->block_mgr_client_, false /* use_initial_small_buffers */,
        false /* read_write */));
    status = parent->serialize_stream_->Init(parent->id(), parent->runtime_profile(),
        false);
    if (!status.ok()) {
      hash_tbl->Close();
      hash_tbl.reset();
      return status;
    }
    DCHECK(parent->serialize_stream_->has_write_block());
  }
  return Status::OK();
}

Status PartitionedAggregationNode::Partition::Spill() {
  DCHECK(!is_closed);
  DCHECK(!is_spilled());

  RETURN_IF_ERROR(SerializeStreamForSpilling());

  // Free the in-memory result data.
  for (int i = 0; i < agg_fn_ctxs.size(); ++i) {
    agg_fn_ctxs[i]->impl()->Close();
  }

  if (agg_fn_pool.get() != NULL) {
    agg_fn_pool->FreeAll();
    agg_fn_pool.reset();
  }

  hash_tbl->Close();
  hash_tbl.reset();

  // Try to switch both streams to IO-sized buffers to avoid allocating small buffers
  // for spilled partition.
  bool got_buffer = true;
  if (aggregated_row_stream->using_small_buffers()) {
    RETURN_IF_ERROR(aggregated_row_stream->SwitchToIoBuffers(&got_buffer));
  }
  // Unpin the stream as soon as possible to increase the chances that the
  // SwitchToIoBuffers() call below will succeed.  If we're repartitioning, rows that
  // were already aggregated (rows from the input partition's aggregated stream) will
  // need to be added to this hash partition's aggregated stream, so we need to leave
  // the write block pinned.
  // TODO: when not repartitioning, don't leave the write block pinned.
  DCHECK(!got_buffer || aggregated_row_stream->has_write_block())
      << aggregated_row_stream->DebugString();
  RETURN_IF_ERROR(aggregated_row_stream->UnpinStream(false));

  if (got_buffer && unaggregated_row_stream->using_small_buffers()) {
    RETURN_IF_ERROR(unaggregated_row_stream->SwitchToIoBuffers(&got_buffer));
  }
  if (!got_buffer) {
    // We'll try again to get the buffers when the stream fills up the small buffers.
    VLOG_QUERY << "Not enough memory to switch to IO-sized buffer for partition "
               << this << " of agg=" << parent->id_ << " agg small buffers="
               << aggregated_row_stream->using_small_buffers()
               << " unagg small buffers="
               << unaggregated_row_stream->using_small_buffers();
    VLOG_FILE << GetStackTrace();
  }

  COUNTER_ADD(parent->num_spilled_partitions_, 1);
  if (parent->num_spilled_partitions_->value() == 1) {
    parent->AddRuntimeExecOption("Spilled");
  }
  return Status::OK();
}

void PartitionedAggregationNode::Partition::Close(bool finalize_rows) {
  if (is_closed) return;
  is_closed = true;
  if (aggregated_row_stream.get() != NULL) {
    if (finalize_rows && hash_tbl.get() != NULL) {
      // We need to walk all the rows and Finalize them here so the UDA gets a chance
      // to cleanup. If the hash table is gone (meaning this was spilled), the rows
      // should have been finalized/serialized in Spill().
      parent->CleanupHashTbl(agg_fn_ctxs, hash_tbl->Begin(parent->ht_ctx_.get()));
    }
    aggregated_row_stream->Close();
  }
  if (hash_tbl.get() != NULL) hash_tbl->Close();
  if (unaggregated_row_stream.get() != NULL) unaggregated_row_stream->Close();

  for (int i = 0; i < agg_fn_ctxs.size(); ++i) {
    agg_fn_ctxs[i]->impl()->Close();
  }
  if (agg_fn_pool.get() != NULL) agg_fn_pool->FreeAll();
}

Tuple* PartitionedAggregationNode::ConstructSingletonOutputTuple(
    const vector<FunctionContext*>& agg_fn_ctxs, MemPool* pool) {
  DCHECK(grouping_expr_ctxs_.empty());
  Tuple* output_tuple = Tuple::Create(intermediate_tuple_desc_->byte_size(), pool);
  InitAggSlots(agg_fn_ctxs, output_tuple);
  return output_tuple;
}

Tuple* PartitionedAggregationNode::ConstructIntermediateTuple(
    const vector<FunctionContext*>& agg_fn_ctxs, MemPool* pool, Status* status) {
  const int fixed_size = intermediate_tuple_desc_->byte_size();
  const int varlen_size = GroupingExprsVarlenSize();
  uint8_t* tuple_data = pool->TryAllocate(fixed_size + varlen_size);
  if (tuple_data == NULL) {
    *status = Status::MemLimitExceeded();
    status->AddDetail(Substitute("Cannot perform aggregation at node with id $0."
        " Failed to allocate $1 bytes for intermediate tuple.", id_,
        fixed_size + varlen_size));
    state_->SetMemLimitExceeded();
    return NULL;
  }
  memset(tuple_data, 0, fixed_size);
  Tuple* intermediate_tuple = reinterpret_cast<Tuple*>(tuple_data);
  uint8_t* varlen_data = tuple_data + fixed_size;
  CopyGroupingValues(intermediate_tuple, varlen_data, varlen_size);
  InitAggSlots(agg_fn_ctxs, intermediate_tuple);
  return intermediate_tuple;
}

Tuple* PartitionedAggregationNode::ConstructIntermediateTuple(
    const vector<FunctionContext*>& agg_fn_ctxs,
    BufferedTupleStream* stream, Status* status) {
  DCHECK(stream != NULL && status != NULL);
  // Allocate space for the entire tuple in the stream.
  const int fixed_size = intermediate_tuple_desc_->byte_size();
  const int varlen_size = GroupingExprsVarlenSize();
  uint8_t* varlen_buffer;
  uint8_t* fixed_buffer = stream->AllocateRow(fixed_size, varlen_size, &varlen_buffer,
      status);
  if (UNLIKELY(fixed_buffer == NULL)) {
    if (!status->ok() || !stream->using_small_buffers()) return NULL;
    // IMPALA-2352: Make a best effort to switch to IO buffers and re-allocate.
    // If SwitchToIoBuffers() fails the caller of this function can try to free
    // some space, e.g. through spilling, and re-attempt to allocate space for
    // this row.
    bool got_buffer;
    *status = stream->SwitchToIoBuffers(&got_buffer);
    if (!status->ok() || !got_buffer) return NULL;
    fixed_buffer = stream->AllocateRow(fixed_size, varlen_size, &varlen_buffer, status);
    if (fixed_buffer == NULL) return NULL;
  }

  Tuple* intermediate_tuple = reinterpret_cast<Tuple*>(fixed_buffer);
  intermediate_tuple->Init(fixed_size);
  CopyGroupingValues(intermediate_tuple, varlen_buffer, varlen_size);
  InitAggSlots(agg_fn_ctxs, intermediate_tuple);
  return intermediate_tuple;
}

int PartitionedAggregationNode::GroupingExprsVarlenSize() {
  int varlen_size = 0;
  // TODO: The hash table could compute this as it hashes.
  for (int expr_idx: string_grouping_exprs_) {
    StringValue* sv = reinterpret_cast<StringValue*>(ht_ctx_->ExprValue(expr_idx));
    // Avoid branching by multiplying length by null bit.
    varlen_size += sv->len * !ht_ctx_->ExprValueNull(expr_idx);
  }
  return varlen_size;
}

// TODO: codegen this function.
void PartitionedAggregationNode::CopyGroupingValues(Tuple* intermediate_tuple,
    uint8_t* buffer, int varlen_size) {
  // Copy over all grouping slots (the variable length data is copied below).
  for (int i = 0; i < grouping_expr_ctxs_.size(); ++i) {
    SlotDescriptor* slot_desc = intermediate_tuple_desc_->slots()[i];
    if (ht_ctx_->ExprValueNull(i)) {
      intermediate_tuple->SetNull(slot_desc->null_indicator_offset());
    } else {
      void* src = ht_ctx_->ExprValue(i);
      void* dst = intermediate_tuple->GetSlot(slot_desc->tuple_offset());
      memcpy(dst, src, slot_desc->slot_size());
    }
  }

  for (int expr_idx: string_grouping_exprs_) {
    if (ht_ctx_->ExprValueNull(expr_idx)) continue;

    SlotDescriptor* slot_desc = intermediate_tuple_desc_->slots()[expr_idx];
    // ptr and len were already copied to the fixed-len part of string value
    StringValue* sv = reinterpret_cast<StringValue*>(
        intermediate_tuple->GetSlot(slot_desc->tuple_offset()));
    memcpy(buffer, sv->ptr, sv->len);
    sv->ptr = reinterpret_cast<char*>(buffer);
    buffer += sv->len;
  }
}

// TODO: codegen this function.
void PartitionedAggregationNode::InitAggSlots(
    const vector<FunctionContext*>& agg_fn_ctxs, Tuple* intermediate_tuple) {
  vector<SlotDescriptor*>::const_iterator slot_desc =
      intermediate_tuple_desc_->slots().begin() + grouping_expr_ctxs_.size();
  for (int i = 0; i < aggregate_evaluators_.size(); ++i, ++slot_desc) {
    AggFnEvaluator* evaluator = aggregate_evaluators_[i];
    evaluator->Init(agg_fn_ctxs[i], intermediate_tuple);
    // Codegen specific path for min/max.
    // To minimize branching on the UpdateTuple path, initialize the result value
    // so that UpdateTuple doesn't have to check if the aggregation
    // dst slot is null.
    // TODO: remove when we don't use the irbuilder for codegen here.  This optimization
    // will no longer be necessary when all aggregates are implemented with the UDA
    // interface.
    if ((*slot_desc)->type().type != TYPE_STRING &&
        (*slot_desc)->type().type != TYPE_VARCHAR &&
        (*slot_desc)->type().type != TYPE_TIMESTAMP &&
        (*slot_desc)->type().type != TYPE_CHAR) {
      ExprValue default_value;
      void* default_value_ptr = NULL;
      switch (evaluator->agg_op()) {
        case AggFnEvaluator::MIN:
          default_value_ptr = default_value.SetToMax((*slot_desc)->type());
          RawValue::Write(default_value_ptr, intermediate_tuple, *slot_desc, NULL);
          break;
        case AggFnEvaluator::MAX:
          default_value_ptr = default_value.SetToMin((*slot_desc)->type());
          RawValue::Write(default_value_ptr, intermediate_tuple, *slot_desc, NULL);
          break;
        default:
          break;
      }
    }
  }
}

void PartitionedAggregationNode::UpdateTuple(FunctionContext** agg_fn_ctxs,
    Tuple* tuple, TupleRow* row, bool is_merge) {
  DCHECK(tuple != NULL || aggregate_evaluators_.empty());
  for (int i = 0; i < aggregate_evaluators_.size(); ++i) {
    if (is_merge) {
      aggregate_evaluators_[i]->Merge(agg_fn_ctxs[i], row->GetTuple(0), tuple);
    } else {
      aggregate_evaluators_[i]->Add(agg_fn_ctxs[i], row, tuple);
    }
  }
}

Tuple* PartitionedAggregationNode::GetOutputTuple(
    const vector<FunctionContext*>& agg_fn_ctxs, Tuple* tuple, MemPool* pool) {
  DCHECK(tuple != NULL || aggregate_evaluators_.empty()) << tuple;
  Tuple* dst = tuple;
  if (needs_finalize_ && intermediate_tuple_id_ != output_tuple_id_) {
    dst = Tuple::Create(output_tuple_desc_->byte_size(), pool);
  }
  if (needs_finalize_) {
    AggFnEvaluator::Finalize(aggregate_evaluators_, agg_fn_ctxs, tuple, dst);
  } else {
    AggFnEvaluator::Serialize(aggregate_evaluators_, agg_fn_ctxs, tuple);
  }
  // Copy grouping values from tuple to dst.
  // TODO: Codegen this.
  if (dst != tuple) {
    int num_grouping_slots = grouping_expr_ctxs_.size();
    for (int i = 0; i < num_grouping_slots; ++i) {
      SlotDescriptor* src_slot_desc = intermediate_tuple_desc_->slots()[i];
      SlotDescriptor* dst_slot_desc = output_tuple_desc_->slots()[i];
      bool src_slot_null = tuple->IsNull(src_slot_desc->null_indicator_offset());
      void* src_slot = NULL;
      if (!src_slot_null) src_slot = tuple->GetSlot(src_slot_desc->tuple_offset());
      RawValue::Write(src_slot, dst, dst_slot_desc, NULL);
    }
  }
  return dst;
}

Status PartitionedAggregationNode::AppendSpilledRow(BufferedTupleStream* stream,
    TupleRow* row) {
  DCHECK(stream != NULL);
  DCHECK(!stream->is_pinned());
  DCHECK(stream->has_write_block());
  if (LIKELY(stream->AddRow(row, &process_batch_status_))) return Status::OK();

  // Adding fails iff either we hit an error or haven't switched to I/O buffers.
  RETURN_IF_ERROR(process_batch_status_);
  while (true) {
    bool got_buffer;
    RETURN_IF_ERROR(stream->SwitchToIoBuffers(&got_buffer));
    if (got_buffer) break;
    RETURN_IF_ERROR(SpillPartition());
  }

  // Adding the row should succeed after the I/O buffer switch.
  if (stream->AddRow(row, &process_batch_status_)) return Status::OK();
  DCHECK(!process_batch_status_.ok());
  return process_batch_status_;
}

void PartitionedAggregationNode::DebugString(int indentation_level,
    stringstream* out) const {
  *out << string(indentation_level * 2, ' ');
  *out << "PartitionedAggregationNode("
       << "intermediate_tuple_id=" << intermediate_tuple_id_
       << " output_tuple_id=" << output_tuple_id_
       << " needs_finalize=" << needs_finalize_
       << " grouping_exprs=" << Expr::DebugString(grouping_expr_ctxs_)
       << " agg_exprs=" << AggFnEvaluator::DebugString(aggregate_evaluators_);
  ExecNode::DebugString(indentation_level, out);
  *out << ")";
}

Status PartitionedAggregationNode::CreateHashPartitions(int level) {
  if (is_streaming_preagg_) DCHECK_EQ(level, 0);
  if (level >= MAX_PARTITION_DEPTH) {
    return state_->SetMemLimitExceeded(ErrorMsg(
        TErrorCode::PARTITIONED_AGG_MAX_PARTITION_DEPTH, id_, MAX_PARTITION_DEPTH));
  }
  ht_ctx_->set_level(level);

  DCHECK(hash_partitions_.empty());
  for (int i = 0; i < PARTITION_FANOUT; ++i) {
    Partition* new_partition = new Partition(this, level);
    DCHECK(new_partition != NULL);
    hash_partitions_.push_back(partition_pool_->Add(new_partition));
    RETURN_IF_ERROR(new_partition->InitStreams());
    hash_tbls_[i] = NULL;
  }
  if (!is_streaming_preagg_) {
    DCHECK_GT(state_->block_mgr()->num_reserved_buffers_remaining(block_mgr_client_), 0);
  }

  // Now that all the streams are reserved (meaning we have enough memory to execute
  // the algorithm), allocate the hash tables. These can fail and we can still continue.
  for (int i = 0; i < PARTITION_FANOUT; ++i) {
    if (!hash_partitions_[i]->InitHashTable()) {
      // We don't spill on preaggregations. If we have so little memory that we can't
      // allocate small hash tables, the mem limit is just too low.
      if (is_streaming_preagg_) {
        Status status = Status::MemLimitExceeded();
        status.AddDetail(Substitute("Cannot perform aggregation at node with id $0."
              " Failed to initialize hash table in preaggregation. The memory limit"
              " is too low to execute the query.", id_));
        state_->SetMemLimitExceeded();
        return status;
      }
      RETURN_IF_ERROR(hash_partitions_[i]->Spill());
    }
    hash_tbls_[i] = hash_partitions_[i]->hash_tbl.get();
  }

  COUNTER_ADD(partitions_created_, hash_partitions_.size());
  if (!is_streaming_preagg_) {
    COUNTER_SET(max_partition_level_, level);
  }
  return Status::OK();
}

Status PartitionedAggregationNode::CheckAndResizeHashPartitions(int num_rows,
    const HashTableCtx* ht_ctx) {
  DCHECK(!is_streaming_preagg_);
  for (int i = 0; i < PARTITION_FANOUT; ++i) {
    Partition* partition = hash_partitions_[i];
    while (!partition->is_spilled()) {
      {
        SCOPED_TIMER(ht_resize_timer_);
        if (partition->hash_tbl->CheckAndResize(num_rows, ht_ctx)) break;
      }
      RETURN_IF_ERROR(SpillPartition());
    }
  }
  return Status::OK();
}

int64_t PartitionedAggregationNode::LargestSpilledPartition() const {
  int64_t max_rows = 0;
  for (int i = 0; i < hash_partitions_.size(); ++i) {
    Partition* partition = hash_partitions_[i];
    if (partition->is_closed || !partition->is_spilled()) continue;
    int64_t rows = partition->aggregated_row_stream->num_rows() +
        partition->unaggregated_row_stream->num_rows();
    if (rows > max_rows) max_rows = rows;
  }
  return max_rows;
}

Status PartitionedAggregationNode::NextPartition() {
  DCHECK(output_partition_ == NULL);

  // Keep looping until we get to a partition that fits in memory.
  Partition* partition = NULL;
  while (true) {
    partition = NULL;
    // First return partitions that are fully aggregated (and in memory).
    if (!aggregated_partitions_.empty()) {
      partition = aggregated_partitions_.front();
      DCHECK(!partition->is_spilled());
      aggregated_partitions_.pop_front();
      break;
    }

    if (partition == NULL) {
      DCHECK(!spilled_partitions_.empty());
      DCHECK(!is_streaming_preagg_);
      DCHECK_EQ(state_->block_mgr()->num_pinned_buffers(block_mgr_client_),
          needs_serialize_ ? 1 : 0);

      // TODO: we can probably do better than just picking the first partition. We
      // can base this on the amount written to disk, etc.
      partition = spilled_partitions_.front();
      DCHECK(partition->is_spilled());

      // Create the new hash partitions to repartition into.
      // TODO: we don't need to repartition here. We are now working on 1 / FANOUT
      // of the input so it's reasonably likely it can fit. We should look at this
      // partitions size and just do the aggregation if it fits in memory.
      RETURN_IF_ERROR(CreateHashPartitions(partition->level + 1));
      COUNTER_ADD(num_repartitions_, 1);

      // Rows in this partition could have been spilled into two streams, depending
      // on if it is an aggregated intermediate, or an unaggregated row.
      // Note: we must process the aggregated rows first to save a hash table lookup
      // in ProcessBatch().
      RETURN_IF_ERROR(ProcessStream<true>(partition->aggregated_row_stream.get()));
      RETURN_IF_ERROR(ProcessStream<false>(partition->unaggregated_row_stream.get()));

      COUNTER_ADD(num_row_repartitioned_, partition->aggregated_row_stream->num_rows());
      COUNTER_ADD(num_row_repartitioned_,
          partition->unaggregated_row_stream->num_rows());

      partition->Close(false);
      spilled_partitions_.pop_front();

      // Done processing this partition. Move the new partitions into
      // spilled_partitions_/aggregated_partitions_.
      int64_t num_input_rows = partition->aggregated_row_stream->num_rows() +
          partition->unaggregated_row_stream->num_rows();

      // Check if there was any reduction in the size of partitions after repartitioning.
      int64_t largest_partition = LargestSpilledPartition();
      DCHECK_GE(num_input_rows, largest_partition) << "Cannot have a partition with "
          "more rows than the input";
      if (num_input_rows == largest_partition) {
        Status status = Status::MemLimitExceeded();
        status.AddDetail(Substitute("Cannot perform aggregation at node with id $0. "
            "Repartitioning did not reduce the size of a spilled partition. "
            "Repartitioning level $1. Number of rows $2.",
            id_, partition->level + 1, num_input_rows));
        state_->SetMemLimitExceeded();
        return status;
      }
      RETURN_IF_ERROR(MoveHashPartitions(num_input_rows));
    }
  }

  DCHECK(partition->hash_tbl.get() != NULL);
  DCHECK(partition->aggregated_row_stream->is_pinned());

  output_partition_ = partition;
  output_iterator_ = output_partition_->hash_tbl->Begin(ht_ctx_.get());
  COUNTER_ADD(num_hash_buckets_, output_partition_->hash_tbl->num_buckets());
  return Status::OK();
}

template<bool AGGREGATED_ROWS>
Status PartitionedAggregationNode::ProcessStream(BufferedTupleStream* input_stream) {
  DCHECK(!is_streaming_preagg_);
  if (input_stream->num_rows() > 0) {
    while (true) {
      bool got_buffer = false;
      RETURN_IF_ERROR(input_stream->PrepareForRead(true, &got_buffer));
      if (got_buffer) break;
      // Did not have a buffer to read the input stream. Spill and try again.
      RETURN_IF_ERROR(SpillPartition());
    }

    TPrefetchMode::type prefetch_mode = state_->query_options().prefetch_mode;
    bool eos = false;
    RowBatch batch(AGGREGATED_ROWS ? *intermediate_row_desc_ : children_[0]->row_desc(),
                   state_->batch_size(), mem_tracker());
    do {
      RETURN_IF_ERROR(input_stream->GetNext(&batch, &eos));
      RETURN_IF_ERROR(
          ProcessBatch<AGGREGATED_ROWS>(&batch, prefetch_mode, ht_ctx_.get()));
      RETURN_IF_ERROR(state_->GetQueryStatus());
      FreeLocalAllocations();
      batch.Reset();
    } while (!eos);
  }
  input_stream->Close();
  return Status::OK();
}

Status PartitionedAggregationNode::SpillPartition() {
  int64_t max_freed_mem = 0;
  int partition_idx = -1;

  // Iterate over the partitions and pick the largest partition that is not spilled.
  for (int i = 0; i < hash_partitions_.size(); ++i) {
    if (hash_partitions_[i]->is_closed) continue;
    if (hash_partitions_[i]->is_spilled()) continue;
    // Pass 'true' because we need to keep the write block pinned. See Partition::Spill().
    int64_t mem = hash_partitions_[i]->aggregated_row_stream->bytes_in_mem(true);
    mem += hash_partitions_[i]->hash_tbl->ByteSize();
    mem += hash_partitions_[i]->agg_fn_pool->total_reserved_bytes();
    DCHECK_GT(mem, 0); // At least the hash table buckets should occupy memory.
    if (mem > max_freed_mem) {
      max_freed_mem = mem;
      partition_idx = i;
    }
  }
  if (partition_idx == -1) {
    // Could not find a partition to spill. This means the mem limit was just too low.
    return state_->block_mgr()->MemLimitTooLowError(block_mgr_client_, id());
  }

  hash_tbls_[partition_idx] = NULL;
  return hash_partitions_[partition_idx]->Spill();
}

Status PartitionedAggregationNode::MoveHashPartitions(int64_t num_input_rows) {
  DCHECK(!hash_partitions_.empty());
  stringstream ss;
  ss << "PA(node_id=" << id() << ") partitioned(level="
     << hash_partitions_[0]->level << ") "
     << num_input_rows << " rows into:" << endl;
  for (int i = 0; i < hash_partitions_.size(); ++i) {
    Partition* partition = hash_partitions_[i];
    int64_t aggregated_rows = partition->aggregated_row_stream->num_rows();
    int64_t unaggregated_rows = 0;
    if (partition->unaggregated_row_stream != NULL) {
      unaggregated_rows = partition->unaggregated_row_stream->num_rows();
    }
    double total_rows = aggregated_rows + unaggregated_rows;
    double percent = total_rows * 100 / num_input_rows;
    ss << "  " << i << " "  << (partition->is_spilled() ? "spilled" : "not spilled")
       << " (fraction=" << fixed << setprecision(2) << percent << "%)" << endl
       << "    #aggregated rows:" << aggregated_rows << endl
       << "    #unaggregated rows: " << unaggregated_rows << endl;

    // TODO: update counters to support doubles.
    COUNTER_SET(largest_partition_percent_, static_cast<int64_t>(percent));

    if (total_rows == 0) {
      partition->Close(false);
    } else if (partition->is_spilled()) {
      DCHECK(partition->hash_tbl.get() == NULL);
      // We need to unpin all the spilled partitions to make room to allocate new
      // hash_partitions_ when we repartition the spilled partitions.
      // TODO: we only need to do this when we have memory pressure. This might be
      // okay though since the block mgr should only write these to disk if there
      // is memory pressure.
      RETURN_IF_ERROR(partition->aggregated_row_stream->UnpinStream(true));
      RETURN_IF_ERROR(partition->unaggregated_row_stream->UnpinStream(true));

      // Push new created partitions at the front. This means a depth first walk
      // (more finely partitioned partitions are processed first). This allows us
      // to delete blocks earlier and bottom out the recursion earlier.
      spilled_partitions_.push_front(partition);
    } else {
      aggregated_partitions_.push_back(partition);
    }

  }
  VLOG(2) << ss.str();
  hash_partitions_.clear();
  return Status::OK();
}

void PartitionedAggregationNode::ClosePartitions() {
  for (int i = 0; i < hash_partitions_.size(); ++i) {
    hash_partitions_[i]->Close(true);
  }
  for (list<Partition*>::iterator it = aggregated_partitions_.begin();
      it != aggregated_partitions_.end(); ++it) {
    (*it)->Close(true);
  }
  for (list<Partition*>::iterator it = spilled_partitions_.begin();
      it != spilled_partitions_.end(); ++it) {
    (*it)->Close(true);
  }
  aggregated_partitions_.clear();
  spilled_partitions_.clear();
  hash_partitions_.clear();
  memset(hash_tbls_, 0, sizeof(hash_tbls_));
  partition_pool_->Clear();
}

Status PartitionedAggregationNode::QueryMaintenance(RuntimeState* state) {
  for (int i = 0; i < aggregate_evaluators_.size(); ++i) {
    ExprContext::FreeLocalAllocations(aggregate_evaluators_[i]->input_expr_ctxs());
  }
  ExprContext::FreeLocalAllocations(agg_fn_ctxs_);
  for (int i = 0; i < hash_partitions_.size(); ++i) {
    ExprContext::FreeLocalAllocations(hash_partitions_[i]->agg_fn_ctxs);
  }
  return ExecNode::QueryMaintenance(state);
}

// IR Generation for updating a single aggregation slot. Signature is:
// void UpdateSlot(FunctionContext* fn_ctx, AggTuple* agg_tuple, char** row)
//
// The IR for sum(double_col) is:
// define void @UpdateSlot(%"class.impala_udf::FunctionContext"* %fn_ctx,
//                         { i8, double }* %agg_tuple,
//                         %"class.impala::TupleRow"* %row) #20 {
// entry:
//   %src = call { i8, double } @GetSlotRef(%"class.impala::ExprContext"* inttoptr
//     (i64 128241264 to %"class.impala::ExprContext"*), %"class.impala::TupleRow"* %row)
//   %0 = extractvalue { i8, double } %src, 0
//   %is_null = trunc i8 %0 to i1
//   br i1 %is_null, label %ret, label %src_not_null
//
// src_not_null:                                     ; preds = %entry
//   %dst_slot_ptr = getelementptr inbounds { i8, double }* %agg_tuple, i32 0, i32 1
//   call void @SetNotNull({ i8, double }* %agg_tuple)
//   %dst_val = load double* %dst_slot_ptr
//   %val = extractvalue { i8, double } %src, 1
//   %1 = fadd double %dst_val, %val
//   store double %1, double* %dst_slot_ptr
//   br label %ret
//
// ret:                                              ; preds = %src_not_null, %entry
//   ret void
// }
//
// The IR for ndv(double_col) is:
// define void @UpdateSlot(%"class.impala_udf::FunctionContext"* %fn_ctx,
//                         { i8, %"struct.impala::StringValue" }* %agg_tuple,
//                         %"class.impala::TupleRow"* %row) #20 {
// entry:
//   %dst_lowered_ptr = alloca { i64, i8* }
//   %src_lowered_ptr = alloca { i8, double }
//   %src = call { i8, double } @GetSlotRef(%"class.impala::ExprContext"* inttoptr
//     (i64 120530832 to %"class.impala::ExprContext"*), %"class.impala::TupleRow"* %row)
//   %0 = extractvalue { i8, double } %src, 0
//   %is_null = trunc i8 %0 to i1
//   br i1 %is_null, label %ret, label %src_not_null
//
// src_not_null:                                     ; preds = %entry
//   %dst_slot_ptr = getelementptr inbounds
//     { i8, %"struct.impala::StringValue" }* %agg_tuple, i32 0, i32 1
//   call void @SetNotNull({ i8, %"struct.impala::StringValue" }* %agg_tuple)
//   %dst_val = load %"struct.impala::StringValue"* %dst_slot_ptr
//   store { i8, double } %src, { i8, double }* %src_lowered_ptr
//   %src_unlowered_ptr = bitcast { i8, double }* %src_lowered_ptr
//                        to %"struct.impala_udf::DoubleVal"*
//   %ptr = extractvalue %"struct.impala::StringValue" %dst_val, 0
//   %dst_stringval = insertvalue { i64, i8* } zeroinitializer, i8* %ptr, 1
//   %len = extractvalue %"struct.impala::StringValue" %dst_val, 1
//   %1 = extractvalue { i64, i8* } %dst_stringval, 0
//   %2 = zext i32 %len to i64
//   %3 = shl i64 %2, 32
//   %4 = and i64 %1, 4294967295
//   %5 = or i64 %4, %3
//   %dst_stringval1 = insertvalue { i64, i8* } %dst_stringval, i64 %5, 0
//   store { i64, i8* } %dst_stringval1, { i64, i8* }* %dst_lowered_ptr
//   %dst_unlowered_ptr = bitcast { i64, i8* }* %dst_lowered_ptr
//                        to %"struct.impala_udf::StringVal"*
//   call void @HllUpdate(%"class.impala_udf::FunctionContext"* %fn_ctx,
//                        %"struct.impala_udf::DoubleVal"* %src_unlowered_ptr,
//                        %"struct.impala_udf::StringVal"* %dst_unlowered_ptr)
//   %anyval_result = load { i64, i8* }* %dst_lowered_ptr
//   %6 = extractvalue { i64, i8* } %anyval_result, 1
//   %7 = insertvalue %"struct.impala::StringValue" zeroinitializer, i8* %6, 0
//   %8 = extractvalue { i64, i8* } %anyval_result, 0
//   %9 = ashr i64 %8, 32
//   %10 = trunc i64 %9 to i32
//   %11 = insertvalue %"struct.impala::StringValue" %7, i32 %10, 1
//   store %"struct.impala::StringValue" %11, %"struct.impala::StringValue"* %dst_slot_ptr
//   br label %ret
//
// ret:                                              ; preds = %src_not_null, %entry
//   ret void
// }
Status PartitionedAggregationNode::CodegenUpdateSlot(
    AggFnEvaluator* evaluator, SlotDescriptor* slot_desc, Function** fn) {
  LlvmCodeGen* codegen;
  RETURN_IF_ERROR(state_->GetCodegen(&codegen));

  DCHECK_EQ(evaluator->input_expr_ctxs().size(), 1);
  ExprContext* input_expr_ctx = evaluator->input_expr_ctxs()[0];
  Expr* input_expr = input_expr_ctx->root();

  // TODO: implement timestamp
  if (input_expr->type().type == TYPE_TIMESTAMP &&
      evaluator->agg_op() != AggFnEvaluator::AVG) {
    return Status("PartitionedAggregationNode::CodegenUpdateSlot(): timestamp input type "
        "NYI");
  }

  Function* agg_expr_fn;
  RETURN_IF_ERROR(input_expr->GetCodegendComputeFn(state_, &agg_expr_fn));

  PointerType* fn_ctx_type =
      codegen->GetPtrType(FunctionContextImpl::LLVM_FUNCTIONCONTEXT_NAME);
  StructType* tuple_struct = intermediate_tuple_desc_->GetLlvmStruct(codegen);
  if (tuple_struct == NULL) {
    return Status("PartitionedAggregationNode::CodegenUpdateSlot(): failed to generate "
        "intermediate tuple desc");
  }
  PointerType* tuple_ptr_type = PointerType::get(tuple_struct, 0);
  PointerType* tuple_row_ptr_type = codegen->GetPtrType(TupleRow::LLVM_CLASS_NAME);

  // Create UpdateSlot prototype
  LlvmCodeGen::FnPrototype prototype(codegen, "UpdateSlot", codegen->void_type());
  prototype.AddArgument(LlvmCodeGen::NamedVariable("fn_ctx", fn_ctx_type));
  prototype.AddArgument(LlvmCodeGen::NamedVariable("agg_tuple", tuple_ptr_type));
  prototype.AddArgument(LlvmCodeGen::NamedVariable("row", tuple_row_ptr_type));

  LlvmCodeGen::LlvmBuilder builder(codegen->context());
  Value* args[3];
  *fn = prototype.GeneratePrototype(&builder, &args[0]);
  Value* fn_ctx_arg = args[0];
  Value* agg_tuple_arg = args[1];
  Value* row_arg = args[2];

  BasicBlock* src_not_null_block =
      BasicBlock::Create(codegen->context(), "src_not_null", *fn);
  BasicBlock* ret_block = BasicBlock::Create(codegen->context(), "ret", *fn);

  // Call expr function to get src slot value
  Value* expr_ctx = codegen->CastPtrToLlvmPtr(
      codegen->GetPtrType(ExprContext::LLVM_CLASS_NAME), input_expr_ctx);
  Value* agg_expr_fn_args[] = { expr_ctx, row_arg };
  CodegenAnyVal src = CodegenAnyVal::CreateCallWrapped(
      codegen, &builder, input_expr->type(), agg_expr_fn, agg_expr_fn_args, "src");

  Value* src_is_null = src.GetIsNull();
  builder.CreateCondBr(src_is_null, ret_block, src_not_null_block);

  // Src slot is not null, update dst_slot
  builder.SetInsertPoint(src_not_null_block);
  Value* dst_ptr = builder.CreateStructGEP(NULL, agg_tuple_arg, slot_desc->llvm_field_idx(),
      "dst_slot_ptr");
  Value* result = NULL;

  if (slot_desc->is_nullable()) {
    // Dst is NULL, just update dst slot to src slot and clear null bit
    Function* clear_null_fn = slot_desc->GetUpdateNullFn(codegen, false);
    builder.CreateCall(clear_null_fn, ArrayRef<Value*>({agg_tuple_arg}));
  }

  // Update the slot
  Value* dst_value = builder.CreateLoad(dst_ptr, "dst_val");
  switch (evaluator->agg_op()) {
    case AggFnEvaluator::COUNT:
      if (evaluator->is_merge()) {
        result = builder.CreateAdd(dst_value, src.GetVal(), "count_sum");
      } else {
        result = builder.CreateAdd(dst_value,
            codegen->GetIntConstant(TYPE_BIGINT, 1), "count_inc");
      }
      break;
    case AggFnEvaluator::MIN: {
      Function* min_fn = codegen->CodegenMinMax(slot_desc->type(), true);
      Value* min_args[] = { dst_value, src.GetVal() };
      result = builder.CreateCall(min_fn, min_args, "min_value");
      break;
    }
    case AggFnEvaluator::MAX: {
      Function* max_fn = codegen->CodegenMinMax(slot_desc->type(), false);
      Value* max_args[] = { dst_value, src.GetVal() };
      result = builder.CreateCall(max_fn, max_args, "max_value");
      break;
    }
    case AggFnEvaluator::SUM:
      if (slot_desc->type().type != TYPE_DECIMAL) {
        if (slot_desc->type().type == TYPE_FLOAT ||
            slot_desc->type().type == TYPE_DOUBLE) {
          result = builder.CreateFAdd(dst_value, src.GetVal());
        } else {
          result = builder.CreateAdd(dst_value, src.GetVal());
        }
        break;
      }
      DCHECK_EQ(slot_desc->type().type, TYPE_DECIMAL);
      // Fall through to xcompiled case
    case AggFnEvaluator::AVG:
    case AggFnEvaluator::NDV: {
      // Get xcompiled update/merge function from IR module
      const string& symbol = evaluator->is_merge() ?
                             evaluator->merge_symbol() : evaluator->update_symbol();
      const ColumnType& dst_type = evaluator->intermediate_type();
      Function* ir_fn = codegen->module()->getFunction(symbol);
      DCHECK(ir_fn != NULL);

      // Clone and replace constants.
      ir_fn = codegen->CloneFunction(ir_fn);
      vector<FunctionContext::TypeDesc> arg_types;
      arg_types.push_back(AnyValUtil::ColumnTypeToTypeDesc(input_expr->type()));
      Expr::InlineConstants(AnyValUtil::ColumnTypeToTypeDesc(dst_type), arg_types,
          codegen, ir_fn);

      // Create pointer to src to pass to ir_fn. We must use the unlowered type.
      Value* src_lowered_ptr = codegen->CreateEntryBlockAlloca(
          *fn, LlvmCodeGen::NamedVariable("src_lowered_ptr", src.value()->getType()));
      builder.CreateStore(src.value(), src_lowered_ptr);
      Type* unlowered_ptr_type =
          CodegenAnyVal::GetUnloweredPtrType(codegen, input_expr->type());
      Value* src_unlowered_ptr =
          builder.CreateBitCast(src_lowered_ptr, unlowered_ptr_type, "src_unlowered_ptr");

      // Create intermediate argument 'dst' from 'dst_value'
      CodegenAnyVal dst = CodegenAnyVal::GetNonNullVal(
          codegen, &builder, dst_type, "dst");
      dst.SetFromRawValue(dst_value);
      // Create pointer to dst to pass to ir_fn. We must use the unlowered type.
      Value* dst_lowered_ptr = codegen->CreateEntryBlockAlloca(
          *fn, LlvmCodeGen::NamedVariable("dst_lowered_ptr", dst.value()->getType()));
      builder.CreateStore(dst.value(), dst_lowered_ptr);
      unlowered_ptr_type = CodegenAnyVal::GetUnloweredPtrType(codegen, dst_type);
      Value* dst_unlowered_ptr =
          builder.CreateBitCast(dst_lowered_ptr, unlowered_ptr_type, "dst_unlowered_ptr");

      // Call 'ir_fn'
      builder.CreateCall(ir_fn,
          ArrayRef<Value*>({fn_ctx_arg, src_unlowered_ptr, dst_unlowered_ptr}));

      // Convert StringVal intermediate 'dst_arg' back to StringValue
      Value* anyval_result = builder.CreateLoad(dst_lowered_ptr, "anyval_result");
      result = CodegenAnyVal(codegen, &builder, dst_type, anyval_result).ToNativeValue();
      break;
    }
    default:
      DCHECK(false) << "bad aggregate operator: " << evaluator->agg_op();
  }

  // TODO: Store to register in the loop and store once to memory at the end of the loop.
  builder.CreateStore(result, dst_ptr);
  builder.CreateBr(ret_block);

  builder.SetInsertPoint(ret_block);
  builder.CreateRetVoid();

  *fn = codegen->FinalizeFunction(*fn);
  if (*fn == NULL) {
    return Status("PartitionedAggregationNode::CodegenUpdateSlot(): codegen'd "
        "UpdateSlot() function failed verification, see log");
  }
  return Status::OK();
}

// IR codegen for the UpdateTuple loop.  This loop is query specific and based on the
// aggregate functions.  The function signature must match the non- codegen'd UpdateTuple
// exactly.
// For the query:
// select count(*), count(int_col), sum(double_col) the IR looks like:
//

// ; Function Attrs: alwaysinline
// define void @UpdateTuple(%"class.impala::PartitionedAggregationNode"* %this_ptr,
//                          %"class.impala_udf::FunctionContext"** %agg_fn_ctxs,
//                          %"class.impala::Tuple"* %tuple,
//                          %"class.impala::TupleRow"* %row,
//                          i1 %is_merge) #20 {
// entry:
//   %tuple1 = bitcast %"class.impala::Tuple"* %tuple to { i8, i64, i64, double }*
//   %src_slot = getelementptr inbounds { i8, i64, i64, double }* %tuple1, i32 0, i32 1
//   %count_star_val = load i64* %src_slot
//   %count_star_inc = add i64 %count_star_val, 1
//   store i64 %count_star_inc, i64* %src_slot
//   %0 = getelementptr %"class.impala_udf::FunctionContext"** %agg_fn_ctxs, i32 1
//   %fn_ctx = load %"class.impala_udf::FunctionContext"** %0
//   call void @UpdateSlot(%"class.impala_udf::FunctionContext"* %fn_ctx,
//                         { i8, i64, i64, double }* %tuple1,
//                         %"class.impala::TupleRow"* %row)
//   %1 = getelementptr %"class.impala_udf::FunctionContext"** %agg_fn_ctxs, i32 2
//   %fn_ctx2 = load %"class.impala_udf::FunctionContext"** %1
//   call void @UpdateSlot5(%"class.impala_udf::FunctionContext"* %fn_ctx2,
//                          { i8, i64, i64, double }* %tuple1,
//                          %"class.impala::TupleRow"* %row)
//   ret void
// }
Status PartitionedAggregationNode::CodegenUpdateTuple(Function** fn) {
  LlvmCodeGen* codegen;
  RETURN_IF_ERROR(state_->GetCodegen(&codegen));
  SCOPED_TIMER(codegen->codegen_timer());

  int j = grouping_expr_ctxs_.size();
  for (int i = 0; i < aggregate_evaluators_.size(); ++i, ++j) {
    SlotDescriptor* slot_desc = intermediate_tuple_desc_->slots()[j];
    AggFnEvaluator* evaluator = aggregate_evaluators_[i];

    // Don't codegen things that aren't builtins (for now)
    if (!evaluator->is_builtin()) {
      return Status("PartitionedAggregationNode::CodegenUpdateTuple(): UDA codegen NYI");
    }

    bool supported = true;
    AggFnEvaluator::AggregationOp op = evaluator->agg_op();
    PrimitiveType type = slot_desc->type().type;
    // Char and timestamp intermediates aren't supported
    if (type == TYPE_TIMESTAMP || type == TYPE_CHAR) supported = false;
    // Only AVG and NDV support string intermediates
    if ((type == TYPE_STRING || type == TYPE_VARCHAR) &&
        !(op == AggFnEvaluator::AVG || op == AggFnEvaluator::NDV)) {
      supported = false;
    }
    if (!supported) {
      stringstream ss;
      ss << "Could not codegen PartitionedAggregationNode::UpdateTuple because "
         << "intermediate type " << slot_desc->type() << " is not yet supported for "
         << "aggregate function \"" << evaluator->fn_name() << "()\"";
      return Status(ss.str());
    }
  }

  if (intermediate_tuple_desc_->GetLlvmStruct(codegen) == NULL) {
    return Status("PartitionedAggregationNode::CodegenUpdateTuple(): failed to generate "
        "intermediate tuple desc");
  }

  // Get the types to match the UpdateTuple signature
  Type* agg_node_type = codegen->GetType(PartitionedAggregationNode::LLVM_CLASS_NAME);
  Type* fn_ctx_type = codegen->GetType(FunctionContextImpl::LLVM_FUNCTIONCONTEXT_NAME);
  Type* tuple_type = codegen->GetType(Tuple::LLVM_CLASS_NAME);
  Type* tuple_row_type = codegen->GetType(TupleRow::LLVM_CLASS_NAME);

  PointerType* agg_node_ptr_type = agg_node_type->getPointerTo();
  PointerType* fn_ctx_ptr_ptr_type = fn_ctx_type->getPointerTo()->getPointerTo();
  PointerType* tuple_ptr_type = tuple_type->getPointerTo();
  PointerType* tuple_row_ptr_type = tuple_row_type->getPointerTo();

  StructType* tuple_struct = intermediate_tuple_desc_->GetLlvmStruct(codegen);
  PointerType* tuple_ptr = PointerType::get(tuple_struct, 0);
  LlvmCodeGen::FnPrototype prototype(codegen, "UpdateTuple", codegen->void_type());
  prototype.AddArgument(LlvmCodeGen::NamedVariable("this_ptr", agg_node_ptr_type));
  prototype.AddArgument(LlvmCodeGen::NamedVariable("agg_fn_ctxs", fn_ctx_ptr_ptr_type));
  prototype.AddArgument(LlvmCodeGen::NamedVariable("tuple", tuple_ptr_type));
  prototype.AddArgument(LlvmCodeGen::NamedVariable("row", tuple_row_ptr_type));
  prototype.AddArgument(LlvmCodeGen::NamedVariable("is_merge", codegen->boolean_type()));

  LlvmCodeGen::LlvmBuilder builder(codegen->context());
  Value* args[5];
  *fn = prototype.GeneratePrototype(&builder, &args[0]);

  Value* agg_fn_ctxs_arg = args[1];
  Value* tuple_arg = args[2];
  Value* row_arg = args[3];

  // Cast the parameter types to the internal llvm runtime types.
  // TODO: get rid of this by using right type in function signature
  tuple_arg = builder.CreateBitCast(tuple_arg, tuple_ptr, "tuple");

  // Loop over each expr and generate the IR for that slot.  If the expr is not
  // count(*), generate a helper IR function to update the slot and call that.
  j = grouping_expr_ctxs_.size();
  for (int i = 0; i < aggregate_evaluators_.size(); ++i, ++j) {
    SlotDescriptor* slot_desc = intermediate_tuple_desc_->slots()[j];
    AggFnEvaluator* evaluator = aggregate_evaluators_[i];
    if (evaluator->is_count_star()) {
      // TODO: we should be able to hoist this up to the loop over the batch and just
      // increment the slot by the number of rows in the batch.
      int field_idx = slot_desc->llvm_field_idx();
      Value* const_one = codegen->GetIntConstant(TYPE_BIGINT, 1);
      Value* slot_ptr = builder.CreateStructGEP(NULL, tuple_arg, field_idx, "src_slot");
      Value* slot_loaded = builder.CreateLoad(slot_ptr, "count_star_val");
      Value* count_inc = builder.CreateAdd(slot_loaded, const_one, "count_star_inc");
      builder.CreateStore(count_inc, slot_ptr);
    } else {
      Function* update_slot_fn;
      RETURN_IF_ERROR(CodegenUpdateSlot(evaluator, slot_desc, &update_slot_fn));
      Value* fn_ctx_ptr = builder.CreateConstGEP1_32(agg_fn_ctxs_arg, i);
      Value* fn_ctx = builder.CreateLoad(fn_ctx_ptr, "fn_ctx");
      builder.CreateCall(update_slot_fn, ArrayRef<Value*>({fn_ctx, tuple_arg, row_arg}));
    }
  }
  builder.CreateRetVoid();

  // CodegenProcessBatch() does the final optimizations.
  *fn = codegen->FinalizeFunction(*fn);
  if (*fn == NULL) {
    return Status("PartitionedAggregationNode::CodegeUpdateTuple(): codegen'd "
        "UpdateTuple() function failed verification, see log");
  }
  return Status::OK();
}

Status PartitionedAggregationNode::CodegenProcessBatch() {
  LlvmCodeGen* codegen;
  RETURN_IF_ERROR(state_->GetCodegen(&codegen));
  SCOPED_TIMER(codegen->codegen_timer());

  Function* update_tuple_fn;
  RETURN_IF_ERROR(CodegenUpdateTuple(&update_tuple_fn));

  // Get the cross compiled update row batch function
  IRFunction::Type ir_fn = (!grouping_expr_ctxs_.empty() ?
      IRFunction::PART_AGG_NODE_PROCESS_BATCH_UNAGGREGATED :
      IRFunction::PART_AGG_NODE_PROCESS_BATCH_NO_GROUPING);
  Function* process_batch_fn = codegen->GetFunction(ir_fn, true);
  DCHECK(process_batch_fn != NULL);

  int replaced;
  if (!grouping_expr_ctxs_.empty()) {
    // Codegen for grouping using hash table

    // Replace prefetch_mode with constant so branches can be optimised out.
    TPrefetchMode::type prefetch_mode = state_->query_options().prefetch_mode;
    Value* prefetch_mode_arg = codegen->GetArgument(process_batch_fn, 3);
    prefetch_mode_arg->replaceAllUsesWith(
        ConstantInt::get(Type::getInt32Ty(codegen->context()), prefetch_mode));

    // The codegen'd ProcessBatch function is only used in Open() with level_ = 0,
    // so don't use murmur hash
    Function* hash_fn;
    RETURN_IF_ERROR(ht_ctx_->CodegenHashCurrentRow(state_, /* use murmur */ false,
        &hash_fn));

    // Codegen HashTable::Equals<true>
    Function* build_equals_fn;
    RETURN_IF_ERROR(ht_ctx_->CodegenEquals(state_, true, &build_equals_fn));

    // Codegen for evaluating input rows
    Function* eval_grouping_expr_fn;
    RETURN_IF_ERROR(ht_ctx_->CodegenEvalRow(state_, false, &eval_grouping_expr_fn));

    // Replace call sites
    replaced = codegen->ReplaceCallSites(process_batch_fn, eval_grouping_expr_fn,
        "EvalProbeRow");
    DCHECK_EQ(replaced, 1);

    replaced = codegen->ReplaceCallSites(process_batch_fn, hash_fn, "HashCurrentRow");
    DCHECK_EQ(replaced, 1);

    replaced = codegen->ReplaceCallSites(process_batch_fn, build_equals_fn, "Equals");
    DCHECK_EQ(replaced, 1);

    HashTableCtx::HashTableReplacedConstants replaced_constants;
    const bool stores_duplicates = false;
    RETURN_IF_ERROR(ht_ctx_->ReplaceHashTableConstants(state_, stores_duplicates, 1,
        process_batch_fn, &replaced_constants));
    DCHECK_GE(replaced_constants.stores_nulls, 1);
    DCHECK_GE(replaced_constants.finds_some_nulls, 1);
    DCHECK_GE(replaced_constants.stores_duplicates, 1);
    DCHECK_GE(replaced_constants.stores_tuples, 1);
    DCHECK_GE(replaced_constants.quadratic_probing, 1);
  }

  replaced = codegen->ReplaceCallSites(process_batch_fn, update_tuple_fn, "UpdateTuple");
  DCHECK_GE(replaced, 1);
  process_batch_fn = codegen->FinalizeFunction(process_batch_fn);
  if (process_batch_fn == NULL) {
    return Status("PartitionedAggregationNode::CodegenProcessBatch(): codegen'd "
        "ProcessBatch() function failed verification, see log");
  }

  void **codegened_fn_ptr = grouping_expr_ctxs_.empty() ?
      reinterpret_cast<void**>(&process_batch_no_grouping_fn_) :
      reinterpret_cast<void**>(&process_batch_fn_);
  codegen->AddFunctionToJit(process_batch_fn, codegened_fn_ptr);
  return Status::OK();
}

Status PartitionedAggregationNode::CodegenProcessBatchStreaming() {
  DCHECK(is_streaming_preagg_);
  LlvmCodeGen* codegen;
  RETURN_IF_ERROR(state_->GetCodegen(&codegen));
  SCOPED_TIMER(codegen->codegen_timer());

  IRFunction::Type ir_fn = IRFunction::PART_AGG_NODE_PROCESS_BATCH_STREAMING;
  Function* process_batch_streaming_fn = codegen->GetFunction(ir_fn, true);
  DCHECK(process_batch_streaming_fn != NULL);

  // Make needs_serialize arg constant so dead code can be optimised out.
  Value* needs_serialize_arg = codegen->GetArgument(process_batch_streaming_fn, 2);
  needs_serialize_arg->replaceAllUsesWith(
      ConstantInt::get(Type::getInt1Ty(codegen->context()), needs_serialize_));

  // Replace prefetch_mode with constant so branches can be optimised out.
  TPrefetchMode::type prefetch_mode = state_->query_options().prefetch_mode;
  Value* prefetch_mode_arg = codegen->GetArgument(process_batch_streaming_fn, 3);
  prefetch_mode_arg->replaceAllUsesWith(
      ConstantInt::get(Type::getInt32Ty(codegen->context()), prefetch_mode));

  Function* update_tuple_fn;
  RETURN_IF_ERROR(CodegenUpdateTuple(&update_tuple_fn));

  // We only use the top-level hash function for streaming aggregations.
  Function* hash_fn;
  RETURN_IF_ERROR(ht_ctx_->CodegenHashCurrentRow(state_, false, &hash_fn));

  // Codegen HashTable::Equals
  Function* equals_fn;
  RETURN_IF_ERROR(ht_ctx_->CodegenEquals(state_, true, &equals_fn));

  // Codegen for evaluating input rows
  Function* eval_grouping_expr_fn;
  RETURN_IF_ERROR(ht_ctx_->CodegenEvalRow(state_, false, &eval_grouping_expr_fn));

  // Replace call sites
  int replaced = codegen->ReplaceCallSites(process_batch_streaming_fn, update_tuple_fn,
      "UpdateTuple");
  DCHECK_EQ(replaced, 2);

  replaced = codegen->ReplaceCallSites(process_batch_streaming_fn, eval_grouping_expr_fn,
      "EvalProbeRow");
  DCHECK_EQ(replaced, 1);

  replaced = codegen->ReplaceCallSites(process_batch_streaming_fn, hash_fn,
      "HashCurrentRow");
  DCHECK_EQ(replaced, 1);

  replaced = codegen->ReplaceCallSites(process_batch_streaming_fn, equals_fn, "Equals");
  DCHECK_EQ(replaced, 1);

  HashTableCtx::HashTableReplacedConstants replaced_constants;
  const bool stores_duplicates = false;
  RETURN_IF_ERROR(ht_ctx_->ReplaceHashTableConstants(state_, stores_duplicates, 1,
      process_batch_streaming_fn, &replaced_constants));
  DCHECK_GE(replaced_constants.stores_nulls, 1);
  DCHECK_GE(replaced_constants.finds_some_nulls, 1);
  DCHECK_GE(replaced_constants.stores_duplicates, 1);
  DCHECK_GE(replaced_constants.stores_tuples, 1);
  DCHECK_GE(replaced_constants.quadratic_probing, 1);

  DCHECK(process_batch_streaming_fn != NULL);
  process_batch_streaming_fn = codegen->FinalizeFunction(process_batch_streaming_fn);
  if (process_batch_streaming_fn == NULL) {
    return Status("PartitionedAggregationNode::CodegenProcessBatchStreaming(): codegen'd "
        "ProcessBatchStreaming() function failed verification, see log");
  }

  codegen->AddFunctionToJit(process_batch_streaming_fn,
      reinterpret_cast<void**>(&process_batch_streaming_fn_));
  return Status::OK();
}

}
