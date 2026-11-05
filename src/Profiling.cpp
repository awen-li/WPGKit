// Adapted from ConcurDep's profiling.h/profiling.cpp (Apache-2.0), revision
// 32c4ff0bc513d964cf153f6a604403fe79861a74. Adaptations: session ownership,
// scoped measurements and explicit RSS availability.
#include "Profiling.h"
#include <algorithm>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#if defined(__linux__)
#include <unistd.h>
#endif

namespace wpgkit {
namespace {
using Clock = std::chrono::steady_clock;
}

class Profiler::Implementation final {
public:
  Implementation() : sampler_([this] { sampleLoop(); }) {}

  ~Implementation() {
    {
      std::lock_guard lock(mutex_);
      stopping_ = true;
    }
    condition_.notify_all();
    sampler_.join();
  }

  void start(std::string_view component) {
    if (component.empty())
      throw std::invalid_argument("profiling component must not be empty");
    std::lock_guard lock(mutex_);
    auto &region = measurements_[std::string(component)].active;
    if (region)
      throw std::logic_error("profiling component is already active");
    const auto rss = sampleRss();
    region = ActiveRegion{Clock::now(), rss.value_or(0), rss.value_or(0),
                          rss.has_value()};
  }

  void end(std::string_view component) noexcept {
    std::lock_guard lock(mutex_);
    const auto found = measurements_.find(component);
    assert(found != measurements_.end());
    auto &region = found->second.active;
    assert(region);
    const auto now = Clock::now();
    const auto rss = sampleRss();
    auto &aggregate = found->second.aggregate;
    if (aggregate.invocations == 0)
      aggregate.startRssBytes = region->startRss;
    ++aggregate.invocations;
    aggregate.elapsedNanoseconds += static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now -
                                                             region->started)
            .count());
    aggregate.endRssBytes = rss.value_or(0);
    aggregate.peakRssBytes = std::max(aggregate.peakRssBytes, region->peakRss);
    aggregate.rssAvailable = aggregate.rssAvailable && region->rssAvailable;
    region.reset();
  }

  std::vector<ProfilingStatistic> statistics() const {
    std::lock_guard lock(mutex_);
    std::vector<ProfilingStatistic> result;
    for (const auto &[name, measurement] : measurements_) {
      if (measurement.aggregate.invocations == 0)
        continue;
      result.push_back(measurement.aggregate);
      result.back().component = name;
    }
    return result;
  }

private:
  struct ActiveRegion {
    Clock::time_point started;
    std::uint64_t startRss;
    std::uint64_t peakRss;
    bool rssAvailable;
  };

  struct Measurement {
    std::optional<ActiveRegion> active;
    ProfilingStatistic aggregate;
  };

  std::optional<std::uint64_t> residentSetBytes() {
    if (!statm_.is_open() || pageSize_ <= 0)
      return std::nullopt;
    statm_.clear();
    statm_.seekg(0);
    std::uint64_t totalPages = 0, residentPages = 0;
    if (!(statm_ >> totalPages >> residentPages))
      return std::nullopt;
    return residentPages * static_cast<std::uint64_t>(pageSize_);
  }

  // Endpoint samples also update enclosing scopes, including short-lived peaks.
  std::optional<std::uint64_t> sampleRss() {
    const auto rss = residentSetBytes();
    for (auto &[name, measurement] : measurements_) {
      auto &region = measurement.active;
      if (!region)
        continue;
      region->rssAvailable = region->rssAvailable && rss.has_value();
      region->peakRss = std::max(region->peakRss, rss.value_or(0));
    }
    return rss;
  }

  void sampleLoop() {
    std::unique_lock lock(mutex_);
    while (!condition_.wait_for(lock,
                                std::chrono::milliseconds(sampleIntervalMs),
                                [this] { return stopping_; })) {
      if (std::any_of(measurements_.begin(), measurements_.end(),
                      [](const auto &entry) {
                        return entry.second.active.has_value();
                      }))
        sampleRss();
    }
  }

  mutable std::mutex mutex_;
  std::condition_variable condition_;
#if defined(__linux__)
  std::ifstream statm_{"/proc/self/statm"};
  long pageSize_ = sysconf(_SC_PAGESIZE);
#else
  std::ifstream statm_;
  long pageSize_ = 0;
#endif
  bool stopping_ = false;
  std::map<std::string, Measurement, std::less<>> measurements_;
  std::thread sampler_; // Construct last; join before destroying sampled state.
};

Profiler::Profiler() : implementation_(std::make_unique<Implementation>()) {}

Profiler::~Profiler() = default;

std::vector<ProfilingStatistic> Profiler::statistics() const {
  return implementation_->statistics();
}

Profiler::Scope::Scope(Profiler *profiler, std::string_view component)
    : profiler_(profiler),
      component_(profiler ? component : std::string_view{}) {
  if (profiler_)
    profiler_->implementation_->start(component_);
}

Profiler::Scope::~Scope() {
  if (profiler_)
    profiler_->implementation_->end(component_);
}

} // namespace wpgkit
