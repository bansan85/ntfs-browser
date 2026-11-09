#include "upcase/upcase-loader.h"

#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include <ntfs-browser/namespace.h>

#include "attr/base.h"
#include "cache/strategy.h"
#include "io/file-record.h"
#include "mft/idx.h"
#include "mft/metadata-opener.h"
#include "upcase/upcase.h"
#include "win-types.h"

namespace NTFS_BROWSER_NAMESPACE::UpCase {

template <Cache::Strategy S>
TableLoader<S>::TableLoader(const Mft::MetadataOpener<S>& opener) noexcept
    : opener_(opener) {}

template <Cache::Strategy S>
std::unique_ptr<const Table> TableLoader<S>::Load() const {
  const std::unique_ptr<Io::FileRecord<S>> record =
      opener_.OpenData(static_cast<ULONGLONG>(Mft::Idx::UpCase));
  if (!record) {
    return {};
  }

  const Attr::AttrBase<S>* data = record->FindStream({});
  if (data == nullptr || data->GetDataSize() < Table::byte_count) {
    return {};
  }

  std::vector<BYTE> bytes(Table::byte_count);
  const std::optional<ULONGLONG> len = data->ReadData(0, bytes);
  if (!len || *len != bytes.size()) {
    return {};
  }

  std::optional<Table> table = Table::FromBytes(bytes);
  if (!table) {
    return {};
  }
  return std::make_unique<const Table>(std::move(*table));
}

template class TableLoader<Cache::Strategy::NoCache>;
template class TableLoader<Cache::Strategy::FullCache>;

}  // namespace NTFS_BROWSER_NAMESPACE::UpCase
