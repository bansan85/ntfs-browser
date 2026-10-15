#pragma once

#include <ntfs-browser/win-types.h>

#include <cassert>
#include <cstring>
#include <span>

#include <ntfs-browser/namespace.h>

#include "data/file-record-header.h"

namespace NTFS_BROWSER_NAMESPACE::Tests {

// Data::FileRecordHeader is a union spanning max_file_record_size, but fixture
// buffers are smaller (a fake record is 1024 bytes), so binding a Data& to
// them is undefined behaviour. This edits the header through a full-size
// copy and writes only the named header fields back.
template <typename Edit>
void EditFileRecordHeader(std::span<BYTE> buffer, const Edit& edit) {
  assert(buffer.size() >= NTFS_BROWSER_NAMESPACE::Data::FileRecordHeader::
                              min_file_record_header_size);
  NTFS_BROWSER_NAMESPACE::Data::FileRecordHeader header{};
  std::memcpy(&header, buffer.data(),
              NTFS_BROWSER_NAMESPACE::Data::FileRecordHeader::
                  min_file_record_header_size);
  edit(header);
  std::memcpy(buffer.data(), &header,
              NTFS_BROWSER_NAMESPACE::Data::FileRecordHeader::
                  min_file_record_header_size);
}

}  // namespace NTFS_BROWSER_NAMESPACE::Tests
