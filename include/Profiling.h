// Adapted from ConcurDep's profiling.h/profiling.cpp (Apache-2.0), revision
// 32c4ff0bc513d964cf153f6a604403fe79861a74. Adaptations: session ownership,
// scoped measurements and explicit RSS availability.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace wpgkit {

struct ProfilingStatistic {
  std::string component;
  std::uint64_t invocations = 0;
  std::uint64_t elapsedNanoseconds = 0;
  std::uint64_t startRssBytes = 0; // First invocation's start.
  std::uint64_t endRssBytes = 0;   // Last invocation's end.
  std::uint64_t peakRssBytes =
      0;                    // Maximum sampled process RSS across invocations.
  bool rssAvailable = true; // False if any RSS read was unavailable.
};

// Own only when profiling is requested. RSS is process-wide, not memory owned
// by the component; elapsed times include nested scopes. Sampling may miss
// spikes. Callers choose stable component names; one aggregate is retained per
// name.
class Profiler final {
public:
  static constexpr unsigned sampleIntervalMs = 10;
  Profiler();
  ~Profiler();
  Profiler(const Profiler &) = delete;
  Profiler &operator=(const Profiler &) = delete;
  std::vector<ProfilingStatistic> statistics() const; // Completed invocations.

  // The profiler must outlive its scopes. Different components can nest, but a
  // component cannot overlap itself. Names are copied; a null profiler is a
  // no-op.
  class Scope final {
  public:
    Scope(Profiler *profiler, std::string_view component);
    ~Scope();
    Scope(const Scope &) = delete;
    Scope &operator=(const Scope &) = delete;

  private:
    Profiler *profiler_;
    std::string component_;
  };

private:
  class Implementation;
  std::unique_ptr<Implementation> implementation_;
};

} // namespace wpgkit
