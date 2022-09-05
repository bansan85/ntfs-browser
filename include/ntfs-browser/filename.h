#pragma once

#include <string>

#include <windows.h>

namespace NtfsBrowser
{
namespace Attr
{
struct Filename;
}  // namespace Attr

namespace Flag
{
enum class Filename : DWORD;
}  // namespace Flag

class Filename
{
 public:
  Filename() = default;
  Filename(Filename&& other) noexcept = delete;
  Filename(Filename const& other) = default;
  Filename& operator=(Filename&& other) noexcept = delete;
  Filename& operator=(Filename const& other) = delete;
  virtual ~Filename() = default;

 protected:
  void SetFilename(const Attr::Filename& fn);
  void CopyFilename(const Filename& fn, const Attr::Filename& afn);

 private:
  // May be NULL for an IndexEntry
  const Attr::Filename* filename_{nullptr};
  // Uppercase Unicode File Name, used to compare file names
  std::wstring_view filename_wuc_;

  void GetFilenameWUC();

 public:
  [[nodiscard]] int Compare(std::wstring_view fn) const noexcept;

  [[nodiscard]] ULONGLONG GetFileSize() const noexcept;
  [[nodiscard]] virtual Flag::Filename GetFilePermission() const noexcept;
  [[nodiscard]] virtual bool IsReadOnly() const noexcept;
  [[nodiscard]] virtual bool IsHidden() const noexcept;
  [[nodiscard]] virtual bool IsSystem() const noexcept;
  [[nodiscard]] virtual bool IsDirectory() const noexcept;
  [[nodiscard]] virtual bool IsCompressed() const noexcept;
  [[nodiscard]] virtual bool IsEncrypted() const noexcept;
  [[nodiscard]] virtual bool IsSparse() const noexcept;

  [[nodiscard]] std::wstring_view GetFilename() const;
  [[nodiscard]] bool HasName() const noexcept;
  [[nodiscard]] bool IsWin32Name() const noexcept;

  virtual void GetFileTime(FILETIME* writeTm, FILETIME* createTm,
                           FILETIME* accessTm) const noexcept;
};  // Filename

}  // namespace NtfsBrowser