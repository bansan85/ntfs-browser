#pragma once

#include <ntfs-browser/namespace.h>

#include "log/ntfs-common.h"

namespace NTFS_BROWSER_NAMESPACE {
namespace Attr {

struct HeaderCommon;

}  // namespace Attr

namespace Cache {

enum class Strategy : std::uint8_t;

}  // namespace Cache

namespace Io {

template <Cache::Strategy S>
class FileRecord;

}  // namespace Io
}  // namespace NTFS_BROWSER_NAMESPACE

namespace NTFS_BROWSER_NAMESPACE {

namespace Attr {

template <class Resident, Cache::Strategy S>
class AttrData : public Resident {
 public:
  AttrData(const HeaderCommon& ahc, const Io::FileRecord<S>& file_record)
      : Resident(ahc, file_record) {
    Log::Trace("Attribute: Data ({}Resident)",
               this->IsNonResident() ? "Non" : "");
  }

  AttrData(AttrData&& other) noexcept = delete;
  AttrData(const AttrData& other) = delete;
  AttrData& operator=(AttrData&& other) noexcept = delete;
  AttrData& operator=(const AttrData& other) = delete;

  ~AttrData() override { Log::Trace("AttrData deleted"); }
};  // AttrData

}  // namespace Attr

}  // namespace NTFS_BROWSER_NAMESPACE
