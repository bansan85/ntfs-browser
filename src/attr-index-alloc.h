#pragma once

#include <ntfs-browser/win-types.h>

#include <cstdint>
#include <span>

#include "attr-non-resident.h"
#include "internal-export.h"

namespace NtfsBrowser
{
class IndexBlock;
template <Strategy S>
class FileRecord;
struct AttrHeaderCommon;
enum class Strategy : std::uint8_t;

// Whether offset_of_us and the Update Sequence Array after it fit inside an
// index_block_size-byte buffer without overlapping the block header.
// Callers MUST reject the block instead of reading through it when this
// returns false.
[[nodiscard]] NTFS_BROWSER_EXPORT_TESTS_ONLY bool
    IndexBlockUsOffsetInBounds(WORD offset_of_us, DWORD sectors,
                               DWORD index_block_size) noexcept;

template <Strategy S>
class AttrIndexAlloc : public AttrNonResident<S>
{
 public:
  AttrIndexAlloc(const AttrHeaderCommon& ahc, const FileRecord<S>& file_record);
  AttrIndexAlloc(AttrIndexAlloc&& other) noexcept = delete;
  AttrIndexAlloc(AttrIndexAlloc const& other) = delete;
  AttrIndexAlloc& operator=(AttrIndexAlloc&& other) noexcept = delete;
  AttrIndexAlloc& operator=(AttrIndexAlloc const& other) = delete;
  ~AttrIndexAlloc() override;

  friend class FileRecord<S>;

 private:
  ULONGLONG index_block_count_{0};

  [[nodiscard]] bool PatchUS(std::span<WORD> block, DWORD sectors, WORD usn,
                             std::span<const WORD> usarray);

  [[nodiscard]] ULONGLONG GetIndexBlockCount() const noexcept;
  [[nodiscard]] bool ParseIndexBlock(const ULONGLONG& vcn,
                                     IndexBlock& ib_class);
  [[nodiscard]] bool FixupIndexBlock(std::span<BYTE> block);
  [[nodiscard]] bool ParseIndexEntries(std::span<BYTE> block,
                                       IndexBlock& ib_class);
};  // AttrIndexAlloc

}  // namespace NtfsBrowser
