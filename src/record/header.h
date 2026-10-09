#pragma once

#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <span>
#include <vector>

#include <ntfs-browser/strategy.h>

#include "../internal-export.h"
#include "data/file-record-header.h"

namespace NtfsBrowser {

struct AttrHeaderCommon;

namespace Record {

template <Strategy S>
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
  // Returns nullptr if offset_of_attr doesn't fit in the record buffer.
  [[nodiscard]] const AttrHeaderCommon* HeaderCommon() const noexcept;

  [[nodiscard]] virtual const Data::FileRecordHeader* GetData() const = 0;
};

template <Strategy S>
struct HeaderImpl {};

template <>
struct NTFS_BROWSER_EXPORT_TESTS_ONLY
    HeaderImpl<Strategy::NoCache> : public Header {
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
    HeaderImpl<Strategy::FullCache> : public Header {
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
}  // namespace NtfsBrowser
