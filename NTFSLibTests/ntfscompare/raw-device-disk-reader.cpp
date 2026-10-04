#ifndef _WIN32

  #include "raw-device-disk-reader.h"

  #include <cstdio>
  #include <span>
  #include <string>
  #include <string_view>

  #include <fcntl.h>
  #include <gsl/narrow>
  #include <sys/types.h>
  #include <unistd.h>

  #include "ntfs-browser/win-types.h"

namespace NtfsCompare
{

namespace
{
// Device node paths are always plain ASCII, so a per-code-point narrow cast
// is exact - no need for a general wide -> UTF-8 encoder just for this.
std::string WideToNarrowAscii(std::wstring_view wide)
{
  std::string out;
  out.reserve(wide.size());
  for (const wchar_t character : wide)
  {
    out.push_back(gsl::narrow<char>(character));
  }
  return out;
}
}  // namespace

RawDeviceDiskReader::~RawDeviceDiskReader()
{
  if (fd_ >= 0)
  {
    close(fd_);
  }
}

bool RawDeviceDiskReader::Open(std::wstring_view path)
{
  const std::string narrow = WideToNarrowAscii(path);
  fd_ = open(narrow.c_str(), O_RDONLY);
  return fd_ >= 0;
}

bool RawDeviceDiskReader::ReadInto(LARGE_INTEGER& addr,
                                   std::span<BYTE> dest) const
{
  const ssize_t bytes_read =
      pread(fd_, dest.data(), dest.size(), addr.QuadPart);
  if (bytes_read < 0 || static_cast<size_t>(bytes_read) != dest.size())
  {
    static_cast<void>(std::fprintf(stderr,
                                   "Cannot read device at offset %lld\n",
                                   static_cast<long long>(addr.QuadPart)));
    return false;
  }
  return true;
}

}  // namespace NtfsCompare

#endif  // !_WIN32
