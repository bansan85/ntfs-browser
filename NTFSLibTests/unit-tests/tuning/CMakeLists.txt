target_sources(
  NtfsBrowserTests
  PRIVATE
    "knob-defaults-tests.cpp"
    "read-pattern-forwarding-tests.cpp"
    "scan-tuning-tests.cpp"
    "mft-knob-effect-tests.cpp"
    "probe-knob-effect-tests.cpp"
    "read-pattern-tests.cpp"
    "volume-knob-effect-tests.cpp"
    "apply-knob-tests.cpp"
)

# Win32DiskReader reads an image file through an overlapped handle: Windows only.
if(WIN32)
  target_sources(NtfsBrowserTests PRIVATE "win32-reader-knob-tests.cpp")
endif()
