#pragma once

#ifdef _WIN32

  #include <string>
  #include <vector>

  #include <ntfs-browser/namespace.h>
  #include <ntfs-browser/tuning/tuning.h>

namespace NTFS_BROWSER_NAMESPACE::Test::Support {

// Before main(), the library's query for the volume that hosts an image file
// is replaced by one that answers Unknown and opens nothing: no test opens a
// device, and no result depends on the disk of the machine. A test that needs
// another answer makes one of these. It answers kind until it goes out of
// scope, then restores the previous answer.
class HostMediaAnswer final {
 public:
  explicit HostMediaAnswer(NTFS_BROWSER_NAMESPACE::Tuning::MediaKind kind);
  HostMediaAnswer(const HostMediaAnswer&) = delete;
  HostMediaAnswer& operator=(const HostMediaAnswer&) = delete;
  HostMediaAnswer(HostMediaAnswer&&) = delete;
  HostMediaAnswer& operator=(HostMediaAnswer&&) = delete;
  ~HostMediaAnswer();

  // The paths the query was asked about since this answer was made, in order.
  [[nodiscard]] static const std::vector<std::wstring>& Asked();

 private:
  NTFS_BROWSER_NAMESPACE::Tuning::MediaKind previous_;
};

}  // namespace NTFS_BROWSER_NAMESPACE::Test::Support

#endif  // _WIN32
