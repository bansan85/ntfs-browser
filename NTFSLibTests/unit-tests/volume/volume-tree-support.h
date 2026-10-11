#pragma once

#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <memory>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/attr/type.h>
#include <ntfs-browser/disk-reader.h>
#include <ntfs-browser/mft/idx.h>

#include "fake-ntfs-image.h"
#include "memory-disk-reader.h"

namespace Attr = NtfsBrowser::Attr;
namespace Mft = NtfsBrowser::Mft;
using NtfsBrowserTests::MemoryDiskReader;

namespace {

// $MFT starts at byte 1024: lcn_mft 1 times the fake 1 KiB cluster.
constexpr size_t mft_base_offset = 1024;

// Offset of offset_of_attr, a WORD in the file record header.
constexpr size_t record_first_attr_field = 20;

// Offset of an attribute header's total_size DWORD.
constexpr size_t attr_total_size_field = 4;

// Offset of a resident attribute's body offset, a WORD.
constexpr size_t attr_value_offset_field = 20;

// Marks the end of a file record's attribute list.
constexpr DWORD end_of_attrs_marker = 0xFFFFFFFF;

// Bytes of a file record: the fixture's record size.
constexpr size_t record_bytes = NtfsBrowserTests::fake_file_record_size;

// Record number of the root directory.
constexpr ULONGLONG root_idx = static_cast<ULONGLONG>(Mft::Idx::Root);

template <typename T>
[[nodiscard]] T Load(const std::vector<BYTE>& image, size_t offset) {
  T value{};
  std::memcpy(&value, &image.at(offset), sizeof(T));
  return value;
}

template <typename T>
void Store(std::vector<BYTE>& image, size_t offset, T value) {
  std::memcpy(&image.at(offset), &value, sizeof(T));
}

[[nodiscard]] inline size_t RecordOffset(ULONGLONG idx) {
  return mft_base_offset + record_bytes * static_cast<size_t>(idx);
}

// Zeroes one whole file record, so its magic no longer matches.
inline void ZeroRecord(std::vector<BYTE>& image, ULONGLONG idx) {
  std::fill_n(image.begin() + static_cast<std::ptrdiff_t>(RecordOffset(idx)),
              record_bytes, BYTE{0});
}

// Byte offsets of every attribute of the given type in record idx, in order.
[[nodiscard]] inline std::vector<size_t>
    AttrOffsets(const std::vector<BYTE>& image, ULONGLONG idx,
                Attr::Type type) {
  std::vector<size_t> found;
  const size_t record = RecordOffset(idx);
  const size_t record_end = record + record_bytes;
  size_t offset = record + Load<WORD>(image, record + record_first_attr_field);
  while (offset + (2 * sizeof(DWORD)) <= record_end) {
    const auto attr_type = Load<DWORD>(image, offset);
    const auto total_size = Load<DWORD>(image, offset + attr_total_size_field);
    if (attr_type == end_of_attrs_marker || total_size == 0) {
      break;
    }
    if (attr_type == static_cast<DWORD>(type)) {
      found.push_back(offset);
    }
    offset += total_size;
  }
  return found;
}

// Byte offset of the first attribute of the given type; fails when absent.
[[nodiscard]] inline size_t FirstAttr(const std::vector<BYTE>& image,
                                      ULONGLONG idx, Attr::Type type) {
  const std::vector<size_t> offsets = AttrOffsets(image, idx, type);
  REQUIRE(!offsets.empty());
  return offsets.front();
}

// Byte offset of a resident attribute's body.
[[nodiscard]] inline size_t ResidentBody(const std::vector<BYTE>& image,
                                         size_t attr) {
  return attr + Load<WORD>(image, attr + attr_value_offset_field);
}

// Serves a fake image, refusing every read that overlaps [begin, end).
class RefusingDiskReader : public NtfsBrowser::IDiskReader {
 public:
  RefusingDiskReader(std::vector<BYTE> image, ULONGLONG begin, ULONGLONG end,
                     std::shared_ptr<size_t> refused)
      : inner_(std::move(image)),
        begin_(begin),
        end_(end),
        refused_(std::move(refused)) {}

  bool Open(std::wstring_view /*path*/) override { return false; }

  [[nodiscard]] bool ReadInto(LARGE_INTEGER& addr,
                              std::span<BYTE> dest) const override {
    const auto start = static_cast<ULONGLONG>(addr.QuadPart);
    if (start < end_ && start + dest.size() > begin_) {
      ++*refused_;
      return false;
    }
    return inner_.ReadInto(addr, dest);
  }

 private:
  MemoryDiskReader inner_;
  ULONGLONG begin_;
  ULONGLONG end_;
  std::shared_ptr<size_t> refused_;
};

}  // namespace
