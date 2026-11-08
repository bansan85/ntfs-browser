#include "corpus-trace-scenarios.h"

#include <ntfs-browser/win-types.h>

#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ntfs-browser/disk-reader.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/tool/fuzzgen/scenario-registry.h>
#include <ntfs-browser/tool/fuzzgen/scenario.h>

#include "corpus-probes.h"
#include "recording-reader.h"
#include "resource-locator.h"
#include "sparse-disk-reader.h"
#include "sparse-image.h"

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

namespace {

// One saved trace.
struct TraceSource {
  // The name of the trace, without the folder and the extension.
  std::string_view trace;
  // The resource archive (folder) that holds its layers and residue.
  std::string_view corpus;
  // The probe that records it.
  CorpusProbes::Probe probe;
  // The feature that the build MUST have to read the volume.
  Feature needs = Feature::None;
};

constexpr std::string_view dftt_corpus = "dftt";
constexpr std::string_view samples_corpus = "ntfs-samples";
constexpr std::string_view nps_corpus = "nps";

// The corpus traces of the family, in the order of the test files.
const std::vector<TraceSource>& Sources() {
  static const std::vector<TraceSource> sources = {
      {"dftt-autodetect-part1", dftt_corpus, CorpusProbes::AutodetectPartition},
      {"dftt-autodetect-part2", dftt_corpus, CorpusProbes::AutodetectPartition},
      {"dftt-autodetect-part3", dftt_corpus, CorpusProbes::AutodetectPartition},
      {"dftt-kwsrch", dftt_corpus, CorpusProbes::KeywordSearch},
      {"dftt-kwsrch-deleted", dftt_corpus, CorpusProbes::KeywordSearch},
      {"dftt-undel", dftt_corpus, CorpusProbes::Undelete},
      {"ntfs-fragmented-mft", samples_corpus, CorpusProbes::FragmentedMft},
      {"ntfs-lastaccess", samples_corpus, CorpusProbes::LastAccess},
      {"ntfs-ptrn", samples_corpus, CorpusProbes::Ptrn},
      {"ntfs-si-vs-fn", samples_corpus, CorpusProbes::SiVsFn},
      {"ntfs", samples_corpus, CorpusProbes::TwoFileTimes},
      {"ntfs-ramslack", samples_corpus, CorpusProbes::RamSlack},
      {"ntfs-2m", samples_corpus, CorpusProbes::TwoMegabyteClusters},
      {"nps-ntfs1-raw", nps_corpus, CorpusProbes::NpsRaw},
      {"nps-ntfs1-compressed", nps_corpus, CorpusProbes::NpsCompressed,
       Feature::Decompression},
      {"nps-ntfs1-scan-threads", nps_corpus, CorpusProbes::NpsScanThreads,
       Feature::Decompression},
  };
  return sources;
}

// The bytes of the trace that source stands for: its probe runs on the sparse
// image of the trace, and a recording reader keeps what the probe reads.
std::vector<BYTE> Record(const TraceSource& source,
                         const ResourceLocator& resources) {
  // The traces that the probe opened, by name. The map keeps the buffers where
  // the readers write.
  std::map<std::string, std::vector<BYTE>> recorded;
  const CorpusProbes::Opener open = [&](std::string_view requested)
      -> std::unique_ptr<NTFS_BROWSER_NAMESPACE::IDiskReader> {
    const std::string name(requested.empty() ? source.trace : requested);
    // A trace file starts again each time the volume is opened.
    recorded[name].clear();
    const std::string stem = std::string(source.corpus) + "/" + name;
    const std::vector<BYTE> layers = resources.Read(stem + ".layers");
    const std::vector<BYTE> residue = resources.Read(stem + ".residue");
    const SparseImage::BlobSource blobs = [&](std::string_view blob) {
      return resources.Read(blob);
    };
    auto image = std::make_shared<const SparseImage>(SparseImage::Load(
        std::string_view(reinterpret_cast<const char*>(layers.data()),
                         layers.size()),
        residue, blobs));
    return std::make_unique<RecordingReader>(
        std::make_unique<SparseDiskReader>(std::move(image)), recorded[name]);
  };
  source.probe(open);
  return recorded[std::string(source.trace)];
}

}  // namespace

void CorpusTraceScenarios::Register(ScenarioRegistry& registry) {
  for (const TraceSource& source : Sources()) {
    registry.Add(
        {.name = "corpus-traces/" + std::string(source.trace) + ".trace",
         .needs = source.needs,
         .make = [&source](const ResourceLocator& resources) {
           return Record(source, resources);
         }});
  }
}

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
