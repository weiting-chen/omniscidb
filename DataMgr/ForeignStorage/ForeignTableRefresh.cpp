/*
 * Copyright 2020 OmniSci, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "ForeignTableRefresh.h"

#include "LockMgr/LockMgr.h"

namespace foreign_storage {
void refresh_foreign_table(Catalog_Namespace::Catalog& catalog,
                           const std::string& table_name,
                           const bool evict_cached_entries) {
  auto& data_mgr = catalog.getDataMgr();
  auto table_lock =
      std::make_unique<lockmgr::TableSchemaLockContainer<lockmgr::WriteLock>>(
          lockmgr::TableSchemaLockContainer<lockmgr::WriteLock>::acquireTableDescriptor(
              catalog, table_name, false));

  const TableDescriptor* td = (*table_lock)();
  if (td->storageType != StorageType::FOREIGN_TABLE) {
    throw std::runtime_error{
        table_name +
        " is not a foreign table. Refreshes are applicable to only foreign tables."};
  }

  catalog.removeFragmenterForTable(td->tableId);
  ChunkKey table_key{catalog.getCurrentDB().dbId, td->tableId};
  data_mgr.deleteChunksWithPrefix(table_key, MemoryLevel::CPU_LEVEL);
  data_mgr.deleteChunksWithPrefix(table_key, MemoryLevel::GPU_LEVEL);

  try {
    data_mgr.getPersistentStorageMgr()->getForeignStorageMgr()->refreshTable(
        table_key, evict_cached_entries);
    catalog.updateForeignTableRefreshTimes(td->tableId);
  } catch (PostEvictionRefreshException& e) {
    catalog.updateForeignTableRefreshTimes(td->tableId);
    throw e.getOriginalException();
  }
}

void ForeignTableRefreshScheduler::start(std::atomic<bool>& is_program_running) {
  if (is_program_running && !is_scheduler_running_) {
    is_scheduler_running_ = true;
    scheduler_thread_ = std::thread([&is_program_running]() {
      while (is_program_running && is_scheduler_running_) {
        auto& sys_catalog = Catalog_Namespace::SysCatalog::instance();
        // Exit if scheduler has been stopped asynchronously
        if (!is_program_running || !is_scheduler_running_) {
          return;
        }
        for (const auto& catalog : sys_catalog.getCatalogsForAllDbs()) {
          // Exit if scheduler has been stopped asynchronously
          if (!is_program_running || !is_scheduler_running_) {
            return;
          }
          auto tables = catalog->getAllForeignTablesForRefresh();
          for (auto table : tables) {
            // Exit if scheduler has been stopped asynchronously
            if (!is_program_running || !is_scheduler_running_) {
              return;
            }
            try {
              refresh_foreign_table(*catalog, table->tableName, false);
            } catch (std::runtime_error& e) {
              LOG(ERROR) << "Scheduled refresh for table \"" << table->tableName
                         << "\" resulted in an error. " << e.what();
            }
            has_refreshed_table_ = true;
          }
        }
        // Exit if scheduler has been stopped asynchronously
        if (!is_program_running || !is_scheduler_running_) {
          return;
        }

        // A condition variable is used here (instead of a sleep call)
        // in order to allow for thread wake-up, even in the middle
        // of a wait interval.
        std::unique_lock<std::mutex> wait_lock(wait_mutex_);
        wait_condition_.wait_for(wait_lock, thread_wait_duration_);
      }
    });
  }
}

void ForeignTableRefreshScheduler::stop() {
  if (is_scheduler_running_) {
    is_scheduler_running_ = false;
    wait_condition_.notify_one();
    scheduler_thread_.join();
  }
}

void ForeignTableRefreshScheduler::setWaitDuration(int64_t duration_in_seconds) {
  thread_wait_duration_ = std::chrono::seconds{duration_in_seconds};
}

bool ForeignTableRefreshScheduler::isRunning() {
  return is_scheduler_running_;
}

bool ForeignTableRefreshScheduler::hasRefreshedTable() {
  return has_refreshed_table_;
}

void ForeignTableRefreshScheduler::resetHasRefreshedTable() {
  has_refreshed_table_ = false;
}

std::atomic<bool> ForeignTableRefreshScheduler::is_scheduler_running_{false};
std::chrono::seconds ForeignTableRefreshScheduler::thread_wait_duration_{60};
std::thread ForeignTableRefreshScheduler::scheduler_thread_;
std::atomic<bool> ForeignTableRefreshScheduler::has_refreshed_table_{false};
std::mutex ForeignTableRefreshScheduler::wait_mutex_;
std::condition_variable ForeignTableRefreshScheduler::wait_condition_;
}  // namespace foreign_storage
