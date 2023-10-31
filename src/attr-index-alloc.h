#pragma once

#include "attr-non-resident.h"

#include <ntfs-browser/strategy.h>

namespace NtfsBrowser
{
class IndexBlock;
template <Strategy S>
class FileRecord;
struct AttrHeaderCommon;

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