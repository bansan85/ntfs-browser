#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <format>
#include <initializer_list>
#include <iostream>
#include <istream>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <ntfs-browser/example/ntfsundel/ntfsundel-cli.h>
#include <ntfs-browser/example/ntfsundel/ntfsundel-core.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/reader.h>
#include <ntfs-browser/tool/device-access/device-path.h>
#include <ntfs-browser/tool/device-access/text-convert.h>
#include <ntfs-browser/tool/device-access/tool-support.h>
#include <ntfs-browser/tool/device-access/volume-identity.h>
#include <ntfs-browser/tool/device-access/volume-target.h>
#include <ntfs-browser/types.h>

#ifdef _WIN32
  #include <io.h>
#else
  #include <unistd.h>
#endif

namespace Lib = NTFS_BROWSER_NAMESPACE;

namespace NTFS_BROWSER_NAMESPACE::Example::NtfsUndel {

namespace {

namespace Device = Tool::DeviceAccess;

// Digits a record number or a list position may have. Nine always fit a
// 32-bit value, so the parse needs no overflow test for a position. A record
// number is checked on its own.
constexpr std::size_t max_position_digits = 9;

// What one recovery came to.
enum class Outcome : std::uint8_t {
  // The file is in the output folder.
  Recovered,
  // Nothing to copy: the record has no data.
  NoData,
  // The file was left out on purpose or could not be recovered.
  Failed
};

// Parses text as a number of decimal digits. Nothing for an empty text, a
// character that is no digit, or a value that does not fit.
std::optional<std::uint64_t> ParseUnsigned(std::wstring_view text) {
  if (text.empty()) {
    return std::nullopt;
  }
  std::uint64_t value = 0;
  for (const wchar_t character : text) {
    if (character < L'0' || character > L'9') {
      return std::nullopt;
    }
    const auto digit = static_cast<std::uint64_t>(character - L'0');
    if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) {
      return std::nullopt;
    }
    value = value * 10 + digit;
  }
  return value;
}

// How an option that takes a value met an argument.
enum class ValueResult : std::uint8_t {
  // The argument is another option.
  NotThis,
  // The value is in value.
  Found,
  // The option is last, and has no value.
  Missing
};

// Reads the value of the option called name from argv[index]. The value is
// the text after an equal sign, or the next argument, which index then skips.
ValueResult TakeValue(std::wstring_view name, int argc, wchar_t* argv[],
                      int& index, std::wstring& value) {
  const std::wstring_view arg = argv[index];
  if (arg == name) {
    if (index + 1 >= argc) {
      return ValueResult::Missing;
    }
    value = argv[++index];
    return ValueResult::Found;
  }
  if (arg.size() > name.size() && arg.starts_with(name) &&
      arg[name.size()] == L'=') {
    value = arg.substr(name.size() + 1);
    return ValueResult::Found;
  }
  return ValueResult::NotThis;
}

// Fills the option of args that argv[index] names, when it names one of the
// options ntfsundel adds to the common ones. Returns false when the option
// is malformed, and says why in error. handled says whether it was one of
// them.
bool ParseOwnOption(int argc, wchar_t* argv[], int& index, Arguments& args,
                    bool& handled, std::wstring& error) {
  const std::wstring_view arg = argv[index];
  handled = true;
  if (arg == L"--all") {
    args.all = true;
    return true;
  }
  if (arg == L"--include-doubtful") {
    args.include_doubtful = true;
    return true;
  }

  // Takes the value of the option under any of names. A value that is absent
  // or empty is the error needs.
  std::wstring value;
  const auto take = [&](std::initializer_list<std::wstring_view> names,
                        std::wstring_view needs) {
    ValueResult result = ValueResult::NotThis;
    for (const std::wstring_view name : names) {
      result = TakeValue(name, argc, argv, index, value);
      if (result != ValueResult::NotThis) {
        break;
      }
    }
    if (result == ValueResult::Found && value.empty()) {
      result = ValueResult::Missing;
    }
    if (result == ValueResult::Missing) {
      error = needs;
    }
    return result;
  };

  constexpr std::wstring_view record_needs = L"--record needs a record number";
  if (take({L"--record"}, record_needs) != ValueResult::NotThis) {
    const std::optional<std::uint64_t> record = ParseUnsigned(value);
    if (!record) {
      error = record_needs;
      return false;
    }
    args.records.push_back(*record);
    return true;
  }

  ValueResult result = take({L"--name"}, L"--name needs a pattern");
  if (result != ValueResult::NotThis) {
    if (result == ValueResult::Found) {
      args.names.push_back(value);
    }
    return result == ValueResult::Found;
  }

  result = take({L"--output", L"-o"}, L"--output needs a folder");
  if (result != ValueResult::NotThis) {
    if (result == ValueResult::Found) {
      args.output = value;
    }
    return result == ValueResult::Found;
  }

  handled = false;
  return true;
}

// Writes text to stream as UTF-8.
void Say(std::FILE* stream, const std::wstring& text) {
  Device::WriteText(stream, text);
}

// The text of a path as the tools print it.
std::wstring Shown(const std::filesystem::path& path) {
  return Device::WideFromPath(path);
}

// What a record that the Reader cannot read tells the user.
std::wstring OpenFailureText(std::uint64_t reference) {
  return std::format(L"Cannot read record {}", reference);
}

// What a failed CreateRecoveryFile() tells the user.
std::wstring CreateFailureText(CreateError error, std::wstring_view name,
                               std::wstring_view directory) {
  switch (error) {
    case CreateError::PathTooLong:
      return std::format(L"File path too long for {} in {}", name, directory);
    case CreateError::NoFreeName:
      return std::format(L"Too many files called {} in {}", name, directory);
    case CreateError::Failed:
      break;
  }
  return std::format(L"Cannot create {} in {}", name, directory);
}

// What to add to the report of a file that was saved under another name.
std::wstring_view ChangeNote(NameChange change) {
  switch (change) {
    case NameChange::Unsafe:
      return L" (the name was changed: the file system cannot take it)";
    case NameChange::Taken:
      return L" (a file of that name was already there)";
    case NameChange::None:
      break;
  }
  return {};
}

// What a recovery run works with, the same for every file.
struct RecoveryContext {
  Lib::Reader& reader;
  const Arguments& args;
  const Environment& env;
};

// Decides whether the file may be recovered. Says why on env.err when it may
// not, and warns when it may with a doubt. state is what DescribeFile() found
// for the same record.
bool ConfirmRecovery(const DeletedFile& file, const FileState& state,
                     const RecoveryContext& context) {
  const Arguments& args = context.args;
  const Environment& env = context.env;
  const bool go_on =
      ShouldRecover(state.check, [&](const RecoveryCheck& check) {
        const std::wstring message = RecoveryMessage(check);
        if (check.advice == RecoveryAdvice::Warn || args.include_doubtful) {
          Say(env.err, std::format(L"Warning, record {} ({}): {}\n",
                                   file.reference, file.full_path, message));
          return true;
        }
        Say(env.err, std::format(L"Skipped record {} ({}): {} Use "
                                 L"--include-doubtful to recover it anyway.\n",
                                 file.reference, file.full_path, message));
        return false;
      });
  if (!go_on) {
    return false;
  }
  if (state.size > kConfirmFileSize && !args.include_doubtful) {
    Say(env.err,
        std::format(L"Skipped record {} ({}): the file is {} bytes. Use "
                    L"--include-doubtful to recover it anyway.\n",
                    file.reference, file.full_path, state.size));
    return false;
  }
  return true;
}

// Recovers the unnamed stream of file into directory. state is what
// DescribeFile() found for the same record.
Outcome RecoverOne(const RecoveryContext& context, const DeletedFile& file,
                   const FileState& state, std::wstring_view directory) {
  const Environment& env = context.env;
  if (!state.readable) {
    Say(env.err, OpenFailureText(file.reference) + L"\n");
    return Outcome::Failed;
  }
  if (!state.has_data) {
    Say(env.err, std::format(L"Skipped record {} ({}): it has no data\n",
                             file.reference, file.full_path));
    return Outcome::NoData;
  }
  if (!ConfirmRecovery(file, state, context)) {
    return Outcome::Failed;
  }

  CreatedFile created =
      CreateRecoveryFile(directory, file.name, ExistingFile::KeepBoth);
  if (!created.file) {
    Say(env.err,
        CreateFailureText(created.error, file.name, directory) + L"\n");
    return Outcome::Failed;
  }

  RecoveryFile& target = *created.file;
  const CopyResult copied = CopyUnnamedData(
      context.reader, file.handle, [](std::uint64_t) { return true; },
      [&target](std::span<const std::uint8_t> chunk) {
        return target.Write(chunk);
      });
  if (copied.error != RecoverError::None) {
    Say(env.err,
        std::format(L"{} while recovering record {} to {}\n",
                    copied.error == RecoverError::Write ? L"Write data error"
                                                        : L"Read data error",
                    file.reference, target.Path()));
    return Outcome::Failed;
  }
  Say(env.out,
      std::format(L"Recovered record {} to {}, {} bytes{}\n", file.reference,
                  target.Path(), copied.bytes, ChangeNote(target.Change())));
  return Outcome::Recovered;
}

// Asks for the files to recover and returns their positions. An empty line,
// "q" and the end of the input all give none.
std::vector<std::size_t> AskForFiles(const Environment& env,
                                     std::size_t count) {
  if (env.in == nullptr || count == 0) {
    return {};
  }
  std::string line;
  while (true) {
    Say(env.out,
        L"Files to recover (numbers like 1,3,5-8, all, or empty to quit): ");
    (void)std::fflush(env.out);
    if (!std::getline(*env.in, line)) {
      return {};
    }
    if (line == "q" || line == "Q") {
      return {};
    }
    if (const auto positions = ParseNumberList(line, count)) {
      return *positions;
    }
    Say(env.err,
        std::format(L"Not a valid list. Use numbers from 1 to {}, ranges like "
                    L"3-5, or all.\n",
                    count));
  }
}

// What the search found and what is known of each file.
struct Listing {
  std::vector<DeletedFile> files;
  std::vector<FileState> states;
  bool truncated = false;
};

// Scans the volume for deleted files, and describes each one. Nothing when the
// scan cannot run.
std::optional<Listing> BuildListing(Lib::Reader& reader,
                                    std::wstring_view target,
                                    bool recover_errors) {
  std::optional<SearchResult> found =
      ListDeletedFiles(target, recover_errors, SearchControl{});
  if (!found) {
    return std::nullopt;
  }

  Listing listing;
  listing.truncated = found->truncated;
  for (DeletedFile& file : found->files) {
    listing.states.push_back(DescribeFile(reader, file));
    listing.files.push_back(std::move(file));
  }
  return listing;
}

// Recovers the files at positions into the folder output.
int RecoverFiles(const Listing& listing,
                 const std::vector<std::size_t>& positions,
                 const std::filesystem::path& output,
                 const RecoveryContext& context) {
  const Environment& env = context.env;
  std::error_code error;
  std::filesystem::create_directories(Device::ExtendedFsPath(output), error);
  if (error) {
    Say(env.err, L"Cannot create the folder " + Shown(output) + L"\n");
    return failure_exit_code;
  }

  const std::wstring directory = Shown(output);
  std::size_t recovered = 0;
  std::size_t failed = 0;
  for (const std::size_t position : positions) {
    const Outcome outcome = RecoverOne(context, listing.files.at(position),
                                       listing.states.at(position), directory);
    recovered += outcome == Outcome::Recovered ? 1 : 0;
    failed += outcome == Outcome::Failed ? 1 : 0;
  }
  Say(env.out, std::format(L"{} recovered, {} failed\n", recovered, failed));
  return failed == 0 ? 0 : failure_exit_code;
}

// Reports whether input is a terminal a person types at.
bool StdinIsTerminal() {
#ifdef _WIN32
  return _isatty(_fileno(stdin)) != 0;
#else
  return isatty(STDIN_FILENO) != 0;
#endif
}

}  // namespace

std::string UsageText() {
  std::string usage =
      "Usage: ntfsundel [options] \"target\"\n"
      "Lists the deleted files of a volume. With --output, recovers some of "
      "them.\n"
      "\n"
      "Selection (with --output; one of them, or none to be asked):\n"
      "  --all                recover every listed file\n"
      "  --record N           recover the file of MFT record N (repeatable)\n"
      "  --name PATTERN       recover the files whose name matches PATTERN,\n"
      "                       with * and ?. Case does not count: letters of\n"
      "                       any script are folded through the $UpCase table\n"
      "                       of the volume. Repeatable. A pattern with a\n"
      "                       slash is matched against the whole path.\n"
      "  With none of them and a terminal on input, the list is numbered and\n"
      "  the numbers to recover are read from the input.\n"
      "\n"
      "Destination:\n"
      "  --output DIR, -o DIR the folder that receives the files. Without it\n"
      "                       the program only lists. DIR MUST be on another\n"
      "                       drive than the one that is read, so the files\n"
      "                       do not overwrite deleted data. There is no way\n"
      "                       around this check. A file that exists is kept:\n"
      "                       the copy gets a number, as in \"name (2)\".\n"
      "  --include-doubtful   also recover a file whose clusters were given "
      "to\n"
      "                       another file, that sit on bad clusters, or that\n"
      "                       is over 100 MiB. Without it such a file is left\n"
      "                       out and the exit code is 1.\n"
      "\n"
      "Other options:\n";
  usage += Device::CommonOptionsUsage();
  usage +=
      "                       (--deleted is always on here)\n"
      "\n"
      "Each listed line holds the number, the MFT record, the size in bytes,\n"
      "the data state (intact, reallocated, bad, unchecked, no-data,\n"
      "unreadable), the last write time and the path.\n"
      "\n"
      "target is an image file or a device, followed by an optional folder\n"
      "inside the volume that restricts the list";
#ifdef _WIN32
  usage += ", or a drive path.\n";
  usage += "eg. ntfsundel c:\\ --output d:\\recovered --all\n";
  usage += "eg. ntfsundel c:\\Users --name *.docx --output d:\\recovered\n";
#else
  usage += ".\n";
  usage += "eg. ntfsundel /dev/sda1 --output /mnt/backup/recovered --all\n";
  usage += "eg. ntfsundel ./disk.img/Docs --record 1234 -o /mnt/backup/out\n";
#endif
  return usage;
}

std::optional<Arguments> ParseArguments(int argc, wchar_t* argv[],
                                        std::wstring& error) {
  Arguments args;
  bool has_target = false;
  for (int i = 1; i < argc; i++) {
    const std::wstring_view arg = argv[i];
    const Device::OptionResult common =
        Device::ParseCommonOption(arg, args.common);
    if (common == Device::OptionResult::Malformed) {
      error = L"Malformed option " + std::wstring(arg);
      return std::nullopt;
    }
    if (common == Device::OptionResult::Accepted) {
      continue;
    }

    bool handled = false;
    if (!ParseOwnOption(argc, argv, i, args, handled, error)) {
      return std::nullopt;
    }
    if (handled) {
      continue;
    }
    if (arg.starts_with(L"-") && arg.size() > 1) {
      error = L"Unknown option " + std::wstring(arg);
      return std::nullopt;
    }
    if (has_target) {
      error = L"Only one target is allowed";
      return std::nullopt;
    }
    args.target = arg;
    has_target = true;
  }

  if (!has_target && !args.common.help) {
    error = L"A target is needed";
    return std::nullopt;
  }
  return args;
}

Selection SelectFiles(const std::vector<DeletedFile>& files,
                      const Arguments& args,
                      const std::set<std::uint64_t>& named) {
  Selection selection;
  for (std::size_t i = 0; i < files.size(); i++) {
    const DeletedFile& file = files[i];
    const bool by_record =
        std::ranges::find(args.records, file.reference) != args.records.end();
    const bool by_name = named.contains(file.reference);
    if (args.all || by_record || by_name) {
      selection.positions.push_back(i);
    }
  }
  for (const std::uint64_t record : args.records) {
    const bool listed =
        std::ranges::any_of(files, [record](const DeletedFile& file) {
          return file.reference == record;
        });
    if (!listed && std::ranges::find(selection.missing_records, record) ==
                       selection.missing_records.end()) {
      selection.missing_records.push_back(record);
    }
  }
  return selection;
}

namespace {

// Reads one number of 1 to count, as a position from 0.
std::optional<std::size_t> ParsePosition(std::string_view text,
                                         std::size_t count) {
  if (text.empty() || text.size() > max_position_digits) {
    return std::nullopt;
  }
  std::size_t value = 0;
  for (const char character : text) {
    if (character < '0' || character > '9') {
      return std::nullopt;
    }
    value = value * 10 + static_cast<std::size_t>(character - '0');
  }
  if (value == 0 || value > count) {
    return std::nullopt;
  }
  return value - 1;
}

// The positions of a typed list, in the order typed, each once.
class PositionList {
 public:
  // A list over files numbered from 1 to count.
  explicit PositionList(std::size_t count) : seen_(count, false) {}

  // Returns the number of files the positions may point at.
  [[nodiscard]] std::size_t Count() const { return seen_.size(); }

  // Appends position, unless the list already holds it.
  void Add(std::size_t position) {
    if (!seen_[position]) {
      seen_[position] = true;
      positions_.push_back(position);
    }
  }

  // Hands over the positions. The list is spent after that.
  [[nodiscard]] std::vector<std::size_t> Release() {
    return std::move(positions_);
  }

 private:
  std::vector<std::size_t> positions_;
  std::vector<bool> seen_;
};

// Adds what one token of a typed list stands for: "all", a number, or a range
// like "3-5". Returns false when the token is none of them.
bool AddToken(std::string_view token, PositionList& list) {
  if (token == "all" || token == "ALL" || token == "All") {
    for (std::size_t i = 0; i < list.Count(); i++) {
      list.Add(i);
    }
    return true;
  }
  const std::size_t dash = token.find('-');
  const std::optional<std::size_t> first =
      ParsePosition(token.substr(0, dash), list.Count());
  if (!first) {
    return false;
  }
  if (dash == std::string_view::npos) {
    list.Add(*first);
    return true;
  }
  const std::optional<std::size_t> last =
      ParsePosition(token.substr(dash + 1), list.Count());
  if (!last || *last < *first) {
    return false;
  }
  for (std::size_t i = *first; i <= *last; i++) {
    list.Add(i);
  }
  return true;
}

}  // namespace

std::optional<std::vector<std::size_t>> ParseNumberList(std::string_view line,
                                                        std::size_t count) {
  PositionList list(count);
  std::size_t start = 0;
  while (start < line.size()) {
    const std::size_t end = line.find_first_of(" ,\t\r", start);
    const std::string_view token =
        line.substr(start, end == std::string_view::npos ? end : end - start);
    start = end == std::string_view::npos ? line.size() : end + 1;
    if (!token.empty() && !AddToken(token, list)) {
      return std::nullopt;
    }
  }
  return list.Release();
}

FileState DescribeFile(Lib::Reader& reader, const DeletedFile& file) {
  const auto streams = reader.GetStreams(file.handle);
  if (!streams) {
    return {};
  }
  FileState state;
  state.readable = true;
  const auto unnamed =
      std::ranges::find_if(streams.value, [](const Lib::StreamInfo& stream) {
        return stream.name.empty();
      });
  state.has_data = unnamed != streams.value.end();
  state.size = state.has_data ? unnamed->size : 0;
  state.check = AssessRecovery(reader, file.handle);
  return state;
}

std::wstring_view StateName(const FileState& state) {
  if (!state.readable) {
    return L"unreadable";
  }
  if (!state.has_data) {
    return L"no-data";
  }
  switch (state.check.cause) {
    case RecoveryCause::Reallocated:
      return L"reallocated";
    case RecoveryCause::BadClusters:
      return L"bad";
    case RecoveryCause::Unchecked:
      return L"unchecked";
    case RecoveryCause::None:
      break;
  }
  return L"intact";
}

std::wstring FormatListLine(std::size_t number, const DeletedFile& file,
                            const FileState& state) {
  return std::format(L"{:>4}  {:>8}  {:>12}  {:<11}  {}  {}", number,
                     file.reference, state.size, StateName(state),
                     FormatLocalTime(file.write_time), file.full_path);
}

std::wstring CheckOutputVolume(const std::filesystem::path& source,
                               const std::filesystem::path& output,
                               const VolumeIdFunction& volume_id) {
  const std::optional<std::string> output_id = volume_id(output);
  if (!output_id) {
    return L"Cannot tell which drive holds " + Shown(output) +
           L", so it is not used.";
  }
  const std::optional<std::string> source_id = volume_id(source);
  if (!source_id) {
    return L"Cannot tell which drive holds " + Shown(source) +
           L", so the folder " + Shown(output) + L" is not used.";
  }
  if (*output_id == *source_id) {
    return L"The folder " + Shown(output) + L" is on the drive that holds " +
           Shown(source) +
           L". Recovered files could overwrite deleted data. Choose a folder "
           L"on another drive.";
  }
  return {};
}

namespace {

// Checks the target and the output folder, then opens the Reader on the
// target. Says why on env.err and returns nothing when the run cannot go on.
std::unique_ptr<Lib::Reader> OpenForRecovery(const Arguments& args,
                                             const Environment& env) {
  const std::optional<std::filesystem::path> source =
      Device::TargetSource(args.target);
  if (!source) {
    Say(env.err,
        L"Cannot find a device or image file in " + args.target + L"\n");
    return nullptr;
  }

  if (!args.output.empty()) {
    const std::wstring refusal = CheckOutputVolume(
        *source, Device::PathFromWide(args.output), env.volume_id);
    if (!refusal.empty()) {
      Say(env.err, refusal + L"\n");
      return nullptr;
    }
  }

  auto reader = std::make_unique<Lib::Reader>(Lib::ReaderOptions{
      .target = ToUtf16(args.target),
      .recover_errors = args.common.volume_options.recover_errors});
  if (!reader->IsOpen()) {
    Say(env.err, L"Cannot read an NTFS volume from " + Shown(*source) + L"\n");
    return nullptr;
  }
  return reader;
}

// Prints one line per listed file, and a warning when the list is cut short.
void PrintListing(const Listing& listing, const Environment& env) {
  for (std::size_t i = 0; i < listing.files.size(); i++) {
    Say(env.out,
        FormatListLine(i + 1, listing.files[i], listing.states[i]) + L"\n");
  }
  if (listing.truncated) {
    Say(env.err, TooManyFilesMessage() + L"\n");
  }
}

// What to recover, or the exit code of a run that stops before.
struct Choice {
  std::vector<std::size_t> positions;
  std::optional<int> exit_code;
};

// A choice that ends the run with code.
Choice StopWith(int code) { return {.positions = {}, .exit_code = code}; }

// Chooses the files that --all, --record and --name pick. A record that is
// not listed, and a selection that picks nothing, end the run.
Choice ChooseBySelection(const Listing& listing, const Arguments& args,
                         const Environment& env) {
  std::set<std::uint64_t> named;
  for (const std::wstring& pattern : args.names) {
    const std::optional<std::set<std::uint64_t>> found = FindNamedFiles(
        args.target, args.common.volume_options.recover_errors, pattern);
    if (!found) {
      Say(env.err, L"The scan failed\n");
      return StopWith(failure_exit_code);
    }
    named.insert(found->begin(), found->end());
  }
  Selection selection = SelectFiles(listing.files, args, named);
  for (const std::uint64_t record : selection.missing_records) {
    Say(env.err, std::format(L"Record {} is not in the list\n", record));
  }
  if (!selection.missing_records.empty()) {
    return StopWith(failure_exit_code);
  }
  if (selection.positions.empty()) {
    Say(env.err, L"No listed file matches\n");
    return StopWith(failure_exit_code);
  }
  return {.positions = std::move(selection.positions), .exit_code = {}};
}

// Chooses the files to recover: the selection of the command line, else the
// answer of the user, else nothing and a refusal.
Choice ChoosePositions(const Listing& listing, const Arguments& args,
                       const Environment& env) {
  if (args.HasSelection()) {
    return ChooseBySelection(listing, args, env);
  }
  if (env.interactive) {
    std::vector<std::size_t> positions = AskForFiles(env, listing.files.size());
    if (positions.empty()) {
      return StopWith(0);
    }
    return {.positions = std::move(positions), .exit_code = {}};
  }
  Say(env.err, L"Nothing is selected. Give --all, --record or --name.\n");
  return StopWith(failure_exit_code);
}

}  // namespace

int UndelTarget(const Arguments& args, const Environment& env) {
  const std::unique_ptr<Lib::Reader> reader = OpenForRecovery(args, env);
  if (!reader) {
    return failure_exit_code;
  }

  const std::optional<Listing> listing = BuildListing(
      *reader, args.target, args.common.volume_options.recover_errors);
  if (!listing) {
    Say(env.err, L"The scan failed\n");
    return failure_exit_code;
  }
  PrintListing(*listing, env);
  if (args.output.empty()) {
    Say(env.err, std::format(L"{} deleted files. Add --output DIR and --all, "
                             L"--record or --name to recover some.\n",
                             listing->files.size()));
    return 0;
  }

  const Choice choice = ChoosePositions(*listing, args, env);
  if (choice.exit_code) {
    return *choice.exit_code;
  }
  return RecoverFiles(*listing, choice.positions,
                      Device::PathFromWide(args.output),
                      {.reader = *reader, .args = args, .env = env});
}

int RunNtfsUndel(int argc, wchar_t* argv[]) {
  std::wstring error;
  const std::optional<Arguments> args = ParseArguments(argc, argv, error);
  if (!args) {
    Say(stderr, error + L"\n");
    (void)std::fputs(UsageText().c_str(), stderr);
    return failure_exit_code;
  }
  if (args->common.help) {
    (void)std::fputs(UsageText().c_str(), stdout);
    return 0;
  }

  // Names print as UTF-8: a console shows them right with its code page set.
  const Device::Utf8ConsoleOutput console;
  if (!Device::ConfigureLog(args->common.log_config)) {
    Device::ReportLogFileError(args->common.log_config);
  }
  const Environment environment{.out = stdout,
                                .err = stderr,
                                .in = &std::cin,
                                .interactive = StdinIsTerminal(),
                                .volume_id = Device::HostVolumeId};
  return UndelTarget(*args, environment);
}

}  // namespace NTFS_BROWSER_NAMESPACE::Example::NtfsUndel
