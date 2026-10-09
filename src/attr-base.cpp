#include <ntfs-browser/win-types.h>

#include <span>
#include <string>
#include <string_view>

#include <ntfs-browser/attr-base.h>
#include <ntfs-browser/data/attr-header-common.h>
#include <ntfs-browser/file-record.h>  // IWYU pragma: keep
#include <ntfs-browser/ntfs-volume.h>  // IWYU pragma: keep
#include <ntfs-browser/strategy.h>

#include "ntfs-common.h"
#include "utf.h"

namespace NtfsBrowser {

namespace Attr {

enum class Type : DWORD;

}  // namespace Attr

template <Cache::Strategy S>
AttrBase<S>::AttrBase(const Attr::HeaderCommon& ahc,
                      const FileRecord<S>& file_record) noexcept
    : volume_(file_record.GetVolume()), attr_header_(ahc) {}

template <Cache::Strategy S>
const Attr::HeaderCommon& AttrBase<S>::GetAttrHeader() const noexcept {
  return attr_header_;
}

template <Cache::Strategy S>
Attr::Type AttrBase<S>::GetAttrType() const noexcept {
  return attr_header_.type;
}

template <Cache::Strategy S>
DWORD AttrBase<S>::GetAttrTotalSize() const noexcept {
  return attr_header_.total_size;
}

template <Cache::Strategy S>
bool AttrBase<S>::IsNonResident() const noexcept {
  return attr_header_.non_resident != 0;
}

template <Cache::Strategy S>
WORD AttrBase<S>::GetAttrFlags() const noexcept {
  return attr_header_.flags;
}

// Get UNICODE Attribute name
template <Cache::Strategy S>
std::wstring_view AttrBase<S>::GetAttrName() const {
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
bool AttrBase<S>::IsUnNamed() const noexcept {
  return attr_header_.name_length == 0;
}

template <Cache::Strategy S>
WORD AttrBase<S>::GetSectorSize() const noexcept {
  return volume_.GetSectorSize();
}

template <Cache::Strategy S>
DWORD AttrBase<S>::GetClusterSize() const noexcept {
  return volume_.GetClusterSize();
}

template <Cache::Strategy S>
DWORD AttrBase<S>::GetIndexBlockSize() const noexcept {
  return volume_.GetIndexBlockSize();
}

template class AttrBase<Cache::Strategy::NoCache>;
template class AttrBase<Cache::Strategy::FullCache>;

}  // namespace NtfsBrowser
