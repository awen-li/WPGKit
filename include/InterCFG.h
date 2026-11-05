#pragma once

#include "FunctionCFG.h"
#include "GraphIds.h"
#include "CallEffects.h"
#include <memory>
#include <map>
#include <span>

namespace wpgkit {
struct CapturedCFG;

struct CallConnection final {
  CFGPosition source;
  CallSiteId site;
  BodyId callee;
};

struct CallSummary final {
  CFGPosition source;
  CallSiteId site; // Invalid for a CFG call lacking a CG/PTA fact.
  ExternalSummary effects;
};

enum class InterCFGGapKind : std::uint8_t {
  MissingCFG,
  UnmappedCall,
  UnmodeledCall,
  InvalidCallMapping,
  AmbiguousCallOrder
};

struct InterCFGGap final {
  InterCFGGapKind kind;
  FunctionId function;
  BodyId body;
  CallSiteId site;
};

enum class InterCFGEdgeKind : std::uint8_t {
  Step,
  Unevaluated,
  Branch,
  Call,
  ReturnOrUnwind,
  Summary
};

struct InterCFGEdge final {
  CFGPosition target;
  InterCFGEdgeKind kind;
  CallConnectionId connection; // Same ID on a call and its matching return.
  CallSiteId site;
  ExternalSummary effects;
};

// Incremental overlay with append-only connection identities. Queries require
// no ASTs, parsing, or solver work. Only reachable function storage is shared.
//
// Clang's merged exits and existing EH branches are retained as a MAY graph:
// ReturnOrUnwind resumes at the call's continuation, from which native normal
// and EH successors remain possible. It is not a precise exception/unwinding
// graph; implicit cleanup/exception gaps in Clang are not repaired here.
// Consumers must match connection IDs (a stack or context-sensitive summaries)
// to exclude returns to other callers. Ordinary graph reachability is looser.
class InterCFG final {
public:
  // Registers extraction gaps and returns call occurrences to the CG driver.
  // This temporary mapping is not retained in graph snapshots.
  std::vector<std::pair<CallSiteId, CFGPosition>>
  addBody(BodyId id, std::shared_ptr<const CapturedCFG> body);
  // The CG driver installs each discovered pair once; IDs remain stable.
  CallConnectionId addConnection(CFGPosition source, CallSiteId site,
                                 BodyId callee);
  // The driver can inspect pending edits without preparing traversal indexes.
  [[nodiscard]] std::optional<ExternalSummary> summary(CFGPosition source,
                                                       CallSiteId site) const;
  void setSummary(CFGPosition source, CallSiteId site,
                  std::optional<ExternalSummary> effects);

  void setInputStatus(bool closed, bool finalized) {
    inputClosed_ = closed;
    inputFinalized_ = finalized;
  }

  // Batch query-index maintenance, never graph or pointer-state reconstruction.
  void prepare();

  [[nodiscard]] const FunctionCFG *cfg(BodyId body) const noexcept;
  [[nodiscard]] std::optional<CFGPosition> entry(BodyId body) const noexcept;
  [[nodiscard]] std::optional<CFGPosition> exit(BodyId body) const noexcept;
  [[nodiscard]] std::vector<InterCFGEdge> successors(CFGPosition source) const;
  [[nodiscard]] const CallConnection *
  connection(CallConnectionId id) const noexcept;

  [[nodiscard]] std::span<const CallConnection> connections() const {
    return calls_;
  }

  [[nodiscard]] std::span<const CallSummary> summaries() const {
    return summaries_;
  }

  [[nodiscard]] std::span<const InterCFGGap> gaps() const {
    return gaps_;
  }

  [[nodiscard]] std::size_t bodyCount() const {
    return bodyCount_;
  }

  // CG/PTA input status only, not a claim of complete interprocedural flow.
  // Clients must also inspect gaps and the documented exception-flow boundary.
  [[nodiscard]] bool inputClosed() const {
    return inputClosed_;
  }

  [[nodiscard]] bool inputFinalized() const {
    return inputFinalized_;
  }

private:
  std::optional<ExternalSummary> publishedSummary(CFGPosition source,
                                                  CallSiteId site) const;

  std::vector<std::shared_ptr<const CapturedCFG>> bodies_;
  std::vector<CallConnection>
      calls_; // Append order; index + 1 is its stable ID.
  std::vector<CallConnectionId> outgoing_; // Sorted by source.
  std::vector<CallConnectionId> returns_;  // Sorted by callee body.
  std::vector<CallSummary> summaries_;     // Sorted by source.
  // Only changed summaries; null removes an earlier summary.
  std::map<std::pair<CFGPosition, CallSiteId>, std::optional<ExternalSummary>>
      pendingSummaries_;
  std::vector<InterCFGGap> gaps_;
  std::size_t bodyCount_ = 0;
  bool inputClosed_ = false;
  bool inputFinalized_ = false;
};

} // namespace wpgkit
