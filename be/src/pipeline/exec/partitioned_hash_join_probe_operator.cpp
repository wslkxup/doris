// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "partitioned_hash_join_probe_operator.h"

#include "util/mem_info.h"
#include "vec/spill/spill_stream_manager.h"

namespace doris::pipeline {

PartitionedHashJoinProbeLocalState::PartitionedHashJoinProbeLocalState(RuntimeState* state,
                                                                       OperatorXBase* parent)
        : JoinProbeLocalState(state, parent) {}

Status PartitionedHashJoinProbeLocalState::init(RuntimeState* state, LocalStateInfo& info) {
    RETURN_IF_ERROR(JoinProbeLocalState::init(state, info));
    _internal_runtime_profile.reset(new RuntimeProfile("internal_profile"));
    auto& p = _parent->cast<PartitionedHashJoinProbeOperatorX>();

    _partitioned_blocks.resize(p._partition_count);
    _probe_spilling_streams.resize(p._partition_count);
    _partitioner = std::make_unique<PartitionerType>(p._partition_count);
    RETURN_IF_ERROR(_partitioner->init(p._probe_exprs));
    RETURN_IF_ERROR(_partitioner->prepare(state, p._child_x->row_desc()));

    _spill_and_partition_label = ADD_LABEL_COUNTER(profile(), "SpillAndPartition");
    _partition_timer = ADD_CHILD_TIMER(profile(), "PartitionTime", "SpillAndPartition");
    _partition_shuffle_timer =
            ADD_CHILD_TIMER(profile(), "PartitionShuffleTime", "SpillAndPartition");
    _spill_build_rows =
            ADD_CHILD_COUNTER(profile(), "SpillBuildRows", TUnit::UNIT, "SpillAndPartition");
    _recovery_build_rows =
            ADD_CHILD_COUNTER(profile(), "RecoveryBuildRows", TUnit::UNIT, "SpillAndPartition");
    _spill_probe_rows =
            ADD_CHILD_COUNTER(profile(), "SpillProbeRows", TUnit::UNIT, "SpillAndPartition");
    _recovery_probe_rows =
            ADD_CHILD_COUNTER(profile(), "RecoveryProbeRows", TUnit::UNIT, "SpillAndPartition");
    _spill_build_blocks =
            ADD_CHILD_COUNTER(profile(), "SpillBuildBlocks", TUnit::UNIT, "SpillAndPartition");
    _recovery_build_blocks =
            ADD_CHILD_COUNTER(profile(), "RecoveryBuildBlocks", TUnit::UNIT, "SpillAndPartition");
    _spill_probe_blocks =
            ADD_CHILD_COUNTER(profile(), "SpillProbeBlocks", TUnit::UNIT, "SpillAndPartition");
    _recovery_probe_blocks =
            ADD_CHILD_COUNTER(profile(), "RecoveryProbeBlocks", TUnit::UNIT, "SpillAndPartition");

    // Build phase
    _build_phase_label = ADD_LABEL_COUNTER(profile(), "BuildPhase");
    _build_rows_counter = ADD_CHILD_COUNTER(profile(), "BuildRows", TUnit::UNIT, "BuildPhase");
    _publish_runtime_filter_timer =
            ADD_CHILD_TIMER(profile(), "PublishRuntimeFilterTime", "BuildPhase");
    _runtime_filter_compute_timer =
            ADD_CHILD_TIMER(profile(), "RuntimeFilterComputeTime", "BuildPhase");
    _build_table_timer = ADD_CHILD_TIMER(profile(), "BuildTableTime", "BuildPhase");
    _build_side_merge_block_timer =
            ADD_CHILD_TIMER(profile(), "BuildSideMergeBlockTime", "BuildPhase");
    _build_table_insert_timer = ADD_CHILD_TIMER(profile(), "BuildTableInsertTime", "BuildPhase");
    _build_expr_call_timer = ADD_CHILD_TIMER(profile(), "BuildExprCallTime", "BuildPhase");
    _build_side_compute_hash_timer =
            ADD_CHILD_TIMER(profile(), "BuildSideHashComputingTime", "BuildPhase");
    _allocate_resource_timer = ADD_CHILD_TIMER(profile(), "AllocateResourceTime", "BuildPhase");

    // Probe phase
    _probe_phase_label = ADD_LABEL_COUNTER(profile(), "ProbePhase");
    _probe_next_timer = ADD_CHILD_TIMER(profile(), "ProbeFindNextTime", "ProbePhase");
    _probe_expr_call_timer = ADD_CHILD_TIMER(profile(), "ProbeExprCallTime", "ProbePhase");
    _search_hashtable_timer =
            ADD_CHILD_TIMER(profile(), "ProbeWhenSearchHashTableTime", "ProbePhase");
    _build_side_output_timer =
            ADD_CHILD_TIMER(profile(), "ProbeWhenBuildSideOutputTime", "ProbePhase");
    _probe_side_output_timer =
            ADD_CHILD_TIMER(profile(), "ProbeWhenProbeSideOutputTime", "ProbePhase");
    _probe_process_hashtable_timer =
            ADD_CHILD_TIMER(profile(), "ProbeWhenProcessHashTableTime", "ProbePhase");
    _process_other_join_conjunct_timer =
            ADD_CHILD_TIMER(profile(), "OtherJoinConjunctTime", "ProbePhase");
    _init_probe_side_timer = ADD_CHILD_TIMER(profile(), "InitProbeSideTime", "ProbePhase");
    return Status::OK();
}
#define UPDATE_PROFILE(counter, name)                           \
    do {                                                        \
        auto* child_counter = child_profile->get_counter(name); \
        if (child_counter != nullptr) {                         \
            COUNTER_UPDATE(counter, child_counter->value());    \
        }                                                       \
    } while (false)

void PartitionedHashJoinProbeLocalState::update_build_profile(RuntimeProfile* child_profile) {
    UPDATE_PROFILE(_build_rows_counter, "BuildRows");
    UPDATE_PROFILE(_publish_runtime_filter_timer, "PublishRuntimeFilterTime");
    UPDATE_PROFILE(_runtime_filter_compute_timer, "RuntimeFilterComputeTime");
    UPDATE_PROFILE(_build_table_timer, "BuildTableTime");
    UPDATE_PROFILE(_build_side_merge_block_timer, "BuildSideMergeBlockTime");
    UPDATE_PROFILE(_build_table_insert_timer, "BuildTableInsertTime");
    UPDATE_PROFILE(_build_expr_call_timer, "BuildExprCallTime");
    UPDATE_PROFILE(_build_side_compute_hash_timer, "BuildSideHashComputingTime");
    UPDATE_PROFILE(_allocate_resource_timer, "AllocateResourceTime");
}

void PartitionedHashJoinProbeLocalState::update_probe_profile(RuntimeProfile* child_profile) {
    UPDATE_PROFILE(_probe_timer, "ProbeTime");
    UPDATE_PROFILE(_join_filter_timer, "JoinFilterTimer");
    UPDATE_PROFILE(_build_output_block_timer, "BuildOutputBlock");
    UPDATE_PROFILE(_probe_rows_counter, "ProbeRows");
    UPDATE_PROFILE(_probe_next_timer, "ProbeFindNextTime");
    UPDATE_PROFILE(_probe_expr_call_timer, "ProbeExprCallTime");
    UPDATE_PROFILE(_search_hashtable_timer, "ProbeWhenSearchHashTableTime");
    UPDATE_PROFILE(_build_side_output_timer, "ProbeWhenBuildSideOutputTime");
    UPDATE_PROFILE(_probe_side_output_timer, "ProbeWhenProbeSideOutputTime");
    UPDATE_PROFILE(_probe_process_hashtable_timer, "ProbeWhenProcessHashTableTime");
    UPDATE_PROFILE(_process_other_join_conjunct_timer, "OtherJoinConjunctTime");
    UPDATE_PROFILE(_init_probe_side_timer, "InitProbeSideTime");
}

#undef UPDATE_PROFILE

Status PartitionedHashJoinProbeLocalState::open(RuntimeState* state) {
    RETURN_IF_ERROR(PipelineXLocalStateBase::open(state));
    return _partitioner->open(state);
}
Status PartitionedHashJoinProbeLocalState::close(RuntimeState* state) {
    RETURN_IF_ERROR(JoinProbeLocalState::close(state));
    return Status::OK();
}

Status PartitionedHashJoinProbeLocalState::spill_build_block(RuntimeState* state,
                                                             uint32_t partition_index) {
    auto& partitioned_build_blocks = _shared_state->partitioned_build_blocks;
    auto& mutable_block = partitioned_build_blocks[partition_index];
    if (!mutable_block || mutable_block->rows() == 0) {
        --_spilling_task_count;
        return Status::OK();
    }

    auto& build_spilling_stream = _shared_state->spilled_streams[partition_index];
    if (!build_spilling_stream) {
        RETURN_IF_ERROR(ExecEnv::GetInstance()->spill_stream_mgr()->register_spill_stream(
                state, build_spilling_stream, print_id(state->query_id()), "hash_build_sink",
                _parent->id(), std::numeric_limits<int32_t>::max(),
                std::numeric_limits<size_t>::max(), _runtime_profile.get()));
        RETURN_IF_ERROR(build_spilling_stream->prepare_spill());
    }

    auto* spill_io_pool = ExecEnv::GetInstance()->spill_stream_mgr()->get_spill_io_thread_pool(
            build_spilling_stream->get_spill_root_dir());
    return spill_io_pool->submit_func([state, &build_spilling_stream, &mutable_block, this] {
        (void)state; // avoid ut compile error
        SCOPED_ATTACH_TASK(state);
        if (_spill_status_ok) {
            auto build_block = mutable_block->to_block();
            DCHECK_EQ(mutable_block->rows(), 0);
            auto st = build_spilling_stream->spill_block(build_block, false);
            if (!st.ok()) {
                std::unique_lock<std::mutex> lock(_spill_lock);
                _spill_status_ok = false;
                _spill_status = std::move(st);
            } else {
                COUNTER_UPDATE(_spill_build_rows, build_block.rows());
                COUNTER_UPDATE(_spill_build_blocks, 1);
            }
        }
        --_spilling_task_count;

        if (_spilling_task_count == 0) {
            std::unique_lock<std::mutex> lock(_spill_lock);
            _dependency->set_ready();
        }
    });
}

Status PartitionedHashJoinProbeLocalState::spill_probe_blocks(RuntimeState* state,
                                                              uint32_t partition_index) {
    auto& spilling_stream = _probe_spilling_streams[partition_index];
    if (!spilling_stream) {
        RETURN_IF_ERROR(ExecEnv::GetInstance()->spill_stream_mgr()->register_spill_stream(
                state, spilling_stream, print_id(state->query_id()), "hash_probe", _parent->id(),
                std::numeric_limits<int32_t>::max(), std::numeric_limits<size_t>::max(),
                _runtime_profile.get()));
        RETURN_IF_ERROR(spilling_stream->prepare_spill());
    }

    auto* spill_io_pool = ExecEnv::GetInstance()->spill_stream_mgr()->get_spill_io_thread_pool(
            spilling_stream->get_spill_root_dir());

    auto& blocks = _probe_blocks[partition_index];

    if (!blocks.empty()) {
        return spill_io_pool->submit_func([state, &blocks, &spilling_stream, this] {
            (void)state; // avoid ut compile error
            SCOPED_ATTACH_TASK(state);
            for (auto& block : blocks) {
                if (_spill_status_ok) {
                    auto st = spilling_stream->spill_block(block, false);
                    if (!st.ok()) {
                        std::unique_lock<std::mutex> lock(_spill_lock);
                        _spill_status_ok = false;
                        _spill_status = std::move(st);
                        break;
                    }
                    COUNTER_UPDATE(_spill_probe_rows, block.rows());
                } else {
                    break;
                }
            }

            COUNTER_UPDATE(_spill_probe_blocks, blocks.size());
            blocks.clear();
            --_spilling_task_count;

            if (_spilling_task_count == 0) {
                std::unique_lock<std::mutex> lock(_spill_lock);
                _dependency->set_ready();
            }
        });
    } else {
        --_spilling_task_count;
        if (_spilling_task_count == 0) {
            std::unique_lock<std::mutex> lock(_spill_lock);
            _dependency->set_ready();
        }
    }
    return Status::OK();
}

Status PartitionedHashJoinProbeLocalState::finish_spilling(uint32_t partition_index) {
    auto& build_spilling_stream = _shared_state->spilled_streams[partition_index];
    if (build_spilling_stream) {
        build_spilling_stream->end_spill(Status::OK());
        RETURN_IF_ERROR(build_spilling_stream->spill_eof());
    }

    auto& probe_spilling_stream = _probe_spilling_streams[partition_index];

    if (probe_spilling_stream) {
        probe_spilling_stream->end_spill(Status::OK());
        RETURN_IF_ERROR(probe_spilling_stream->spill_eof());
    }

    return Status::OK();
}

Status PartitionedHashJoinProbeLocalState::recovery_build_blocks_from_disk(RuntimeState* state,
                                                                           uint32_t partition_index,
                                                                           bool& has_data) {
    auto& spilled_stream = _shared_state->spilled_streams[partition_index];
    has_data = false;
    if (!spilled_stream) {
        return Status::OK();
    }

    auto& mutable_block = _shared_state->partitioned_build_blocks[partition_index];
    DCHECK(mutable_block != nullptr);

    auto read_func = [this, state, &spilled_stream, &mutable_block] {
        Defer defer([this] { --_spilling_task_count; });
        (void)state; // avoid ut compile error
        SCOPED_ATTACH_TASK(state);
        DCHECK_EQ(_spill_status_ok.load(), true);

        bool eos = false;
        while (!eos) {
            vectorized::Block block;
            auto st = spilled_stream->read_next_block_sync(&block, &eos);
            if (!st.ok()) {
                std::unique_lock<std::mutex> lock(_spill_lock);
                _spill_status_ok = false;
                _spill_status = std::move(st);
                break;
            }
            COUNTER_UPDATE(_recovery_build_rows, block.rows());
            COUNTER_UPDATE(_recovery_build_blocks, 1);

            if (block.empty()) {
                continue;
            }

            if (mutable_block->empty()) {
                *mutable_block = std::move(block);
            } else {
                st = mutable_block->merge(std::move(block));
                if (!st.ok()) {
                    std::unique_lock<std::mutex> lock(_spill_lock);
                    _spill_status_ok = false;
                    _spill_status = std::move(st);
                    break;
                }
            }
        }

        ExecEnv::GetInstance()->spill_stream_mgr()->delete_spill_stream(spilled_stream);
        spilled_stream.reset();
        _dependency->set_ready();
    };

    auto* spill_io_pool = ExecEnv::GetInstance()->spill_stream_mgr()->get_async_task_thread_pool();
    has_data = true;
    _dependency->block();

    ++_spilling_task_count;
    auto st = spill_io_pool->submit_func(read_func);
    if (!st.ok()) {
        --_spilling_task_count;
    }
    return st;
}

Status PartitionedHashJoinProbeLocalState::recovery_probe_blocks_from_disk(RuntimeState* state,
                                                                           uint32_t partition_index,
                                                                           bool& has_data) {
    auto& spilled_stream = _probe_spilling_streams[partition_index];
    has_data = false;
    if (!spilled_stream) {
        return Status::OK();
    }

    auto& blocks = _probe_blocks[partition_index];

    /// TODO: maybe recovery more blocks each time.
    auto read_func = [this, state, &spilled_stream, &blocks] {
        Defer defer([this] { --_spilling_task_count; });
        (void)state; // avoid ut compile error
        SCOPED_ATTACH_TASK(state);
        DCHECK_EQ(_spill_status_ok.load(), true);

        vectorized::Block block;
        bool eos = false;
        auto st = spilled_stream->read_next_block_sync(&block, &eos);
        if (!st.ok()) {
            std::unique_lock<std::mutex> lock(_spill_lock);
            _spill_status_ok = false;
            _spill_status = std::move(st);
        } else {
            COUNTER_UPDATE(_recovery_probe_rows, block.rows());
            COUNTER_UPDATE(_recovery_probe_blocks, 1);
            blocks.emplace_back(std::move(block));
        }

        if (eos) {
            ExecEnv::GetInstance()->spill_stream_mgr()->delete_spill_stream(spilled_stream);
            spilled_stream.reset();
        }

        _dependency->set_ready();
    };

    auto* spill_io_pool = ExecEnv::GetInstance()->spill_stream_mgr()->get_async_task_thread_pool();
    DCHECK(spill_io_pool != nullptr);
    _dependency->block();
    has_data = true;
    ++_spilling_task_count;
    auto st = spill_io_pool->submit_func(read_func);
    if (!st.ok()) {
        --_spilling_task_count;
    }
    return st;
}

PartitionedHashJoinProbeOperatorX::PartitionedHashJoinProbeOperatorX(ObjectPool* pool,
                                                                     const TPlanNode& tnode,
                                                                     int operator_id,
                                                                     const DescriptorTbl& descs,
                                                                     uint32_t partition_count)
        : JoinProbeOperatorX<PartitionedHashJoinProbeLocalState>(pool, tnode, operator_id, descs),
          _join_distribution(tnode.hash_join_node.__isset.dist_type ? tnode.hash_join_node.dist_type
                                                                    : TJoinDistributionType::NONE),
          _distribution_partition_exprs(tnode.__isset.distribute_expr_lists
                                                ? tnode.distribute_expr_lists[0]
                                                : std::vector<TExpr> {}),
          _tnode(tnode),
          _descriptor_tbl(descs),
          _partition_count(partition_count) {}

Status PartitionedHashJoinProbeOperatorX::init(const TPlanNode& tnode, RuntimeState* state) {
    RETURN_IF_ERROR(JoinProbeOperatorX::init(tnode, state));
    _op_name = "PARTITIONED_HASH_JOIN_PROBE_OPERATOR";
    auto tnode_ = _tnode;
    tnode_.runtime_filters.clear();

    for (auto& conjunct : tnode.hash_join_node.eq_join_conjuncts) {
        _probe_exprs.emplace_back(conjunct.left);
    }

    _sink_operator =
            std::make_unique<HashJoinBuildSinkOperatorX>(_pool, 0, tnode_, _descriptor_tbl, false);
    _probe_operator = std::make_unique<HashJoinProbeOperatorX>(_pool, tnode_, 0, _descriptor_tbl);
    RETURN_IF_ERROR(_sink_operator->init(tnode_, state));
    return _probe_operator->init(tnode_, state);
}
Status PartitionedHashJoinProbeOperatorX::prepare(RuntimeState* state) {
    RETURN_IF_ERROR(OperatorXBase::prepare(state));
    RETURN_IF_ERROR(vectorized::VExpr::prepare(_output_expr_ctxs, state, *_intermediate_row_desc));
    RETURN_IF_ERROR(_probe_operator->set_child(_child_x));
    RETURN_IF_ERROR(_probe_operator->set_child(_build_side_child));
    RETURN_IF_ERROR(_sink_operator->set_child(_build_side_child));
    RETURN_IF_ERROR(_probe_operator->prepare(state));
    RETURN_IF_ERROR(_sink_operator->prepare(state));
    return Status::OK();
}

Status PartitionedHashJoinProbeOperatorX::open(RuntimeState* state) {
    RETURN_IF_ERROR(JoinProbeOperatorX::open(state));
    RETURN_IF_ERROR(_probe_operator->open(state));
    RETURN_IF_ERROR(_sink_operator->open(state));
    return Status::OK();
}

Status PartitionedHashJoinProbeOperatorX::push(RuntimeState* state, vectorized::Block* input_block,
                                               bool eos) const {
    auto& local_state = get_local_state(state);
    const auto rows = input_block->rows();
    auto& partitioned_blocks = local_state._partitioned_blocks;
    if (rows == 0) {
        if (eos) {
            for (uint32_t i = 0; i != _partition_count; ++i) {
                if (partitioned_blocks[i] && !partitioned_blocks[i]->empty()) {
                    local_state._probe_blocks[i].emplace_back(partitioned_blocks[i]->to_block());
                    partitioned_blocks[i].reset();
                }
            }
        }
        return Status::OK();
    }
    {
        SCOPED_TIMER(local_state._partition_timer);
        RETURN_IF_ERROR(local_state._partitioner->do_partitioning(state, input_block,
                                                                  local_state._mem_tracker.get()));
    }

    std::vector<uint32_t> partition_indexes[_partition_count];
    auto* channel_ids = reinterpret_cast<uint64_t*>(local_state._partitioner->get_channel_ids());
    for (uint32_t i = 0; i != rows; ++i) {
        partition_indexes[channel_ids[i]].emplace_back(i);
    }

    SCOPED_TIMER(local_state._partition_shuffle_timer);
    for (uint32_t i = 0; i != _partition_count; ++i) {
        const auto count = partition_indexes[i].size();
        if (UNLIKELY(count == 0)) {
            continue;
        }

        if (!partitioned_blocks[i]) {
            partitioned_blocks[i] =
                    vectorized::MutableBlock::create_unique(input_block->clone_empty());
        }
        partitioned_blocks[i]->add_rows(input_block, &(partition_indexes[i][0]),
                                        &(partition_indexes[i][count]));

        if (partitioned_blocks[i]->rows() > 2 * 1024 * 1024 ||
            (eos && partitioned_blocks[i]->rows() > 0)) {
            local_state._probe_blocks[i].emplace_back(partitioned_blocks[i]->to_block());
            partitioned_blocks[i].reset();
        }
    }

    return Status::OK();
}

Status PartitionedHashJoinProbeOperatorX::_setup_internal_operators(
        PartitionedHashJoinProbeLocalState& local_state, RuntimeState* state) const {
    if (local_state._runtime_state) {
        _update_profile_from_internal_states(local_state);
    }

    local_state._runtime_state = RuntimeState::create_unique(
            nullptr, state->fragment_instance_id(), state->query_id(), state->fragment_id(),
            state->query_options(), TQueryGlobals {}, state->exec_env(), state->get_query_ctx());

    local_state._runtime_state->set_query_mem_tracker(state->query_mem_tracker());

    local_state._runtime_state->set_task_execution_context(
            state->get_task_execution_context().lock());
    local_state._runtime_state->set_be_number(state->be_number());

    local_state._runtime_state->set_desc_tbl(&state->desc_tbl());
    local_state._runtime_state->resize_op_id_to_local_state(-1);
    local_state._runtime_state->set_pipeline_x_runtime_filter_mgr(
            state->local_runtime_filter_mgr());

    local_state._in_mem_shared_state_sptr = _sink_operator->create_shared_state();

    // set sink local state
    LocalSinkStateInfo info {0,  local_state._internal_runtime_profile.get(),
                             -1, local_state._in_mem_shared_state_sptr.get(),
                             {}, {}};
    RETURN_IF_ERROR(_sink_operator->setup_local_state(local_state._runtime_state.get(), info));

    LocalStateInfo state_info {local_state._internal_runtime_profile.get(),
                               {},
                               local_state._in_mem_shared_state_sptr.get(),
                               {},
                               0};
    RETURN_IF_ERROR(
            _probe_operator->setup_local_state(local_state._runtime_state.get(), state_info));

    auto* sink_local_state = local_state._runtime_state->get_sink_local_state();
    DCHECK(sink_local_state != nullptr);
    RETURN_IF_ERROR(sink_local_state->open(state));

    auto* probe_local_state =
            local_state._runtime_state->get_local_state(_probe_operator->operator_id());
    DCHECK(probe_local_state != nullptr);
    RETURN_IF_ERROR(probe_local_state->open(state));

    auto& partitioned_block =
            local_state._shared_state->partitioned_build_blocks[local_state._partition_cursor];
    vectorized::Block block;
    if (partitioned_block && partitioned_block->rows() > 0) {
        block = partitioned_block->to_block();
        partitioned_block.reset();
    }
    RETURN_IF_ERROR(_sink_operator->sink(local_state._runtime_state.get(), &block, true));
    return Status::OK();
}

Status PartitionedHashJoinProbeOperatorX::pull(doris::RuntimeState* state,
                                               vectorized::Block* output_block, bool* eos) const {
    auto& local_state = get_local_state(state);
    if (!local_state._spill_status_ok) {
        DCHECK_NE(local_state._spill_status.code(), 0);
        return local_state._spill_status;
    }

    if (_should_revoke_memory(state)) {
        bool wait_for_io = false;
        RETURN_IF_ERROR((const_cast<PartitionedHashJoinProbeOperatorX*>(this))
                                ->_revoke_memory(state, wait_for_io));
        if (wait_for_io) {
            return Status::OK();
        }
    }

    if (local_state._need_to_setup_internal_operators) {
        *eos = false;
        bool has_data = false;
        CHECK_EQ(local_state._dependency->is_blocked_by(), nullptr);
        RETURN_IF_ERROR(local_state.recovery_build_blocks_from_disk(
                state, local_state._partition_cursor, has_data));
        if (has_data) {
            return Status::OK();
        }
        RETURN_IF_ERROR(_setup_internal_operators(local_state, state));
        local_state._need_to_setup_internal_operators = false;
    }

    auto partition_index = local_state._partition_cursor;
    bool in_mem_eos_;
    auto* runtime_state = local_state._runtime_state.get();
    auto& probe_blocks = local_state._probe_blocks[partition_index];
    while (_probe_operator->need_more_input_data(runtime_state)) {
        if (probe_blocks.empty()) {
            *eos = false;
            bool has_data = false;
            RETURN_IF_ERROR(
                    local_state.recovery_probe_blocks_from_disk(state, partition_index, has_data));
            if (!has_data) {
                vectorized::Block block;
                RETURN_IF_ERROR(_probe_operator->push(runtime_state, &block, true));
                break;
            } else {
                return Status::OK();
            }
        }

        auto block = std::move(probe_blocks.back());
        probe_blocks.pop_back();
        RETURN_IF_ERROR(_probe_operator->push(runtime_state, &block, false));
    }

    RETURN_IF_ERROR(
            _probe_operator->pull(local_state._runtime_state.get(), output_block, &in_mem_eos_));

    *eos = false;
    if (in_mem_eos_) {
        local_state._partition_cursor++;
        if (local_state._partition_cursor == _partition_count) {
            *eos = true;
        } else {
            RETURN_IF_ERROR(local_state.finish_spilling(local_state._partition_cursor));
            local_state._need_to_setup_internal_operators = true;
        }
    }

    return Status::OK();
}

bool PartitionedHashJoinProbeOperatorX::need_more_input_data(RuntimeState* state) const {
    auto& local_state = get_local_state(state);
    return !local_state._child_eos;
}

bool PartitionedHashJoinProbeOperatorX::need_data_from_children(RuntimeState* state) const {
    auto& local_state = get_local_state(state);
    if (local_state._spilling_task_count != 0) {
        return true;
    }

    return JoinProbeOperatorX::need_data_from_children(state);
}

size_t PartitionedHashJoinProbeOperatorX::revocable_mem_size(RuntimeState* state) const {
    auto& local_state = get_local_state(state);
    size_t mem_size = 0;

    auto& partitioned_build_blocks = local_state._shared_state->partitioned_build_blocks;
    auto& probe_blocks = local_state._probe_blocks;
    for (uint32_t i = local_state._partition_cursor + 1; i < _partition_count; ++i) {
        auto& build_block = partitioned_build_blocks[i];
        if (build_block && build_block->rows() > 0) {
            mem_size += build_block->allocated_bytes();
        }

        for (auto& block : probe_blocks[i]) {
            mem_size += block.allocated_bytes();
        }
    }
    return mem_size;
}

Status PartitionedHashJoinProbeOperatorX::_revoke_memory(RuntimeState* state, bool& wait_for_io) {
    auto& local_state = get_local_state(state);
    wait_for_io = false;
    if (_partition_count > (local_state._partition_cursor + 1)) {
        local_state._spilling_task_count =
                (_partition_count - local_state._partition_cursor - 1) * 2;
    } else {
        return Status::OK();
    }

    for (uint32_t i = local_state._partition_cursor + 1; i < _partition_count; ++i) {
        RETURN_IF_ERROR(local_state.spill_build_block(state, i));
        RETURN_IF_ERROR(local_state.spill_probe_blocks(state, i));
    }

    if (local_state._spilling_task_count > 0) {
        std::unique_lock<std::mutex> lock(local_state._spill_lock);
        if (local_state._spilling_task_count > 0) {
            local_state._dependency->block();
            wait_for_io = true;
        }
    }
    return Status::OK();
}

bool PartitionedHashJoinProbeOperatorX::_should_revoke_memory(RuntimeState* state) const {
    auto sys_mem_available = MemInfo::sys_mem_available();
    auto sys_mem_warning_water_mark = doris::MemInfo::sys_mem_available_warning_water_mark();

    if (sys_mem_available <
        sys_mem_warning_water_mark * config::spill_mem_warning_water_mark_multiplier) {
        const auto revocable_size = revocable_mem_size(state);
        const auto min_revocable_size = state->min_revocable_mem();
        return min_revocable_size > 0 && revocable_size > min_revocable_size;
    }
    return false;
}

void PartitionedHashJoinProbeOperatorX::_update_profile_from_internal_states(
        PartitionedHashJoinProbeLocalState& local_state) const {
    if (local_state._runtime_state) {
        auto* sink_local_state = local_state._runtime_state->get_sink_local_state();
        local_state.update_build_profile(sink_local_state->profile());
        auto* probe_local_state =
                local_state._runtime_state->get_local_state(_probe_operator->operator_id());
        local_state.update_probe_profile(probe_local_state->profile());
    }
}

Status PartitionedHashJoinProbeOperatorX::get_block(RuntimeState* state, vectorized::Block* block,
                                                    bool* eos) {
    *eos = false;
    auto& local_state = get_local_state(state);
    SCOPED_TIMER(local_state.exec_time_counter());
    if (need_more_input_data(state)) {
        local_state._child_block->clear_column_data();

        if (_should_revoke_memory(state)) {
            bool wait_for_io = false;
            RETURN_IF_ERROR(_revoke_memory(state, wait_for_io));
            if (wait_for_io) {
                return Status::OK();
            }
        }

        RETURN_IF_ERROR(_child_x->get_block_after_projects(state, local_state._child_block.get(),
                                                           &local_state._child_eos));

        if (local_state._child_eos) {
            RETURN_IF_ERROR(local_state.finish_spilling(0));
        } else if (local_state._child_block->rows() == 0) {
            return Status::OK();
        }
        {
            SCOPED_TIMER(local_state.exec_time_counter());
            RETURN_IF_ERROR(push(state, local_state._child_block.get(), local_state._child_eos));
        }
    }

    if (!need_more_input_data(state)) {
        SCOPED_TIMER(local_state.exec_time_counter());
        RETURN_IF_ERROR(pull(state, block, eos));
        local_state.add_num_rows_returned(block->rows());
        if (*eos) {
            _update_profile_from_internal_states(local_state);
        }
    }
    return Status::OK();
}

} // namespace doris::pipeline