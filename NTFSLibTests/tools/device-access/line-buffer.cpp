#include "line-buffer.h"

#include <string_view>

#include "text-convert.h"

namespace NtfsDevice {

void LineBuffer::AppendText(std::wstring_view text) {
#ifdef _WIN32
  // stdout is binary here: a line feed in a name becomes the CR LF the text
  // mode made.
  size_t start = 0;
  for (size_t line_feed = text.find(L'\n');
       line_feed != std::wstring_view::npos;
       line_feed = text.find(L'\n', start)) {
    AppendAsUtf8(buffer_, text.substr(start, line_feed - start));
    buffer_.append(line_end);
    start = line_feed + 1;
  }
  AppendAsUtf8(buffer_, text.substr(start));
#else
  AppendAsUtf8(buffer_, text);
#endif
}

}  // namespace NtfsDevice
