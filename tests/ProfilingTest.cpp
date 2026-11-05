#include "Profiling.h"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <thread>
#if defined(__linux__)
#include <cstring>
#include <sys/mman.h>
#endif

using namespace wpgkit;

static void require(bool condition, const char *message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

int main() {
  Profiler profiler;
  {
    Profiler::Scope total(&profiler, "analysis");
    for (int i = 0; i < 2; ++i) {
      Profiler::Scope phase(&profiler, "model_lookup");
    }
    try {
      Profiler::Scope phase(&profiler, "serialization");
      throw std::runtime_error("extraction error");
    } catch (const std::runtime_error &) {
    }
    { Profiler::Scope phase(&profiler, "serialization"); }
    bool rejected = false;
    try {
      Profiler::Scope duplicate(&profiler, "analysis");
    } catch (const std::logic_error &) {
      rejected = true;
    }
    require(rejected, "overlapping invocations must not corrupt a phase");
    Profiler::Scope disabled(nullptr, "analysis");
  }
  const auto values = profiler.statistics();
  require(values.size() == 3, "nested and repeated phases must aggregate");
  require(values[0].invocations == 1 && values[1].invocations == 2 &&
              values[2].invocations == 2,
          "exception exit must close its scope");
  require(values[0].elapsedNanoseconds >=
              values[1].elapsedNanoseconds + values[2].elapsedNanoseconds,
          "nested durations must be inclusive");
  for (const auto &value : values) {
    require(value.elapsedNanoseconds > 0, "phase must record elapsed time");
    require(value.peakRssBytes >= value.startRssBytes &&
                value.peakRssBytes >= value.endRssBytes,
            "peak must include endpoints");
  }
  bool rejectedEmpty = false;
  try {
    Profiler::Scope invalid(&profiler, "");
  } catch (const std::invalid_argument &) {
    rejectedEmpty = true;
  }
  require(rejectedEmpty, "empty component names must be rejected");
  // A caller can supply its own names without changing the common module.
  std::string label = "another_module.long_lived_measurement";
  const auto expected = label;
  {
    Profiler::Scope named(&profiler, label);
    label.assign("changed");
  }
  const auto namedValues = profiler.statistics();
  require(std::any_of(namedValues.begin(), namedValues.end(),
                      [&](const auto &value) {
                        return value.component == expected &&
                               value.invocations == 1;
                      }),
          "profiler and scope must own the caller's component name");
  Profiler independent;
  require(independent.statistics().empty(),
          "sessions must not share statistics");
#if defined(__linux__)
  // The allocation is gone at the end boundary; only periodic sampling can
  // observe its peak. mmap avoids retaining allocator arenas after the test.
  {
    Profiler::Scope phase(&independent, "model_lookup");
    constexpr std::size_t bytes = 32 * 1024 * 1024;
    void *memory = mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    require(memory != MAP_FAILED, "test allocation failed");
    std::memset(memory, 1, bytes);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    require(munmap(memory, bytes) == 0, "test allocation release failed");
  }
  const auto sampled = independent.statistics().front();
  require(sampled.rssAvailable, "Linux RSS sampling must be available");
  require(sampled.peakRssBytes > sampled.startRssBytes + 16 * 1024 * 1024 &&
              sampled.peakRssBytes > sampled.endRssBytes + 16 * 1024 * 1024,
          "sampler must observe a transient allocation between endpoints");
#endif
}
