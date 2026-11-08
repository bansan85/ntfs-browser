#pragma once

#include <string_view>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/tool/fuzzgen/sparse-image.h>

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

// Reads the layers file of a trace and adds what it describes to a
// SparseImage. One layer per line, the address first (volume relative):
//   ptrn ADDR LENGTH                  the "PTRN" fill (SparseImage::AddPtrn)
//   upcase ADDR UNITS                 an identity $UpCase table
//   stamp ADDR stride=N count=N bytes=HEX
//                                     a block repeated on every page
//   boot ADDR code=RESOURCE FIELDS    the boot area (BootSectorLayer)
//   record ADDR count=N FIELDS        count file records, 1 KiB each, with the
//                                     fields of RecordLayer. The lines after
//                                     it (si, fn, attr, nonres, indexroot,
//                                     entry) are the attributes of every
//                                     record of the series.
//   indx ADDR size=N FIELDS           an index block (IndexLayer). The entry
//                                     lines after it are its entries.
class LayerScript {
 public:
  // Adds the layers of text to image. blobs gives the resources that a line
  // names. Throws std::runtime_error, naming the line, when one is wrong.
  static void Fill(SparseImage& image, std::string_view text,
                   const SparseImage::BlobSource& blobs);
};

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
