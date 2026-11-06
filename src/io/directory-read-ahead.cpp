#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <utility>

#include <ntfs-browser/attr/base.h>
#include <ntfs-browser/attr/type.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/io/directory-read-ahead.h>
#include <ntfs-browser/io/file-record.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/tuning/tuning.h>

#include "attr/index-alloc.h"
#include "io/batch-reader.h"
#include "io/file-record-index.h"
#include "io/probe-lane.h"
#include "io/read-ahead-held.h"

namespace NTFS_BROWSER_NAMESPACE::Io {

template <Cache::Strategy S>
class DirectoryReadAhead<S>::Impl {
 public:
  explicit Impl(const NtfsVolume<S>& volume)
      : volume_(&volume),
        walk_(volume.GetOptions().tuning.walk),
        // A FullCache volume holds what it read in its block cache already.
        shared_(walk_, S == Cache::Strategy::NoCache,
                volume.GetFileRecordSize()),
        lane_(volume, shared_),
        batch_(volume, shared_, lane_) {}

  Impl(const Impl&) = delete;
  Impl& operator=(const Impl&) = delete;
  Impl(Impl&&) = delete;
  Impl& operator=(Impl&&) = delete;

  ~Impl() { Detach(); }

  [[nodiscard]] bool Enabled() const noexcept {
    return walk_.read_ahead_dirs != 0;
  }

  [[nodiscard]] std::size_t Window() const noexcept {
    return walk_.read_ahead_dirs;
  }

  void Attach(FileRecord<S>& listing);
  void Begin();
  void Plan(std::span<const ULONGLONG> upcoming);
  void Release(ULONGLONG directory);

  [[nodiscard]] DirectoryReadAheadStats GetStats() const noexcept {
    DirectoryReadAheadStats stats = shared_.stats;
    stats.held_bytes = shared_.store.Bytes();
    return stats;
  }

 private:
  // The bytes the listing's record asks for, empty when not held.
  [[nodiscard]] std::span<const BYTE> HeldRecordBytes(ULONGLONG record);

  // Takes the sources off the attached record, and off its current index
  // allocation.
  void Detach() noexcept;

  const NtfsVolume<S>* volume_;
  Tuning::WalkTuning walk_;
  ReadAheadShared shared_;
  ProbeLane<S> lane_;
  BatchReader<S> batch_;
  FileRecord<S>* listing_{nullptr};
};

template <Cache::Strategy S>
std::span<const BYTE>
    DirectoryReadAhead<S>::Impl::HeldRecordBytes(ULONGLONG record) {
  const std::span<const BYTE> held = shared_.store.ServeRecord(record);
  return held.empty() ? shared_.keeper.FindInWindow(record) : held;
}

template <Cache::Strategy S>
void DirectoryReadAhead<S>::Impl::Attach(FileRecord<S>& listing) {
  if (!Enabled()) {
    return;
  }
  listing_ = &listing;
  lane_.Attach(listing);
  listing.SetRecordSource(
      [this](ULONGLONG record) { return HeldRecordBytes(record); });
  listing.SetIndexBlockSource([this](ULONGLONG directory, ULONGLONG vcn) {
    return shared_.store.ServeBlock(directory, vcn);
  });
  // The three parts of the store work apart: a FullCache volume keeps no
  // window, and gets a store that only notes the runs read.
  typename FileRecord<S>::ProbeStore store;
  if (shared_.keeper.Enabled()) {
    store.find = [this](ULONGLONG record) {
      return shared_.keeper.FindForProbe(record);
    };
    store.keep = [this](ULONGLONG first_record, std::span<const BYTE> bytes) {
      shared_.keeper.KeepRun(first_record, bytes);
    };
  }
  if (lane_.On()) {
    store.ran = [this](ULONGLONG first_slot, ULONGLONG count) {
      lane_.RunRead(first_slot, count);
    };
  }
  if (store.find || store.keep || store.ran) {
    listing.SetProbeStore(std::move(store));
  }
}

template <Cache::Strategy S>
void DirectoryReadAhead<S>::Impl::Detach() noexcept {
  lane_.Detach();
  if (listing_ == nullptr) {
    return;
  }
  listing_->SetRecordSource({});
  listing_->SetIndexBlockSource({});
  listing_->SetProbeStore({});
  for (const auto& attr : listing_->GetAttr(Attr::Type::IndexAllocation)) {
    static_cast<Attr::AttrIndexAlloc<S>*>(attr.get())->SetBlockSource({}, 0);
  }
  listing_ = nullptr;
}

template <Cache::Strategy S>
void DirectoryReadAhead<S>::Impl::Release(ULONGLONG directory) {
  lane_.Release(directory);
  shared_.store.Release(directory);
}

template <Cache::Strategy S>
void DirectoryReadAhead<S>::Impl::Plan(std::span<const ULONGLONG> upcoming) {
  const DWORD record_size = volume_->GetFileRecordSize();
  if (!Enabled() || listing_ == nullptr || record_size == 0 ||
      !volume_->HasMftBase()) {
    return;
  }
  batch_.ReadUpcoming(upcoming);
  // The stack changed: the lane follows it.
  lane_.Reorder(upcoming);
}

template <Cache::Strategy S>
void DirectoryReadAhead<S>::Impl::Begin() {
  if (!Enabled() || listing_ == nullptr || volume_->HasAttrRawCB()) {
    return;
  }
  const std::optional<ULONGLONG> directory = listing_->GetFileReference();
  if (!directory || shared_.store.Contains(*directory)) {
    return;
  }
  std::optional<typename DirectoryIndex<S>::RootChildren> children =
      DirectoryIndex<S>(*listing_).RootChildBlocks();
  if (!children) {
    // The index sits in the record: there is no block to read ahead, but the
    // probes of its entries are known.
    lane_.PlanRunsOf(*directory, *listing_);
    lane_.Refill();
    return;
  }

  batch_.ReadRootBlocks(*directory, children->alloc, std::move(children->vcns));
  lane_.PlanRunsOf(*directory, *listing_);

  // The attributes already exist: they take their source from here on.
  for (const auto& attr : listing_->GetAttr(Attr::Type::IndexAllocation)) {
    static_cast<Attr::AttrIndexAlloc<S>*>(attr.get())
        ->SetBlockSource(
            [this](ULONGLONG owner, ULONGLONG vcn) {
              return shared_.store.ServeBlock(owner, vcn);
            },
            *directory);
  }
  lane_.Refill();
}

template <Cache::Strategy S>
DirectoryReadAhead<S>::DirectoryReadAhead(const NtfsVolume<S>& volume)
    : impl_(std::make_unique<Impl>(volume)) {}

template <Cache::Strategy S>
DirectoryReadAhead<S>::~DirectoryReadAhead() = default;

template <Cache::Strategy S>
bool DirectoryReadAhead<S>::Enabled() const noexcept {
  return impl_->Enabled();
}

template <Cache::Strategy S>
std::size_t DirectoryReadAhead<S>::Window() const noexcept {
  return impl_->Window();
}

template <Cache::Strategy S>
void DirectoryReadAhead<S>::Attach(FileRecord<S>& listing) {
  impl_->Attach(listing);
}

template <Cache::Strategy S>
void DirectoryReadAhead<S>::Begin() {
  impl_->Begin();
}

template <Cache::Strategy S>
void DirectoryReadAhead<S>::Plan(std::span<const ULONGLONG> upcoming) {
  impl_->Plan(upcoming);
}

template <Cache::Strategy S>
void DirectoryReadAhead<S>::Release(ULONGLONG directory) {
  impl_->Release(directory);
}

template <Cache::Strategy S>
DirectoryReadAheadStats DirectoryReadAhead<S>::GetStats() const noexcept {
  return impl_->GetStats();
}

template class DirectoryReadAhead<Cache::Strategy::NoCache>;
template class DirectoryReadAhead<Cache::Strategy::FullCache>;

}  // namespace NTFS_BROWSER_NAMESPACE::Io
