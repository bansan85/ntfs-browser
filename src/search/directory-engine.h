#pragma once

#include <stop_token>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/types.h>

#include "cache/strategy.h"
#include "internal-export.h"
#include "search/engine.h"
#include "search/target.h"

namespace NTFS_BROWSER_NAMESPACE {

// The NtfsDir walk: reads the index of the target folder and, with
// Options::recursive, of every folder below it, depth first, in index order.
// NoCache and FullCache are the two engines of that name. Sees one name at a
// time, so link_count stays 0, and the size and the times are those of the
// $FILE_NAME in the index. A deleted entry gets its data_state from the file
// record, when the options ask. Hides the system files (records below 16) and
// the DOS alias names, like NtfsDir.
template <NTFS_BROWSER_NAMESPACE::Cache::Strategy S>
class NTFS_BROWSER_EXPORT_TESTS_ONLY DirectoryEngine final : public Engine {
 public:
  DirectoryEngine(Options options, TargetParts parts);

  [[nodiscard]] bool Run(EntrySink& sink, std::stop_token stop) override;

 private:
  Options options_;
  TargetParts parts_;
};

}  // namespace NTFS_BROWSER_NAMESPACE
