#pragma once

#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>

#include <windows.h>

#include <ntfs-browser/namespace.h>

#include "disk-reader.h"
#include "internal-export.h"
#include "io/win32-prefetcher.h"
#include "tuning/tuning.h"

namespace NTFS_BROWSER_NAMESPACE::Io {

// Production IDiskReader: a real disk/device, or an ordinary file treated
// the same way (CreateFileW handles both identically). The library uses it
// through FileReader<S>::Open(); the unit tests reach it directly.
class NTFS_BROWSER_EXPORT_TESTS_ONLY Win32DiskReader : public IDiskReader {
 public:
  // caps are the read-ahead caps of each pattern, M1 in force until
  // SetReadPattern() says otherwise. The defaults are the library's own.
  explicit Win32DiskReader(const ReaderCaps& caps = {});
  ~Win32DiskReader() override;
  Win32DiskReader(const Win32DiskReader&) = delete;
  Win32DiskReader& operator=(const Win32DiskReader&) = delete;
  Win32DiskReader(Win32DiskReader&&) = delete;
  Win32DiskReader& operator=(Win32DiskReader&&) = delete;

  // Whether Open() asks the volume that hosts a plain file for its media kind
  // when the file itself does not say. On by default. Call it before Open().
  void SetAskHostVolume(bool ask) noexcept { ask_host_volume_ = ask; }

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

  // Announces a range on the standing lane: a second queue, read ahead under
  // the same caps as the first, that CancelPrefetch() and the reads of the
  // first queue leave alone. See IDiskReader::PrefetchStanding().
  void PrefetchStanding(ULONGLONG offset, ULONGLONG length) const override;

  // Withdraws the ranges of the standing lane that overlap the interval.
  void CancelStanding(ULONGLONG offset, ULONGLONG length) const override;

  // Switches the read-ahead caps to those of pattern.
  void SetReadPattern(Tuning::Pattern pattern) const override;

 private:
  using HandlePtr =
      std::unique_ptr<std::remove_pointer_t<HANDLE>, decltype(&::CloseHandle)>;

  // Reads from the disk through the main handle, for a read that no range read
  // ahead holds, and notes it when a session measures. False when it fails.
  [[nodiscard]] bool ReadSynchronously(const LARGE_INTEGER& addr,
                                       std::span<BYTE> dest) const;

  HandlePtr handle_;
  Tuning::MediaKind media_kind_{Tuning::MediaKind::Unknown};
  bool ask_host_volume_{true};
  ReaderCaps caps_;
  // Null until Open() succeeds. Declared after handle_: it goes first.
  std::unique_ptr<Win32Prefetcher> prefetcher_;
};

}  // namespace NTFS_BROWSER_NAMESPACE::Io
