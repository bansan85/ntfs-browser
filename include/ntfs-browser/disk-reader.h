#pragma once

#include <ntfs-browser/win-types.h>

#include <optional>
#include <span>
#include <string_view>

#include <ntfs-browser/namespace.h>

namespace NTFS_BROWSER_NAMESPACE {

// Abstracts "get raw bytes from a backing store" so FileReader<S> doesn't
// depend on a Win32 file/device handle directly. An implementation decides
// what addr/dest mean (eg. a disk offset, or an ignored parameter).
class IDiskReader {
 public:
  IDiskReader() = default;
  IDiskReader(const IDiskReader&) = delete;
  IDiskReader& operator=(const IDiskReader&) = delete;
  IDiskReader(IDiskReader&&) = delete;
  IDiskReader& operator=(IDiskReader&&) = delete;
  virtual ~IDiskReader() = default;

  // Opens the backing store at path.
  virtual bool Open(std::wstring_view path) = 0;

  // Reads from addr into dest.
  [[nodiscard]] virtual bool ReadInto(LARGE_INTEGER& addr,
                                      std::span<BYTE> dest) const = 0;

  // Size of the backing store in bytes, or nullopt when it has no fixed size.
  [[nodiscard]] virtual std::optional<ULONGLONG> Size() const {
    return std::nullopt;
  }

  // Announces that the bytes [offset, offset + length) are about to be read,
  // after the ranges announced before them. A hint: the reader MAY start
  // reading them now, so that a later ReadInto() inside the range waits less.
  // Only when the bytes are read changes, never which bytes a read asks for.
  // Does nothing by default.
  virtual void Prefetch(ULONGLONG /*offset*/, ULONGLONG /*length*/) const {}

  // Forgets the ranges Prefetch() announced that no read has used yet.
  virtual void CancelPrefetch() const {}
};

}  // namespace NTFS_BROWSER_NAMESPACE
