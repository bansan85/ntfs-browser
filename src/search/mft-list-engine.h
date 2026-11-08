#pragma once

#include <stop_token>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/search/types.h>

#include "internal-export.h"
#include "search/engine.h"
#include "search/target.h"

namespace NTFS_BROWSER_NAMESPACE::Search {

// The NtfsMftList walk: reads every record of the $MFT in record order, builds
// the tree of names from the parent reference of each $FILE_NAME, and gives
// the sink one entry for each name of each record.
//
// - A record with several names (hard links) gives one entry for each, all with
//   the same record number and a link_count of the names. A DOS 8.3 alias is
//   no name of its own and is hidden, like in the directory engines. A record
//   without a name is not listed: there is nothing to find it by.
// - An entry that no directory reaches (an orphan) is listed. Its path, when
//   asked, starts with "\<lost>".
// - Size, allocated size, the four times and the attribute bits come from the
//   record itself: the unnamed $DATA stream and $STANDARD_INFORMATION. Without
//   a $DATA stream the size is the one of the $FILE_NAME. The attribute bits
//   are those that the record shows: read only, hidden, system, directory,
//   archive, sparse, compressed and encrypted.
// - A deleted record has standing Deleted. With Options::deleted_data_state it
//   gets a data_state and the count of its clusters by condition.
// - It always reads the whole volume. With an inside path it lists only what
//   sits under that folder: its direct entries, or with Options::recursive all
//   the entries below. With no inside path it lists every entry, whatever
//   Options::recursive says.
// - Reports the percent of the record slots read.
class NTFS_BROWSER_EXPORT_TESTS_ONLY MftListEngine final : public Engine {
 public:
  MftListEngine(Options options, TargetParts parts);

  [[nodiscard]] bool Run(EntrySink& sink, std::stop_token stop) override;

 private:
  Options options_;
  TargetParts parts_;
};

}  // namespace NTFS_BROWSER_NAMESPACE::Search
