#include "JoinHashTable.h"
#include "Execute.h"
#include "HashJoinRuntime.h"
#include "RuntimeFunctions.h"

#include "../Chunk/Chunk.h"

#include <glog/logging.h>

std::shared_ptr<JoinHashTable> JoinHashTable::getInstance(
    const std::shared_ptr<Analyzer::BinOper> qual_bin_oper,
    const Catalog_Namespace::Catalog& cat,
    const std::vector<Fragmenter_Namespace::QueryInfo>& query_infos,
    const Data_Namespace::MemoryLevel memory_level,
    const Executor* executor) {
  return nullptr;
  CHECK_EQ(kEQ, qual_bin_oper->get_optype());
  const auto lhs = qual_bin_oper->get_left_operand();
  const auto rhs = qual_bin_oper->get_right_operand();
  if (lhs->get_type_info() != rhs->get_type_info()) {
    return nullptr;
  }
  const auto lhs_col = dynamic_cast<const Analyzer::ColumnVar*>(lhs);
  const auto rhs_col = dynamic_cast<const Analyzer::ColumnVar*>(rhs);
  if (!lhs_col || !rhs_col) {
    return nullptr;
  }
  const Analyzer::ColumnVar* inner_col_var{nullptr};
  if (lhs_col->get_rte_idx() == 0 && rhs_col->get_rte_idx() == 1) {
    inner_col_var = rhs_col;
  }
  if (lhs_col->get_rte_idx() == 1 && rhs_col->get_rte_idx() == 0) {
    inner_col_var = lhs_col;
  }
  if (!inner_col_var->get_type_info().is_integer()) {
    return nullptr;
  }
  const auto col_range = getExpressionRange(inner_col_var, query_infos, nullptr);
  if (col_range.has_nulls) {  // TODO(alex): lift this constraint
    return nullptr;
  }
  auto join_hash_table =
      std::shared_ptr<JoinHashTable>(new JoinHashTable(inner_col_var, cat, query_infos, memory_level, col_range, executor));
  const int err = join_hash_table->reify();
  if (err) {
    return nullptr;
  }
  return join_hash_table;
}

int JoinHashTable::reify() {
  int err = 0;
  const auto& query_info = query_infos_[col_var_->get_rte_idx()];
  if (query_info.fragments.size() != 1) {  // we don't support multiple fragment inner tables (yet)
    return -1;
  }
  const auto& fragment = query_info.fragments.front();
  auto chunk_meta_it = fragment.chunkMetadataMap.find(col_var_->get_column_id());
  CHECK(chunk_meta_it != fragment.chunkMetadataMap.end());
  ChunkKey chunk_key{
      cat_.get_currentDB().dbId, col_var_->get_table_id(), col_var_->get_column_id(), fragment.fragmentId};
  const auto cd = cat_.getMetadataForColumn(col_var_->get_table_id(), col_var_->get_column_id());
  CHECK(!(cd->isVirtualCol));
  const int device_id = fragment.deviceIds[static_cast<int>(memory_level_)];
  const auto chunk = Chunk_NS::Chunk::getChunk(cd,
                                               &cat_.get_dataMgr(),
                                               chunk_key,
                                               memory_level_,
                                               memory_level_ == Data_Namespace::CPU_LEVEL ? 0 : device_id,
                                               chunk_meta_it->second.numBytes,
                                               chunk_meta_it->second.numElements);
  CHECK(chunk);
  auto ab = chunk->get_buffer();
  CHECK(ab->getMemoryPtr());
  const auto col_buff = reinterpret_cast<int8_t*>(ab->getMemoryPtr());
  const int32_t groups_buffer_entry_count = col_range_.int_max - col_range_.int_min + 1;
  if (memory_level_ == Data_Namespace::CPU_LEVEL) {
    cpu_hash_table_buff_.resize(2 * groups_buffer_entry_count);
    err = init_hash_join_buff(&cpu_hash_table_buff_[0],
                              groups_buffer_entry_count,
                              col_buff,
                              chunk_meta_it->second.numElements,
                              col_var_->get_type_info().get_size(),
                              col_range_.int_min);
  } else {
#ifdef HAVE_CUDA
    CHECK_EQ(Data_Namespace::GPU_LEVEL, memory_level_);
    auto& data_mgr = cat_.get_dataMgr();
    gpu_hash_table_buff_ = alloc_gpu_mem(&data_mgr, 2 * groups_buffer_entry_count * sizeof(int64_t), device_id);
    auto dev_err_buff = alloc_gpu_mem(&data_mgr, sizeof(int), device_id);
    copy_to_gpu(&data_mgr, dev_err_buff, &err, sizeof(err), device_id);
    init_hash_join_buff_on_device(reinterpret_cast<int64_t*>(gpu_hash_table_buff_),
                                  reinterpret_cast<int*>(dev_err_buff),
                                  groups_buffer_entry_count,
                                  col_buff,
                                  chunk_meta_it->second.numElements,
                                  col_var_->get_type_info().get_size(),
                                  col_range_.int_min,
                                  executor_->blockSize(),
                                  executor_->gridSize());
    copy_from_gpu(&data_mgr, &err, dev_err_buff, sizeof(err), device_id);
#else
    CHECK(false);
#endif
  }
  return err;
}

llvm::Value* JoinHashTable::codegenSlot(llvm::Value* key_val, const Executor* executor) {
#ifdef HAVE_CUDA
  const int64_t join_hash_buff_ptr = memory_level_ == Data_Namespace::CPU_LEVEL
                                         ? reinterpret_cast<int64_t>(&cpu_hash_table_buff_[0])
                                         : gpu_hash_table_buff_;
#else
  CHECK_EQ(Data_Namespace::CPU_LEVEL, memory_level_);
  const int64_t join_hash_buff_ptr = reinterpret_cast<int64_t>(&cpu_hash_table_buff_[0]);
#endif
  const auto i64_ty = get_int_type(64, executor->cgen_state_->context_);
  const auto hash_ptr = llvm::ConstantInt::get(i64_ty, join_hash_buff_ptr);
  // TODO(alex): maybe make the join hash table buffer a parameter (or a hoisted literal?),
  //             otoh once we fully set up the join hash table caching it won't change often
  return executor->cgen_state_->emitCall(
      "hash_join_idx", {hash_ptr, key_val, executor->ll_int(col_range_.int_min), executor->ll_int(col_range_.int_max)});
}