#pragma once

#include "Andersen.h"
#include "ExternalModules.h"
#include <array>
#include <optional>
#include <span>
#include <vector>

namespace wpgkit {

// Borrowed only for one synchronous query. All inputs are numeric; effects and
// unresolved refer to the caller's result. No AST, graph ownership or call IDs.
struct ExternalCall final {
  std::span<const CallArgument> args;
  Andersen::Node result;
  std::optional<Andersen::Node> receiver;
  bool returnsPointer;
  std::uint8_t formats;
  bool &unresolved;
  ExternalSummary &externalEffects;
  FunctionManagerOperation managerOperation = FunctionManagerOperation::Unknown;

  bool pointerArgument(std::size_t index) const {
    return index < args.size() && args[index].kind != ParamKind::Other;
  }
};

// Applies the catalog's runtime contracts using ordinary PTA operations. The
// analysis owns the solver; this component owns only runtime-model state.
// It cannot add graph edges, activate bodies or access the function inventory.
class ExternalEffects final {
public:
  explicit ExternalEffects(Andersen &solver) : pta_(solver) {}

  void apply(ExternalCall call, ExternalModel model);
  void addField(Andersen::Node field);
  void unknownMemory();

private:
  void applyFormatModel(ExternalCall &site, ExternalModel model,
                        ExternalSummary summary);
  void applyHandlerModel(ExternalCall &site, ExternalModel model);
  void applySpecialModel(ExternalCall &site, ExternalModel model);
  void applyOutputModel(ExternalCall &site, ExternalModel model);
  void updateReceiverPointers(ExternalCall &site);
  void applyFunctionObject(ExternalCall &site, ExternalModel model);
  void allocateResult(ExternalCall &site, ExternalModel model);
  void applyMemoryCopyModel(ExternalCall &site, ExternalModel model);
  void applyTokenizeModel(ExternalCall &site);
  void applyStringModel(ExternalCall &site, ExternalModel model);
  void clobberFields();
  void writeMemory(Andersen::Node pointer);
  void writeObject(Andersen::Node pointer);

  Andersen &pta_;
  std::array<std::optional<Andersen::Node>, 2> handlers_; // New and terminate.
  std::optional<Andersen::Node> callbackContexts_;
  std::vector<Andersen::Node> fieldStorage_;
  bool fieldsClobbered_ = false;
  bool unknownMemory_ = false;
};

} // namespace wpgkit
