#include "disk-identity.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
  #include <cwctype>

  #include <windows.h>
  #include <winioctl.h>
#else
  #include <filesystem>
  #include <fstream>
#endif

#include "text-convert.h"

namespace NtfsDevice {

namespace {

// Removes the spaces and NULs that firmware pads a field with.
std::string Trim(std::string text) {
  const auto is_pad = [](char c) { return c == ' ' || c == '\0' || c == '\t'; };
  while (!text.empty() && is_pad(text.back())) {
    text.pop_back();
  }
  std::size_t first = 0;
  while (first < text.size() && is_pad(text.at(first))) {
    first++;
  }
  return text.substr(first);
}

// Fills the key of identity from its fields.
void MakeKey(DiskIdentity& identity) {
  identity.vendor = Trim(identity.vendor);
  identity.product = Trim(identity.product);
  identity.serial = Trim(identity.serial);
  std::string name = identity.vendor;
  if (!identity.product.empty()) {
    if (!name.empty()) {
      name += ' ';
    }
    name += identity.product;
  }
  identity.key = name + "|" + identity.serial;
}

#ifdef _WIN32

// The \\.\X: path of a target that starts with a drive letter, or the device
// path itself. Empty for anything else, such as an image file.
std::wstring DevicePathOf(std::wstring_view target) {
  if (target.starts_with(L"\\\\.\\")) {
    return std::wstring(target);
  }
  const bool drive = !target.empty() && std::iswalpha(target.front()) != 0 &&
                     (target.size() == 1 || target[1] == L':');
  if (!drive) {
    return {};
  }
  return std::wstring(L"\\\\.\\") + static_cast<wchar_t>(target.front()) + L":";
}

// The string a STORAGE_DEVICE_DESCRIPTOR offset points at.
std::string DescriptorString(const std::vector<BYTE>& buffer, DWORD offset) {
  if (offset == 0 || offset >= buffer.size()) {
    return {};
  }
  const char* const start =
      reinterpret_cast<const char*>(buffer.data()) + offset;
  const std::size_t room = buffer.size() - offset;
  std::size_t length = 0;
  while (length < room && start[length] != '\0') {
    length++;
  }
  return std::string(start, length);
}

std::optional<DiskIdentity> Query(const std::wstring& path) {
  const HANDLE handle =
      CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                  OPEN_EXISTING, 0, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return std::nullopt;
  }

  struct Closer {
    HANDLE handle;

    ~Closer() { CloseHandle(handle); }
  } closer{handle};

  STORAGE_PROPERTY_QUERY query{};
  query.PropertyId = StorageDeviceProperty;
  query.QueryType = PropertyStandardQuery;
  std::vector<BYTE> buffer(4096);
  DWORD returned = 0;
  if (DeviceIoControl(handle, IOCTL_STORAGE_QUERY_PROPERTY, &query,
                      sizeof(query), buffer.data(),
                      static_cast<DWORD>(buffer.size()), &returned,
                      nullptr) == FALSE ||
      returned < sizeof(STORAGE_DEVICE_DESCRIPTOR)) {
    return std::nullopt;
  }
  buffer.resize(returned);
  const auto* descriptor =
      reinterpret_cast<const STORAGE_DEVICE_DESCRIPTOR*>(buffer.data());

  DiskIdentity identity;
  identity.vendor = DescriptorString(buffer, descriptor->VendorIdOffset);
  identity.product = DescriptorString(buffer, descriptor->ProductIdOffset);
  identity.serial = DescriptorString(buffer, descriptor->SerialNumberOffset);
  MakeKey(identity);

  // No seek penalty means a solid-state disk.
  STORAGE_PROPERTY_QUERY penalty_query{};
  penalty_query.PropertyId = StorageDeviceSeekPenaltyProperty;
  penalty_query.QueryType = PropertyStandardQuery;
  DEVICE_SEEK_PENALTY_DESCRIPTOR penalty{};
  if (DeviceIoControl(handle, IOCTL_STORAGE_QUERY_PROPERTY, &penalty_query,
                      sizeof(penalty_query), &penalty, sizeof(penalty),
                      &returned, nullptr) != FALSE) {
    identity.solid_state = penalty.IncursSeekPenalty == FALSE;
  }
  return identity;
}

#else

// Contents of a one-line sysfs file, or empty.
std::string ReadLine(const std::filesystem::path& path) {
  std::ifstream in(path);
  std::string line;
  std::getline(in, line);
  return line;
}

#endif

}  // namespace

std::optional<DiskIdentity> IdentifyDisk(std::wstring_view target) {
#ifdef _WIN32
  const std::wstring path = DevicePathOf(target);
  if (path.empty()) {
    return std::nullopt;
  }
  return Query(path);
#else
  namespace fs = std::filesystem;
  std::error_code error;
  const fs::path device = fs::path(WideToUtf8(target));
  if (!fs::is_block_file(device, error)) {
    return std::nullopt;
  }
  // /sys/class/block/<name> links to .../block/<disk>[/<partition>].
  fs::path node =
      fs::canonical(fs::path("/sys/class/block") / device.filename(), error);
  if (error) {
    return std::nullopt;
  }
  if (fs::exists(node / "partition", error)) {
    node = node.parent_path();
  }
  DiskIdentity identity;
  identity.product = ReadLine(node / "device" / "model");
  identity.vendor = ReadLine(node / "device" / "vendor");
  identity.serial = ReadLine(node / "device" / "serial");
  if (identity.serial.empty()) {
    identity.serial = ReadLine(node / "device" / "wwid");
  }
  MakeKey(identity);
  const std::string rotational = ReadLine(node / "queue" / "rotational");
  if (!rotational.empty()) {
    identity.solid_state = rotational == "0";
  }
  return identity;
#endif
}

}  // namespace NtfsDevice
