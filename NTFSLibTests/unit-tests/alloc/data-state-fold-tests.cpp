#include <array>
#include <span>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/alloc/types.h>
#include <ntfs-browser/namespace.h>

#include "alloc/data-state.h"

namespace Alloc = NTFS_BROWSER_NAMESPACE::Alloc;
using Alloc::ClusterState;
using Alloc::DataState;
using Alloc::FoldDataState;

TEST_CASE("an allocated extent makes a stream reallocated", "[alloc][fold]") {
  const std::array<ClusterState, 3> states{
      ClusterState::Free, ClusterState::Allocated, ClusterState::Free};

  CHECK(FoldDataState(true, states) == DataState::Reallocated);
}

TEST_CASE("an allocated extent survives damage elsewhere", "[alloc][fold]") {
  const std::array<ClusterState, 3> states{
      ClusterState::Unknown, ClusterState::OutOfRange, ClusterState::Allocated};

  CHECK(FoldDataState(false, states) == DataState::Reallocated);
}

TEST_CASE("an incomplete stream is unknown unless it is reallocated",
          "[alloc][fold]") {
  const std::array<ClusterState, 2> free_states{ClusterState::Free,
                                                ClusterState::Free};

  CHECK(FoldDataState(false, free_states) == DataState::Unknown);
  CHECK(FoldDataState(false, {}) == DataState::Unknown);
}

TEST_CASE("an extent that cannot be judged makes a stream unknown",
          "[alloc][fold]") {
  const std::array<ClusterState, 2> out_of_range{ClusterState::Free,
                                                 ClusterState::OutOfRange};
  const std::array<ClusterState, 2> unknown{ClusterState::Unknown,
                                            ClusterState::Free};

  CHECK(FoldDataState(true, out_of_range) == DataState::Unknown);
  CHECK(FoldDataState(true, unknown) == DataState::Unknown);
}

TEST_CASE("free extents make a complete stream intact", "[alloc][fold]") {
  const std::array<ClusterState, 2> states{ClusterState::Free,
                                           ClusterState::Free};

  CHECK(FoldDataState(true, states) == DataState::Intact);
}

TEST_CASE("a complete stream with no extent is intact", "[alloc][fold]") {
  CHECK(FoldDataState(true, {}) == DataState::Intact);
}

TEST_CASE("a bad extent makes a stream bad unless it is reallocated",
          "[alloc][fold]") {
  const std::array<ClusterState, 3> bad_only{
      ClusterState::Free, ClusterState::Bad, ClusterState::Free};
  const std::array<ClusterState, 3> reused{
      ClusterState::Bad, ClusterState::Allocated, ClusterState::Free};

  CHECK(FoldDataState(true, bad_only) == DataState::Bad);
  CHECK(FoldDataState(true, reused) == DataState::Reallocated);
}

TEST_CASE("a bad extent outranks damage and an unjudged extent",
          "[alloc][fold]") {
  const std::array<ClusterState, 3> states{
      ClusterState::Unknown, ClusterState::OutOfRange, ClusterState::Bad};

  CHECK(FoldDataState(false, states) == DataState::Bad);
  CHECK(FoldDataState(true, states) == DataState::Bad);
}
