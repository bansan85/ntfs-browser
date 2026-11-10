#pragma once

#include <functional>
#include <string>
#include <vector>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/tool/fuzzgen/build-features.h>
#include <ntfs-browser/tool/fuzzgen/scenario-registry.h>

#include "win-types.h"

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

// The inputs that the public API reads in step: a sound volume that Scan lists
// and Reader reads. Each image is saved three times, as the reads of the
// MftList scan, of the recursive walk and of the Reader.
class PublicApiScenarios {
 public:
  // Adds the three inputs of one image: api_mft_list_<stem>,
  // api_directory_<stem> and api_reader_<stem>. With with_folder, also the two
  // inputs that enter the folder Docs of the image: api_mft_list_folder_<stem>
  // and api_directory_folder_<stem>. The inputs need needs.
  static void AddImage(ScenarioRegistry& registry, const std::string& stem,
                       const std::function<std::vector<BYTE>()>& make,
                       bool with_folder = false, Feature needs = Feature::None);

  // Adds the inputs of the fake volumes whose files cover every kind of entry
  // the listings and the Reader know: the tree of names, the $MFT tree, a file
  // with an attribute list, freed files, a named stream and a journal.
  static void Register(ScenarioRegistry& registry);
};

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
