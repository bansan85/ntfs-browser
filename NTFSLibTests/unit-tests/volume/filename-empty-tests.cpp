#include <ntfs-browser/win-types.h>

#include <limits>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/filename.h>
#include <ntfs-browser/namespace.h>

#include "data/filename-flag.h"
#include "data/filename-namespace.h"
#include "data/filename.h"

namespace Data = NTFS_BROWSER_NAMESPACE::Data;
using NTFS_BROWSER_NAMESPACE::Filename;

namespace {

// Time the nameless $FILE_NAME of the probe carries: the largest FILETIME.
constexpr ULONGLONG largest_time = std::numeric_limits<ULONGLONG>::max();

// The local time the library gives for a UTC time: converted on Windows, left
// as it is elsewhere.
FILETIME LocalTimeOf(ULONGLONG utc) {
  constexpr unsigned dword_bits = 32;
  const FILETIME in{.dwLowDateTime = static_cast<DWORD>(utc),
                    .dwHighDateTime = static_cast<DWORD>(utc >> dword_bits)};
#ifdef _WIN32
  FILETIME out{};
  if (FileTimeToLocalFileTime(&in, &out) != 0) {
    return out;
  }
#endif
  return in;
}

bool SameTime(const FILETIME& left, const FILETIME& right) {
  return left.dwLowDateTime == right.dwLowDateTime &&
         left.dwHighDateTime == right.dwHighDateTime;
}

// A Filename that can be bound to a $FILE_NAME of the test's making.
class BindableFilename final : public Filename {
 public:
  using Filename::SetFilename;
};

}  // namespace

TEST_CASE("a Filename with no name behind it reports an empty file",
          "[filename]") {
  const Filename name;

  CHECK(name.GetFileSize() == 0);
  CHECK(name.GetAllocatedSize() == 0);
  CHECK(name.GetParentReference() == 0);
  CHECK(name.GetParentSequenceNumber() == 0);
  CHECK(name.GetFilePermission() == Data::FilenameFlag::None);
  CHECK_FALSE(name.IsReadOnly());
  CHECK_FALSE(name.IsHidden());
  CHECK_FALSE(name.IsSystem());
  CHECK_FALSE(name.IsArchive());
  CHECK_FALSE(name.IsDirectory());
  CHECK_FALSE(name.IsCompressed());
  CHECK_FALSE(name.IsEncrypted());
  CHECK_FALSE(name.IsSparse());
  CHECK(name.GetFilename().empty());
  CHECK_FALSE(name.HasName());
  CHECK_FALSE(name.IsWin32Name());
}

TEST_CASE("a Filename with no name behind it has all its times at the epoch",
          "[filename]") {
  const Filename name;
  FILETIME write{.dwLowDateTime = 1, .dwHighDateTime = 1};
  FILETIME create{.dwLowDateTime = 1, .dwHighDateTime = 1};
  FILETIME access{.dwLowDateTime = 1, .dwHighDateTime = 1};
  FILETIME change{.dwLowDateTime = 1, .dwHighDateTime = 1};

  name.GetFileTime(&write, &create, &access, &change);

  for (const FILETIME* time : {&write, &create, &access, &change}) {
    CHECK(SameTime(*time, LocalTimeOf(0)));
  }
  CHECK(name.Compare(L"") == 0);
  CHECK(name.Compare(L"a") > 0);
}

TEST_CASE("a $FILE_NAME with no characters is not a Win32 name", "[filename]") {
  Data::Filename raw{};
  raw.name_space = Data::FilenameNamespace::Win32;
  raw.alter_time = largest_time;
  raw.create_time = largest_time;
  raw.read_time = largest_time;
  raw.mft_time = largest_time;
  BindableFilename name;
  name.SetFilename(raw);

  CHECK_FALSE(name.HasName());
  CHECK_FALSE(name.IsWin32Name());

  FILETIME write{};
  FILETIME create{};
  FILETIME access{};
  FILETIME change{};
  name.GetFileTime(&write, &create, &access, &change);
  for (const FILETIME* time : {&write, &create, &access, &change}) {
    CHECK(SameTime(*time, LocalTimeOf(largest_time)));
  }
}
