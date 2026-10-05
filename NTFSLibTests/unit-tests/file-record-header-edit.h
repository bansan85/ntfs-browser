#pragma once

#include <ntfs-browser/win-types.h>

#include <cassert>
#include <cstring>
#include <span>

#include "data/file-record-header.h"

namespace NtfsBrowserTests
{

// FileRecordHeader::Data is a union spanning max_file_record_size, but fixture
// buffers are smaller (a fake record is 1024 bytes), so binding a Data& to
// them is undefined behaviour. This edits the header through a full-size
// copy and writes only the named header fields back.
template <typename Edit>
void EditFileRecordHeader(std::span<BYTE> buffer, const Edit& edit)
{
  assert(buffer.size() >= NtfsBrowser::min_file_record_header_size);
  NtfsBrowser::FileRecordHeader::Data header{};
  std::memcpy(&header, buffer.data(), NtfsBrowser::min_file_record_header_size);
  edit(header);
  std::memcpy(buffer.data(), &header, NtfsBrowser::min_file_record_header_size);
}

}  // namespace NtfsBrowserTests
