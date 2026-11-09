#include "log/log.h"

#include <memory>
#include <string_view>
#include <utility>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/search/log.h>

#include "log/sink.h"

namespace NTFS_BROWSER_NAMESPACE::Search {

namespace {

// Hands every line the threshold lets through to the function of the caller.
class FunctionSink final : public NTFS_BROWSER_NAMESPACE::Log::ISink {
 public:
  FunctionSink(LogSink sink, LogLevel threshold)
      : sink_(std::move(sink)), threshold_(threshold) {}

  [[nodiscard]] bool
      ShouldLog(NTFS_BROWSER_NAMESPACE::Log::Level level) const override {
    return level != NTFS_BROWSER_NAMESPACE::Log::Level::Off &&
           static_cast<LogLevel>(level) <= threshold_;
  }

  void Write(NTFS_BROWSER_NAMESPACE::Log::Level level,
             std::string_view message) override {
    sink_(static_cast<LogLevel>(level), message);
  }

  void Flush() override {}

 private:
  LogSink sink_;
  LogLevel threshold_;
};

}  // namespace

void SetLogSink(LogSink sink, LogLevel threshold) {
  if (!sink || threshold == LogLevel::Off) {
    NTFS_BROWSER_NAMESPACE::Log::SetSinks({});
    return;
  }
  NTFS_BROWSER_NAMESPACE::Log::SetSinks(
      {std::make_shared<FunctionSink>(std::move(sink), threshold)});
}

}  // namespace NTFS_BROWSER_NAMESPACE::Search
