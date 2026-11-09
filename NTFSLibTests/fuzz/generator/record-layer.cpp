#include "record-layer.h"

#include <stdexcept>
#include <string>
#include <vector>

#include <gsl/narrow>

#include <ntfs-browser/namespace.h>

#include "attribute-values.h"
#include "index-layer.h"
#include "layer-line.h"
#include "legacy-record.h"
#include "win-types.h"

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

namespace {

// The type codes of the attributes that have a line of their own.
constexpr DWORD type_standard_information = 0x10;
constexpr DWORD type_file_name = 0x30;
constexpr DWORD type_index_root = 0x90;

// The attribute that attribute stands for, without its value.
LegacyRecord::Attribute Header(const RecordLayer::Attribute& attribute,
                               DWORD type, bool indexed_by_default) {
  const LayerLine& line = attribute.line;
  LegacyRecord::Attribute out;
  out.type = gsl::narrow<DWORD>(line.Number("type", type));
  out.flags = gsl::narrow<WORD>(line.Number("flags", 0));
  out.id = gsl::narrow<WORD>(line.Number("id", 0));
  const std::u16string name = AttributeValues::Name(line, "aname");
  out.name.assign(name.begin(), name.end());
  out.indexed = line.Number("indexed", indexed_by_default ? 1 : 0) != 0;
  if (line.Has("noff")) {
    out.name_offset = gsl::narrow<WORD>(line.Number("noff"));
  }
  return out;
}

// The attribute that a line of the record stands for.
LegacyRecord::Attribute ToAttribute(const RecordLayer::Attribute& attribute) {
  const LayerLine& line = attribute.line;
  const std::string& kind = line.Kind();
  if (kind == "si") {
    LegacyRecord::Attribute out =
        Header(attribute, type_standard_information, false);
    out.value = AttributeValues::StandardInformation(line);
    return out;
  }
  if (kind == "fn") {
    LegacyRecord::Attribute out = Header(attribute, type_file_name, true);
    out.value = AttributeValues::FileName(line);
    return out;
  }
  if (kind == "indexroot") {
    LegacyRecord::Attribute out = Header(attribute, type_index_root, false);
    out.value = IndexLayer::Root(line, attribute.entries);
    return out;
  }
  if (kind == "attr") {
    LegacyRecord::Attribute out = Header(attribute, 0, false);
    out.type = gsl::narrow<DWORD>(line.Number("type"));
    out.value = line.Bytes("value");
    return out;
  }
  if (kind == "nonres") {
    LegacyRecord::Attribute out = Header(attribute, 0, false);
    out.type = gsl::narrow<DWORD>(line.Number("type"));
    out.non_resident = true;
    out.start_vcn = line.Number("start", 0);
    out.last_vcn = line.Number("last", 0);
    out.allocated_size = line.Number("alloc", 0);
    out.real_size = line.Number("real", 0);
    out.initialized_size = line.Number("init", 0);
    out.compression_unit = gsl::narrow<BYTE>(line.Number("unit", 0));
    if (line.Has("csize")) {
      out.compressed_size = line.Number("csize");
    }
    out.raw_runs = line.Bytes("runs");
    return out;
  }
  throw std::runtime_error("unknown attribute line: " + line.Source());
}

}  // namespace

std::vector<BYTE> RecordLayer::Render(const LayerLine& line,
                                      const std::vector<Attribute>& attributes,
                                      ULONGLONG offset) {
  const ULONGLONG number = line.Number("number") + offset;
  LegacyRecord::Spec spec;
  spec.flags = gsl::narrow<WORD>(line.Number("flags", 0));
  spec.log_sequence = line.Number("lsn", 0);
  spec.base_reference = line.Number("base", 0);
  spec.sequence = line.Has("seq") && line.Text("seq") == "@number"
                      ? gsl::narrow<WORD>(number)
                      : gsl::narrow<WORD>(line.Number("seq", 0));
  spec.links = gsl::narrow<WORD>(line.Number("links", 0));
  spec.update_sequence_number = gsl::narrow<WORD>(line.Number("usn", 0));
  spec.next_attribute_id = gsl::narrow<WORD>(line.Number("next_id", 0));
  spec.record_number = gsl::narrow<DWORD>(number);
  spec.end_padding = gsl::narrow<DWORD>(line.Number("pad", 0));
  for (const Attribute& attribute : attributes) {
    spec.attributes.push_back(ToAttribute(attribute));
  }
  return LegacyRecord::Bytes(spec);
}

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
