#include "LinkMapInventory.h"
#include "clang/Tooling/CompilationDatabase.h"
#include "llvm/Support/LineIterator.h"
#include "llvm/Support/MemoryBuffer.h"
#include <filesystem>
#include <vector>

namespace wpgkit {
namespace {

std::string absolute(const std::filesystem::path &base,
                     const std::string &name) {
  const std::filesystem::path path(name);
  return (path.is_absolute() ? path : base / path).lexically_normal().string();
}

std::string outputPath(const clang::tooling::CompileCommand &command) {
  if (!command.Output.empty())
    return command.Output;
  for (std::size_t i = 0; i < command.CommandLine.size(); ++i) {
    const auto &arg = command.CommandLine[i];
    if (arg == "-o" && i + 1 < command.CommandLine.size())
      return command.CommandLine[i + 1];
    if (arg.size() > 2 && arg.starts_with("-o"))
      return arg.substr(2);
  }
  return {};
}

bool isObject(const std::string &path) {
  return path.ends_with(".o") || path.ends_with(".obj");
}

bool isSharedLibrary(const std::string &path) {
  return path.ends_with(".dylib") || path.find(".so") != std::string::npos;
}

void mapObjectsToSources(const std::set<std::string> &selected,
                         const std::map<std::string, std::string> &compiled,
                         LinkMapReport &report) {
  for (const auto &object : selected) {
    const auto open = object.find(".a(");
    if (open == std::string::npos) {
      if (const auto found = compiled.find(object); found != compiled.end()) {
        report.objectSources.emplace(object, found->second);
        report.objectOutputs.emplace(object, found->first);
      } else
        report.unmappedObjects.insert(object);
      continue;
    }
    const auto archive = object.substr(0, open + 2);
    const auto member = object.substr(open + 3, object.size() - open - 4);
    auto target = std::filesystem::path(archive).stem().string();
    if (target.starts_with("lib"))
      target.erase(0, 3);
    const auto targetDirectory = "/CMakeFiles/" + target + ".dir/";
    const auto objectTargetDirectory =
        "/CMakeFiles/" + target + ".objects.dir/";
    const auto llvmObjectDirectory = "/CMakeFiles/obj." + target + ".dir/";
    std::vector<std::pair<std::string, std::string>> candidates;
    for (const auto &[output, source] : compiled)
      if ((output.find(targetDirectory) != std::string::npos ||
           output.find(objectTargetDirectory) != std::string::npos ||
           output.find(llvmObjectDirectory) != std::string::npos) &&
          std::filesystem::path(output).filename() == member)
        candidates.emplace_back(output, source);
    if (candidates.empty())
      for (const auto &[output, source] : compiled)
        if (std::filesystem::path(output).parent_path() ==
                std::filesystem::path(archive).parent_path() &&
            std::filesystem::path(output).filename() == member)
          candidates.emplace_back(output, source);
    if (candidates.size() == 1) {
      report.objectOutputs.emplace(object, candidates.front().first);
      report.objectSources.emplace(object, candidates.front().second);
    } else {
      report.unmappedObjects.insert(object);
      for (const auto &[output, source] : candidates) {
        report.ambiguousOutputs.insert(output);
        report.ambiguousSources.insert(source);
      }
    }
  }
}

} // namespace

llvm::Expected<LinkMapReport>
LinkMapInventory::inspect(const std::string &mapPath,
                          const std::string &databaseDirectory) {
  std::string diagnostic;
  auto database = clang::tooling::CompilationDatabase::loadFromDirectory(
      databaseDirectory, diagnostic);
  if (!database)
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "cannot load compilation database: %s",
                                   diagnostic.c_str());
  auto buffer = llvm::MemoryBuffer::getFile(mapPath);
  if (!buffer)
    return llvm::createStringError(buffer.getError(), "cannot read linker map");

  const auto buildDirectory = std::filesystem::absolute(databaseDirectory);
  std::map<std::string, std::string> compiled;
  for (const auto &command : database->getAllCompileCommands()) {
    const auto output = outputPath(command);
    if (output.empty())
      continue;
    auto directory = std::filesystem::path(command.Directory);
    if (directory.is_relative())
      directory = buildDirectory / directory;
    compiled.emplace(absolute(directory, output),
                     absolute(directory, command.Filename));
  }

  LinkMapReport report;
  std::set<std::string> selected;
  bool sawMap = false;
  for (llvm::line_iterator lines(*buffer.get(), false); !lines.is_at_end();
       ++lines) {
    const llvm::StringRef line = *lines;
    if (line == "Linker script and memory map")
      sawMap = true;
    std::size_t cursor = 0;
    while (true) {
      const auto marker = line.find(".a(", cursor);
      if (marker == std::string::npos)
        break;
      const auto close = line.find(')', marker + 3);
      if (close == std::string::npos)
        break;
      const auto space = line.find_last_of(" \t", marker);
      const auto begin = space == std::string::npos ? 0 : space + 1;
      const auto member = line.substr(marker + 3, close - marker - 3).str();
      if (begin < marker && isObject(member)) {
        const auto archive = absolute(
            buildDirectory, line.substr(begin, marker + 2 - begin).str());
        report.linkedArchives.insert(archive);
        selected.insert(archive + "(" + member + ")");
      }
      cursor = close + 1;
    }
    if (!line.starts_with("LOAD "))
      continue;
    const auto loaded = absolute(buildDirectory, line.substr(5).str());
    if (isObject(loaded))
      selected.insert(loaded);
    if (isSharedLibrary(loaded))
      report.sharedLibraries.insert(loaded);
  }
  if (!sawMap || selected.empty())
    return llvm::createStringError(
        llvm::inconvertibleErrorCode(),
        "linker map has no recognizable object inputs");

  mapObjectsToSources(selected, compiled, report);
  return report;
}

llvm::Expected<LinkMapReport>
LinkMapInventory::inspectMany(const std::vector<std::string> &mapPaths,
                              const std::string &databaseDirectory) {
  if (mapPaths.empty())
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "at least one linker map is required");
  LinkMapReport merged;
  for (const auto &path : mapPaths) {
    auto part = inspect(path, databaseDirectory);
    if (!part)
      return part.takeError();
    for (const auto &[object, source] : part->objectSources) {
      const auto found = merged.objectSources.find(object);
      if (found != merged.objectSources.end() && found->second != source)
        return llvm::createStringError(
            llvm::inconvertibleErrorCode(),
            "conflicting source for linked object: %s", object.c_str());
      merged.objectSources.emplace(object, source);
    }
    for (const auto &[object, output] : part->objectOutputs) {
      const auto found = merged.objectOutputs.find(object);
      if (found != merged.objectOutputs.end() && found->second != output)
        return llvm::createStringError(
            llvm::inconvertibleErrorCode(),
            "conflicting compile output for linked object: %s", object.c_str());
      merged.objectOutputs.emplace(object, output);
    }
    merged.ambiguousOutputs.insert(part->ambiguousOutputs.begin(),
                                   part->ambiguousOutputs.end());
    merged.ambiguousSources.insert(part->ambiguousSources.begin(),
                                   part->ambiguousSources.end());
    merged.unmappedObjects.insert(part->unmappedObjects.begin(),
                                  part->unmappedObjects.end());
    merged.sharedLibraries.insert(part->sharedLibraries.begin(),
                                  part->sharedLibraries.end());
    merged.linkedArchives.insert(part->linkedArchives.begin(),
                                 part->linkedArchives.end());
  }
  return merged;
}

} // namespace wpgkit
