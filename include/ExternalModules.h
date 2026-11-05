#pragma once

#include "CallEffects.h"

namespace clang {
class ASTContext;
class CallExpr;
class Expr;
class FunctionDecl;
} // namespace clang

namespace wpgkit {

// Numeric, owned contracts. Recognition uses Clang only during extraction;
// querying an effect summary never needs an AST or a spelling lookup.
enum class ExternalModel : std::uint8_t {
  Unknown,
  ForwardReference,
  AddressOf,
  ScalarIntrinsic,
  NoReturn,
  TrivialDestructor,
  Allocate,
  Reallocate,
  Deallocate,
  CopyMemory,
  SetMemory,
  Fprintf,
  PointerIdentity,
  ReadMemory,
  FindMemory,
  ScalarLibrary,
  OverflowArithmetic,
  StackAllocate,
  EmptyConstructor,
  CAllocate,
  CFree,
  Printf,
  Sprintf,
  Snprintf,
  VFormat,
  StreamOutput,
  StreamInput,
  Math,
  MathOutput,
  FloatingIntrinsic,
  NumericConversion,
  DuplicateString,
  OutAllocate,
  ExternalPointer,
  StaticBuffer,
  Buffer0,
  Buffer1,
  Buffer2,
  ReturnBuffer0,
  ReturnBuffer1,
  SystemQuery,
  SystemUpdate,
  IORead,
  IOWrite,
  CopyInterior,
  CallbackRuntime,
  ThreadRuntime,
  ProcessRuntime,
  MappingRuntime,
  CppRuntime,
  Tokenize,
  VarArgs,
  AtomicFlag,
  TerminateRuntime,
  OpenStream,
  CloseStream,
  TrivialScalarConstructor,
  PointerStructure,
  AtomicFence,
  ScalarRehashPolicy,
  AtomicClear,
  ImmediateExit,
  ScalarSystemOutput,
  ConditionInitialize,
  ConditionNotify,
  ConditionDestroy,
  FutexWake,
  Once,
  Sort,
  SortWithContext,
  Search,
  VPrintf,
  VFprintf,
  VSprintf,
  VSnprintf,
  VarArgsStart,
  MapMemory,
  UnmapMemory,
  ProtectMemory,
  SetNewHandler,
  GetNewHandler,
  LockState,
  SignalStack,
  DelegatingDelete,
  StringErase,
  StringReturnSelf,
  DefaultAllocate,
  DelegatingAllocate,
  StringCreate,
  StringMutate,
  SetTerminateHandler,
  GetTerminateHandler,
  ExitRegistration,
  PasswordLookup,
  StreamDelegate,
  StreamPosition,
  StaticCategory,
  LoaderQuery,
  FutexWait,
  RuntimeDestructor,
  StringSwap,
  TrivialDefaultConstructor,
  NullObjectInitialization,
  DirectoryOpen,
  DirectoryRead,
  DirectoryClose,
  ProcessReplace,
  ReceiverPointerUpdate,
  StreamGetline,
  LoaderIterate,
  UnwindTrace,
  FunctionObjectPointer,
  InlineFunctionCopy,
  InlineFunctionManager,
  FunctionObjectQuery,
  FunctionTargetPointer,
  EmptyFunctionObject,
  InlineFunctionPointer
};

enum class FormatEffect : std::uint8_t {
  Dynamic,
  OutputOnly,
  WriteCount,
  Unsupported
};

enum class FunctionManagerOperation : std::uint8_t {
  Unknown,
  TypeInfo,
  Pointer,
  Clone,
  Destroy
};

// Properties that may hold even when other runtime effects remain open.
// A delegated property must be established for the selected owned function ID.
enum class RuntimeProperty : std::uint8_t {
  None,
  FreshResult,
  ReceiverResult,
  BoundedDeletion,
  Argument0Result
};

struct RuntimeContract final {
  RuntimeProperty guaranteed = RuntimeProperty::None;
  RuntimeProperty delegated = RuntimeProperty::None;
};

// Returned by every query, including misses. A recognized rule can still have
// incomplete effects; matched() is not a claim that summary.complete() holds.
// This is an owned query result, not additional per-node/per-edge storage.
struct ExternalModelResult final {
  ExternalModel model = ExternalModel::Unknown;
  ExternalSummary summary = ExternalSummary::unknown();

  bool matched() const {
    return model != ExternalModel::Unknown;
  }
};

class ExternalModules final {
public:
  // A catalog is a per-analysis value. An empty catalog disables every
  // reviewed summary, including intrinsics; queries still return the default
  // conservative summary. WPG owns scheduling and applying global uncertainty.
  ExternalModules() = default;

  static ExternalModules empty() {
    return ExternalModules(false);
  }

  bool isEmpty() const {
    return !enabled_;
  }

  // Clang stores the selected new/delete builtin overload's function type on
  // the callee expression. Resolve it before releasing the native AST.
  static const clang::FunctionDecl *resolveCallee(const clang::CallExpr &call);
  // AST lookup during extraction. Null declarations and catalog misses return
  // the conservative default. Format bits use the captured call-site encoding.
  ExternalModelResult query(const clang::FunctionDecl *declaration,
                            std::uint8_t formats = 0,
                            bool readOnlyCallback = false,
                            FunctionManagerOperation operation =
                                FunctionManagerOperation::Unknown) const;
  // Same query over the saved model ID after AST release. Missing call facts
  // leave format/callback effects uncertain. No argument means the fallback.
  static ExternalModelResult
  query(ExternalModel model = ExternalModel::Unknown, std::uint8_t formats = 0,
        bool readOnlyCallback = false,
        FunctionManagerOperation operation = FunctionManagerOperation::Unknown);
  // Capture a constant manager selector even for an indirect call. A dynamic
  // selector retains the union of effects; no native expression survives.
  static FunctionManagerOperation managerOperation(const clang::Expr &argument,
                                                   clang::ASTContext &context);
  // The reviewed wrapper's selected overload, captured as an owned function
  // ID before AST release. This query neither adds a call nor loads its body.
  static const clang::FunctionDecl *
  delegate(const clang::FunctionDecl &declaration, ExternalModel model);
  static ExternalSummary summary(ExternalModel model);
  static RuntimeContract contract(ExternalModel model);
  static int callbackArgument(ExternalModel model);
  static ExternalSummary callbackSummary(ExternalModel model, bool readOnly);
  // Runtime models wait until imports finish, even when their contract is
  // complete. Intrinsics also include the guarded std::function header
  // contracts; user specializations are excluded from their recognition.
  static bool isIntrinsic(ExternalModel model);
  // Two bits per candidate position; no format text survives AST release.
  static FormatEffect formatEffect(const clang::Expr &format,
                                   clang::ASTContext &context);
  static int formatArgument(ExternalModel model);
  static int variadicListArgument(ExternalModel model);
  static ExternalSummary formattedSummary(ExternalModel model,
                                          FormatEffect format);
  // PTA byte-write masks and return aliases for reviewed runtime contracts.
  // Typed output stores have separate rules; proven scalar-only layouts need
  // no PTA mask. No masks or spellings are stored per graph edge.
  static std::uint32_t writtenArguments(ExternalModel model);
  static int returnedArgument(ExternalModel model);
  // Operand interpreted as a reference even when a builtin rewrites its type.
  static int referenceArgument(ExternalModel model);

private:
  explicit ExternalModules(bool enabled) : enabled_(enabled) {}

  ExternalModel recognize(const clang::FunctionDecl &declaration) const;

  bool enabled_ = true;
};

} // namespace wpgkit
