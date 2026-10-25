#pragma once

#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include <ntfs-browser/alloc/types.h>
#include <ntfs-browser/attr/type.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/export.h>
#include <ntfs-browser/namespace.h>

namespace NTFS_BROWSER_NAMESPACE {

template <Cache::Strategy S>
class NtfsVolume;

namespace Cache {

enum class Strategy : std::uint8_t;

}  // namespace Cache

namespace Io {

template <Cache::Strategy S>
class FileRecord;

}  // namespace Io

namespace Attr {

struct HeaderCommon;

template <Cache::Strategy S>
class NTFS_BROWSER_EXPORT AttrBase {
 public:
  AttrBase(const Attr::HeaderCommon& ahc,
           const Io::FileRecord<S>& file_record) noexcept;
  AttrBase(AttrBase&& other) noexcept = delete;
  AttrBase(const AttrBase& other) = delete;
  AttrBase& operator=(AttrBase&& other) noexcept = delete;
  AttrBase& operator=(const AttrBase& other) = delete;
  virtual ~AttrBase() = default;

  // An attribute comes from a pool of small blocks kept by the thread, not from
  // the heap each time: a scan of $MFT makes and drops several of them for each
  // record.
  static void* operator new(std::size_t size);
  static void operator delete(void* block, std::size_t size) noexcept;

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
  // The real clusters this attribute's data lives on. Resident data owns none.
  // A non-resident attribute lists the runs that map clusters, sparse runs
  // left out, and is not complete when its run list is shorter than the
  // allocated size it declares, or holds a damaged run.
  [[nodiscard]] virtual Alloc::StreamExtents GetExtents() const;
  // The real (non-sparse) byte ranges of this attribute's data, in its own
  // address space, sorted and cut at GetDataSize(). Resident data is one range
  // that covers all of it. Never reads the disk, so a stream with a huge sparse
  // head costs nothing to ask about. Not complete when a run list is short or
  // damaged, as for GetExtents().
  [[nodiscard]] virtual Alloc::StreamRanges GetRealRanges() const;
};  // AttrBase

}  // namespace Attr

}  // namespace NTFS_BROWSER_NAMESPACE
