target_sources(
  NtfsBrowserTests
  PRIVATE
    "fuzzer-command-line-tests.cpp"
    "fuzzgen-command-line-tests.cpp"
    "fuzzer-expected-messages.h"
    "fuzzer-regression-tests.cpp"
)
