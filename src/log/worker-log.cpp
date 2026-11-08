#include "log/worker-log.h"

#include <cstddef>
#include <mutex>

#include <ntfs-browser/namespace.h>

#include "log/ntfs-common.h"

namespace NTFS_BROWSER_NAMESPACE::Log::Detail {

void WorkerLog::Prepare(LevelMask levels) noexcept {
  const std::scoped_lock lock(mutex_);
  levels_ = levels;
  for (Slot& slot : slots_) {
    slot.log.Reset(levels);
    slot.from = {};
    slot.to = {};
  }
  fill_ = 0;
  ready_ = false;
  reading_ = false;
  finished_ = false;
  recovered_base_ = 0;
  requested_ = false;
}

void WorkerLog::Begin() noexcept {
  const std::scoped_lock lock(mutex_);
  recovered_base_ = RecoveredCount();
  scope_.emplace(slots_.at(fill_).log);
}

void WorkerLog::Checkpoint() noexcept {
  if (!requested_.load(std::memory_order_relaxed)) {
    return;
  }
  const std::scoped_lock lock(mutex_);
  requested_ = false;
  if (ready_ || reading_ || slots_.at(fill_).log.End().lines == 0) {
    return;
  }
  HandOver();
}

void WorkerLog::End() noexcept {
  const std::scoped_lock lock(mutex_);
  Slot& slot = slots_.at(fill_);
  slot.to = slot.log.End();
  slot.from = {.lines = 0, .recovered = recovered_base_};
  scope_.reset();
  finished_ = true;
}

void WorkerLog::Replay() noexcept {
  // At most two slots hold lines: one handed over, and the last one of a
  // finished worker.
  for (std::size_t round = 0; round < 2; ++round) {
    Slot* const slot = TakeReady();
    if (slot == nullptr) {
      return;
    }
    slot->log.Replay(slot->from, slot->to);
    Release(*slot);
  }
}

WorkerLog::Slot* WorkerLog::TakeReady() noexcept {
  const std::scoped_lock lock(mutex_);
  if (ready_) {
    ready_ = false;
    reading_ = true;
    return &slots_.at(1 - fill_);
  }
  if (finished_ && slots_.at(fill_).to.lines != 0) {
    // The worker is gone: its slot is no longer in use. The marks of an empty
    // slot are left alone, so a slot is taken once.
    Slot& taken = slots_.at(fill_);
    fill_ = 1 - fill_;
    reading_ = true;
    return &taken;
  }
  requested_ = true;
  return nullptr;
}

void WorkerLog::Release(Slot& slot) noexcept {
  // Only the owner touches the slot until reading_ is cleared.
  slot.log.Reset(levels_);
  slot.from = {};
  slot.to = {};
  const std::scoped_lock lock(mutex_);
  reading_ = false;
}

void WorkerLog::HandOver() noexcept {
  Slot& slot = slots_.at(fill_);
  slot.to = slot.log.End();
  slot.from = {.lines = 0, .recovered = recovered_base_};
  recovered_base_ = slot.to.recovered;
  ready_ = true;
  fill_ = 1 - fill_;
  scope_.reset();
  scope_.emplace(slots_.at(fill_).log);
}

}  // namespace NTFS_BROWSER_NAMESPACE::Log::Detail
