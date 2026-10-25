#pragma once

#include <ntfs-browser/win-types.h>

#include <array>
#include <cassert>
#include <cstddef>
#include <iterator>
#include <span>

#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/namespace.h>

#include "../internal-export.h"
#include "data/file-record-header.h"

namespace NTFS_BROWSER_NAMESPACE {

namespace Attr {

struct HeaderCommon;

}  // namespace Attr

namespace Record {

// The words of an update sequence array, one per sector of the record. A record
// holds 4096 bytes at most, so 8 sectors: the words sit in the object, and a
// header costs the heap no second block.
class UpdateSequenceWords {
 public:
  // Most words an array holds.
  static constexpr size_t max_words =
      Data::FileRecordHeader::max_file_record_size /
      Data::FileRecordHeader::update_sequence_stride;

  // Adds a word. The array MUST NOT be full.
  void push_back(WORD value) noexcept {
    assert(size_ < max_words);
    // The assert above, and the caller, keep size_ below max_words.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    words_[size_++] = value;
  }

  [[nodiscard]] size_t size() const noexcept { return size_; }

  // The word at index. index MUST be below size().
  [[nodiscard]] WORD operator[](size_t index) const noexcept {
    assert(index < size_);
    // The assert above, and the caller, keep index below max_words.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    return words_[index];
  }

  [[nodiscard]] const WORD* begin() const noexcept { return words_.data(); }

  [[nodiscard]] const WORD* end() const noexcept {
    return std::next(words_.data(), static_cast<std::ptrdiff_t>(size_));
  }

 private:
  std::array<WORD, max_words> words_{};
  size_t size_{0};
};

// Validates a file record's header and applies its update sequence fixups.
struct NTFS_BROWSER_EXPORT_TESTS_ONLY Header {
  WORD us_number{0};
  UpdateSequenceWords us_array;
  // Actual buffer size this instance was constructed with.
  size_t buffer_size;

  explicit Header(std::span<const BYTE> buffer);
  Header(const Header&) = delete;
  Header& operator=(const Header&) = delete;
  Header(Header&&) = delete;
  Header& operator=(Header&&) = delete;
  virtual ~Header() = default;

  // A header comes from a pool of small blocks kept by the thread, not from
  // the heap each time: a scan of $MFT makes one for each record.
  static void* operator new(size_t size);
  static void operator delete(void* block, size_t size) noexcept;

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
