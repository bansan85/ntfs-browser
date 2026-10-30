#pragma once

#include <ntfs-browser/win-types.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <span>

#include <ntfs-browser/namespace.h>

#include "attr/non-resident.h"

namespace NTFS_BROWSER_NAMESPACE {

namespace Cache {

enum class Strategy : std::uint8_t;

}  // namespace Cache

}  // namespace NTFS_BROWSER_NAMESPACE

namespace NTFS_BROWSER_NAMESPACE {
namespace Io {

template <Cache::Strategy S>
class FileRecord;

}  // namespace Io
}  // namespace NTFS_BROWSER_NAMESPACE

namespace NTFS_BROWSER_NAMESPACE {

namespace Attr {

struct HeaderCommon;

}  // namespace Attr

namespace Attr {

class IndexBlock;

template <Cache::Strategy S>
class AttrIndexAlloc : public AttrNonResident<S> {
 public:
  AttrIndexAlloc(const HeaderCommon& ahc, const Io::FileRecord<S>& file_record);
  AttrIndexAlloc(AttrIndexAlloc&& other) noexcept = delete;
  AttrIndexAlloc(const AttrIndexAlloc& other) = delete;
  AttrIndexAlloc& operator=(AttrIndexAlloc&& other) noexcept = delete;
  AttrIndexAlloc& operator=(const AttrIndexAlloc& other) = delete;
  ~AttrIndexAlloc() override;

  // The raw bytes of the block of the directory `directory` at a sub-node VCN,
  // when the caller holds them. Same type as
  // Io::FileRecord<S>::IndexBlockSource.
  using BlockSource =
      std::function<std::span<const BYTE>(ULONGLONG directory, ULONGLONG vcn)>;

  [[nodiscard]] ULONGLONG GetIndexBlockCount() const noexcept;
  [[nodiscard]] bool ParseIndexBlock(const ULONGLONG& vcn,
                                     IndexBlock& ib_class);

  // Makes ParseIndexBlock() take the bytes of a block from source, when it
  // holds them, instead of reading them. directory is the record number the
  // source is asked under. The source MUST outlive this attribute, or be
  // replaced by an empty one first.
  void SetBlockSource(BlockSource source, ULONGLONG directory);

  // The byte offset in the stream of the block at sub-node VCN vcn, or nothing
  // when vcn names no block of this attribute. Logs why, as ParseIndexBlock()
  // does.
  [[nodiscard]] std::optional<ULONGLONG>
      BlockOffset(const ULONGLONG& vcn) const;

  // Reads the raw block at byte offset offset into out, the bytes
  // ParseIndexBlock() would parse. out MUST be one index block long.
  [[nodiscard]] bool ReadBlockBytes(ULONGLONG offset,
                                    std::span<BYTE> out) const;

 private:
  ULONGLONG index_block_count_{0};
  // Where ParseIndexBlock() looks for a block before it reads one. Empty:
  // always from the disk.
  BlockSource block_source_;
  ULONGLONG source_directory_{0};

  [[nodiscard]] bool PatchUS(std::span<WORD> block, DWORD sectors, WORD usn,
                             std::span<const WORD> usarray);

  [[nodiscard]] bool FixupIndexBlock(std::span<BYTE> block);
  [[nodiscard]] bool ParseIndexEntries(std::span<BYTE> block,
                                       IndexBlock& ib_class);
};  // AttrIndexAlloc

}  // namespace Attr

}  // namespace NTFS_BROWSER_NAMESPACE
