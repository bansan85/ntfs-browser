#include "search/engine.h"

#include <memory>
#include <utility>

#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/search/types.h>

#include "log/ntfs-common.h"
#include "search/directory-engine.h"
#include "search/mft-list-engine.h"
#include "search/target.h"

namespace NTFS_BROWSER_NAMESPACE::Search {

EngineKind ResolveEngine(const Options& options, const TargetParts& parts) {
  if (options.engine != EngineKind::Auto) {
    return options.engine;
  }
  // A folder is walked through its index. The whole volume is read in $MFT
  // order, which needs no seek.
  return parts.inside.empty() ? EngineKind::MftList
                              : EngineKind::DirectoryFullCache;
}

std::unique_ptr<Engine> MakeEngine(const Options& options,
                                   const TargetParts& parts) {
  switch (ResolveEngine(options, parts)) {
    case EngineKind::DirectoryNoCache:
      return std::make_unique<DirectoryEngine<Cache::Strategy::NoCache>>(
          options, parts);
    case EngineKind::DirectoryFullCache:
      return std::make_unique<DirectoryEngine<Cache::Strategy::FullCache>>(
          options, parts);
    case EngineKind::MftList:
      return std::make_unique<MftListEngine>(options, parts);
    case EngineKind::Auto:
      break;
  }
  Log::Error("This scan engine is not available");
  return nullptr;
}

}  // namespace NTFS_BROWSER_NAMESPACE::Search
