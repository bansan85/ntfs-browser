#pragma once

#include <condition_variable>
#include <cstdio>
#include <format>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/tool/device-access/line-buffer.h>

namespace NTFS_BROWSER_NAMESPACE::Tool::DeviceAccess {

// Bytes of a listing gathered before stdout gets them, unless the tuning says
// otherwise (Tuning::OutputTuning). 1 MiB turns a listing of millions of lines
// into a few dozen writes.
inline constexpr size_t output_flush_bytes = size_t{1024} * 1024;

// Gathers a listing, and hands it to stdout in big chunks. A chunk ends between
// two pieces of text, so a console never gets a UTF-8 sequence cut in two. A
// thread of its own writes each chunk while the next one is built: writing a
// big listing costs about as much as building it. The text is built as a
// LineBuffer builds it.
//
// While one lives, stdout is binary on Windows and nothing else MAY write to
// it: the listing goes through this object alone, and ends its own lines.
class Output : public LineBuffer {
 public:
  // flush_bytes is the size of a chunk, see output_flush_bytes. stream is where
  // the listing goes: stdout, or a file a test reads back. A stream other than
  // stdout MUST be binary, as the listing ends its own lines.
  explicit Output(size_t flush_bytes = output_flush_bytes,
                  std::FILE* stream = stdout);
  Output(const Output&) = delete;
  Output(Output&&) = delete;
  Output& operator=(const Output&) = delete;
  Output& operator=(Output&&) = delete;
  // Writes what is left, and gives stdout back its mode.
  ~Output();

  // The pieces that end a line, or may cross the size of a chunk, hand the
  // chunk to the writer when it is full. The others only gather text.
  template <class... Args>
  void Print(std::format_string<Args...> fmt, Args&&... args) {
    LineBuffer::Print(fmt, std::forward<Args>(args)...);
    FlushIfFull();
  }

  template <class... Args>
  void PrintLine(std::format_string<Args...> fmt, Args&&... args) {
    LineBuffer::PrintLine(fmt, std::forward<Args>(args)...);
    FlushIfFull();
  }

  void AppendText(std::wstring_view text) {
    LineBuffer::AppendText(text);
    FlushIfFull();
  }

  void EndLine() {
    LineBuffer::EndLine();
    FlushIfFull();
  }

  // Appends text made apart, a whole number of lines of a listing.
  void AppendBlock(std::string_view text) {
    buffer_.append(text);
    FlushIfFull();
  }

  // Hands what is gathered so far to the writer, once it has written the
  // previous chunk.
  void Flush();

 private:
  // Body of the writer thread: writes each chunk Flush() hands over, until
  // StopWriter() and the last chunk are done.
  void WriteChunks();

  // Lets the writer finish the chunk it has, then ends it.
  void StopWriter();

  void FlushIfFull() {
    if (buffer_.size() >= flush_bytes_) {
      Flush();
    }
  }

  size_t flush_bytes_;
  std::FILE* stream_;
#ifdef _WIN32
  // The mode stdout had before, restored once the listing is written. -1 when
  // it could not be changed.
  int text_mode_{-1};
#endif

  // Guards everything below but the thread itself.
  std::mutex mutex_;
  // Signalled when a chunk waits for the writer, and when it has written one.
  std::condition_variable ready_;
  std::condition_variable written_;
  // The chunk the writer writes, and whether it holds one to write.
  std::string writing_;
  bool chunk_ready_{false};
  bool stopping_{false};
  // Not joinable when no thread could be had. Declared last: it starts once
  // everything above is built.
  std::thread writer_;
};

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::DeviceAccess
