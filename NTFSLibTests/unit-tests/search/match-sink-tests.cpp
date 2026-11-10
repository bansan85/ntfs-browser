#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/types.h>

#include "search/engine.h"
#include "search/match-sink.h"
#include "upcase/upcase.h"

namespace Lib = NTFS_BROWSER_NAMESPACE;

namespace {

// A sink that keeps the names and the percents it receives.
class RecordingSink final : public Lib::EntrySink {
 public:
  void Add(const Lib::Entry& entry) override { names.emplace_back(entry.name); }

  void SetPercent(double percent) override { percents.push_back(percent); }

  std::vector<std::u16string> names;
  std::vector<double> percents;
};

}  // namespace

TEST_CASE("A matching sink passes the progress on and filters the entries",
          "[search][match-sink]") {
  RecordingSink inner;
  Lib::Options options;
  options.pattern = {.kind = Lib::PatternKind::Exact, .text = u"keep.txt"};
  const auto sink = Lib::MatchingSink::Make(
      inner, options, Lib::UpCase::Table::BuiltIn(), Lib::EngineKind::MftList);
  REQUIRE(sink != nullptr);
  Lib::Entry kept;
  kept.name = u"keep.txt";
  Lib::Entry dropped;
  dropped.name = u"drop.txt";

  sink->SetPercent(40.0);
  sink->Add(dropped);
  sink->Add(kept);

  REQUIRE(inner.percents.size() == 1);
  CHECK(inner.percents.front() == 40.0);
  REQUIRE(inner.names.size() == 1);
  CHECK(inner.names.front() == u"keep.txt");
}
