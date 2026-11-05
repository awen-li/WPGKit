#include "LinkMapInventory.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/raw_ostream.h"

using wpgkit::LinkMapInventory;

int main(int argc, char **argv) {
  if (argc < 3) {
    llvm::errs() << "usage: wpgkit-link-inventory COMPDB_DIR LINK_MAP...\n";
    return 2;
  }
  std::vector<std::string> mapPaths(argv + 2, argv + argc);
  auto report = LinkMapInventory::inspectMany(mapPaths, argv[1]);
  if (!report) {
    llvm::errs() << llvm::toString(report.takeError()) << '\n';
    return 1;
  }
  llvm::json::Array mapped;
  for (const auto &[object, source] : report->objectSources)
    mapped.push_back(
        llvm::json::Object{{"object", object}, {"source", source}});
  llvm::json::Array unmapped;
  for (const auto &object : report->unmappedObjects)
    unmapped.push_back(object);
  llvm::json::Array ambiguousSources;
  for (const auto &source : report->ambiguousSources)
    ambiguousSources.push_back(source);
  llvm::json::Array libraries;
  for (const auto &library : report->sharedLibraries)
    libraries.push_back(library);
  llvm::json::Array archives;
  for (const auto &archive : report->linkedArchives)
    archives.push_back(archive);
  llvm::json::Array maps;
  for (const auto &map : mapPaths)
    maps.push_back(map);
  llvm::json::Object output{
      {"scope", "gnu_linker_maps"},
      {"all_objects_mapped", report->allObjectsMapped()},
      {"linked_program_closed", false},
      {"link_maps", std::move(maps)},
      {"mapped_objects", std::move(mapped)},
      {"unmapped_objects", std::move(unmapped)},
      {"ambiguous_source_candidates", std::move(ambiguousSources)},
      {"shared_libraries", std::move(libraries)},
      {"linked_archives", std::move(archives)}};
  llvm::outs() << llvm::formatv("{0:2}\n",
                                llvm::json::Value(std::move(output)));
}
