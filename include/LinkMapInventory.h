#pragma once

#include "llvm/Support/Error.h"
#include <map>
#include <set>
#include <string>
#include <vector>

namespace wpgkit {

// Evidence from one GNU linker map. This is an inventory, not a proof that
// external libraries or unmodeled effects have been analyzed.
struct LinkMapReport {
  std::map<std::string, std::string>
      objectSources; // linked object -> compile input
  std::map<std::string, std::string>
      objectOutputs; // linked object -> compile output
  std::set<std::string>
      ambiguousOutputs; // all plausible outputs for ambiguous members
  std::set<std::string> ambiguousSources;
  std::set<std::string> unmappedObjects;
  std::set<std::string> sharedLibraries;
  std::set<std::string> linkedArchives;

  bool allObjectsMapped() const {
    return unmappedObjects.empty();
  }
};

class LinkMapInventory final {
public:
  // The linker map and compilation database must come from the same build.
  // Relative map paths are resolved against the build directory.
  static llvm::Expected<LinkMapReport>
  inspect(const std::string &mapPath, const std::string &databaseDirectory);
  static llvm::Expected<LinkMapReport>
  inspectMany(const std::vector<std::string> &mapPaths,
              const std::string &databaseDirectory);
};

} // namespace wpgkit
