#pragma once

#include <ntfs-browser/win-types.h>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include <ntfs-browser/data/attr-type.h>
#include <ntfs-browser/export.h>
#include <ntfs-browser/strategy.h>

namespace NtfsBrowser {

template <Cache::Strategy S>
class FileRecord;
template <Cache::Strategy S>
class NtfsVolume;

namespace Cache {

enum class Strategy : std::uint8_t;

}  // namespace Cache

namespace Attr {

struct HeaderCommon;

}  // namespace Attr

template <Cache::Strategy S>
class NTFS_BROWSER_EXPORT AttrBase {
 public:
  AttrBase(const Attr::HeaderCommon& ahc,
           const FileRecord<S>& file_record) noexcept;
  AttrBase(AttrBase&& other) noexcept = delete;
  AttrBase(const AttrBase& other) = delete;
  AttrBase& operator=(AttrBase&& other) noexcept = delete;
  AttrBase& operator=(const AttrBase& other) = delete;
  virtual ~AttrBase() = default;

 protected:
  // NOLINTBEGIN(cppcoreguidelines-non-private-member-variables-in-classes)
  const NtfsVolume<S>& volume_;
  // NOLINTEND(cppcoreguidelines-non-private-member-variables-in-classes)

 private:
  const Attr::HeaderCommon& attr_header_;
  // GetAttrName()'s decoded name, cached since it is const. The on-disk
  // bytes are raw UTF-16 code units (WORD), which is not what wchar_t is
  // made of once it is wider than 16 bits, so this is an owned decode, not
  // a view into attr_header_. AttrBase is never moved or copied, so a
  // member is safe to alias.
  mutable std::wstring attr_name_cache_;

 public:
  [[nodiscard]] const Attr::HeaderCommon& GetAttrHeader() const noexcept;
  [[nodiscard]] Attr::Type GetAttrType() const noexcept;
  [[nodiscard]] DWORD GetAttrTotalSize() const noexcept;
  [[nodiscard]] bool IsNonResident() const noexcept;
  [[nodiscard]] WORD GetAttrFlags() const noexcept;
  [[nodiscard]] std::wstring_view GetAttrName() const;
  [[nodiscard]] bool IsUnNamed() const noexcept;

 protected:
  [[nodiscard]] WORD GetSectorSize() const noexcept;
  [[nodiscard]] DWORD GetClusterSize() const noexcept;
  [[nodiscard]] DWORD GetIndexBlockSize() const noexcept;

 public:
  [[nodiscard]] virtual const BYTE* GetData() const noexcept = 0;
  [[nodiscard]] virtual ULONGLONG GetDataSize() const noexcept = 0;
  // Bytes reserved on disk for this attribute. For a resident attribute this
  // is the space NTFS reserves for it within the attribute record, padded to
  // the record's own alignment - not necessarily GetDataSize(), which is the
  // real content length alone.
  [[nodiscard]] virtual ULONGLONG GetAllocatedSize() const noexcept = 0;
  [[nodiscard]] virtual std::optional<ULONGLONG>
      ReadData(ULONGLONG offset, const std::span<BYTE>& buffer) const = 0;
};  // AttrBase

}  // namespace NtfsBrowser
