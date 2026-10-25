#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <ntfs-browser/attr/base.h>
#include <ntfs-browser/attr/header-common.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/io/file-record.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>

#include "log/ntfs-common.h"
#include "ntfs-browser/alloc/types.h"
#include "utf/utf.h"
#include "util/small-pool.h"

namespace NTFS_BROWSER_NAMESPACE {
namespace Io {

template <Cache::Strategy S>
class FileRecord;

}  // namespace Io
}  // namespace NTFS_BROWSER_NAMESPACE

namespace NTFS_BROWSER_NAMESPACE::Attr {

enum class Type : DWORD;

template <Cache::Strategy S>
Attr::AttrBase<S>::AttrBase(const Attr::HeaderCommon& ahc,
                            const Io::FileRecord<S>& file_record) noexcept
    : volume_(file_record.GetVolume()), attr_header_(ahc) {}

template <Cache::Strategy S>
void* Attr::AttrBase<S>::operator new(std::size_t size) {
  return Util::SmallBlockPool::Allocate(size);
}

template <Cache::Strategy S>
void Attr::AttrBase<S>::operator delete(void* block,
                                        std::size_t size) noexcept {
  Util::SmallBlockPool::Deallocate(block, size);
}

template <Cache::Strategy S>
const Attr::HeaderCommon& Attr::AttrBase<S>::GetAttrHeader() const noexcept {
  return attr_header_;
}

template <Cache::Strategy S>
Attr::Type Attr::AttrBase<S>::GetAttrType() const noexcept {
  return attr_header_.type;
}

template <Cache::Strategy S>
DWORD Attr::AttrBase<S>::GetAttrTotalSize() const noexcept {
  return attr_header_.total_size;
}

template <Cache::Strategy S>
bool Attr::AttrBase<S>::IsNonResident() const noexcept {
  return attr_header_.non_resident != 0;
}

template <Cache::Strategy S>
WORD Attr::AttrBase<S>::GetAttrFlags() const noexcept {
  return attr_header_.flags;
}

// Get UNICODE Attribute name
template <Cache::Strategy S>
std::wstring_view Attr::AttrBase<S>::GetAttrName() const {
  if (attr_header_.name_length == 0) {
    Log::Trace("Attribute is unnamed");
    return {};
  }

  if (static_cast<ULONGLONG>(attr_header_.name_offset) +
          (static_cast<ULONGLONG>(attr_header_.name_length) * sizeof(WCHAR)) >
      attr_header_.total_size) {
    Log::Warn("Attribute name exceeds attribute bounds.");
    return {};
  }

  // The name sits at name_offset bytes past the header, as raw on-disk
  // UTF-16 (WCHAR, always 16 bits) - not wchar_t, wider than that off
  // Windows, so this decodes rather than reinterpret_casts.
  const std::span<const BYTE> attr(reinterpret_cast<const BYTE*>(&attr_header_),
                                   attr_header_.total_size);
  // The bounds check above puts name_offset below total_size.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
  const auto* const name = &attr[attr_header_.name_offset];
  attr_name_cache_ = Utf::Utf16ToWide(std::u16string_view(
      reinterpret_cast<const char16_t*>(name), attr_header_.name_length));

  Log::Trace("Unicode Attribute Name");
  return attr_name_cache_;
}

// Verify if this attribute is unnamed
// Useful in analyzing MultiStream files
template <Cache::Strategy S>
bool Attr::AttrBase<S>::IsUnNamed() const noexcept {
  return attr_header_.name_length == 0;
}

// Resident data lives in the file record: it owns no cluster.
template <Cache::Strategy S>
Alloc::StreamExtents Attr::AttrBase<S>::GetExtents() const {
  return {};
}

// The whole data, as one range: right for an attribute that has no run list.
template <Cache::Strategy S>
Alloc::StreamRanges Attr::AttrBase<S>::GetRealRanges() const {
  const ULONGLONG size = GetDataSize();
  if (size == 0) {
    return {};
  }
  return {.ranges = {{.offset = 0, .length = size}}, .complete = true};
}

template <Cache::Strategy S>
WORD Attr::AttrBase<S>::GetSectorSize() const noexcept {
  return volume_.GetSectorSize();
}

template <Cache::Strategy S>
DWORD Attr::AttrBase<S>::GetClusterSize() const noexcept {
  return volume_.GetClusterSize();
}

template <Cache::Strategy S>
DWORD Attr::AttrBase<S>::GetIndexBlockSize() const noexcept {
  return volume_.GetIndexBlockSize();
}

template class AttrBase<Cache::Strategy::NoCache>;
template class AttrBase<Cache::Strategy::FullCache>;

}  // namespace NTFS_BROWSER_NAMESPACE::Attr
