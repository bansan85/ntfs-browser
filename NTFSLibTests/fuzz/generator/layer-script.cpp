#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/tool/fuzzgen/boot-sector-layer.h>
#include <ntfs-browser/tool/fuzzgen/index-layer.h>
#include <ntfs-browser/tool/fuzzgen/layer-line.h>
#include <ntfs-browser/tool/fuzzgen/layer-script.h>
#include <ntfs-browser/tool/fuzzgen/record-layer.h>
#include <ntfs-browser/tool/fuzzgen/sparse-image.h>

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

namespace {

// The lines that belong to the layer above them.
bool IsAttributeLine(const std::string& kind) {
  return kind == "si" || kind == "fn" || kind == "attr" || kind == "nonres" ||
         kind == "indexroot";
}

// The lines of text that hold a word, as layer lines.
std::vector<LayerLine> Split(std::string_view text) {
  std::vector<LayerLine> lines;
  std::istringstream stream{std::string(text)};
  std::string line;
  while (std::getline(stream, line)) {
    if (line.find_first_not_of(" \r\t") != std::string::npos) {
      lines.emplace_back(line);
    }
  }
  return lines;
}

// Collects the entry lines that start at index in lines, and moves index past
// them.
std::vector<LayerLine> TakeEntries(const std::vector<LayerLine>& lines,
                                   std::size_t& index) {
  std::vector<LayerLine> entries;
  while (index < lines.size() && lines[index].Kind() == "entry") {
    entries.push_back(lines[index++]);
  }
  return entries;
}

// Adds the series of file records that the line at index starts, and moves
// index past its attributes.
void AddRecords(SparseImage& image, const std::vector<LayerLine>& lines,
                std::size_t& index) {
  const LayerLine& header = lines[index++];
  std::vector<RecordLayer::Attribute> attributes;
  while (index < lines.size() && IsAttributeLine(lines[index].Kind())) {
    const LayerLine& line = lines[index++];
    attributes.push_back({line, TakeEntries(lines, index)});
  }
  std::vector<BYTE> series;
  const ULONGLONG count = header.Number("count", 1);
  for (ULONGLONG offset = 0; offset < count; ++offset) {
    const std::vector<BYTE> record =
        RecordLayer::Render(header, attributes, offset);
    series.insert(series.end(), record.begin(), record.end());
  }
  image.AddBytes(header.Positional(0), std::move(series));
}

}  // namespace

void LayerScript::Fill(SparseImage& image, std::string_view text,
                       const SparseImage::BlobSource& blobs) {
  const std::vector<LayerLine> lines = Split(text);
  std::size_t index = 0;
  while (index < lines.size()) {
    const LayerLine& line = lines[index];
    const std::string& kind = line.Kind();
    if (kind == "record") {
      AddRecords(image, lines, index);
      continue;
    }
    ++index;
    if (kind == "ptrn") {
      image.AddPtrn(line.Positional(0), line.Positional(1));
    } else if (kind == "upcase") {
      image.AddIdentityUpcase(line.Positional(0), line.Positional(1));
    } else if (kind == "stamp") {
      image.AddStamp(line.Positional(0), line.Number("stride"),
                     line.Number("count"), line.Bytes("bytes"));
    } else if (kind == "boot") {
      image.AddBytes(line.Positional(0),
                     BootSectorLayer::Render(line, blobs(line.Text("code"))));
    } else if (kind == "indx") {
      image.AddBytes(line.Positional(0),
                     IndexLayer::Block(line, TakeEntries(lines, index)));
    } else {
      throw std::runtime_error("unknown layer: " + line.Source());
    }
  }
}

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
