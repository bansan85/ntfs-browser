#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include <gsl/narrow>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/tool/fuzzgen/attribute-values.h>
#include <ntfs-browser/tool/fuzzgen/layer-line.h>

#include "data/filename.h"
#include "data/standard-information.h"

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

namespace {

namespace Data = NTFS_BROWSER_NAMESPACE::Data;

// The value of a $STANDARD_INFORMATION of NTFS 3 and 1.2, in bytes.
constexpr ULONGLONG standard_information_v3_bytes = 72;

// Writes value at offset of bytes.
template <typename T>
void Store(std::vector<BYTE>& bytes, std::size_t offset, T value) {
  std::memcpy(&bytes.at(offset), &value, sizeof(T));
}

}  // namespace

std::vector<BYTE> AttributeValues::StandardInformation(const LayerLine& line) {
  std::vector<BYTE> value(sizeof(Data::StandardInformation));
  Store<ULONGLONG>(value, offsetof(Data::StandardInformation, create_time),
                   line.Number("create", 0));
  Store<ULONGLONG>(value, offsetof(Data::StandardInformation, alter_time),
                   line.Number("alter", 0));
  Store<ULONGLONG>(value, offsetof(Data::StandardInformation, mft_time),
                   line.Number("mft", 0));
  Store<ULONGLONG>(value, offsetof(Data::StandardInformation, read_time),
                   line.Number("read", 0));
  Store<DWORD>(value, offsetof(Data::StandardInformation, permission),
               gsl::narrow<DWORD>(line.Number("perm", 0)));
  Store<DWORD>(value, offsetof(Data::StandardInformation, max_version_no),
               gsl::narrow<DWORD>(line.Number("max_ver", 0)));
  Store<DWORD>(value, offsetof(Data::StandardInformation, version_no),
               gsl::narrow<DWORD>(line.Number("ver", 0)));
  Store<DWORD>(value, offsetof(Data::StandardInformation, class_id),
               gsl::narrow<DWORD>(line.Number("class", 0)));
  Store<DWORD>(value, offsetof(Data::StandardInformation, owner_id),
               gsl::narrow<DWORD>(line.Number("owner", 0)));
  Store<DWORD>(value, offsetof(Data::StandardInformation, security_id),
               gsl::narrow<DWORD>(line.Number("security", 0)));
  Store<ULONGLONG>(value, offsetof(Data::StandardInformation, quota_charged),
                   line.Number("quota", 0));
  Store<ULONGLONG>(value, offsetof(Data::StandardInformation, usn),
                   line.Number("usn", 0));
  value.resize(gsl::narrow<std::size_t>(
      line.Number("len", standard_information_v3_bytes)));
  return value;
}

std::vector<BYTE> AttributeValues::FileName(const LayerLine& line) {
  const std::u16string name = Name(line, "name");
  std::vector<BYTE> value(offsetof(Data::Filename, name) +
                          (name.size() * sizeof(WORD)));
  Store<ULONGLONG>(value, offsetof(Data::Filename, parent_ref),
                   line.Number("parent", 0));
  Store<ULONGLONG>(value, offsetof(Data::Filename, create_time),
                   line.Number("create", 0));
  Store<ULONGLONG>(value, offsetof(Data::Filename, alter_time),
                   line.Number("alter", 0));
  Store<ULONGLONG>(value, offsetof(Data::Filename, mft_time),
                   line.Number("mft", 0));
  Store<ULONGLONG>(value, offsetof(Data::Filename, read_time),
                   line.Number("read", 0));
  Store<ULONGLONG>(value, offsetof(Data::Filename, alloc_size),
                   line.Number("alloc", 0));
  Store<ULONGLONG>(value, offsetof(Data::Filename, real_size),
                   line.Number("real", 0));
  Store<DWORD>(value, offsetof(Data::Filename, flags),
               gsl::narrow<DWORD>(line.Number("attrs", 0)));
  Store<DWORD>(value, offsetof(Data::Filename, er),
               gsl::narrow<DWORD>(line.Number("er", 0)));
  Store<BYTE>(value, offsetof(Data::Filename, name_length),
              gsl::narrow<BYTE>(name.size()));
  Store<BYTE>(value, offsetof(Data::Filename, name_space),
              gsl::narrow<BYTE>(line.Number("space", 0)));
  for (std::size_t i = 0; i < name.size(); ++i) {
    Store<WORD>(value, offsetof(Data::Filename, name) + (i * sizeof(WORD)),
                static_cast<WORD>(name[i]));
  }
  return value;
}

std::u16string AttributeValues::Name(const LayerLine& line,
                                     std::string_view key) {
  std::u16string name;
  const std::string hex_key = std::string(key) + "16";
  if (line.Has(hex_key)) {
    const std::vector<BYTE> units = line.Bytes(hex_key);
    for (std::size_t i = 0; i + 1 < units.size(); i += sizeof(WORD)) {
      name.push_back(static_cast<char16_t>(units[i] | (units[i + 1] << 8)));
    }
  } else if (line.Has(key)) {
    for (const char letter : line.Text(key)) {
      name.push_back(static_cast<char16_t>(letter));
    }
  }
  return name;
}

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
