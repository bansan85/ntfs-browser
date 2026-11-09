#include "layer-script.h"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <map>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ntfs-browser/namespace.h>

#include "boot-sector-layer.h"
#include "counter-text.h"
#include "index-layer.h"
#include "layer-line.h"
#include "lznt1-compressor.h"
#include "record-layer.h"
#include "sparse-image.h"
#include "win-types.h"

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

// The bytes of the resource that a blob line names, from offset on, length
// bytes of them. Throws when the resource is shorter.
std::vector<BYTE> Slice(const std::vector<BYTE>& resource,
                        const LayerLine& line) {
  const ULONGLONG offset = line.Number("offset");
  const ULONGLONG length = line.Number("length");
  if (offset > resource.size() || length > resource.size() - offset) {
    throw std::runtime_error("the resource is too short: " + line.Source());
  }
  const auto first = resource.begin() + static_cast<std::ptrdiff_t>(offset);
  return {first, first + static_cast<std::ptrdiff_t>(length)};
}

// The two zero bytes that end a stream of LZNT1 chunks.
constexpr std::size_t stream_end_bytes = 2;

// Writes into out the plain bytes of a stream that start at offset.
using PlainSource = std::function<void(ULONGLONG offset, std::span<BYTE> out)>;

// The count chunks that compress the plain bytes from origin on, then the end
// of the stream.
std::vector<BYTE> CompressedStream(ULONGLONG origin, ULONGLONG count,
                                   const PlainSource& plain_source) {
  std::vector<BYTE> stream;
  std::vector<BYTE> plain(Lznt1Compressor::chunk_bytes);
  for (ULONGLONG i = 0; i < count; ++i) {
    plain_source(origin + (i * Lznt1Compressor::chunk_bytes), plain);
    const std::vector<BYTE> chunk = Lznt1Compressor::Chunk(plain);
    stream.insert(stream.end(), chunk.begin(), chunk.end());
  }
  stream.resize(stream.size() + stream_end_bytes);
  return stream;
}

// The plain bytes of a resource: the file whose content no code makes.
PlainSource ResourceBytes(const std::vector<BYTE>& resource) {
  return [&resource](ULONGLONG offset, std::span<BYTE> out) {
    if (offset > resource.size() || out.size() > resource.size() - offset) {
      throw std::runtime_error("the resource is too short for its chunks");
    }
    std::copy_n(resource.begin() + static_cast<std::ptrdiff_t>(offset),
                out.size(), out.begin());
  };
}

}  // namespace

void LayerScript::Fill(SparseImage& image, std::string_view text,
                       const SparseImage::BlobSource& source) {
  // A resource that several lines name is read once.
  std::map<std::string, std::vector<BYTE>, std::less<>> loaded;
  const auto blobs = [&](const std::string& name) -> const std::vector<BYTE>& {
    const auto found = loaded.find(name);
    return found != loaded.end()
               ? found->second
               : loaded.emplace(name, source(name)).first->second;
  };
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
    } else if (kind == "text") {
      image.AddCounterText(line.Positional(0), line.Positional(1),
                           line.Number("file"));
    } else if (kind == "lznt1") {
      const PlainSource plain_source =
          line.Has("name") ? ResourceBytes(blobs(line.Text("name")))
                           : PlainSource(CounterText::Fill);
      image.AddBytes(line.Positional(0),
                     CompressedStream(line.Number("file"), line.Number("count"),
                                      plain_source));
    } else if (kind == "blob") {
      image.AddBytes(line.Positional(0), Slice(blobs(line.Text("name")), line));
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
