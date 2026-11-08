#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/search/reader.h>
#include <ntfs-browser/search/types.h>
#include <ntfs-browser/test/alloc/alloc-test-support.h>
#include <ntfs-browser/test/fake-image/fake-ntfs-image.h>

#include "reader-support.h"
#include "scan-support.h"

namespace TestSearch = NTFS_BROWSER_NAMESPACE::Test::Search;
namespace TestFake = NTFS_BROWSER_NAMESPACE::Test::FakeImage;
namespace TestAlloc = NTFS_BROWSER_NAMESPACE::Test::Alloc;
namespace Lib = NTFS_BROWSER_NAMESPACE::Search;
using TestSearch::ContentBytes;
using TestSearch::RowNamed;
using TestSearch::TempImage;
using Bytes = std::vector<std::uint8_t>;

namespace {

constexpr std::uint64_t cluster = TestFake::fake_cluster_size;

// All the bytes of the unnamed stream of handle, read in one call.
Bytes ReadAll(Lib::Reader& reader, const Lib::Handle& handle,
              std::uint64_t size) {
  Bytes bytes(static_cast<std::size_t>(size), 0xEE);
  const auto read = reader.Read(handle, u"", 0, bytes);
  REQUIRE(read);
  REQUIRE(read.value == size);
  return bytes;
}

}  // namespace

TEST_CASE("A Reader reads a resident file, live or deleted",
          "[search][reader]") {
  const TempImage image(TestFake::BuildFakeNtfsImageWithFreedFileTree());
  const auto rows = TestSearch::ListImage(image);
  Lib::Reader reader({.target = image.Target()});
  const auto row = RowNamed(rows, u"old.tmp");
  REQUIRE(row.name == u"old.tmp");
  CHECK(row.standing == Lib::Standing::Deleted);
  const std::string_view expected = TestFake::freed_file_content;

  SECTION("the whole content") {
    const Bytes bytes = ReadAll(reader, row.handle, expected.size());

    CHECK(std::string_view(reinterpret_cast<const char*>(bytes.data()),
                           bytes.size()) == expected);
  }

  SECTION("a part of it") {
    Bytes bytes(4, 0xEE);

    const auto read = reader.Read(row.handle, u"", 10, bytes);

    REQUIRE(read);
    CHECK(read.value == 4);
    CHECK(std::string_view(reinterpret_cast<const char*>(bytes.data()), 4) ==
          expected.substr(10, 4));
  }

  SECTION("a buffer larger than the rest stops at the end") {
    Bytes bytes(100, 0xEE);

    const auto read = reader.Read(row.handle, u"", 10, bytes);

    REQUIRE(read);
    CHECK(read.value == expected.size() - 10);
    CHECK(bytes.at(expected.size() - 10) == 0xEE);
  }

  SECTION("at the end and past it") {
    Bytes bytes(8, 0xEE);

    const auto at_end = reader.Read(row.handle, u"", expected.size(), bytes);
    const auto past = reader.Read(row.handle, u"", expected.size() + 99, bytes);

    REQUIRE(at_end);
    CHECK(at_end.value == 0);
    REQUIRE(past);
    CHECK(past.value == 0);
    CHECK(bytes == Bytes(8, 0xEE));
  }
}

TEST_CASE("A Reader reads a live file that sits in clusters",
          "[search][reader]") {
  const TempImage image(TestSearch::ContentImage());
  const auto rows = TestSearch::ListImage(image);
  Lib::Reader reader({.target = image.Target()});
  const auto row = RowNamed(rows, u"data.bin");
  REQUIRE(row.name == u"data.bin");

  SECTION("the whole content") {
    CHECK(ReadAll(reader, row.handle, TestSearch::data_bin_size) ==
          ContentBytes(0, TestSearch::data_bin_size));
  }

  SECTION("a slice across two clusters, not aligned") {
    Bytes bytes(300, 0xEE);

    const auto read = reader.Read(row.handle, u"", cluster - 100, bytes);

    REQUIRE(read);
    CHECK(read.value == 300);
    CHECK(bytes == ContentBytes(cluster - 100, 300));
  }

  SECTION("the last bytes, which end inside a cluster") {
    Bytes bytes(500, 0xEE);

    const auto read =
        reader.Read(row.handle, u"", TestSearch::data_bin_size - 20, bytes);

    REQUIRE(read);
    CHECK(read.value == 20);
    CHECK(Bytes(bytes.begin(), bytes.begin() + 20) ==
          ContentBytes(TestSearch::data_bin_size - 20, 20));
  }
}

TEST_CASE("A Reader reads the hole of a sparse file as zeros",
          "[search][reader]") {
  const TempImage image(TestSearch::ContentImage());
  const auto rows = TestSearch::ListImage(image);
  Lib::Reader reader({.target = image.Target()});
  const auto row = RowNamed(rows, u"holes.bin");
  REQUIRE(row.name == u"holes.bin");

  const Bytes bytes = ReadAll(reader, row.handle, TestSearch::holes_bin_size);

  CHECK(Bytes(bytes.begin(), bytes.begin() + cluster) ==
        ContentBytes(0, cluster));
  CHECK(Bytes(bytes.begin() + cluster, bytes.begin() + 3 * cluster) ==
        Bytes(2 * cluster, 0));
  CHECK(Bytes(bytes.begin() + 3 * cluster, bytes.end()) ==
        ContentBytes(3 * cluster, cluster));
}

TEST_CASE("A Reader reads the clusters of a deleted file", "[search][reader]") {
  const TempImage image(TestSearch::ContentImage());
  const auto rows = TestSearch::ListImage(image);
  Lib::Reader reader({.target = image.Target()});
  const auto row = RowNamed(rows, u"gone.bin");
  REQUIRE(row.name == u"gone.bin");
  CHECK(row.standing == Lib::Standing::Deleted);

  CHECK(ReadAll(reader, row.handle, TestSearch::gone_bin_size) ==
        ContentBytes(0, TestSearch::gone_bin_size));
}

TEST_CASE("A Reader copies a stream to a sink in chunks", "[search][reader]") {
  const TempImage image(TestSearch::ContentImage());
  const auto rows = TestSearch::ListImage(image);
  Lib::Reader reader({.target = image.Target()});
  const auto row = RowNamed(rows, u"data.bin");
  Bytes copied;
  std::size_t chunks = 0;
  const Lib::ByteSink sink = [&](std::span<const std::uint8_t> chunk) {
    copied.insert(copied.end(), chunk.begin(), chunk.end());
    ++chunks;
    return true;
  };

  SECTION("everything") {
    const auto result = reader.CopyTo(row.handle, u"", sink);

    REQUIRE(result);
    CHECK(result.value == TestSearch::data_bin_size);
    CHECK(copied == ContentBytes(0, TestSearch::data_bin_size));
    CHECK(chunks == 1);
  }

  SECTION("an offset and a length") {
    const auto result = reader.CopyTo(row.handle, u"", sink, 1500, 700);

    REQUIRE(result);
    CHECK(result.value == 700);
    CHECK(copied == ContentBytes(1500, 700));
  }

  SECTION("a length past the end stops at the end") {
    const auto result = reader.CopyTo(row.handle, u"", sink,
                                      TestSearch::data_bin_size - 5, 1000);

    REQUIRE(result);
    CHECK(result.value == 5);
  }

  SECTION("an offset past the end gives nothing") {
    const auto result =
        reader.CopyTo(row.handle, u"", sink, TestSearch::data_bin_size + 1);

    REQUIRE(result);
    CHECK(result.value == 0);
    CHECK(chunks == 0);
  }

  SECTION("a sink that refuses ends the copy") {
    const Lib::ByteSink refuse = [](std::span<const std::uint8_t>) {
      return false;
    };

    const auto result = reader.CopyTo(row.handle, u"", refuse);

    CHECK(result.error == Lib::ReadError::Aborted);
    CHECK(result.value == 0);
  }
}

TEST_CASE("A Reader cuts a large copy into chunks of copy_chunk_bytes",
          "[search][reader]") {
  constexpr std::uint64_t clusters = 70;
  constexpr std::uint64_t size = clusters * cluster;
  std::vector<TestFake::FakeTreeNode> nodes;
  nodes.push_back({.links = {{.name = L"big.bin"}},
                   .runs = {{.lcn = std::nullopt, .clusters = clusters}},
                   .stream_size = size});
  const TempImage image(TestFake::BuildFakeNtfsImageWithNameTree(nodes));
  const auto rows = TestSearch::ListImage(image);
  Lib::Reader reader({.target = image.Target()});
  const auto row = RowNamed(rows, u"big.bin");
  REQUIRE(row.name == u"big.bin");
  std::vector<std::size_t> sizes;
  const Lib::ByteSink sink = [&](std::span<const std::uint8_t> chunk) {
    sizes.push_back(chunk.size());
    return true;
  };

  const auto result = reader.CopyTo(row.handle, u"", sink);

  REQUIRE(result);
  CHECK(result.value == size);
  REQUIRE(sizes.size() == 2);
  CHECK(sizes.front() == Lib::copy_chunk_bytes);
  CHECK(sizes.back() == size - Lib::copy_chunk_bytes);
}

TEST_CASE("A Reader fails cleanly on a stream that does not exist",
          "[search][reader]") {
  const TempImage image(TestSearch::ContentImage());
  const auto rows = TestSearch::ListImage(image);
  Lib::Reader reader({.target = image.Target()});
  const auto row = RowNamed(rows, u"data.bin");
  Bytes bytes(4, 0xEE);

  CHECK(reader.Read(row.handle, u"nothing", 0, bytes).error ==
        Lib::ReadError::NoSuchStream);
  CHECK(
      reader.CopyTo(row.handle, u"nothing", [](auto) { return true; }).error ==
      Lib::ReadError::NoSuchStream);
  CHECK(reader.GetDataState(row.handle, u"nothing").error ==
        Lib::ReadError::NoSuchStream);
  CHECK(bytes == Bytes(4, 0xEE));
}

TEST_CASE("A Reader tells what is known of the clusters of a deleted file",
          "[search][reader]") {
  constexpr std::uint64_t first_lcn = 33;
  constexpr std::uint64_t size = 2 * cluster;
  std::vector<BYTE> volume =
      TestFake::BuildFakeNtfsImageWithFreedNonResidentFile(
          size, {{.lcn = first_lcn, .clusters = 2}});

  SECTION("clusters that another file took") {
    const std::vector<TestFake::FakeDataRun> other_file{
        {.lcn = first_lcn + 1, .clusters = 1}};
    const TempImage image(TestAlloc::WithBitmap(std::move(volume), other_file));
    const auto rows = TestSearch::ListImage(image);
    Lib::Reader reader({.target = image.Target()});

    const auto state = reader.GetDataState(RowNamed(rows, u"stale.txt").handle);

    REQUIRE(state);
    CHECK(state.value == Lib::DataState::Reallocated);
  }

  SECTION("clusters that are free") {
    const TempImage image(TestAlloc::WithBitmap(std::move(volume), {}));
    const auto rows = TestSearch::ListImage(image);
    Lib::Reader reader({.target = image.Target()});

    const auto state = reader.GetDataState(RowNamed(rows, u"stale.txt").handle);

    REQUIRE(state);
    CHECK(state.value == Lib::DataState::Intact);
  }

  SECTION("a record with no data stream") {
    const TempImage image(TestAlloc::WithBitmap(std::move(volume), {}));
    const auto rows = TestSearch::ListImage(image);
    Lib::Reader reader({.target = image.Target()});

    const auto state = reader.GetDataState(RowNamed(rows, u"Docs").handle);

    CHECK(state.error == Lib::ReadError::NoSuchStream);
  }
}
