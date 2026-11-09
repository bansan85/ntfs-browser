#include "search/regex-matcher.h"

// PCRE2 in its 16 bit form: a code unit is a UTF-16 unit.
#define PCRE2_CODE_UNIT_WIDTH 16
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <pcre2.h>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/types.h>

#include "log/ntfs-common.h"
#include "search/case-folder.h"
#include "search/matcher.h"
#include "utf/utf.h"

namespace NTFS_BROWSER_NAMESPACE {

namespace {

// Nested backtracking points the interpreter may hold. A path has at most
// 32 767 units and a repeated group can nest once for each of them: twice that
// leaves room. The JIT ignores this limit.
constexpr std::uint32_t depth_limit = 65536;

// First size of the JIT stack, in bytes. It grows up to the limit of the
// pattern. 32 KiB is the default of PCRE2 and enough for most names.
constexpr std::size_t jit_stack_start_bytes = 32 * 1024;

// Size of the buffer that receives a PCRE2 error message, in code units. The
// longest messages are about 100 characters.
constexpr std::size_t error_message_units = 256;

// Most units of a text that a warning quotes. A path can hold 32 767.
constexpr std::size_t quoted_units = 200;

// Frees a code, a match context, match data and a JIT stack of PCRE2.
struct CodeFree {
  void operator()(pcre2_code* code) const noexcept { pcre2_code_free(code); }
};

struct MatchDataFree {
  void operator()(pcre2_match_data* data) const noexcept {
    pcre2_match_data_free(data);
  }
};

struct MatchContextFree {
  void operator()(pcre2_match_context* context) const noexcept {
    pcre2_match_context_free(context);
  }
};

struct JitStackFree {
  void operator()(pcre2_jit_stack* stack) const noexcept {
    pcre2_jit_stack_free(stack);
  }
};

// The text of a PCRE2 error code. The messages are ASCII.
std::string ErrorMessage(int code) {
  std::array<PCRE2_UCHAR, error_message_units> units{};
  const int length = pcre2_get_error_message(code, units.data(), units.size());
  if (length < 0) {
    return "unknown PCRE2 error " + std::to_string(code);
  }
  std::string text;
  for (int i = 0; i < length; ++i) {
    const PCRE2_UCHAR unit = units.at(static_cast<std::size_t>(i));
    text.push_back(unit < 0x80 ? static_cast<char>(unit) : '?');
  }
  return text;
}

// The start of text, as UTF-8 for the log.
std::string Quote(std::u16string_view text) {
  const bool cut = text.size() > quoted_units;
  const std::u16string_view head = text.substr(0, quoted_units);
  return Utf::WideToUtf8(Utf::Utf16ToWide(head)) + (cut ? "..." : "");
}

}  // namespace

struct RegexMatcher::State {
  std::unique_ptr<pcre2_code, CodeFree> code;
  // The stack comes before the context that points at it: the context is freed
  // first.
  std::unique_ptr<pcre2_jit_stack, JitStackFree> jit_stack;
  std::unique_ptr<pcre2_match_context, MatchContextFree> context;
  std::unique_ptr<pcre2_match_data, MatchDataFree> data;
  // The text of the call, as PCRE2 units. Reused from call to call.
  std::vector<PCRE2_UCHAR> subject;
  // Whether the warning of this scan was logged.
  bool warned{false};
};

std::unique_ptr<RegexMatcher> RegexMatcher::Make(const Pattern& pattern,
                                                 const CaseFolder& folder) {
  auto state = std::make_unique<State>();
  // An empty text matches no entry, as with the other kinds. It needs no code.
  if (!pattern.text.empty()) {
    std::vector<PCRE2_UCHAR> text;
    text.reserve(pattern.text.size());
    for (const char16_t unit : pattern.text) {
      text.push_back(static_cast<PCRE2_UCHAR>(unit));
    }
    // Names are UTF-16 and may hold an unpaired surrogate: the invalid units
    // match nothing instead of failing the match. UCP makes \w, \d and \b
    // follow Unicode.
    std::uint32_t flags = PCRE2_UTF | PCRE2_MATCH_INVALID_UTF | PCRE2_UCP;
    if (!pattern.case_sensitive) {
      flags |= PCRE2_CASELESS;
    }
    int error_code = 0;
    PCRE2_SIZE error_offset = 0;
    state->code.reset(pcre2_compile(text.data(), text.size(), flags,
                                    &error_code, &error_offset, nullptr));
    if (!state->code) {
      Log::Error("Invalid regular expression: {} (at unit {})",
                 ErrorMessage(error_code), error_offset);
      return nullptr;
    }

    // A CPU that the JIT does not know fails here: the interpreter then does
    // the work.
    const bool jit =
        pcre2_jit_compile(state->code.get(), PCRE2_JIT_COMPLETE) == 0;
    state->context.reset(pcre2_match_context_create(nullptr));
    state->data.reset(
        pcre2_match_data_create_from_pattern(state->code.get(), nullptr));
    if (!state->context || !state->data) {
      Log::Error("Not enough memory for the regular expression");
      return nullptr;
    }
    const std::uint32_t match_limit = pattern.regex_match_limit != 0
                                          ? pattern.regex_match_limit
                                          : default_regex_match_limit;
    pcre2_set_match_limit(state->context.get(), match_limit);
    pcre2_set_depth_limit(state->context.get(), depth_limit);
    if (jit) {
      const std::size_t stack_max = pattern.regex_jit_stack_bytes != 0
                                        ? pattern.regex_jit_stack_bytes
                                        : default_regex_jit_stack_bytes;
      state->jit_stack.reset(pcre2_jit_stack_create(
          std::min(jit_stack_start_bytes, stack_max), stack_max, nullptr));
      if (state->jit_stack) {
        pcre2_jit_stack_assign(state->context.get(), nullptr,
                               state->jit_stack.get());
      }
    }
    Log::Debug("Regular expression compiled, {}", jit ? "JIT" : "interpreter");
  }
  return std::unique_ptr<RegexMatcher>(new RegexMatcher(
      pattern.target, folder, !pattern.case_sensitive, std::move(state)));
}

RegexMatcher::RegexMatcher(MatchTarget target, const CaseFolder& folder,
                           bool fold, std::unique_ptr<State> state)
    : Matcher(target), folder_(folder), fold_(fold), state_(std::move(state)) {}

RegexMatcher::~RegexMatcher() = default;

bool RegexMatcher::MatchesText(std::u16string_view text) const {
  State& state = *state_;
  if (!state.code) {
    return false;
  }
  state.subject.clear();
  for (const char16_t unit : text) {
    state.subject.push_back(
        static_cast<PCRE2_UCHAR>(fold_ ? folder_.Fold(unit) : unit));
  }
  const int result =
      pcre2_match(state.code.get(), state.subject.data(), state.subject.size(),
                  0, 0, state.data.get(), state.context.get());
  if (result >= 0) {
    return true;
  }
  if (result != PCRE2_ERROR_NOMATCH && !state.warned) {
    state.warned = true;
    Log::Warn(
        "The regular expression gave up on \"{}\": {}. A text like it counts "
        "as no match. This warning comes once for each scan.",
        Quote(text), ErrorMessage(result));
  }
  return false;
}

}  // namespace NTFS_BROWSER_NAMESPACE
