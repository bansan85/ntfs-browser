#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <utility>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/search/types.h>

#include "search/engine.h"

namespace NTFS_BROWSER_NAMESPACE::Test::Search {

// A door that a thread opens and another waits at. The worker of a scan waits
// at it to hold the scan, the test waits at it to know the worker got there.
class Gate final {
 public:
  Gate() = default;
  Gate(const Gate&) = delete;
  Gate& operator=(const Gate&) = delete;
  Gate(Gate&&) = delete;
  Gate& operator=(Gate&&) = delete;
  ~Gate() = default;

  void Open() {
    {
      const std::scoped_lock lock(mutex_);
      open_ = true;
    }
    opened_.notify_all();
  }

  [[nodiscard]] bool IsOpen() {
    const std::scoped_lock lock(mutex_);
    return open_;
  }

  // Waits until the gate opens or stop is requested. For the worker.
  void Wait(const std::stop_token& stop) {
    std::unique_lock lock(mutex_);
    opened_.wait(lock, stop, [this] { return open_; });
  }

  // Waits until the gate opens, at most a long time. False when it never did:
  // a stuck test fails instead of hanging. For the test thread.
  [[nodiscard]] bool WaitOpen() {
    std::unique_lock lock(mutex_);
    return opened_.wait_for(lock, std::chrono::seconds(60),
                            [this] { return open_; });
  }

 private:
  std::mutex mutex_;
  std::condition_variable_any opened_;
  bool open_{false};
};

// What an engine of a test does on the worker thread.
using EngineScript = std::function<bool(
    NTFS_BROWSER_NAMESPACE::Search::EntrySink& sink, std::stop_token stop)>;

// An engine that runs a script.
class ScriptedEngine final : public NTFS_BROWSER_NAMESPACE::Search::Engine {
 public:
  ScriptedEngine(EngineScript script, std::shared_ptr<Gate> destroyed)
      : script_(std::move(script)), destroyed_(std::move(destroyed)) {}

  ScriptedEngine(const ScriptedEngine&) = delete;
  ScriptedEngine& operator=(const ScriptedEngine&) = delete;
  ScriptedEngine(ScriptedEngine&&) = delete;
  ScriptedEngine& operator=(ScriptedEngine&&) = delete;

  // Opens the gate that tells the test the engine is gone.
  ~ScriptedEngine() override {
    if (destroyed_) {
      destroyed_->Open();
    }
  }

  [[nodiscard]] bool Run(NTFS_BROWSER_NAMESPACE::Search::EntrySink& sink,
                         std::stop_token stop) override {
    return script_(sink, std::move(stop));
  }

 private:
  EngineScript script_;
  std::shared_ptr<Gate> destroyed_;
};

// Makes every scan use an engine that runs script, until the object dies. The
// scan still splits its target first: give it the path of an image.
class ScriptedEngineScope final {
 public:
  explicit ScriptedEngineScope(EngineScript script)
      : destroyed_(std::make_shared<Gate>()) {
    NTFS_BROWSER_NAMESPACE::Search::SetEngineFactoryForTests(
        [script = std::move(script), destroyed = destroyed_](
            const NTFS_BROWSER_NAMESPACE::Search::Options&,
            const NTFS_BROWSER_NAMESPACE::Search::TargetParts&) {
          return std::make_unique<ScriptedEngine>(script, destroyed);
        });
  }

  ScriptedEngineScope(const ScriptedEngineScope&) = delete;
  ScriptedEngineScope& operator=(const ScriptedEngineScope&) = delete;
  ScriptedEngineScope(ScriptedEngineScope&&) = delete;
  ScriptedEngineScope& operator=(ScriptedEngineScope&&) = delete;

  ~ScriptedEngineScope() {
    NTFS_BROWSER_NAMESPACE::Search::SetEngineFactoryForTests({});
  }

  // Opens when the worker destroyed the engine, as it leaves its work.
  [[nodiscard]] Gate& Destroyed() const { return *destroyed_; }

 private:
  std::shared_ptr<Gate> destroyed_;
};

// Gives sink one entry named "file<number>", with record set to number.
inline void AddNumbered(NTFS_BROWSER_NAMESPACE::Search::EntrySink& sink,
                        std::uint64_t number) {
  const std::string digits = std::to_string(number);
  std::u16string name = u"file";
  name.append(digits.begin(), digits.end());
  NTFS_BROWSER_NAMESPACE::Search::Entry entry;
  entry.name = name;
  entry.record = number;
  sink.Add(entry);
}

}  // namespace NTFS_BROWSER_NAMESPACE::Test::Search
