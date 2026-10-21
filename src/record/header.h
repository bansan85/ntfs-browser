#pragma once

#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <span>
#include <vector>

#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/namespace.h>

#include "../internal-export.h"
#include "data/file-record-header.h"

namespace NTFS_BROWSER_NAMESPACE {

namespace Attr {

struct HeaderCommon;

}  // namespace Attr

namespace Record {

template <Cache::Strategy S>
struct HeaderImpl;

// Validates a file record's header and applies its update sequence fixups.
struct NTFS_BROWSER_EXPORT_TESTS_ONLY Header {
  WORD us_number{0};
  std::vector<WORD> us_array;
  // Actual buffer size this instance was constructed with.
  size_t buffer_size;

  explicit Header(std::span<const BYTE> buffer);
  Header(const Header&) = delete;
  Header& operator=(const Header&) = delete;
  Header(Header&&) = delete;
  Header& operator=(Header&&) = delete;
  virtual ~Header() = default;
  // Verify US and update sectors
  [[nodiscard]] bool PatchUS() noexcept;
  // Patches every sector whose last word verifies, and leaves the others as
  // they are: their last word is not the update sequence number nor its saved
  // value, so it is the sector's own, and what it replaced is lost. Returns
  // how many sectors failed.
  [[nodiscard]] size_t PatchUSSalvage() noexcept;

  // How many sectors the update sequence array covers.
  [[nodiscard]] size_t SectorCount() const noexcept { return us_array.size(); }

  // Returns nullptr if offset_of_attr doesn't fit in the record buffer.
  [[nodiscard]] const Attr::HeaderCommon* HeaderCommon() const noexcept;

  [[nodiscard]] virtual const Data::FileRecordHeader* GetData() const = 0;

 private:
  // Shared body of PatchUS() and PatchUSSalvage(). Returns how many sectors
  // failed, or 1 at the first failure when stop_at_first_failure is set.
  [[nodiscard]] size_t PatchSectors(bool stop_at_first_failure) noexcept;
};

template <Cache::Strategy S>
struct HeaderImpl {};

template <>
struct NTFS_BROWSER_EXPORT_TESTS_ONLY
    HeaderImpl<Cache::Strategy::NoCache> : public Header {
  std::span<const BYTE> data;

  explicit HeaderImpl(std::span<const BYTE> buffer);
  HeaderImpl(const HeaderImpl&) = delete;
  HeaderImpl& operator=(const HeaderImpl&) = delete;
  HeaderImpl(HeaderImpl&&) = delete;
  HeaderImpl& operator=(HeaderImpl&&) = delete;
  ~HeaderImpl() override = default;

  [[nodiscard]] const Data::FileRecordHeader* GetData() const override;
};

template <>
struct NTFS_BROWSER_EXPORT_TESTS_ONLY
    HeaderImpl<Cache::Strategy::FullCache> : public Header {
  Data::FileRecordHeader data{};

  explicit HeaderImpl(std::span<const BYTE> buffer);
  HeaderImpl(const HeaderImpl&) = delete;
  HeaderImpl& operator=(const HeaderImpl&) = delete;
  HeaderImpl(HeaderImpl&&) = delete;
  HeaderImpl& operator=(HeaderImpl&&) = delete;
  ~HeaderImpl() override = default;

  [[nodiscard]] const Data::FileRecordHeader* GetData() const override;
};

}  // namespace Record
}  // namespace NTFS_BROWSER_NAMESPACE
