#ifdef _WIN32

  #include "host-media-support.h"

  #include <string>
  #include <vector>

  #include <windows.h>

  #include <ntfs-browser/namespace.h>
  #include <ntfs-browser/tuning/tuning.h>

  #include "io/host-media-kind.h"

namespace NTFS_BROWSER_NAMESPACE::Tests {
namespace {

struct Answer {
  Tuning::MediaKind kind{Tuning::MediaKind::Unknown};
  std::vector<std::wstring> asked;
};

Answer& Current() {
  static Answer answer;
  return answer;
}

Tuning::MediaKind Ask(const std::wstring& path) {
  Answer& answer = Current();
  answer.asked.push_back(path);
  return answer.kind;
}

// Runs before main(), so the first test already opens no device. The
// variable is inherited by the console tools that tests run as child
// processes, which cannot use the query above: their bench options then set
// the media kind to Unknown, and the library asks no host volume.
[[maybe_unused]] const bool g_installed = [] {
  Io::SetHostMediaQueryForTests(&Ask);
  SetEnvironmentVariableW(L"NTFS_BROWSER_NO_HOST_MEDIA_QUERY", L"1");
  return true;
}();

}  // namespace

HostMediaAnswer::HostMediaAnswer(Tuning::MediaKind kind)
    : previous_(Current().kind) {
  Current().kind = kind;
  Current().asked.clear();
}

HostMediaAnswer::~HostMediaAnswer() { Current().kind = previous_; }

const std::vector<std::wstring>& HostMediaAnswer::Asked() {
  return Current().asked;
}

}  // namespace NTFS_BROWSER_NAMESPACE::Tests

#endif  // _WIN32
