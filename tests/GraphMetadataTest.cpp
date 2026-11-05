#include "GraphMetadata.h"
#include <cstdlib>
#include <iostream>
#include <type_traits>

using namespace wpgkit;

static_assert(std::is_trivially_copyable_v<SourceLocation>);
static_assert(std::is_trivially_copyable_v<CallLocation>);

static void check(bool condition, const char *message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
  }
}

int main() {
  GraphMetadata snapshot;
  TextId shortId, longId;
  {
    GraphMetadata original;
    shortId = original.intern("x");
    longId = original.intern("/a/long/source/path/that/must/be/owned.cpp");
    check(original.intern("x") == shortId && original.size() == 2,
          "repeated metadata has one ID and one stored text");
    check(!original.intern("").isValid() && original.text({}).empty() &&
              !original.find("absent").isValid() && original.size() == 2,
          "unavailable metadata and lookups do not allocate IDs");
    snapshot = original;
    check(&snapshot.text(shortId) != &original.text(shortId) &&
              &snapshot.text(longId) != &original.text(longId),
          "a snapshot owns both short and long strings independently");
    for (unsigned i = 0; i != 1000; ++i)
      original.intern(std::to_string(i));
    check(original.text(shortId) == "x" && snapshot.size() == 2,
          "growth preserves IDs and does not change an earlier snapshot");
  }
  GraphMetadata copied(snapshot);
  snapshot = GraphMetadata{};
  GraphMetadata moved(std::move(copied));
  copied = std::move(moved);
  check(copied.text(shortId) == "x" &&
            copied.text(longId) == "/a/long/source/path/that/must/be/owned.cpp",
        "copy and move retain metadata after the original owners die");
  const CallLocation location{{longId, 17}, {shortId, 3}};
  check(copied.format(location) ==
            "/a/long/source/path/that/must/be/owned.cpp:17:x:3",
        "source text is reconstructed from integer file IDs and offsets");
}
