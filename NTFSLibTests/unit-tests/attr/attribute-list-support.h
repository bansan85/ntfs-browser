#pragma once

#include <ntfs-browser/win-types.h>

#include <string>
#include <utility>
#include <vector>

#include <ntfs-browser/attr/mask.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/volume-options.h>

#include "nonresident-index-support.h"
#include "test-log-sink.h"

namespace Attr = NtfsBrowser::Attr;

namespace {

// Opens record idx of image with options, mask set, not yet attribute-parsed.
// The mask goes on after ParseFileRecord(): only ParseAttrs() reads it.
template <Cache::Strategy S>
ParsedRecord<S> OpenListRecord(std::vector<BYTE> image, ULONGLONG idx,
                               Attr::Mask mask, VolumeOptions options = {}) {
  ParsedRecord<S> opened = OpenRecord<S>(std::move(image), idx, options);
  opened.record->SetAttrMask(mask);
  return opened;
}

// Opens a volume over image, and a record of it that is not parsed yet.
template <Cache::Strategy S>
ParsedRecord<S> OpenUnparsed(std::vector<BYTE> image,
                             VolumeOptions options = {}) {
  return OpenVolumeFrom<S>(
      std::make_unique<NtfsBrowserTests::MemoryDiskReader>(std::move(image)),
      options);
}

// Outcome of one ParseAttrs() call: its result, and the log it wrote.
struct ListParse {
  bool parsed;
  std::string log;
};

// Parses the $ATTRIBUTE_LIST of record idx in image, strict or recovering.
// Only the log of ParseAttrs() is returned, not that of ParseFileRecord().
template <Cache::Strategy S>
ListParse ParseListLog(std::vector<BYTE> image, ULONGLONG idx, bool recover,
                       Attr::Mask mask) {
  const ParsedRecord<S> opened = OpenListRecord<S>(
      std::move(image), idx, mask, VolumeOptions{.recover_errors = recover});
  (void)NtfsBrowserTests::TakeCapturedLog();

  const bool parsed = opened.record->ParseAttrs();
  return {parsed, NtfsBrowserTests::TakeCapturedLog()};
}

}  // namespace
