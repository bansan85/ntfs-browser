#pragma once

#include <ntfs-browser/win-types.h>

#include <cassert>
#include <cstring>
#include <span>

#include "data/file-record-header.h"

namespace NtfsBrowserTests
{

// FileRecordHeader::Data is a union spanning kMaxFileRecordSize, but fixture
// buffers are smaller (a fake record is 1024 bytes), so binding a Data& to
// them is undefined behaviour. This edits the header through a full-size
// copy and writes only the named header fields back.
template <typename Edit>
void EditFileRecordHeader(std::span<BYTE> buffer, Edit&& edit)
{
  assert(buffer.size() >= NtfsBrowser::kMinFileRecordHeaderSize);
  NtfsBrowser::FileRecordHeader::Data header{};
  std::memcpy(&header, buffer.data(), NtfsBrowser::kMinFileRecordHeaderSize);
  edit(header);
  std::memcpy(buffer.data(), &header, NtfsBrowser::kMinFileRecordHeaderSize);
}

}  // namespace NtfsBrowserTests
