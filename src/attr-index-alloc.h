#pragma once

#include "attr-non-resident.h"

#include <ntfs-browser/strategy.h>

namespace NtfsBrowser
{
class IndexBlock;
template <Strategy S>
class FileRecord;
struct AttrHeaderCommon;

// Whether offset_of_us, and the Update Sequence Array that follows it (one
// WORD per sector, read by AttrIndexAlloc<S>::PatchUS()), stay within an
// index_block_size-byte buffer without overlapping the Data::IndexBlock
// header itself. Both offset_of_us and sectors are attacker-controlled
// (offset_of_us straight off disk; sectors derived from the already-validated
// GetIndexBlockSize()/GetSectorSize()), so callers must reject the block
// instead of dereferencing past it when this returns false.
[[nodiscard]] bool IndexBlockUsOffsetInBounds(WORD offset_of_us, DWORD sectors,
                                              DWORD index_block_size) noexcept;

template <Strategy S>
class AttrIndexAlloc : public AttrNonResident<S>
{
 public:
  AttrIndexAlloc(const AttrHeaderCommon& ahc, const FileRecord<S>& fr);
  AttrIndexAlloc(AttrIndexAlloc&& other) noexcept = delete;
  AttrIndexAlloc(AttrIndexAlloc const& other) = delete;
  AttrIndexAlloc& operator=(AttrIndexAlloc&& other) noexcept = delete;
  AttrIndexAlloc& operator=(AttrIndexAlloc const& other) = delete;
  ~AttrIndexAlloc() override;

 private:
  ULONGLONG index_block_count_{0};

  [[nodiscard]] bool PatchUS(WORD* sector, DWORD sectors, WORD usn,
                             const WORD* usarray);

 public:
  [[nodiscard]] ULONGLONG GetIndexBlockCount() const noexcept;
  [[nodiscard]] bool ParseIndexBlock(const ULONGLONG& vcn, IndexBlock& ibClass);
};  // AttrIndexAlloc

}  // namespace NtfsBrowser