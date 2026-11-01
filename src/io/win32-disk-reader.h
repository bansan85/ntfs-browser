#pragma once

#include <array>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>

#include <windows.h>

#include <ntfs-browser/disk-reader.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/tuning/tuning.h>

namespace NTFS_BROWSER_NAMESPACE::Io {

// The read-ahead caps of each read pattern.
using ReaderCaps = std::array<Tuning::ReaderTuning, Tuning::pattern_count>;

// Production IDiskReader: a real disk/device, or an ordinary file treated
// the same way (CreateFileW handles both identically). Never used directly
// outside FileReader<S>::Open(), so it carries no export macro.
class Win32DiskReader : public IDiskReader {
 public:
  // caps are the read-ahead caps of each pattern, M1 in force until
  // SetReadPattern() says otherwise. The defaults are the library's own.
  explicit Win32DiskReader(const ReaderCaps& caps = {});
  ~Win32DiskReader() override;
  Win32DiskReader(const Win32DiskReader&) = delete;
  Win32DiskReader& operator=(const Win32DiskReader&) = delete;
  Win32DiskReader(Win32DiskReader&&) = delete;
  Win32DiskReader& operator=(Win32DiskReader&&) = delete;

  bool Open(std::wstring_view path) override;

  // Reads from addr into dest: from a range read ahead when one holds those
  // bytes, else from the disk.
  [[nodiscard]] bool ReadInto(LARGE_INTEGER& addr,
                              std::span<BYTE> dest) const override;

  // The length of a disk or volume handle, or the size of an ordinary file.
  [[nodiscard]] std::optional<ULONGLONG> Size() const override;

  // What the device behind the handle says: a volume or a disk reports whether
  // it incurs a seek penalty. A plain file, or a device that does not answer,
  // is Unknown. Asked once, at Open().
  [[nodiscard]] Tuning::MediaKind GetMediaKind() const override;

  // Reads the announced ranges ahead, several at once, through a second,
  // asynchronous handle. The reads run while the caller works: no thread is
  // involved.
  void Prefetch(ULONGLONG offset, ULONGLONG length) const override;

  void CancelPrefetch() const override;

  // Switches the read-ahead caps to those of pattern.
  void SetReadPattern(Tuning::Pattern pattern) const override;

 private:
  using HandlePtr =
      std::unique_ptr<std::remove_pointer_t<HANDLE>, decltype(&::CloseHandle)>;

  class Prefetcher;

  HandlePtr handle_;
  Tuning::MediaKind media_kind_{Tuning::MediaKind::Unknown};
  ReaderCaps caps_;
  // Null until Open() succeeds. Declared after handle_: it goes first.
  std::unique_ptr<Prefetcher> prefetcher_;
};

}  // namespace NTFS_BROWSER_NAMESPACE::Io
