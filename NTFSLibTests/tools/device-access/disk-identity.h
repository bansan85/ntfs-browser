#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace NtfsDevice {

// Who a disk is, as its firmware says. The profile keys a disk by it.
struct DiskIdentity {
  std::string vendor;
  std::string product;
  std::string serial;
  // "<vendor> <product>|<serial>", trimmed: the key of a profile section.
  std::string key;
  // True for a disk with no seek penalty (an SSD), false for a spinning one.
  // Empty where the system does not say.
  std::optional<bool> solid_state;
};

// The disk behind target: a drive letter ("c" or "c:", optionally followed by a
// path), a device ("\\.\PhysicalDrive0", "\\.\C:", "/dev/sda1") or an image
// file. Nothing for an image file, and for a disk the system does not
// identify.
[[nodiscard]] std::optional<DiskIdentity>
    IdentifyDisk(std::wstring_view target);

}  // namespace NtfsDevice
