#include "CallGraphPTA.h"
#include "LinkMapInventory.h"
#include "Profiling.h"
#include "clang/AST/ASTContext.h"
#include "clang/Frontend/ASTUnit.h"
#include "clang/Tooling/CompilationDatabase.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/Support/raw_ostream.h"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <utility>
#if defined(__GLIBC__)
#include <malloc.h>
#endif
#if defined(__linux__) || defined(__APPLE__)
#include <sys/resource.h>
#endif
#if defined(__linux__)
#include <unistd.h>
#endif

namespace wpgkit {
namespace {

using clang::tooling::CompileCommand;

std::size_t peakRssMiB() {
#if defined(__linux__) || defined(__APPLE__)
  rusage usage{};
  if (getrusage(RUSAGE_SELF, &usage) == 0) {
#if defined(__APPLE__)
    return static_cast<std::size_t>(usage.ru_maxrss) / (1024 * 1024);
#else
    return static_cast<std::size_t>(usage.ru_maxrss) / 1024;
#endif
  }
#endif
  return 0;
}

std::size_t currentRssMiB() {
#if defined(__linux__)
  std::ifstream status("/proc/self/statm");
  std::size_t total = 0, resident = 0;
  if (status >> total >> resident) {
    const auto pageSize = sysconf(_SC_PAGESIZE);
    if (pageSize > 0)
      return resident * static_cast<std::size_t>(pageSize) / (1024 * 1024);
  }
#endif
  return peakRssMiB();
}

void releaseUnusedAllocatorMemory() {
#if defined(__GLIBC__)
  malloc_trim(0);
#endif
}

void releaseAST(std::unique_ptr<clang::ASTUnit> &unit, Profiler *profiler) {
  Profiler::Scope phase(profiler, "ast_release");
  unit.reset();
  releaseUnusedAllocatorMemory();
}

std::string resolved(const std::filesystem::path &base,
                     const std::string &path) {
  const std::filesystem::path input(path);
  return (input.is_absolute() ? input : base / input)
      .lexically_normal()
      .string();
}

std::string outputOf(const CompileCommand &command,
                     const std::filesystem::path &databaseDirectory) {
  std::string output = command.Output;
  if (output.empty()) {
    for (std::size_t i = 0; i < command.CommandLine.size(); ++i) {
      const auto &arg = command.CommandLine[i];
      if (arg == "-o" && i + 1 < command.CommandLine.size()) {
        output = command.CommandLine[i + 1];
        break;
      }
      if (arg.size() > 2 && arg.starts_with("-o")) {
        output = arg.substr(2);
        break;
      }
    }
  }
  if (output.empty())
    return {};
  return resolved(resolved(databaseDirectory, command.Directory), output);
}

// Clang parses one command synchronously; borrow it instead of storing
// duplicate vectors and a one-entry filename map. Clang's query API returns
// owned copies.
class SingleCommandDatabase final : public clang::tooling::CompilationDatabase {
public:
  explicit SingleCommandDatabase(const CompileCommand &command)
      : command_(command) {}

  std::vector<CompileCommand>
  getCompileCommands(llvm::StringRef filePath) const override {
    return filePath == command_.Filename ? std::vector<CompileCommand>{command_}
                                         : std::vector<CompileCommand>{};
  }

  std::vector<std::string> getAllFiles() const override {
    return {command_.Filename};
  }

  std::vector<CompileCommand> getAllCompileCommands() const override {
    return {command_};
  }

private:
  const CompileCommand &command_;
};

llvm::Expected<std::unique_ptr<clang::ASTUnit>>
parseUnit(const CompileCommand &command, Profiler *profiler) {
  Profiler::Scope phase(profiler, "ast_parse");
  SingleCommandDatabase selected(command);
  clang::tooling::ClangTool tool(selected, {command.Filename});
  std::vector<std::unique_ptr<clang::ASTUnit>> built;
  if (tool.buildASTs(built) != 0 || built.size() != 1 || !built.front() ||
      built.front()->getDiagnostics().hasErrorOccurred())
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "translation unit parsing failed: %s",
                                   command.Filename.c_str());
  return std::move(built.front());
}

llvm::Expected<CallGraphResult>
analyze(const std::vector<CompileCommand> &commands,
        const std::vector<std::string> &qualifiedRoots, Profiler *profiler,
        FunctionFactObserver observe, ExternalModules models) {
  if (commands.empty())
    return llvm::createStringError(
        llvm::inconvertibleErrorCode(),
        "at least one compilation command is required");
  if (qualifiedRoots.empty())
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "at least one root is required");
  const bool progress = std::getenv("WPGKIT_WPG_PROGRESS") != nullptr;
  std::set<std::string> filenames;
  // The owned result survives its session. Destroy solver/extraction storage
  // before trimming the allocator and handing the graph to later stages.
  auto result = [&]() -> llvm::Expected<CallGraphResult> {
    CallGraphSession session(profiler, std::move(observe), models);
    for (std::size_t index = 0; index < commands.size(); ++index) {
      auto unit = parseUnit(commands[index], profiler);
      if (!unit)
        return unit.takeError();
      if (progress) {
        const auto &context = (*unit)->getASTContext();
        llvm::errs() << "parsed " << index + 1 << "/" << commands.size()
                     << " translation units; peak RSS " << peakRssMiB()
                     << " MiB; AST arena "
                     << context.getASTAllocatedMemory() / (1024 * 1024)
                     << " MiB; side tables "
                     << context.getSideTableAllocatedMemory() / (1024 * 1024)
                     << " MiB; source " << commands[index].Filename << '\n';
      }
      // Capture CFG topology and CG/PTA facts together. Reachability controls
      // activation in the solver, never whether a source file needs another
      // parse.
      session.addUnit((*unit)->getASTContext());
      releaseAST(*unit, profiler);
      filenames.insert(commands[index].Filename);
      if (progress)
        llvm::errs() << "released AST; current RSS " << currentRssMiB()
                     << " MiB; peak RSS " << peakRssMiB() << " MiB; "
                     << session.cfgCount() << " captured CFGs; "
                     << session.failedCFGCount() << " failed CFGs\n";
    }
    return std::move(session).finalize(qualifiedRoots);
  }();
  if (!result)
    return result.takeError();
  releaseUnusedAllocatorMemory();
  for (const auto &file : filenames)
    result->sourceFiles.push_back(result->metadata.intern(file));
  result->peakRssMiB = peakRssMiB();
  result->currentRssMiB = currentRssMiB();
  if (progress)
    llvm::errs() << "analyzed " << commands.size() << " units; "
                 << result->reachable.size() << " reachable functions; "
                 << result->unresolvedLinkedSymbols.size()
                 << " missing linker symbols; "
                 << result->statistics.activatedBodies
                 << " activated body variants; "
                 << result->statistics.pointerConstraints
                 << " pointer constraints; peak RSS " << peakRssMiB()
                 << " MiB\n";
  return result;
}

llvm::Expected<std::unique_ptr<clang::tooling::CompilationDatabase>>
loadDatabase(const std::string &directory) {
  std::string diagnostic;
  auto database = clang::tooling::CompilationDatabase::loadFromDirectory(
      directory, diagnostic);
  if (!database)
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "cannot load compilation database: %s",
                                   diagnostic.c_str());
  return database;
}

} // namespace

llvm::Expected<CallGraphResult>
CallGraphPTA::runFiles(const std::vector<std::string> &sources,
                       const std::string &databaseDirectory,
                       const std::vector<std::string> &qualifiedRoots,
                       Profiler *profiler, FunctionFactObserver observe,
                       ExternalModules models) {
  Profiler::Scope phase(profiler, "analysis");
  if (sources.empty())
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "at least one source file is required");
  std::vector<CompileCommand> commands;
  {
    Profiler::Scope inputPhase(profiler, "input_discovery");
    auto database = loadDatabase(databaseDirectory);
    if (!database)
      return database.takeError();

    std::set<std::string> seen;
    for (const auto &source : sources) {
      std::error_code error;
      auto path = std::filesystem::absolute(source, error);
      if (error)
        return llvm::createStringError(error, "cannot resolve source file");
      auto filename = path.lexically_normal().string();
      if (!seen.insert(filename).second)
        return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                       "duplicate source file: %s",
                                       filename.c_str());
      auto choices = (*database)->getCompileCommands(filename);
      if (choices.size() != 1)
        return llvm::createStringError(
            llvm::inconvertibleErrorCode(),
            "expected one compilation command for %s, found %zu",
            filename.c_str(), choices.size());
      choices.front().Filename = filename;
      commands.push_back(std::move(choices.front()));
    }
  }
  return analyze(commands, qualifiedRoots, profiler, std::move(observe),
                 models);
}

llvm::Expected<CallGraphResult> CallGraphPTA::runLinkMap(
    const std::string &mapPath, const std::string &databaseDirectory,
    const std::vector<std::string> &qualifiedRoots, Profiler *profiler,
    FunctionFactObserver observe, ExternalModules models) {
  return runLinkMaps({mapPath}, databaseDirectory, qualifiedRoots, profiler,
                     std::move(observe), models);
}

llvm::Expected<CallGraphResult>
CallGraphPTA::runLinkMaps(const std::vector<std::string> &mapPaths,
                          const std::string &databaseDirectory,
                          const std::vector<std::string> &qualifiedRoots,
                          Profiler *profiler, FunctionFactObserver observe,
                          ExternalModules models) {
  Profiler::Scope phase(profiler, "analysis");
  auto inventory = [&] {
    Profiler::Scope inputPhase(profiler, "input_discovery");
    return LinkMapInventory::inspectMany(mapPaths, databaseDirectory);
  }();
  if (!inventory)
    return inventory.takeError();
  if (inventory->objectOutputs.empty())
    return llvm::createStringError(
        llvm::inconvertibleErrorCode(),
        "linker map has no compilation-database objects");
  const auto directory = std::filesystem::absolute(databaseDirectory);
  std::vector<CompileCommand> selected;
  {
    Profiler::Scope inputPhase(profiler, "input_discovery");
    std::set<std::string> outputs = inventory->ambiguousOutputs;
    for (const auto &entry : inventory->objectOutputs)
      outputs.insert(entry.second);
    auto database = loadDatabase(databaseDirectory);
    if (!database)
      return database.takeError();
    std::map<std::string, std::vector<CompileCommand>> byOutput;
    for (auto &command : (*database)->getAllCompileCommands()) {
      const auto output = outputOf(command, directory);
      if (!outputs.contains(output))
        continue;
      command.Filename =
          resolved(resolved(directory, command.Directory), command.Filename);
      byOutput[output].push_back(std::move(command));
    }
    for (const auto &output : outputs) {
      const auto found = byOutput.find(output);
      if (found == byOutput.end() || found->second.size() != 1)
        return llvm::createStringError(
            llvm::inconvertibleErrorCode(),
            "linked output has no unique compile command: %s", output.c_str());
      selected.push_back(std::move(found->second.front()));
    }
  }
  auto result =
      analyze(selected, qualifiedRoots, profiler, std::move(observe), models);
  if (!result)
    return result.takeError();
  for (const auto &path : mapPaths)
    result->linkMaps.push_back(result->metadata.intern(path));
  result->mappedLinkedObjects = inventory->objectSources.size();
  result->analyzedLinkedObjects = selected.size();
  for (const auto &object : inventory->unmappedObjects)
    result->unmappedLinkedObjects.insert(result->metadata.intern(object));
  for (const auto &library : inventory->sharedLibraries)
    result->sharedLibraries.insert(result->metadata.intern(library));
  return result;
}

} // namespace wpgkit
