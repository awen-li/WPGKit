#pragma once

#include "AnalysisOptions.h"
#include "CapturedCFG.h"
#include "FunctionCFG.h"
#include "GraphIds.h"
#include "ExternalModules.h"
#include "llvm/Support/Error.h"
#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace clang {
class ASTContext;
class CFG;
class CFGElement;
class FunctionDecl;
} // namespace clang

namespace wpgkit {
class Profiler;
class InterCFG;
class DataFlowGraph;
class ClangDataFlow;
class LocalDataFlow;

// A downstream consumer may capture additional facts during the existing
// extraction pass. The event callback is destroyed before native storage is
// released; consumers must retain only owned records. No semantic model or
// analysis state is part of the graph library.
using CFGFactObserver =
    std::function<void(const clang::CFGElement &, CFGPosition, CallSiteId)>;
using FunctionFactObserver = std::function<CFGFactObserver(
    const clang::FunctionDecl &, const clang::CFG &, BodyId,
    const ClangDataFlow &, const LocalDataFlow &)>;

struct CallSiteResult {
  CallSiteId id;
  FunctionId caller;
  std::set<FunctionId> targets;
  bool unresolved = false; // Some targets/effects remain unknown
  BodyId body;
  ExternalSummary externalEffects; // Union of modeled/unknown external effects.
};

struct CallGraphResult {
  struct Statistics {
    std::size_t activatedBodies = 0;
    std::size_t connectedCalls = 0;
    std::size_t pointerConstraints = 0;
    std::size_t processedPointerConstraints = 0;
    std::size_t updatedFunctions =
        0; // Function worklist visits, not unique functions.
    std::size_t resolvedIndirectCalls =
        0; // Visits to changed/new indirect sites.
    std::size_t resolvedVirtualTargets = 0; // New virtual site/target pairs.
    std::size_t refreshedBodyStates = 0; // Dirty function availability checks.
    std::size_t modeledCalls = 0;        // Matched site/target model pairs.
    std::size_t appliedModelSummaries = 0; // Unique site/model applications.
    std::size_t unknownCalls = 0; // Sites conservatively finalized once.
    bool operator==(const Statistics &) const = default;
  } statistics; // Cumulative session work; unchanged for a no-op analyze().

  struct PointerStorage {
    std::size_t nodes = 0;
    std::size_t objects = 0;
    std::size_t targetRelations = 0;
    std::size_t largestTargetSet = 0;
    std::size_t watchers = 0;
    std::size_t cfgLocalVariables = 0;
    std::size_t cfgLocalFallbacks = 0;
  } pointerStorage; // Includes inactive nodes allocated during extraction.

  // All graph attributes are numeric. This separate table owns display text.
  GraphMetadata metadata;
  std::map<FunctionId, TextId> names;
  std::map<FunctionId, TextId> usrs;
  std::map<FunctionId, ExternalModel> externalModels;
  std::map<CallSiteId, CallLocation> locations;
  std::set<FunctionId> reachable;
  std::vector<CallSiteResult> calls;
  // BodyId is the one-based index. Graphs survive AST and session destruction;
  // copying a report does not duplicate their block/edge storage.
  std::vector<std::shared_ptr<const CapturedCFG>> cfgs;
  // The converged overlay used for incremental call bindings.
  // Snapshots share immutable CFGs and preserve connection IDs across imports.
  std::shared_ptr<const InterCFG> interCFG;
  std::shared_ptr<const DataFlowGraph> dataFlow;
  std::set<TextId> limitations;
  std::set<TextId>
      unresolvedLinkedSymbols; // Symbol text needed to locate missing bodies
  std::set<FunctionId>
      unresolvedBodies; // Targets whose defining units are not known
  std::vector<TextId>
      sourceFiles;              // Explicit compilation-database inputs, if used
  std::vector<TextId> linkMaps; // GNU linker maps, if supplied
  bool finalized =
      false; // Unknown effects applied after known-call refinement.
  bool unknownMemoryEffects = false;
  std::size_t mappedLinkedObjects = 0;
  std::size_t analyzedLinkedObjects = 0;
  std::size_t peakRssMiB = 0;
  std::size_t currentRssMiB = 0;
  std::set<TextId> unmappedLinkedObjects;
  std::set<TextId> sharedLibraries;

  bool hasLimitation(std::string_view message) const {
    return limitations.contains(metadata.find(message));
  }

  const CapturedCFG *cfg(BodyId id) const {
    return id.isValid() && id.value() <= cfgs.size()
               ? cfgs[id.value() - 1].get()
               : nullptr;
  }

  // This only checks the supplied units. It does not prove that they contain
  // every object linked into a program or that external code is modeled.
  bool closedWithinInputs() const;
};

// Extracts owned CFGs and CG/PTA facts from one AST at a time. The caller may
// destroy each AST immediately after addUnit; analyze never borrows Clang
// objects.
class CallGraphSession final {
public:
  // Optional profiler is borrowed and must outlive the session. The catalog
  // is copied and fixed before extraction. Empty means conservative fallback
  // for calls without supplied bodies, not absence of their effects.
  // Analysis options are also copied and fixed for the entire session.
  explicit CallGraphSession(Profiler *profiler = nullptr,
                            FunctionFactObserver observe = {},
                            ExternalModules models = {},
                            AnalysisOptions options = {});
  ~CallGraphSession();
  CallGraphSession(CallGraphSession &&) noexcept;
  CallGraphSession &operator=(CallGraphSession &&) noexcept;
  CallGraphSession(const CallGraphSession &) = delete;
  CallGraphSession &operator=(const CallGraphSession &) = delete;

  // Capture available concrete bodies, including template instantiations.
  // Repeated external definitions reuse one body; file-local entities are
  // scoped to their unit. Conflicting definitions leave coverage open.
  // Their constraints and call sites activate only when they become reachable.
  void addUnit(clang::ASTContext &unit);
  std::size_t cfgCount() const;
  std::size_t failedCFGCount() const;
  // A session grows for one fixed root set. Use another session for other
  // roots.
  llvm::Expected<CallGraphResult>
  analyze(const std::vector<std::string> &qualifiedRoots);
  // Finish imports, refine known calls, then apply unknown-call effects and
  // continue the same fixed point. Repeated calls are idempotent. Further AST
  // imports are rejected; analyze() before this leaves missing bodies
  // provisional.
  llvm::Expected<CallGraphResult>
  finalize(const std::vector<std::string> &qualifiedRoots) &;
  // Consume a one-shot session, transferring owned storage instead of copying
  // a reusable snapshot. After this call (including errors), only destruction
  // or move-assignment is supported on the consumed session.
  llvm::Expected<CallGraphResult>
  finalize(const std::vector<std::string> &qualifiedRoots) &&;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// The ASTContext owners must outlive this call. All supplied units are
// extracted first; only reachable bodies contribute to the fixed point.
class CallGraphPTA final {
public:
  static llvm::Expected<CallGraphResult>
  run(const std::vector<clang::ASTContext *> &units,
      const std::vector<std::string> &qualifiedRoots,
      Profiler *profiler = nullptr, FunctionFactObserver observe = {},
      ExternalModules models = {}, AnalysisOptions options = {});

  // Parse all explicit inputs once with their exact compilation commands,
  // release each AST, then solve over owned facts without reparsing. The
  // caller remains responsible for establishing link closure.
  static llvm::Expected<CallGraphResult>
  runFiles(const std::vector<std::string> &sources,
           const std::string &databaseDirectory,
           const std::vector<std::string> &qualifiedRoots,
           Profiler *profiler = nullptr, FunctionFactObserver observe = {},
           ExternalModules models = {}, AnalysisOptions options = {});

  // Parse every command selected by the actual GNU linker map once before
  // solving. Missing object mappings and shared libraries remain explicit.
  static llvm::Expected<CallGraphResult>
  runLinkMap(const std::string &mapPath, const std::string &databaseDirectory,
             const std::vector<std::string> &qualifiedRoots,
             Profiler *profiler = nullptr, FunctionFactObserver observe = {},
             ExternalModules models = {}, AnalysisOptions options = {});
  static llvm::Expected<CallGraphResult>
  runLinkMaps(const std::vector<std::string> &mapPaths,
              const std::string &databaseDirectory,
              const std::vector<std::string> &qualifiedRoots,
              Profiler *profiler = nullptr, FunctionFactObserver observe = {},
              ExternalModules models = {}, AnalysisOptions options = {});
};

} // namespace wpgkit
