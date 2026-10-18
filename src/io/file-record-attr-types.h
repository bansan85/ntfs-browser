#pragma once

#include <ntfs-browser/win-types.h>

#include <type_traits>

#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/namespace.h>

#include "attr/resident.h"

namespace NTFS_BROWSER_NAMESPACE::Io {

// How the walk over a record's attribute headers ended.
enum class WalkEnd : BYTE {
  EndMarker,  // The terminating Attr::Type::All marker was reached.
  RanOut,     // The record ended without a marker, or an attribute overran it.
  Aborted     // An attribute was malformed, or the visit stopped the walk.
};

// The resident base the attribute wrappers of strategy S are built over.
template <Cache::Strategy S>
using ResidentOf =
    std::conditional_t<S == Cache::Strategy::NoCache, Attr::AttrResidentNoCache,
                       Attr::AttrResidentFullCache>;

}  // namespace NTFS_BROWSER_NAMESPACE::Io
