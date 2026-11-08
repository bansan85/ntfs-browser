#pragma once

#include <ntfs-browser/win-types.h>

#include <string>
#include <string_view>
#include <vector>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/tool/fuzzgen/layer-line.h>

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

// The values of the attributes that the layers files name field by field, built
// with the structures of the library.
class AttributeValues {
 public:
  // The $STANDARD_INFORMATION of a line: the fields create, alter, mft and
  // read (the four times), perm, max_ver, ver, class, owner, security, quota
  // and usn, all numbers that default to zero. len is the length of the value:
  // 72 for NTFS 3, 48 for the older layout. It defaults to 72.
  [[nodiscard]] static std::vector<BYTE>
      StandardInformation(const LayerLine& line);

  // The $FILE_NAME of a line: the fields parent (a file reference), create,
  // alter, mft, read, alloc, real, attrs, er and space, all numbers that
  // default to zero, then the name.
  [[nodiscard]] static std::vector<BYTE> FileName(const LayerLine& line);

  // The UTF-16 units of the name in the field called key. The text is the
  // name itself. The field key16 holds the units as hexadecimal text instead,
  // for a name that holds a space, an equals sign or a unit above 126. A line
  // with neither field has the empty name.
  [[nodiscard]] static std::u16string Name(const LayerLine& line,
                                           std::string_view key);
};

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
