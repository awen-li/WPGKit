#include "ExternalModules.h"
#include "clang/AST/Attr.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/AST/Expr.h"
#include "clang/AST/FormatString.h"
#include "clang/Basic/Builtins.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Basic/TargetInfo.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringSwitch.h"
#include <initializer_list>
#include <optional>

namespace wpgkit {

RuntimeContract ExternalModules::contract(ExternalModel model) {
  using P = RuntimeProperty;
  switch (model) {
  case ExternalModel::DefaultAllocate:
    return {P::FreshResult, P::None};
  case ExternalModel::DelegatingAllocate:
  case ExternalModel::StringCreate:
    return {P::None, P::FreshResult};
  case ExternalModel::StringReturnSelf:
    return {P::ReceiverResult, P::None};
  case ExternalModel::StreamGetline:
    return {P::Argument0Result, P::None};
  case ExternalModel::CFree:
    return {P::BoundedDeletion, P::None};
  case ExternalModel::DelegatingDelete:
    return {P::None, P::BoundedDeletion};
  default:
    return {};
  }
}

int ExternalModules::referenceArgument(ExternalModel model) {
  return model == ExternalModel::AddressOf ||
                 model == ExternalModel::ForwardReference
             ? 0
             : -1;
}

namespace {
ExternalSummary flags(std::initializer_list<CallEffect> values) {
  ExternalSummary result;
  for (const auto value : values)
    result.effects |= static_cast<std::uint32_t>(value);
  return result;
}

class FormatEffects final
    : public clang::analyze_format_string::FormatStringHandler {
public:
  explicit FormatEffects(const clang::LangOptions &language)
      : language_(language) {}

  bool
  HandlePrintfSpecifier(const clang::analyze_printf::PrintfSpecifier &specifier,
                        const char *, unsigned,
                        const clang::TargetInfo &) override {
    using Conversion = clang::analyze_format_string::ConversionSpecifier;
    valid &= specifier.hasStandardConversionSpecifier(language_);
    writes |= specifier.getConversionSpecifier().getKind() == Conversion::nArg;
    return valid;
  }

  bool HandleInvalidPrintfConversionSpecifier(
      const clang::analyze_printf::PrintfSpecifier &, const char *,
      unsigned) override {
    valid = false;
    return false;
  }

  bool valid = true;
  bool writes = false;

private:
  const clang::LangOptions &language_;
};

bool systemDeclaration(const clang::FunctionDecl &decl) {
  const auto &sources = decl.getASTContext().getSourceManager();
  for (const auto *redecl : decl.redecls())
    if (sources.isInSystemHeader(redecl->getLocation()))
      return true;
  return false;
}

// Prove that a complete object has no pointer/reference storage before omitting
// PTA byte-write effects. Unknown layouts still need the general memory model.
bool scalarStorage(clang::QualType type, const clang::ASTContext &context) {
  if (type->isArithmeticType() || type->isEnumeralType())
    return true;
  if (const auto *array = context.getAsConstantArrayType(type))
    return scalarStorage(array->getElementType(), context);
  const auto *recordType = type->getAs<clang::RecordType>();
  const auto *record =
      recordType ? recordType->getDecl()->getDefinition() : nullptr;
  if (!record)
    return false;
  if (const auto *cxx = llvm::dyn_cast<clang::CXXRecordDecl>(record)) {
    if (cxx->isPolymorphic() || cxx->getNumVBases() != 0)
      return false;
    for (const auto &base : cxx->bases())
      if (!scalarStorage(base.getType(), context))
        return false;
  }
  for (const auto *field : record->fields())
    if (!scalarStorage(field->getType(), context))
      return false;
  return true;
}

bool namedPointee(clang::QualType type, llvm::StringRef name) {
  if (!type->isPointerType() && !type->isLValueReferenceType())
    return false;
  const auto *record = type->getPointeeType()->getAsCXXRecordDecl();
  return record && record->getQualifiedNameAsString() == name;
}

bool declaredIn(const clang::FunctionDecl &decl, llvm::StringRef header) {
  const auto &sources = decl.getASTContext().getSourceManager();
  for (const auto *redeclaration : decl.redecls()) {
    const auto location = sources.getPresumedLoc(redeclaration->getLocation());
    if (!location.isValid())
      continue;
    const llvm::StringRef file(location.getFilename());
    if (file == header ||
        (file.ends_with(header) && file.size() > header.size() &&
         file[file.size() - header.size() - 1] == '/'))
      return true;
  }
  return false;
}

const clang::ClassTemplateSpecializationDecl *
functionManager(const clang::CXXMethodDecl &method, clang::QualType &functor) {
  const auto *record = llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(
      method.getParent());
  if (!record ||
      record->getSpecializationKind() == clang::TSK_ExplicitSpecialization)
    return nullptr;
  const auto name =
      record->getSpecializedTemplate()->getQualifiedNameAsString();
  const bool handler = name == "std::_Function_handler";
  if (!handler && name != "std::_Function_base::_Base_manager")
    return nullptr;
  const auto &args = record->getTemplateArgs();
  const unsigned index = handler ? 1 : 0;
  if (args.size() != index + 1 ||
      args[index].getKind() != clang::TemplateArgument::Type)
    return nullptr;
  functor = args[index].getAsType();
  if (functor->isDependentType())
    return nullptr;
  if (!handler)
    return record;
  if (record->getNumBases() != 1)
    return nullptr;
  const auto *base =
      llvm::dyn_cast_or_null<clang::ClassTemplateSpecializationDecl>(
          record->bases_begin()->getType()->getAsCXXRecordDecl());
  return base && base->getSpecializedTemplate()->getQualifiedNameAsString() ==
                     "std::_Function_base::_Base_manager"
             ? base
             : nullptr;
}

std::optional<bool>
locallyStoredFunctor(const clang::ClassTemplateSpecializationDecl &base) {
  // Use libstdc++'s instantiated decision, including any user specialization
  // of __is_location_invariant, rather than duplicating its ABI size rules.
  for (const auto *member : base.decls()) {
    const auto *variable = llvm::dyn_cast<clang::VarDecl>(member);
    if (!variable || variable->getName() != "__stored_locally")
      continue;
    const auto *initializer = variable->getAnyInitializer();
    bool local = false;
    if (initializer &&
        initializer->EvaluateAsBooleanCondition(local, base.getASTContext()))
      return local;
    return std::nullopt;
  }
  return std::nullopt;
}

bool inlineTrivialFunctor(const clang::ClassTemplateSpecializationDecl &base,
                          clang::QualType functor) {
  if (functor.isVolatileQualified() ||
      !functor.isTriviallyCopyableType(base.getASTContext()))
    return false;
  if (const auto *record = functor->getAsCXXRecordDecl();
      record &&
      (!record->hasTrivialCopyConstructor() || !record->hasTrivialDestructor()))
    return false;
  return locallyStoredFunctor(base).value_or(false);
}

bool functionHeader(const clang::FunctionDecl &decl) {
  if (decl.getTemplateSpecializationKind() ==
          clang::TSK_ExplicitSpecialization ||
      !systemDeclaration(decl) || !declaredIn(decl, "bits/std_function.h"))
    return false;
  const auto *definition = decl.getDefinition();
  return !definition ||
         decl.getASTContext().getSourceManager().isInSystemHeader(
             definition->getLocation());
}

ExternalModel functionWrapperContract(const clang::FunctionDecl &decl) {
  using M = ExternalModel;
  const auto *method = llvm::dyn_cast<clang::CXXMethodDecl>(&decl);
  if (!method || method->isStatic() || method->isVirtual() ||
      method->isVariadic())
    return M::Unknown;
  const auto *record = method->getParent();
  const auto *identifier = record->getIdentifier();
  if (!identifier ||
      (identifier->getName() != "function" &&
       identifier->getName() != "_Function_base") ||
      !functionHeader(decl))
    return M::Unknown;
  if (identifier->getName() == "_Function_base" &&
      record->getQualifiedNameAsString() == "std::_Function_base" &&
      method->getIdentifier() && method->getName() == "_M_empty" &&
      method->isConst() && method->getNumParams() == 0 &&
      method->getReturnType()->isBooleanType())
    return M::FunctionObjectQuery;
  const auto *specialization =
      llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(record);
  if (!specialization ||
      specialization->getSpecializationKind() ==
          clang::TSK_ExplicitSpecialization ||
      specialization->getSpecializedTemplate()->getQualifiedNameAsString() !=
          "std::function")
    return M::Unknown;
  if (llvm::isa<clang::CXXConstructorDecl>(method)) {
    if (method->getNumParams() == 0 ||
        (method->getNumParams() == 1 &&
         method->getParamDecl(0)->getType()->isNullPtrType()))
      return M::EmptyFunctionObject;
    return M::Unknown;
  }
  if (method->getNumParams() != 0)
    return M::Unknown;
  if (const auto *conversion = llvm::dyn_cast<clang::CXXConversionDecl>(method);
      conversion && conversion->getConversionType()->isBooleanType() &&
      method->isConst())
    return M::FunctionObjectQuery;
  if (method->getIdentifier() && method->getName() == "target" &&
      method->getReturnType()->isPointerType())
    return M::FunctionTargetPointer;
  return M::Unknown;
}

ExternalModel functionObjectContract(const clang::FunctionDecl &decl) {
  using M = ExternalModel;
  const auto *method = llvm::dyn_cast<clang::CXXMethodDecl>(&decl);
  if (!method || !method->isStatic() || !method->getIdentifier() ||
      !llvm::StringSwitch<bool>(method->getName())
           .Cases("_M_manager", "_M_init_functor", "_M_get_pointer", true)
           .Default(false) ||
      method->isVariadic() || !functionHeader(decl))
    return M::Unknown;
  clang::QualType functor;
  const auto *base = functionManager(*method, functor);
  if (!base || functor.isNull())
    return M::Unknown;
  const auto name = method->getNameAsString();
  const auto &context = decl.getASTContext();
  const auto data = [&](unsigned i, bool constant) {
    if (i >= method->getNumParams())
      return false;
    const auto type = method->getParamDecl(i)->getType();
    return type->isLValueReferenceType() &&
           type->getPointeeType().isConstQualified() == constant &&
           !type->getPointeeType().isVolatileQualified() &&
           namedPointee(type, "std::_Any_data");
  };
  if (name == "_M_get_pointer" && method->getNumParams() == 1 &&
      data(0, true) &&
      context.hasSameType(method->getReturnType(),
                          context.getPointerType(functor))) {
    const auto local = locallyStoredFunctor(*base);
    return !local   ? M::Unknown
           : *local ? M::InlineFunctionPointer
                    : M::FunctionObjectPointer;
  }
  if (!inlineTrivialFunctor(*base, functor) || !data(0, false))
    return M::Unknown;
  if (name == "_M_init_functor" && method->getNumParams() == 2 &&
      method->getReturnType()->isVoidType()) {
    // A constructor template can beat a trivial const-reference copy when
    // forwarding a mutable lvalue. Trivially-copyable alone does not prove
    // that this overload performs a trivial construction.
    if (const auto *record = functor->getAsCXXRecordDecl())
      for (const auto *member : record->decls())
        if (const auto *constructor =
                llvm::dyn_cast<clang::FunctionTemplateDecl>(member);
            constructor && llvm::isa<clang::CXXConstructorDecl>(
                               constructor->getTemplatedDecl()))
          return M::Unknown;
    const auto source = method->getParamDecl(1)->getType();
    if (source->isReferenceType() &&
        !source->getPointeeType().isVolatileQualified() &&
        context.hasSameType(source->getPointeeType().getUnqualifiedType(),
                            functor.getUnqualifiedType()))
      return M::InlineFunctionCopy;
  }
  if (name == "_M_manager" && method->getNumParams() == 3 && data(1, true) &&
      method->getReturnType()->isBooleanType())
    if (const auto *op =
            method->getParamDecl(2)->getType()->getAs<clang::EnumType>();
        op &&
        op->getDecl()->getQualifiedNameAsString() == "std::_Manager_operation")
      return M::InlineFunctionManager;
  return M::Unknown;
}

ExternalModel llvmStreamContract(const clang::CXXMethodDecl &method) {
  using M = ExternalModel;
  const auto parent = method.getParent()->getQualifiedNameAsString();
  const bool circular = parent == "llvm::circular_raw_ostream";
  const bool standard = parent == "llvm::raw_os_ostream";
  if (!(circular || standard) || method.isStatic() || method.isVariadic() ||
      !declaredIn(method, circular ? "llvm/Support/circular_raw_ostream.h"
                                   : "llvm/Support/raw_os_ostream.h"))
    return M::Unknown;
  const auto name = method.getNameAsString();
  if (standard && name == "current_pos" && method.isConst() &&
      method.getNumParams() == 0 &&
      method.getReturnType()->isUnsignedIntegerType())
    return M::StreamPosition;
  if (method.isConst() || !method.getReturnType()->isVoidType())
    return M::Unknown;
  if (method.getNumParams() == 0 &&
      ((circular && name == "flushBufferWithBanner") ||
       (standard && llvm::isa<clang::CXXDestructorDecl>(method))))
    return M::StreamDelegate;
  const auto &context = method.getASTContext();
  if (name == "write_impl" && method.getNumParams() == 2 &&
      context.hasSameType(method.getParamDecl(0)->getType(),
                          context.getPointerType(context.CharTy.withConst())) &&
      context.hasSameType(method.getParamDecl(1)->getType(),
                          context.getSizeType()))
    return M::StreamDelegate;
  return M::Unknown;
}

bool binaryErrorType(const clang::CXXRecordDecl *record) {
  const auto *specialization =
      llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(record);
  if (!specialization ||
      specialization->getSpecializedTemplate()->getQualifiedNameAsString() !=
          "llvm::ErrorInfo")
    return false;
  const auto &args = specialization->getTemplateArgs();
  if (args.size() != 2)
    return false;
  for (unsigned i = 0; i < 2; ++i) {
    if (args[i].getKind() != clang::TemplateArgument::Type)
      return false;
    const auto *type = args[i].getAsType()->getAsCXXRecordDecl();
    if (!type || type->getQualifiedNameAsString() !=
                     (i == 0 ? "llvm::object::GenericBinaryError"
                             : "llvm::object::BinaryError"))
      return false;
  }
  return true;
}

ExternalModel llvmRuntime(const clang::FunctionDecl &decl) {
  using M = ExternalModel;
  if (decl.isVariadic())
    return M::Unknown;
  // MemAlloc.cpp delegates to a replaceable operator new selected when LLVM
  // itself is built, independently of the caller's aligned-new flags. Its
  // declared no-alias contract proves a fresh normal result, but does not
  // close replacement, new-handler or unwinding effects.
  const auto &context = decl.getASTContext();
  if (decl.getQualifiedNameAsString() == "llvm::allocate_buffer" &&
      declaredIn(decl, "llvm/Support/MemAlloc.h") &&
      decl.hasAttr<clang::RestrictAttr>() &&
      decl.hasAttr<clang::ReturnsNonNullAttr>() &&
      context.hasSameType(decl.getReturnType(), context.VoidPtrTy) &&
      decl.getNumParams() == 2 &&
      context.hasSameType(decl.getParamDecl(0)->getType(),
                          context.getSizeType()) &&
      context.hasSameType(decl.getParamDecl(1)->getType(),
                          context.getSizeType()))
    return M::DefaultAllocate;
  if (decl.getQualifiedNameAsString() == "llvm::instrprof_category" &&
      declaredIn(decl, "llvm/ProfileData/InstrProf.h") &&
      decl.getNumParams() == 0 &&
      decl.getReturnType()->isLValueReferenceType() &&
      decl.getReturnType()->getPointeeType().isConstQualified() &&
      namedPointee(decl.getReturnType(), "std::error_category"))
    return M::StaticCategory;
  const auto *method = llvm::dyn_cast<clang::CXXMethodDecl>(&decl);
  if (!method || method->isStatic())
    return M::Unknown;
  if (const auto model = llvmStreamContract(*method); model != M::Unknown)
    return model;
  if (!method->isConst() || method->getNumParams() != 1)
    return M::Unknown;
  const auto parent = method->getParent()->getQualifiedNameAsString();
  if (parent == "llvm::object::GenericBinaryError" &&
      method->getName() == "log" && declaredIn(decl, "llvm/Object/Error.h") &&
      method->getReturnType()->isVoidType() &&
      method->getParamDecl(0)->getType()->isLValueReferenceType() &&
      !method->getParamDecl(0)
           ->getType()
           ->getPointeeType()
           .isConstQualified() &&
      namedPointee(method->getParamDecl(0)->getType(), "llvm::raw_ostream"))
    return M::StreamDelegate;
  // Restrict the CRTP chain to the reviewed LLVM parents. A user-supplied
  // ErrorInfo parent may override isA with arbitrary effects.
  if (binaryErrorType(method->getParent()) && method->getName() == "isA" &&
      declaredIn(decl, "llvm/Support/Error.h") &&
      method->getReturnType()->isBooleanType() &&
      method->getASTContext().hasSameType(
          method->getParamDecl(0)->getType().getUnqualifiedType(),
          method->getASTContext().getPointerType(
              method->getASTContext().VoidTy.withConst())))
    return M::ReadMemory;
  return M::Unknown;
}

// Reviewed libstdc++ tree.cc contracts. These helpers manipulate node links,
// never the stored values, comparators, allocators or user callbacks.
ExternalModel treeRuntime(const clang::FunctionDecl &decl) {
  using M = ExternalModel;
  if (decl.isVariadic())
    return M::Unknown;
  const auto name = decl.getQualifiedNameAsString();
  const auto node = [](clang::QualType type) {
    return namedPointee(type, "std::_Rb_tree_node_base");
  };
  if ((name == "std::_Rb_tree_increment" ||
       name == "std::_Rb_tree_decrement") &&
      decl.getNumParams() == 1 && decl.getReturnType()->isPointerType() &&
      node(decl.getReturnType()) &&
      decl.getParamDecl(0)->getType()->isPointerType() &&
      node(decl.getParamDecl(0)->getType()))
    return M::FindMemory;
  const bool insert = name == "std::_Rb_tree_insert_and_rebalance";
  const bool erase = name == "std::_Rb_tree_rebalance_for_erase";
  if (!(insert || erase) || decl.getNumParams() != (insert ? 4u : 2u))
    return M::Unknown;
  if (insert && (!decl.getReturnType()->isVoidType() ||
                 !decl.getParamDecl(0)->getType()->isBooleanType()))
    return M::Unknown;
  if (erase &&
      (!decl.getReturnType()->isPointerType() || !node(decl.getReturnType())))
    return M::Unknown;
  for (unsigned i = insert ? 1 : 0; i < decl.getNumParams(); ++i) {
    const auto type = decl.getParamDecl(i)->getType();
    const bool header = i + 1 == decl.getNumParams();
    if (!node(type) || type->getPointeeType().isConstQualified() ||
        (header ? !type->isLValueReferenceType() : !type->isPointerType()))
      return M::Unknown;
  }
  return M::PointerStructure;
}

ExternalModel listRuntime(const clang::CXXMethodDecl &method) {
  using M = ExternalModel;
  if (method.getParent()->getQualifiedNameAsString() !=
          "std::__detail::_List_node_base" ||
      method.isConst() || method.isVirtual() || method.isVariadic() ||
      !method.getReturnType()->isVoidType())
    return M::Unknown;
  const auto name = method.getNameAsString();
  const auto count = llvm::StringSwitch<int>(name)
                         .Case("_M_hook", 1)
                         .Cases("_M_unhook", "_M_reverse", 0)
                         .Cases("_M_transfer", "swap", 2)
                         .Default(-1);
  if (count < 0 || method.getNumParams() != static_cast<unsigned>(count) ||
      method.isStatic() != (name == "swap"))
    return M::Unknown;
  for (const auto *parameter : method.parameters()) {
    const auto type = parameter->getType().getUnqualifiedType();
    if (!namedPointee(type, "std::__detail::_List_node_base") ||
        type->getPointeeType().isConstQualified() ||
        (name == "swap" ? !type->isLValueReferenceType()
                        : !type->isPointerType()))
      return M::Unknown;
  }
  // libstdc++ list.cc changes links only. It never touches stored values or
  // invokes element destructors, allocators, predicates or comparators.
  return M::PointerStructure;
}

// These two libstdc++ methods compute bucket counts and update a scalar
// threshold. They do not allocate buckets or invoke hashes/comparators.
ExternalModel rehashPolicy(const clang::CXXMethodDecl &method) {
  using M = ExternalModel;
  if (method.getParent()->getQualifiedNameAsString() !=
          "std::__detail::_Prime_rehash_policy" ||
      !method.isConst() || method.isStatic() || method.isVirtual() ||
      method.isVariadic())
    return M::Unknown;
  const auto name = method.getNameAsString();
  if ((name != "_M_next_bkt" || method.getNumParams() != 1) &&
      (name != "_M_need_rehash" || method.getNumParams() != 3))
    return M::Unknown;
  const auto &context = method.getASTContext();
  for (const auto *parameter : method.parameters())
    if (!context.hasSameType(parameter->getType(), context.getSizeType()))
      return M::Unknown;
  if (!scalarStorage(context.getRecordType(method.getParent()), context) ||
      !scalarStorage(method.getReturnType(), context))
    return M::Unknown;
  return M::ScalarRehashPolicy;
}

bool standardCharTemplate(clang::QualType type, llvm::StringRef name) {
  const auto *record =
      llvm::dyn_cast_or_null<clang::ClassTemplateSpecializationDecl>(
          type->getAsCXXRecordDecl());
  if (!record ||
      record->getSpecializedTemplate()->getQualifiedNameAsString() != name)
    return false;
  const auto &args = record->getTemplateArgs();
  return args.size() == 1 &&
         args[0].getKind() == clang::TemplateArgument::Type &&
         record->getASTContext().hasSameType(args[0].getAsType(),
                                             record->getASTContext().CharTy);
}

bool standardCharObject(clang::QualType type, llvm::StringRef name,
                        bool allocator) {
  const auto *record =
      llvm::dyn_cast_or_null<clang::ClassTemplateSpecializationDecl>(
          type->getAsCXXRecordDecl());
  if (!record ||
      record->getSpecializedTemplate()->getQualifiedNameAsString() != name)
    return false;
  const auto &args = record->getTemplateArgs();
  if (args.size() != (allocator ? 3u : 2u))
    return false;
  for (const auto &arg : args.asArray())
    if (arg.getKind() != clang::TemplateArgument::Type)
      return false;
  return record->getASTContext().hasSameType(args[0].getAsType(),
                                             record->getASTContext().CharTy) &&
         standardCharTemplate(args[1].getAsType(), "std::char_traits") &&
         (!allocator ||
          standardCharTemplate(args[2].getAsType(), "std::allocator"));
}

// Only the char/const-char-buffer overloads with standard traits and allocator
// are read-only contracts. String-view conversions and custom traits can call
// user code, even when the enclosing method is const.
bool standardString(const clang::CXXMethodDecl &method) {
  return !method.isStatic() && !method.isVirtual() && !method.isVariadic() &&
         standardCharObject(
             method.getASTContext().getRecordType(method.getParent()),
             "std::basic_string", true);
}

bool cxx11LibraryABI(const clang::CXXRecordDecl &record) {
  const auto *space =
      llvm::dyn_cast<clang::NamespaceDecl>(record.getDeclContext());
  return space && space->isInline() && space->getName() == "__cxx11";
}

// These accessors read standard char-string metadata or return its buffer.
// The buffer may be heap-owned or inline: its address stays unknown, but the
// query itself does not allocate, call user code or mutate pointer storage.
ExternalModel stringAccess(const clang::CXXMethodDecl &method) {
  using M = ExternalModel;
  if (!standardString(method) || method.getNumParams() != 0)
    return M::Unknown;
  const auto &context = method.getASTContext();
  const auto name = method.getNameAsString();
  if (method.isConst() &&
      ((llvm::StringSwitch<bool>(name)
            .Cases("size", "length", "capacity", "max_size", true)
            .Default(false) &&
        context.hasSameType(method.getReturnType(), context.getSizeType())) ||
       (name == "empty" && method.getReturnType()->isBooleanType())))
    return M::ReadMemory;
  if ((name == "data" || name == "c_str") &&
      (method.isConst() || cxx11LibraryABI(*method.getParent())) &&
      context.hasSameType(
          method.getReturnType(),
          context.getPointerType(method.isConst() ? context.CharTy.withConst()
                                                  : context.CharTy)))
    return M::ExternalPointer;
  return M::Unknown;
}

ExternalModel streamBufferPointers(const clang::CXXMethodDecl &method) {
  using M = ExternalModel;
  if (method.isStatic() || method.isVirtual() || method.isConst() ||
      method.isVariadic() || !method.getReturnType()->isVoidType())
    return M::Unknown;
  const auto &context = method.getASTContext();
  const auto type = context.getRecordType(method.getParent());
  const auto name = method.getNameAsString();
  const auto same = [&](unsigned i, clang::QualType expected) {
    return context.hasSameType(method.getParamDecl(i)->getType(), expected);
  };
  const auto chars = context.getPointerType(context.CharTy);
  if (standardCharObject(type, "std::basic_streambuf", false) &&
      ((name == "setg" && method.getNumParams() == 3) ||
       (name == "setp" && method.getNumParams() == 2))) {
    for (unsigned i = 0; i < method.getNumParams(); ++i)
      if (!same(i, chars))
        return M::Unknown;
    return M::ReceiverPointerUpdate;
  }
  if (standardCharObject(type, "std::basic_stringbuf", true) &&
      method.getNumParams() == 3 && same(0, chars) &&
      ((name == "_M_sync" && cxx11LibraryABI(*method.getParent()) &&
        same(1, context.getSizeType()) && same(2, context.getSizeType())) ||
       (name == "_M_pbump" && same(1, chars) &&
        method.getParamDecl(2)->getType()->isSignedIntegerType())))
    return M::ReceiverPointerUpdate;
  return M::Unknown;
}

ExternalModel stringStreamContract(const clang::FunctionDecl &decl) {
  using M = ExternalModel;
  if (decl.isVariadic())
    return M::Unknown;
  if (const auto *method = llvm::dyn_cast<clang::CXXMethodDecl>(&decl))
    return streamBufferPointers(*method);
  const auto &context = decl.getASTContext();
  if (decl.getQualifiedNameAsString() != "std::getline" ||
      (decl.getNumParams() != 2 && decl.getNumParams() != 3))
    return M::Unknown;
  const auto stream = decl.getParamDecl(0)->getType();
  const auto string = decl.getParamDecl(1)->getType();
  if (!stream->isLValueReferenceType() || !string->isLValueReferenceType() ||
      stream->getPointeeType().isConstQualified() ||
      string->getPointeeType().isConstQualified() ||
      !context.hasSameType(decl.getReturnType(), stream) ||
      !standardCharObject(stream->getPointeeType(), "std::basic_istream",
                          false) ||
      !standardCharObject(string->getPointeeType(), "std::basic_string",
                          true) ||
      (decl.getNumParams() == 3 &&
       !context.hasSameType(decl.getParamDecl(2)->getType(), context.CharTy)))
    return M::Unknown;
  return M::StreamGetline;
}

ExternalModel stringSearch(const clang::CXXMethodDecl &method) {
  using M = ExternalModel;
  const auto &context = method.getASTContext();
  if (!standardString(method) || !method.isConst() ||
      !context.hasSameType(method.getReturnType(), context.getSizeType()))
    return M::Unknown;
  if (!llvm::StringSwitch<bool>(method.getNameAsString())
           .Cases("find", "rfind", "find_first_of", "find_last_of",
                  "find_first_not_of", "find_last_not_of", true)
           .Default(false))
    return M::Unknown;
  const auto count = method.getNumParams();
  if (count != 2 && count != 3)
    return M::Unknown;
  const auto first = method.getParamDecl(0)->getType();
  const auto expected =
      count == 2 ? context.CharTy
                 : context.getPointerType(context.CharTy.withConst());
  if (!context.hasSameType(first, expected))
    return M::Unknown;
  for (unsigned i = 1; i < count; ++i)
    if (!context.hasSameType(method.getParamDecl(i)->getType(),
                             context.getSizeType()))
      return M::Unknown;
  return M::ReadMemory;
}

bool stringReturnMutation(const clang::CXXMethodDecl &method) {
  const auto &context = method.getASTContext();
  const auto name = method.getNameAsString();
  const unsigned prefix = name == "insert" ? 1 : name == "replace" ? 2 : 0;
  if (!prefix && name != "append" && name != "assign")
    return false;
  if (!context.hasSameType(method.getReturnType(),
                           context.getLValueReferenceType(
                               context.getRecordType(method.getParent()))))
    return false;
  const auto count = method.getNumParams();
  if (count != prefix + 1 && count != prefix + 2)
    return false;
  const auto same = [&](unsigned i, clang::QualType type) {
    return context.hasSameType(
        method.getParamDecl(i)->getType().getUnqualifiedType(), type);
  };
  for (unsigned i = 0; i < prefix; ++i)
    if (!same(i, context.getSizeType()))
      return false;
  const auto chars = context.getPointerType(context.CharTy.withConst());
  if (same(prefix, chars))
    return count == prefix + 1 || same(prefix + 1, context.getSizeType());
  return count == prefix + 2 && same(prefix, context.getSizeType()) &&
         same(prefix + 1, context.CharTy);
}

ExternalModel stringMutation(const clang::CXXMethodDecl &method) {
  using M = ExternalModel;
  if (!standardString(method) || method.isConst())
    return M::Unknown;
  if (stringReturnMutation(method))
    return M::StringReturnSelf; // Both ABIs return *this; allocation stays
                                // open.
  // The old libstdc++ COW ABI can allocate while detaching for a write.
  if (!cxx11LibraryABI(*method.getParent()))
    return M::Unknown;
  const auto &context = method.getASTContext();
  const auto name = method.getNameAsString();
  const auto same = [&](unsigned i, clang::QualType type) {
    return i < method.getNumParams() &&
           context.hasSameType(
               method.getParamDecl(i)->getType().getUnqualifiedType(), type);
  };
  const auto reference =
      context.getLValueReferenceType(context.getRecordType(method.getParent()));
  if (name == "swap" && method.getNumParams() == 1 && same(0, reference) &&
      method.getReturnType()->isVoidType())
    return M::StringSwap;
  if (name == "_M_replace_aux" && method.getNumParams() == 4 &&
      context.hasSameType(method.getReturnType(), reference) &&
      same(0, context.getSizeType()) && same(1, context.getSizeType()) &&
      same(2, context.getSizeType()) && same(3, context.CharTy))
    return M::StringReturnSelf;
  if (name == "_M_erase" && method.getNumParams() == 2 &&
      method.getReturnType()->isVoidType() && same(0, context.getSizeType()) &&
      same(1, context.getSizeType()))
    return M::StringErase;
  if (name == "_M_create" && method.getNumParams() == 2 &&
      context.hasSameType(method.getReturnType(),
                          context.getPointerType(context.CharTy)) &&
      same(0, context.getLValueReferenceType(context.getSizeType())) &&
      same(1, context.getSizeType()))
    return M::StringCreate;
  const bool append = name == "_M_append";
  const auto chars = context.getPointerType(context.CharTy.withConst());
  if ((append || name == "_M_replace") &&
      method.getNumParams() == (append ? 2u : 4u) &&
      context.hasSameType(method.getReturnType(),
                          context.getLValueReferenceType(
                              context.getRecordType(method.getParent()))) &&
      same(append ? 0 : 2, chars) &&
      same(append ? 1 : 3, context.getSizeType()) &&
      (append ||
       (same(0, context.getSizeType()) && same(1, context.getSizeType()))))
    return M::StringReturnSelf;
  if (!method.getReturnType()->isVoidType())
    return M::Unknown;
  if ((name == "reserve" &&
       (method.getNumParams() == 0 ||
        (method.getNumParams() == 1 && same(0, context.getSizeType())))) ||
      ((name == "resize" || name == "_M_construct") &&
       method.getNumParams() == 2 && same(0, context.getSizeType()) &&
       same(1, context.CharTy)) ||
      (name == "_M_assign" && method.getNumParams() == 1 &&
       same(0, context.getLValueReferenceType(
                   context.getRecordType(method.getParent()).withConst()))) ||
      (name == "_M_mutate" && method.getNumParams() == 4 &&
       same(0, context.getSizeType()) && same(1, context.getSizeType()) &&
       same(2, chars) && same(3, context.getSizeType())))
    return M::StringMutate;
  return M::Unknown;
}

ExternalModel objectLifetime(const clang::CXXMethodDecl &method) {
  using M = ExternalModel;
  const auto *record = method.getParent();
  const auto parent = record->getQualifiedNameAsString();
  if (record->getNumBases() != 0)
    return M::Unknown;
  if (parent == "std::thread::_State" &&
      llvm::isa<clang::CXXDestructorDecl>(method) && record->field_empty())
    return M::RuntimeDestructor;
  const auto *constructor = llvm::dyn_cast<clang::CXXConstructorDecl>(&method);
  if (parent != "std::__future_base::_Result_base" || !constructor ||
      !constructor->isDefaultConstructor() || method.getNumParams() != 0 ||
      record->field_empty())
    return M::Unknown;
  auto field = record->field_begin();
  const auto *error = *field++;
  if (field != record->field_end() || error->getName() != "_M_error")
    return M::Unknown;
  const auto *exception = error->getType()->getAsCXXRecordDecl();
  if (!exception ||
      exception->getQualifiedNameAsString() !=
          "std::__exception_ptr::exception_ptr" ||
      exception->getNumBases() != 0 || exception->isPolymorphic() ||
      exception->field_empty())
    return M::Unknown;
  auto member = exception->field_begin();
  const auto type = (*member++)->getType();
  if (member != exception->field_end() || !type->isVoidPointerType())
    return M::Unknown;
  // future.cc defaults this constructor; exception_ptr initializes its sole
  // field to null. The vptr is handled by dispatch, not as a program pointer.
  return M::NullObjectInitialization;
}

bool integralDuration(clang::QualType type, std::uint64_t denominator) {
  const auto *record =
      llvm::dyn_cast_or_null<clang::ClassTemplateSpecializationDecl>(
          type->getAsCXXRecordDecl());
  if (!record || record->getSpecializedTemplate()->getQualifiedNameAsString() !=
                     "std::chrono::duration")
    return false;
  const auto &args = record->getTemplateArgs();
  if (args.size() != 2 || args[0].getKind() != clang::TemplateArgument::Type ||
      args[1].getKind() != clang::TemplateArgument::Type ||
      !args[0].getAsType()->isSignedIntegerType() ||
      record->getASTContext().getTypeSize(args[0].getAsType()) != 64)
    return false;
  const auto *ratio =
      llvm::dyn_cast_or_null<clang::ClassTemplateSpecializationDecl>(
          args[1].getAsType()->getAsCXXRecordDecl());
  if (!ratio || ratio->getSpecializedTemplate()->getQualifiedNameAsString() !=
                    "std::ratio")
    return false;
  const auto &period = ratio->getTemplateArgs();
  return period.size() == 2 &&
         period[0].getKind() == clang::TemplateArgument::Integral &&
         period[1].getKind() == clang::TemplateArgument::Integral &&
         period[0].getAsIntegral() == 1 &&
         period[1].getAsIntegral() == denominator;
}

ExternalModel synchronizationRuntime(const clang::CXXMethodDecl &method) {
  using M = ExternalModel;
  const auto &context = method.getASTContext();
  if (!context.getTargetInfo().getTriple().isOSLinux() || method.isVariadic() ||
      method.isVirtual())
    return M::Unknown;
  const auto parent = method.getParent()->getQualifiedNameAsString();
  const auto name = method.getNameAsString();
  if (parent == "std::__atomic_futex_unsigned_base" &&
      (name == "_M_futex_wait_until" || name == "_M_futex_wait_until_steady") &&
      !method.isStatic() && !method.isConst() && method.getNumParams() == 5 &&
      method.getReturnType()->isBooleanType() &&
      context.hasSameType(method.getParamDecl(0)->getType(),
                          context.getPointerType(context.UnsignedIntTy)) &&
      context.hasSameType(method.getParamDecl(1)->getType(),
                          context.UnsignedIntTy) &&
      method.getParamDecl(2)->getType()->isBooleanType() &&
      integralDuration(method.getParamDecl(3)->getType(), 1) &&
      integralDuration(method.getParamDecl(4)->getType(), 1000000000))
    return M::FutexWait;
  if (parent == "std::__atomic_futex_unsigned_base" &&
      name == "_M_futex_notify_all" && method.isStatic() &&
      method.getNumParams() == 1 && method.getReturnType()->isVoidType() &&
      context.hasSameType(method.getParamDecl(0)->getType(),
                          context.getPointerType(context.UnsignedIntTy)))
    return M::FutexWake;
  if (parent != "std::condition_variable" || method.isStatic() ||
      method.getNumParams() != 0 || !method.getReturnType()->isVoidType() ||
      !scalarStorage(context.getRecordType(method.getParent()), context))
    return M::Unknown;
  if (const auto *ctor = llvm::dyn_cast<clang::CXXConstructorDecl>(&method);
      ctor && ctor->isDefaultConstructor())
    return M::ConditionInitialize;
  if (llvm::isa<clang::CXXDestructorDecl>(method))
    return M::ConditionDestroy;
  return name == "notify_one" || name == "notify_all" ? M::ConditionNotify
                                                      : M::Unknown;
}

ExternalModel mathModel(llvm::StringRef name) {
  using M = ExternalModel;
  name.consume_front("__builtin_");
  if (name == "modf" || name == "modff" || name == "modfl" || name == "frexp" ||
      name == "frexpf" || name == "frexpl")
    return M::MathOutput;
  const auto scalar = [](llvm::StringRef base) {
    return llvm::StringSwitch<bool>(base)
        .Cases("acos", "asin", "atan", "atan2", "cos", "sin", "tan", true)
        .Cases("acosh", "asinh", "atanh", "cosh", "sinh", "tanh", true)
        .Cases("exp", "exp2", "expm1", "log", "log2", "log10", "log1p", true)
        .Cases("pow", "sqrt", "cbrt", "hypot", "fabs", "copysign", true)
        .Cases("ceil", "floor", "trunc", "round", "lround", "llround", true)
        .Cases("rint", "nearbyint", "lrint", "llrint", "fmod", "remainder",
               true)
        .Cases("erf", "erfc", "tgamma", "lgamma", "fma", "fmin", "fmax", "fdim",
               true)
        .Cases("ldexp", "scalbn", "scalbln", "nextafter", "nexttoward", true)
        .Default(false);
  };
  if (scalar(name) || ((name.ends_with("f") || name.ends_with("l")) &&
                       scalar(name.drop_back())))
    return M::Math;
  return M::Unknown;
}

using M = ExternalModel;

struct RuntimeEntry {
  const char *name;
  M model;
  unsigned arity;
  bool variadic = false;
};

static constexpr RuntimeEntry entries[] = {
    {"malloc", M::CAllocate, 1},
    {"calloc", M::CAllocate, 2},
    {"aligned_alloc", M::CAllocate, 2},
    {"free", M::CFree, 1},
    {"realloc", M::Reallocate, 2},
    {"memcpy", M::CopyMemory, 3},
    {"memmove", M::CopyMemory, 3},
    {"memset", M::SetMemory, 3},
    {"strcpy", M::CopyMemory, 2},
    {"strncpy", M::CopyMemory, 3},
    {"strcat", M::CopyMemory, 2},
    {"strncat", M::CopyMemory, 3},
    {"mempcpy", M::CopyInterior, 3},
    {"stpcpy", M::CopyInterior, 2},
    {"stpncpy", M::CopyInterior, 3},
    {"strdup", M::DuplicateString, 1},
    {"strndup", M::DuplicateString, 2},
    {"strlen", M::ReadMemory, 1},
    {"strcmp", M::ReadMemory, 2},
    {"strncmp", M::ReadMemory, 3},
    {"memcmp", M::ReadMemory, 3},
    {"strspn", M::ReadMemory, 2},
    {"strcspn", M::ReadMemory, 2},
    {"memchr", M::FindMemory, 3},
    {"strchr", M::FindMemory, 2},
    {"strrchr", M::FindMemory, 2},
    {"strstr", M::FindMemory, 2},
    {"strpbrk", M::FindMemory, 2},
    {"printf", M::Printf, 1, true},
    {"fprintf", M::Fprintf, 2, true},
    {"sprintf", M::Sprintf, 2, true},
    {"snprintf", M::Snprintf, 3, true},
    {"vprintf", M::VPrintf, 2},
    {"vfprintf", M::VFprintf, 3},
    {"vsprintf", M::VSprintf, 3},
    {"vsnprintf", M::VSnprintf, 4},
    {"puts", M::StreamOutput, 1},
    {"fputs", M::StreamOutput, 2},
    {"putchar", M::StreamOutput, 1},
    {"fputc", M::StreamOutput, 2},
    {"fwrite", M::StreamOutput, 4},
    {"fflush", M::StreamOutput, 1},
    {"fseek", M::StreamOutput, 3},
    {"ftell", M::StreamOutput, 1},
    {"rewind", M::StreamOutput, 1},
    {"fopen", M::OpenStream, 2},
    {"fclose", M::CloseStream, 1},
    {"fread", M::StreamInput, 4},
    {"fgets", M::StreamInput, 3},
    {"strtod", M::NumericConversion, 2},
    {"strtof", M::NumericConversion, 2},
    {"strtold", M::NumericConversion, 2},
    {"strtol", M::NumericConversion, 3},
    {"strtoll", M::NumericConversion, 3},
    {"strtoul", M::NumericConversion, 3},
    {"strtoull", M::NumericConversion, 3},
    {"strftime", M::Buffer0, 4},
    {"abs", M::ScalarLibrary, 1},
    {"labs", M::ScalarLibrary, 1},
    {"llabs", M::ScalarLibrary, 1},
    {"ldiv", M::ScalarLibrary, 2},
    {"abort", M::TerminateRuntime, 0},
    {"exit", M::TerminateRuntime, 1},
    {"quick_exit", M::TerminateRuntime, 1},
    {"atexit", M::ExitRegistration, 1},
    {"at_quick_exit", M::ExitRegistration, 1},
    {"on_exit", M::ExitRegistration, 2},
    {"__cxa_atexit", M::ExitRegistration, 3},
    {"_Exit", M::ImmediateExit, 1},
    {"__errno_location", M::ExternalPointer, 0},
    {"__ctype_b_loc", M::ExternalPointer, 0},
    {"__ctype_tolower_loc", M::ExternalPointer, 0},
    {"__ctype_toupper_loc", M::ExternalPointer, 0},
    {"getenv", M::ExternalPointer, 1},
    {"strsignal", M::StaticBuffer, 1},
    {"strerror", M::StaticBuffer, 1},
    {"strerror_r", M::ReturnBuffer1, 3},
    {"posix_memalign", M::OutAllocate, 3},
    {"strnlen", M::ReadMemory, 2},
    {"__sched_cpucount", M::ReadMemory, 2},
    {"stat", M::Buffer1, 2},
    {"fstat", M::Buffer1, 2},
    {"lstat", M::Buffer1, 2},
    {"getrlimit", M::Buffer1, 2},
    {"getrusage", M::Buffer1, 2},
    {"clock_gettime", M::Buffer1, 2},
    {"localtime_r", M::ReturnBuffer1, 2},
    {"getcwd", M::ReturnBuffer0, 2},
    {"realpath", M::ReturnBuffer1, 2},
    {"readlink", M::Buffer1, 3},
    {"sched_getaffinity", M::Buffer2, 3},
    {"sigemptyset", M::Buffer0, 1},
    {"sigfillset", M::Buffer0, 1},
    {"sigaltstack", M::SignalStack, 2},
    {"getpwuid_r", M::PasswordLookup, 5},
    {"getpwnam_r", M::PasswordLookup, 5},
    {"getuid", M::SystemQuery, 0},
    {"getpagesize", M::SystemQuery, 0},
    {"sysconf", M::SystemQuery, 1},
    {"isatty", M::SystemQuery, 1},
    {"access", M::SystemQuery, 2},
    {"mallinfo2", M::SystemQuery, 0},
    {"arc4random", M::SystemUpdate, 0},
    {"setsid", M::SystemUpdate, 0},
    {"unlink", M::SystemUpdate, 1},
    {"mkdir", M::SystemUpdate, 2},
    {"remove", M::SystemUpdate, 1},
    {"rename", M::SystemUpdate, 2},
    {"lseek", M::SystemUpdate, 3},
    {"dup2", M::SystemUpdate, 2},
    {"ftruncate", M::SystemUpdate, 2},
    {"setrlimit", M::SystemUpdate, 2},
    // Cancellation, signal callbacks and hidden library state remain open.
    {"read", M::IORead, 3},
    {"pread", M::IORead, 4},
    {"write", M::IOWrite, 3},
    {"pwrite", M::IOWrite, 4},
    {"close", M::IOWrite, 1},
    {"open", M::IOWrite, 2, true},
    {"opendir", M::DirectoryOpen, 1},
    {"readdir", M::DirectoryRead, 1},
    {"closedir", M::DirectoryClose, 1},
    {"qsort", M::Sort, 4},
    {"qsort_r", M::SortWithContext, 5},
    {"bsearch", M::Search, 5},
    {"sigaction", M::CallbackRuntime, 3},
    {"alarm", M::CallbackRuntime, 1},
    {"kill", M::CallbackRuntime, 2},
    {"backtrace", M::CallbackRuntime, 2},
    {"_Unwind_Backtrace", M::UnwindTrace, 2},
    {"dl_iterate_phdr", M::LoaderIterate, 2},
    {"dladdr", M::LoaderQuery, 2},
    {"pthread_once", M::Once, 2},
    {"pthread_create", M::ThreadRuntime, 4},
    {"pthread_join", M::ThreadRuntime, 2},
    {"pthread_self", M::SystemQuery, 0},
    {"pthread_attr_init", M::Buffer0, 1},
    {"pthread_attr_destroy", M::Buffer0, 1},
    {"pthread_attr_setstacksize", M::Buffer0, 2},
    {"pthread_cond_init", M::ConditionInitialize, 2},
    {"pthread_cond_destroy", M::ConditionDestroy, 1},
    {"pthread_cond_signal", M::ConditionNotify, 1},
    {"pthread_cond_broadcast", M::ConditionNotify, 1},
    {"pthread_mutex_lock", M::LockState, 1},
    {"pthread_mutex_unlock", M::LockState, 1},
    {"pthread_rwlock_rdlock", M::LockState, 1},
    {"pthread_rwlock_wrlock", M::LockState, 1},
    {"pthread_rwlock_unlock", M::LockState, 1},
    {"pthread_sigmask", M::ThreadRuntime, 3},
    {"clock_nanosleep", M::ThreadRuntime, 4},
    {"fork", M::ProcessRuntime, 0},
    {"execv", M::ProcessReplace, 2},
    {"execve", M::ProcessReplace, 3},
    {"syscall", M::ProcessRuntime, 1, true},
    {"wait", M::ProcessRuntime, 1},
    {"wait4", M::ProcessRuntime, 4},
    {"posix_spawn", M::ProcessRuntime, 6},
    {"posix_spawn_file_actions_init", M::Buffer0, 1},
    {"posix_spawn_file_actions_destroy", M::Buffer0, 1},
    {"posix_spawn_file_actions_addopen", M::Buffer0, 5},
    {"posix_spawn_file_actions_adddup2", M::Buffer0, 3},
    {"mmap", M::MapMemory, 6},
    {"munmap", M::UnmapMemory, 2},
    {"mprotect", M::ProtectMemory, 3},
    {"_exit", M::ImmediateExit, 1},
    {"strtok_r", M::Tokenize, 3},
    {"__strtok_r", M::Tokenize, 3}};

const RuntimeEntry *runtimeEntry(llvm::StringRef name, unsigned arity,
                                 bool variadic) {
  // Build once, including all signature variants of a name. A catalog miss
  // does not scan unrelated APIs; declaration/type checks still run below.
  static const auto index = [] {
    llvm::StringMap<llvm::SmallVector<const RuntimeEntry *, 1>> result;
    for (const auto &entry : entries)
      result[entry.name].push_back(&entry);
    return result;
  }();
  const auto found = index.find(name);
  if (found == index.end())
    return nullptr;
  for (const auto *entry : found->second)
    if (entry->arity == arity && entry->variadic == variadic)
      return entry;
  return nullptr;
}

ExternalModel comparatorContract(const clang::FunctionDecl &decl, M model) {
  const auto &context = decl.getASTContext();
  const bool search = model == M::Search;
  const bool withContext = model == M::SortWithContext;
  const auto pointer = context.VoidPtrTy;
  const auto constant = context.getPointerType(context.VoidTy.withConst());
  const auto callback = decl.getParamDecl(search ? 4 : 3)->getType();
  const auto *signature =
      callback->isPointerType()
          ? callback->getPointeeType()->getAs<clang::FunctionProtoType>()
          : nullptr;
  const auto same = [&](unsigned index, clang::QualType type) {
    return context.hasSameType(
        decl.getParamDecl(index)->getType().getUnqualifiedType(), type);
  };
  // qsort_r has incompatible platform ABIs; accept the reviewed GNU order.
  if ((withContext && !context.getTargetInfo().getTriple().isOSLinux()) ||
      !context.hasSameType(decl.getReturnType(),
                           search ? pointer : context.VoidTy) ||
      !same(0, search ? constant : pointer) || (search && !same(1, constant)) ||
      !same(search ? 2 : 1, context.getSizeType()) ||
      !same(search ? 3 : 2, context.getSizeType()) ||
      (withContext && !same(4, pointer)) || !signature ||
      signature->isVariadic() ||
      signature->getNumParams() != (withContext ? 3u : 2u) ||
      !context.hasSameType(signature->getReturnType(), context.IntTy))
    return M::CallbackRuntime;
  for (unsigned i = 0; i < signature->getNumParams(); ++i)
    if (!context.hasSameType(signature->getParamType(i).getUnqualifiedType(),
                             i < 2 ? constant : pointer))
      return M::CallbackRuntime;
  return model;
}

ExternalModel traversalContract(const clang::FunctionDecl &decl, M model) {
  const auto &context = decl.getASTContext();
  const bool loader = model == M::LoaderIterate;
  const auto callback = decl.getParamDecl(0)->getType();
  const auto *signature =
      callback->isPointerType()
          ? callback->getPointeeType()->getAs<clang::FunctionProtoType>()
          : nullptr;
  if (!context.getTargetInfo().getTriple().isOSLinux() ||
      (!loader &&
       context.getTargetInfo().getTriple().getArch() != llvm::Triple::x86_64) ||
      !context.hasSameType(decl.getParamDecl(1)->getType(),
                           context.VoidPtrTy) ||
      !signature || signature->isVariadic() ||
      signature->getNumParams() != (loader ? 3u : 2u) ||
      !context.hasSameType(signature->getReturnType(), decl.getReturnType()) ||
      !context.hasSameType(signature->getParamType(loader ? 2 : 1),
                           context.VoidPtrTy))
    return M::CallbackRuntime;
  const auto record = signature->getParamType(0);
  if (!record->isPointerType() || record->getPointeeType().isConstQualified() ||
      !namedPointee(record, loader ? "dl_phdr_info" : "_Unwind_Context"))
    return M::CallbackRuntime;
  if (loader)
    return context.hasSameType(decl.getReturnType(), context.IntTy) &&
                   context.hasSameType(signature->getParamType(1),
                                       context.getSizeType())
               ? model
               : M::CallbackRuntime;
  const auto *result = decl.getReturnType()->getAs<clang::EnumType>();
  if (!result)
    return M::CallbackRuntime;
  const auto *enumeration = result->getDecl();
  const auto *alias = enumeration->getTypedefNameForAnonDecl();
  return (enumeration->getName() == "_Unwind_Reason_Code" ||
          (alias && alias->getName() == "_Unwind_Reason_Code"))
             ? model
             : M::CallbackRuntime;
}

ExternalModel mappingContract(const clang::FunctionDecl &decl, M model) {
  const auto &context = decl.getASTContext();
  if (!context.getTargetInfo().getTriple().isOSLinux() ||
      !context.hasSameType(decl.getParamDecl(0)->getType(),
                           context.VoidPtrTy) ||
      !context.hasSameType(decl.getParamDecl(1)->getType(),
                           context.getSizeType()) ||
      !context.hasSameType(decl.getReturnType(), model == M::MapMemory
                                                     ? context.VoidPtrTy
                                                     : context.IntTy))
    return M::MappingRuntime;
  for (unsigned i = 2; i < decl.getNumParams(); ++i)
    if (i == 5 ? !decl.getParamDecl(i)->getType()->isSignedIntegerType()
               : !context.hasSameType(decl.getParamDecl(i)->getType(),
                                      context.IntTy))
      return M::MappingRuntime;
  return model;
}

ExternalModel handlerContract(const clang::FunctionDecl &decl) {
  const auto name = decl.getQualifiedNameAsString();
  const bool terminate =
      name == "std::set_terminate" || name == "std::get_terminate";
  const bool setter =
      name == "std::set_new_handler" || name == "std::set_terminate";
  if (!setter && name != "std::get_new_handler" && !terminate)
    return M::Unknown;
  const auto type = decl.getReturnType();
  const auto *signature =
      type->isPointerType()
          ? type->getPointeeType()->getAs<clang::FunctionProtoType>()
          : nullptr;
  if (decl.isVariadic() || decl.getNumParams() != (setter ? 1u : 0u) ||
      !signature || signature->getNumParams() != 0 || signature->isVariadic() ||
      !signature->getReturnType()->isVoidType() ||
      (setter && !decl.getASTContext().hasSameType(
                     decl.getParamDecl(0)->getType(), type)))
    return M::CallbackRuntime;
  if (terminate)
    return setter ? M::SetTerminateHandler : M::GetTerminateHandler;
  return setter ? M::SetNewHandler : M::GetNewHandler;
}

ExternalModel exitRegistration(const clang::FunctionDecl &decl) {
  const auto &context = decl.getASTContext();
  if (!context.getTargetInfo().getTriple().isOSLinux() ||
      !context.hasSameType(decl.getReturnType(), context.IntTy))
    return M::CallbackRuntime;
  const auto type = decl.getParamDecl(0)->getType();
  const auto *callback =
      type->isPointerType()
          ? type->getPointeeType()->getAs<clang::FunctionProtoType>()
          : nullptr;
  const auto name = decl.getName();
  const unsigned count = name == "on_exit" ? 2 : name == "__cxa_atexit" ? 1 : 0;
  if (!callback || callback->isVariadic() ||
      callback->getNumParams() != count ||
      !callback->getReturnType()->isVoidType())
    return M::CallbackRuntime;
  for (unsigned i = 0; i < count; ++i)
    if (!context.hasSameType(callback->getParamType(i),
                             name == "on_exit" && i == 0 ? context.IntTy
                                                         : context.VoidPtrTy))
      return M::CallbackRuntime;
  for (unsigned i = 1; i < decl.getNumParams(); ++i)
    if (!context.hasSameType(decl.getParamDecl(i)->getType(),
                             context.VoidPtrTy))
      return M::CallbackRuntime;
  return M::ExitRegistration;
}

ExternalModel nativeStateContract(const clang::FunctionDecl &decl, M model) {
  const auto &context = decl.getASTContext();
  const auto failure =
      model == M::LockState ? M::ThreadRuntime : M::CallbackRuntime;
  // glibc's rwlock implementation explicitly excludes cancellation. Its
  // atomic state/futex operations have the same bounded contract as locks.
  const auto typeName = model == M::SignalStack ? "stack_t"
                        : decl.getName().starts_with("pthread_rwlock_")
                            ? "pthread_rwlock_t"
                            : "pthread_mutex_t";
  if (!context.getTargetInfo().getTriple().isOSLinux() ||
      !context.hasSameType(decl.getReturnType(), context.IntTy))
    return failure;
  for (unsigned i = 0; i < decl.getNumParams(); ++i) {
    const auto type = decl.getParamDecl(i)->getType();
    if (!type->isPointerType())
      return failure;
    const auto value = type->getPointeeType();
    const auto *alias = value->getAs<clang::TypedefType>();
    if (!alias || alias->getDecl()->getName() != typeName ||
        value.isConstQualified() != (model == M::SignalStack && i == 0))
      return failure;
  }
  return model;
}

ExternalModel variadicContract(const clang::FunctionDecl &decl, M model) {
  // The reviewed SysV list is an array whose decay preserves its address.
  // Pointer-valued and other ABIs need explicit list-lvalue extraction first.
  if (decl.getASTContext().getTargetInfo().getBuiltinVaListKind() ==
      clang::TargetInfo::X86_64ABIBuiltinVaList)
    return model;
  return ExternalModules::variadicListArgument(model) >= 0 ? M::VFormat
                                                           : M::VarArgs;
}

ExternalModel passwordContract(const clang::FunctionDecl &decl) {
  const auto &context = decl.getASTContext();
  const auto record = decl.getParamDecl(1)->getType().getUnqualifiedType();
  const auto *type = record->isPointerType()
                         ? record->getPointeeType()->getAs<clang::RecordType>()
                         : nullptr;
  const auto same = [&](unsigned i, clang::QualType expected) {
    return context.hasSameType(
        decl.getParamDecl(i)->getType().getUnqualifiedType(), expected);
  };
  if (!context.getTargetInfo().getTriple().isOSLinux() || !type ||
      type->getDecl()->getQualifiedNameAsString() != "passwd" ||
      record->getPointeeType().isConstQualified() ||
      !context.hasSameType(decl.getReturnType(), context.IntTy) ||
      !same(2, context.getPointerType(context.CharTy)) ||
      !same(3, context.getSizeType()) ||
      !same(4, context.getPointerType(record)))
    return M::CallbackRuntime;
  if (decl.getName() == "getpwnam_r"
          ? !same(0, context.getPointerType(context.CharTy.withConst()))
          : !decl.getParamDecl(0)->getType()->isUnsignedIntegerType())
    return M::CallbackRuntime;
  return M::PasswordLookup;
}

ExternalModel loaderContract(const clang::FunctionDecl &decl) {
  const auto &context = decl.getASTContext();
  const auto type = decl.getParamDecl(1)->getType();
  const auto *alias = type->isPointerType()
                          ? type->getPointeeType()->getAs<clang::TypedefType>()
                          : nullptr;
  return context.getTargetInfo().getTriple().isOSLinux() &&
                 context.hasSameType(decl.getReturnType(), context.IntTy) &&
                 context.hasSameType(
                     decl.getParamDecl(0)->getType(),
                     context.getPointerType(context.VoidTy.withConst())) &&
                 alias && alias->getDecl()->getName() == "Dl_info" &&
                 !type->getPointeeType().isConstQualified()
             ? M::LoaderQuery
             : M::CallbackRuntime;
}

ExternalModel directoryContract(const clang::FunctionDecl &decl, M model) {
  const auto &context = decl.getASTContext();
  const auto input = decl.getParamDecl(0)->getType().getUnqualifiedType();
  const auto result = decl.getReturnType();
  const auto directory = model == M::DirectoryOpen ? result : input;
  const auto *alias =
      directory->isPointerType()
          ? directory->getPointeeType()->getAs<clang::TypedefType>()
          : nullptr;
  if (!context.getTargetInfo().getTriple().isOSLinux() || !alias ||
      alias->getDecl()->getName() != "DIR" ||
      directory->getPointeeType().isConstQualified())
    return M::IOWrite;
  if (model == M::DirectoryOpen &&
      context.hasSameType(input,
                          context.getPointerType(context.CharTy.withConst())))
    return model;
  if (model == M::DirectoryClose && context.hasSameType(result, context.IntTy))
    return model;
  if (model == M::DirectoryRead && namedPointee(result, "dirent") &&
      result->isPointerType() && !result->getPointeeType().isConstQualified())
    return model;
  return M::IOWrite;
}

ExternalModel processReplaceContract(const clang::FunctionDecl &decl) {
  const auto &context = decl.getASTContext();
  if (!context.getTargetInfo().getTriple().isOSLinux() ||
      !context.hasSameType(decl.getReturnType(), context.IntTy) ||
      !context.hasSameType(decl.getParamDecl(0)->getType().getUnqualifiedType(),
                           context.getPointerType(context.CharTy.withConst())))
    return M::ProcessRuntime;
  const auto vector = context.getPointerType(
      context.getPointerType(context.CharTy).withConst());
  for (unsigned i = 1; i < decl.getNumParams(); ++i)
    if (!context.hasSameType(
            decl.getParamDecl(i)->getType().getUnqualifiedType(), vector))
      return M::ProcessRuntime;
  return M::ProcessReplace;
}

ExternalModel runtimeContract(const clang::FunctionDecl &decl, M model) {
  const auto &context = decl.getASTContext();
  if (model == M::ProcessReplace)
    return processReplaceContract(decl);
  if (model == M::DirectoryOpen || model == M::DirectoryRead ||
      model == M::DirectoryClose)
    return directoryContract(decl, model);
  if (model == M::LoaderQuery)
    return loaderContract(decl);
  if (model == M::PasswordLookup)
    return passwordContract(decl);
  if (model == M::ExitRegistration)
    return exitRegistration(decl);
  if (ExternalModules::variadicListArgument(model) >= 0)
    return variadicContract(decl, model);
  if (model == M::LockState || model == M::SignalStack)
    return nativeStateContract(decl, model);
  if (model == M::MapMemory || model == M::UnmapMemory ||
      model == M::ProtectMemory)
    return mappingContract(decl, model);
  if (model == M::Sort || model == M::SortWithContext || model == M::Search)
    return comparatorContract(decl, model);
  if (model == M::LoaderIterate || model == M::UnwindTrace)
    return traversalContract(decl, model);
  if (model == M::Once) {
    const auto callback = decl.getParamDecl(1)->getType();
    const auto *prototype =
        callback->isPointerType()
            ? callback->getPointeeType()->getAs<clang::FunctionProtoType>()
            : nullptr;
    // The reviewed Linux runtime writes an integer once-control and invokes
    // void init(void). Other layouts/signatures retain the partial contract.
    if (!context.getTargetInfo().getTriple().isOSLinux() ||
        !context.hasSameType(decl.getReturnType(), context.IntTy) ||
        !context.hasSameType(
            decl.getParamDecl(0)->getType().getUnqualifiedType(),
            context.getPointerType(context.IntTy)) ||
        !prototype || prototype->getNumParams() != 0 ||
        prototype->isVariadic() || !prototype->getReturnType()->isVoidType())
      return M::ThreadRuntime;
  }
  if (model == M::MathOutput) {
    const auto name = decl.getName();
    auto scalar = context.DoubleTy;
    if (name == "modff" || name == "frexpf")
      scalar = context.FloatTy;
    else if (name == "modfl" || name == "frexpl")
      scalar = context.LongDoubleTy;
    const auto output = name.starts_with("frexp") ? context.IntTy : scalar;
    if (decl.isVariadic() || decl.getNumParams() != 2 ||
        !context.hasSameType(decl.getReturnType(), scalar) ||
        !context.hasSameType(
            decl.getParamDecl(0)->getType().getUnqualifiedType(), scalar) ||
        !context.hasSameType(
            decl.getParamDecl(1)->getType().getUnqualifiedType(),
            context.getPointerType(output)))
      return M::Unknown;
  }
  if (model == M::Tokenize) {
    const auto chars = context.getPointerType(context.CharTy);
    if (!context.hasSameType(decl.getReturnType(), chars) ||
        !context.hasSameType(
            decl.getParamDecl(0)->getType().getUnqualifiedType(), chars) ||
        !context.hasSameType(
            decl.getParamDecl(1)->getType().getUnqualifiedType(),
            context.getPointerType(context.CharTy.withConst())) ||
        !context.hasSameType(
            decl.getParamDecl(2)->getType().getUnqualifiedType(),
            context.getPointerType(chars)))
      return M::Unknown;
  }
  const auto scalarOutput = [&](unsigned index) {
    const auto type = decl.getParamDecl(index)->getType();
    return type->isPointerType() &&
           !type->getPointeeType().isConstQualified() &&
           scalarStorage(type->getPointeeType(), context);
  };
  const bool condition = model == M::ConditionInitialize ||
                         model == M::ConditionNotify ||
                         model == M::ConditionDestroy;
  if (condition && (!context.getTargetInfo().getTriple().isOSLinux() ||
                    !context.hasSameType(decl.getReturnType(), context.IntTy) ||
                    !scalarOutput(0)))
    return M::ThreadRuntime;
  if (model == M::ConditionInitialize) {
    const auto type = decl.getParamDecl(1)->getType();
    if (!type->isPointerType() || !type->getPointeeType().isConstQualified() ||
        !scalarStorage(type->getPointeeType(), context))
      return M::ThreadRuntime;
  }
  if (model == M::Buffer1 &&
      llvm::StringSwitch<bool>(decl.getName())
          .Cases("stat", "fstat", "lstat", "getrlimit", "getrusage",
                 "clock_gettime", true)
          .Default(false) &&
      context.hasSameType(decl.getReturnType(), context.IntTy) &&
      scalarOutput(1))
    return M::ScalarSystemOutput;
  return model;
}

// libsupc++ default wrappers forward to another replaceable overload. Keep
// that dependency so a supplied replacement invalidates the wrapper contract.
ExternalModel allocationContract(const clang::FunctionDecl &decl) {
  const auto op = decl.getOverloadedOperator();
  const bool allocation = op == clang::OO_New || op == clang::OO_Array_New;
  const auto fallback = allocation ? M::Allocate : M::Deallocate;
  const bool linux =
      decl.getASTContext().getTargetInfo().getTriple().isOSLinux();
  if (decl.getNumParams() == 1 && op == clang::OO_Delete)
    return M::CFree;
  if (decl.getNumParams() == 1 && op == clang::OO_New)
    return M::DefaultAllocate;
  if (!linux)
    return fallback;
  for (unsigned i = 1; i < decl.getNumParams(); ++i) {
    const auto type = decl.getParamDecl(i)->getType();
    if (!type->isAlignValT() &&
        !decl.getASTContext().hasSameType(type,
                                          decl.getASTContext().getSizeType()) &&
        !(type->isLValueReferenceType() &&
          type->getPointeeType()->isNothrowT()))
      return fallback;
  }
  if (decl.getNumParams() == 2 &&
      decl.getParamDecl(1)->getType()->isAlignValT()) {
    if (op == clang::OO_New)
      return M::DefaultAllocate;
    if (op == clang::OO_Delete)
      return M::CFree;
  }
  return allocation ? M::DelegatingAllocate : M::DelegatingDelete;
}

ExternalModel systemRuntime(const clang::FunctionDecl &decl) {
  using M = ExternalModel;
  if (!systemDeclaration(decl))
    return M::Unknown;
  const auto spelling = decl.getNameAsString();
  const llvm::StringRef name(spelling);
  // glibc exposes this C symbol with C++ language linkage in C++ headers.
  if (name == "at_quick_exit" && decl.getQualifiedNameAsString() == name &&
      decl.getNumParams() == 1 && !decl.isVariadic())
    if (const auto *label = decl.getAttr<clang::AsmLabelAttr>();
        label && label->getLabel() == name)
      return exitRegistration(decl);
  if (decl.isExternC() &&
      decl.getQualifiedNameAsString() == "__cxxabiv1::__cxa_atexit" &&
      decl.getNumParams() == 3 && !decl.isVariadic())
    return exitRegistration(decl);
  // Non-builtin POSIX/GNU identities require an actual system declaration,
  // global C linkage and the reviewed fixed/variadic arity. Names are used only
  // here, while the AST is alive; graph records retain the numeric model ID.
  if (decl.isExternC() && decl.getQualifiedNameAsString() == name) {
    if (const auto *entry =
            runtimeEntry(name, decl.getNumParams(), decl.isVariadic()))
      return runtimeContract(decl, entry->model);
    return runtimeContract(decl, mathModel(name));
  }

  bool standard = false;
  for (const auto *context = decl.getDeclContext(); context;
       context = context->getParent())
    if (const auto *ns = llvm::dyn_cast<clang::NamespaceDecl>(context);
        ns && ns->isStdNamespace())
      standard = true;
  if (!standard)
    return M::Unknown;
  if (const auto tree = treeRuntime(decl); tree != M::Unknown)
    return tree;
  if (const auto stream = stringStreamContract(decl); stream != M::Unknown)
    return stream;
  const auto qualified = decl.getQualifiedNameAsString();
  if ((qualified == "std::generic_category" ||
       qualified == "std::system_category") &&
      decl.getNumParams() == 0 && !decl.isVariadic() &&
      decl.getReturnType()->isLValueReferenceType() &&
      decl.getReturnType()->getPointeeType().isConstQualified() &&
      namedPointee(decl.getReturnType(), "std::error_category"))
    return M::ExternalPointer;
  if (qualified == "std::chrono::steady_clock::now" ||
      qualified == "std::chrono::system_clock::now" ||
      qualified == "std::thread::hardware_concurrency")
    return M::SystemQuery;
  if (name.starts_with("__throw_") || name == "terminate")
    return M::TerminateRuntime;
  if (const auto handler = handlerContract(decl); handler != M::Unknown)
    return handler;
  if (const auto *method = llvm::dyn_cast<clang::CXXMethodDecl>(&decl)) {
    if (const auto model = objectLifetime(*method); model != M::Unknown)
      return model;
    if (const auto model = listRuntime(*method); model != M::Unknown)
      return model;
    if (const auto model = synchronizationRuntime(*method); model != M::Unknown)
      return model;
    if (const auto model = rehashPolicy(*method); model != M::Unknown)
      return model;
    if (const auto model = stringSearch(*method); model != M::Unknown)
      return model;
    if (const auto model = stringAccess(*method); model != M::Unknown)
      return model;
    if (const auto model = stringMutation(*method); model != M::Unknown)
      return model;
    const auto *identifier = method->getParent()->getIdentifier();
    if (!identifier)
      return M::Unknown;
    // Private library layouts and hidden state are not reconstructed. These
    // are explicitly partial contracts, including receiver/argument havoc.
    return llvm::StringSwitch<M>(identifier->getName())
        .Cases("basic_string", "basic_ios", "basic_stringbuf", "ios_base",
               "locale", M::CppRuntime)
        .Cases("_Prime_rehash_policy", "_List_node_base", "_Result_base",
               M::CppRuntime)
        .Cases("thread", "_State", "condition_variable",
               "__atomic_futex_unsigned_base", M::ThreadRuntime)
        .Default(M::Unknown);
  }
  return llvm::StringSwitch<M>(name)
      .Cases("_Rb_tree_increment", "_Rb_tree_decrement",
             "_Rb_tree_insert_and_rebalance", "_Rb_tree_rebalance_for_erase",
             M::CppRuntime)
      .Cases("getline", "generic_category", "system_category", M::CppRuntime)
      .Case("_Hash_bytes", M::ReadMemory)
      .Default(M::Unknown);
}
} // namespace

FormatEffect ExternalModules::formatEffect(const clang::Expr &format,
                                           clang::ASTContext &context) {
  const auto *literal =
      llvm::dyn_cast<clang::StringLiteral>(format.IgnoreParenImpCasts());
  if (!literal)
    return FormatEffect::Dynamic;
  if (!literal->isOrdinary())
    return FormatEffect::Unsupported;
  const auto text = literal->getBytes().split('\0').first;
  FormatEffects check(context.getLangOpts());
  const bool stopped = clang::analyze_format_string::ParsePrintfString(
      check, text.begin(), text.end(), context.getLangOpts(),
      context.getTargetInfo(), false);
  if (stopped || !check.valid)
    return FormatEffect::Unsupported;
  return check.writes ? FormatEffect::WriteCount : FormatEffect::OutputOnly;
}

int ExternalModules::formatArgument(ExternalModel model) {
  switch (model) {
  case ExternalModel::Printf:
  case ExternalModel::VPrintf:
    return 0;
  case ExternalModel::Fprintf:
  case ExternalModel::Sprintf:
  case ExternalModel::VFprintf:
  case ExternalModel::VSprintf:
    return 1;
  case ExternalModel::Snprintf:
  case ExternalModel::VSnprintf:
    return 2;
  default:
    return -1;
  }
}

int ExternalModules::variadicListArgument(ExternalModel model) {
  switch (model) {
  case ExternalModel::VPrintf:
  case ExternalModel::VFprintf:
  case ExternalModel::VSprintf:
  case ExternalModel::VSnprintf:
    return formatArgument(model) + 1;
  default:
    return -1;
  }
}

ExternalSummary ExternalModules::formattedSummary(ExternalModel model,
                                                  FormatEffect format) {
  // Standard C stdio contract: stream/locale/errno effects remain visible,
  // without arbitrary writes to unrelated pointer storage. GNU custom streams,
  // registered format handlers and library interposition are outside this
  // contract. Supplied definitions are analyzed instead. See the WPG README.
  // Standard %n writes through a variadic argument, not an arbitrary global.
  // Dynamic formats conservatively admit those bounded writes. Extensions
  // and malformed literals retain the unsupported boundary.
  auto result = format == FormatEffect::Unsupported
                    ? summary(ExternalModel::Unknown)
                    : ExternalSummary{};
  const auto output = model == ExternalModel::Sprintf ||
                              model == ExternalModel::Snprintf ||
                              model == ExternalModel::VSprintf ||
                              model == ExternalModel::VSnprintf
                          ? CallEffect::WriteArguments
                          : CallEffect::WriteStream;
  for (const auto effect :
       {CallEffect::ReadArguments, CallEffect::ReadGlobals, output,
        CallEffect::WriteErrno, CallEffect::FloatingEnvironment,
        CallEffect::InputToReturn, CallEffect::InputToMemory})
    result.effects |= static_cast<std::uint32_t>(effect);
  if (format != FormatEffect::OutputOnly || variadicListArgument(model) >= 0)
    result.effects |= static_cast<std::uint32_t>(CallEffect::WriteArguments);
  return result;
}

std::uint32_t ExternalModules::writtenArguments(ExternalModel model) {
  switch (model) {
  case ExternalModel::Buffer0:
  case ExternalModel::ReturnBuffer0:
  case ExternalModel::StreamInput:
  case ExternalModel::Sprintf:
  case ExternalModel::Snprintf:
  case ExternalModel::VSprintf:
  case ExternalModel::VSnprintf:
  case ExternalModel::VarArgsStart:
  case ExternalModel::LockState:
  case ExternalModel::AtomicFlag:
  case ExternalModel::AtomicClear:
  case ExternalModel::Sort:
  case ExternalModel::SortWithContext:
  case ExternalModel::StringSwap:
  case ExternalModel::DirectoryRead:
  case ExternalModel::DirectoryClose:
    return 1;
  case ExternalModel::Buffer1:
  case ExternalModel::ReturnBuffer1:
  case ExternalModel::IORead:
  case ExternalModel::SignalStack:
  case ExternalModel::LoaderQuery:
    return 2;
  case ExternalModel::Buffer2:
    return 4;
  case ExternalModel::PointerStructure:
    return ~std::uint32_t{0};
  case ExternalModel::PasswordLookup:
    return 6; // passwd record and character buffer; result is a typed store.
  case ExternalModel::StreamGetline:
    return 3; // Stream state and destination string/buffer.
  default:
    return 0;
  }
}

int ExternalModules::returnedArgument(ExternalModel model) {
  switch (model) {
  case ExternalModel::StreamGetline:
  case ExternalModel::ReturnBuffer0:
  case ExternalModel::StreamInput:
    return 0;
  case ExternalModel::ReturnBuffer1:
  case ExternalModel::Search:
    return 1;
  default:
    return -1;
  }
}

bool ExternalModules::isIntrinsic(ExternalModel model) {
  switch (model) {
  case ExternalModel::ForwardReference:
  case ExternalModel::AddressOf:
  case ExternalModel::ScalarIntrinsic:
  case ExternalModel::NoReturn:
  case ExternalModel::TrivialDestructor:
  case ExternalModel::PointerIdentity:
  case ExternalModel::OverflowArithmetic:
  case ExternalModel::StackAllocate:
  case ExternalModel::FloatingIntrinsic:
  case ExternalModel::EmptyConstructor:
  case ExternalModel::TrivialScalarConstructor:
  case ExternalModel::TrivialDefaultConstructor:
  case ExternalModel::AtomicFence:
  case ExternalModel::FunctionObjectPointer:
  case ExternalModel::InlineFunctionPointer:
  case ExternalModel::InlineFunctionCopy:
  case ExternalModel::InlineFunctionManager:
  case ExternalModel::FunctionObjectQuery:
  case ExternalModel::FunctionTargetPointer:
  case ExternalModel::EmptyFunctionObject:
    return true;
  default:
    return false;
  }
}

namespace {
ExternalModel scalarBuiltin(unsigned id) {
  using namespace clang;
  using M = ExternalModel;
  switch (id) {
  case Builtin::BI__builtin_expect:
  case Builtin::BI__builtin_expect_with_probability:
  case Builtin::BI__builtin_is_constant_evaluated:
  case Builtin::BI__builtin_constant_p:
  case Builtin::BI__builtin_popcount:
  case Builtin::BI__builtin_popcountl:
  case Builtin::BI__builtin_popcountll:
  case Builtin::BI__builtin_clz:
  case Builtin::BI__builtin_clzl:
  case Builtin::BI__builtin_clzll:
  case Builtin::BI__builtin_ctz:
  case Builtin::BI__builtin_ctzl:
  case Builtin::BI__builtin_ctzll:
  case Builtin::BI__builtin_bswap16:
  case Builtin::BI__builtin_bswap32:
  case Builtin::BI__builtin_bswap64:
  case Builtin::BI__builtin_rotateleft8:
  case Builtin::BI__builtin_rotateleft16:
  case Builtin::BI__builtin_rotateleft32:
  case Builtin::BI__builtin_rotateleft64:
  case Builtin::BI__builtin_rotateright8:
  case Builtin::BI__builtin_rotateright16:
  case Builtin::BI__builtin_rotateright32:
  case Builtin::BI__builtin_rotateright64:
  case Builtin::BI__builtin_prefetch: // Cache hint; argument expressions are
                                      // still evaluated.
  case Builtin::BI__builtin_ffs:
  case Builtin::BI__builtin_ffsl:
  case Builtin::BI__builtin_ffsll:
  case Builtin::BI__builtin_parity:
  case Builtin::BI__builtin_parityl:
  case Builtin::BI__builtin_parityll:
  case Builtin::BI__builtin_clrsb:
  case Builtin::BI__builtin_clrsbl:
  case Builtin::BI__builtin_clrsbll:
  case Builtin::BI__builtin_abs:
  case Builtin::BI__builtin_labs:
  case Builtin::BI__builtin_llabs:
  case Builtin::BI__atomic_always_lock_free:
    return M::ScalarIntrinsic;
  case Builtin::BI__builtin_add_overflow:
  case Builtin::BI__builtin_sub_overflow:
  case Builtin::BI__builtin_mul_overflow:
    return M::OverflowArithmetic;
  case Builtin::BI__builtin_isunordered:
  case Builtin::BI__builtin_isless:
  case Builtin::BI__builtin_islessequal:
  case Builtin::BI__builtin_isgreater:
  case Builtin::BI__builtin_isgreaterequal:
  case Builtin::BI__builtin_islessgreater:
  case Builtin::BI__builtin_isnan:
  case Builtin::BI__builtin_isfinite:
  case Builtin::BI__builtin_isinf:
  case Builtin::BI__builtin_signbit:
  case Builtin::BI__builtin_signbitf:
  case Builtin::BI__builtin_signbitl:
    return M::FloatingIntrinsic;
  default:
    return M::Unknown;
  }
}

ExternalModel compilerBuiltin(const clang::FunctionDecl &decl) {
  using namespace clang;
  using M = ExternalModel;
  if (const auto model = scalarBuiltin(decl.getBuiltinID());
      model != M::Unknown)
    return model;
  switch (decl.getBuiltinID()) {
  case Builtin::BIforward:
  case Builtin::BIforward_like:
  case Builtin::BImove:
  case Builtin::BImove_if_noexcept:
  case Builtin::BIas_const:
    if (decl.getNumParams() == 1 &&
        decl.getParamDecl(0)->getType()->isReferenceType() &&
        decl.getReturnType()->isReferenceType())
      return M::ForwardReference;
    return M::Unknown;
  case Builtin::BIaddressof:
  case Builtin::BI__addressof:
  case Builtin::BI__builtin_addressof:
    return M::AddressOf;
  case Builtin::BI__builtin_assume_aligned:
    return M::PointerIdentity;
  case Builtin::BI__builtin_va_start:
    return variadicContract(decl, M::VarArgsStart);
  case Builtin::BI__builtin_va_end:
    return variadicContract(decl, M::TrivialDestructor);
  case Builtin::BI__builtin_va_copy:
    return variadicContract(decl, M::CopyMemory);
  case Builtin::BI__atomic_thread_fence:
  case Builtin::BI__atomic_signal_fence:
    return M::AtomicFence;
  case Builtin::BI__atomic_test_and_set:
    return M::AtomicFlag;
  case Builtin::BI__atomic_clear:
    return M::AtomicClear;
  case Builtin::BI__builtin_alloca:
  case Builtin::BI__builtin_alloca_with_align:
    return M::StackAllocate;
  case Builtin::BI__builtin_unreachable:
  case Builtin::BI__builtin_trap:
    return M::NoReturn;
  default:
    break;
  }
  // Pinned Clang SSE2 integer operations used by the compiler benchmarks.
  // An allowlist plus Clang's builtin identity excludes user functions; this
  // does not generalize to memory, atomic or floating-point SIMD operations.
  if (decl.getBuiltinID() &&
      llvm::StringSwitch<bool>(decl.getName())
          .Cases("__builtin_ia32_pshufd", "__builtin_ia32_pmuludq128",
                 "__builtin_ia32_psllqi128", "__builtin_ia32_psrlqi128", true)
          .Default(false))
    return M::ScalarIntrinsic;
  return M::Unknown;
}

ExternalModel stringBuiltin(unsigned id) {
  using namespace clang;
  using M = ExternalModel;
  switch (id) {
  case Builtin::BIstrlen:
  case Builtin::BI__builtin_strlen:
  case Builtin::BIstrcmp:
  case Builtin::BI__builtin_strcmp:
  case Builtin::BIstrncmp:
  case Builtin::BI__builtin_strncmp:
  case Builtin::BImemcmp:
  case Builtin::BI__builtin_memcmp:
  case Builtin::BIstrspn:
  case Builtin::BI__builtin_strspn:
  case Builtin::BIstrcspn:
  case Builtin::BI__builtin_strcspn:
    return M::ReadMemory;
  case Builtin::BImemchr:
  case Builtin::BI__builtin_memchr:
  case Builtin::BIstrchr:
  case Builtin::BI__builtin_strchr:
  case Builtin::BIstrrchr:
  case Builtin::BI__builtin_strrchr:
  case Builtin::BIstrstr:
  case Builtin::BI__builtin_strstr:
  case Builtin::BIstrpbrk:
  case Builtin::BI__builtin_strpbrk:
    return M::FindMemory;
  case Builtin::BIisalnum:
  case Builtin::BIisalpha:
  case Builtin::BIisblank:
  case Builtin::BIiscntrl:
  case Builtin::BIisdigit:
  case Builtin::BIisgraph:
  case Builtin::BIislower:
  case Builtin::BIisprint:
  case Builtin::BIispunct:
  case Builtin::BIisspace:
  case Builtin::BIisupper:
  case Builtin::BIisxdigit:
  case Builtin::BItolower:
  case Builtin::BItoupper:
    return M::ScalarLibrary;
  default:
    return M::Unknown;
  }
}

ExternalModel memoryBuiltin(unsigned id) {
  using namespace clang;
  using M = ExternalModel;
  switch (id) {
  case Builtin::BImalloc:
  case Builtin::BIcalloc:
  case Builtin::BI__builtin_malloc:
  case Builtin::BI__builtin_calloc:
    return M::CAllocate;
  case Builtin::BIaligned_alloc:
    return M::CAllocate;
  case Builtin::BI__builtin_operator_new:
    return M::Allocate;
  case Builtin::BIrealloc:
  case Builtin::BI__builtin_realloc:
    return M::Reallocate;
  case Builtin::BIfree:
  case Builtin::BI__builtin_free:
    return M::CFree;
  case Builtin::BI__builtin_operator_delete:
    return M::Deallocate;
  case Builtin::BImemcpy:
  case Builtin::BImemmove:
  case Builtin::BI__builtin_memcpy:
  case Builtin::BI__builtin_memmove:
  case Builtin::BIstrcpy:
  case Builtin::BIstrncpy:
  case Builtin::BIstrcat:
  case Builtin::BIstrncat:
  case Builtin::BI__builtin_strcpy:
  case Builtin::BI__builtin_strncpy:
  case Builtin::BI__builtin_strcat:
  case Builtin::BI__builtin_strncat:
    return M::CopyMemory;
  case Builtin::BI__builtin_mempcpy:
  case Builtin::BI__builtin_stpcpy:
  case Builtin::BI__builtin_stpncpy:
    return M::CopyInterior;
  case Builtin::BImemset:
  case Builtin::BI__builtin_memset:
    return M::SetMemory;
  default:
    return M::Unknown;
  }
}

ExternalModel libraryBuiltin(const clang::FunctionDecl &decl) {
  using namespace clang;
  using M = ExternalModel;
  switch (decl.getBuiltinID()) {
  case Builtin::BIfprintf:
  case Builtin::BI__builtin_fprintf:
    return M::Fprintf;
  case Builtin::BIprintf:
  case Builtin::BI__builtin_printf:
    return M::Printf;
  case Builtin::BIsprintf:
  case Builtin::BI__builtin_sprintf:
    return M::Sprintf;
  case Builtin::BIsnprintf:
  case Builtin::BI__builtin_snprintf:
    return M::Snprintf;
  case Builtin::BIvprintf:
  case Builtin::BI__builtin_vprintf:
    return variadicContract(decl, M::VPrintf);
  case Builtin::BIvfprintf:
  case Builtin::BI__builtin_vfprintf:
    return variadicContract(decl, M::VFprintf);
  case Builtin::BIvsprintf:
  case Builtin::BI__builtin_vsprintf:
    return variadicContract(decl, M::VSprintf);
  case Builtin::BIvsnprintf:
  case Builtin::BI__builtin_vsnprintf:
    return variadicContract(decl, M::VSnprintf);
  case Builtin::BIfwrite:
    return M::StreamOutput;
  case Builtin::BIfread:
    return M::StreamInput;
  case Builtin::BIstrdup:
  case Builtin::BIstrndup:
  case Builtin::BI__builtin_strdup:
  case Builtin::BI__builtin_strndup:
    return M::DuplicateString;
  case Builtin::BIstrtod:
  case Builtin::BIstrtof:
  case Builtin::BIstrtold:
  case Builtin::BIstrtol:
  case Builtin::BIstrtoll:
  case Builtin::BIstrtoul:
  case Builtin::BIstrtoull:
    return M::NumericConversion;
  case Builtin::BIabs:
  case Builtin::BIlabs:
  case Builtin::BIllabs:
    return M::ScalarLibrary;
  case Builtin::BIabort:
  case Builtin::BIexit:
  case Builtin::BI__builtin_abort:
    return M::TerminateRuntime;
  case Builtin::BI_Exit:
    return M::ImmediateExit;
  default:
    return M::Unknown;
  }
}

} // namespace

const clang::FunctionDecl *
ExternalModules::resolveCallee(const clang::CallExpr &call) {
  const auto *direct = call.getDirectCallee();
  if (!direct)
    return nullptr;
  const auto builtin = direct->getBuiltinID();
  if (builtin != clang::Builtin::BI__builtin_operator_new &&
      builtin != clang::Builtin::BI__builtin_operator_delete)
    return direct;
  auto &context = direct->getASTContext();
  const auto operation = builtin == clang::Builtin::BI__builtin_operator_new
                             ? clang::OO_New
                             : clang::OO_Delete;
  const auto name = context.DeclarationNames.getCXXOperatorName(operation);
  // Match Clang 18 CodeGen's lookup of the overload selected by Sema. This
  // preserves sized/aligned signatures without repeating overload resolution.
  for (const auto *decl : context.getTranslationUnitDecl()->lookup(name))
    if (const auto *function = llvm::dyn_cast<clang::FunctionDecl>(decl))
      if (context.hasSameType(function->getType(), call.getCallee()->getType()))
        return function;
  // An unsupported native representation keeps the partial builtin model.
  return direct;
}

ExternalModel
ExternalModules::recognize(const clang::FunctionDecl &decl) const {
  using namespace clang;
  using M = ExternalModel;
  // Clang recognizes these library intrinsics by declaration and signature.
  // A same-spelled user function is not a match.
  if (const auto model = compilerBuiltin(decl); model != M::Unknown)
    return model;
  if (const auto model = functionObjectContract(decl); model != M::Unknown)
    return model;
  if (const auto model = functionWrapperContract(decl); model != M::Unknown)
    return model;
  if (const auto *dtor = llvm::dyn_cast<CXXDestructorDecl>(&decl);
      dtor && dtor->isTrivial())
    return M::TrivialDestructor;
  // Trivial default construction runs no user code. Value initialization can
  // zero pointer members; default initialization need not define them. Neither
  // operation introduces a non-null target, so weak PTA facts can be retained.
  // Copies and default member initializers require their actual effects.
  if (const auto *ctor = llvm::dyn_cast<CXXConstructorDecl>(&decl);
      ctor && ctor->isTrivial() && ctor->isDefaultConstructor() &&
      !ctor->isDeleted()) {
    if (ctor->getParent()->isEmpty())
      return M::EmptyConstructor;
    if (scalarStorage(decl.getASTContext().getRecordType(ctor->getParent()),
                      decl.getASTContext()))
      return M::TrivialScalarConstructor;
    return M::TrivialDefaultConstructor;
  }

  // Analyze supplied implementations instead of overriding their behavior.
  if (decl.hasBody())
    return M::Unknown;
  if (decl.getBuiltinID())
    if (const auto math = mathModel(decl.getName()); math != M::Unknown)
      return math;
  for (const auto model :
       {libraryBuiltin(decl), stringBuiltin(decl.getBuiltinID()),
        memoryBuiltin(decl.getBuiltinID())})
    if (model != M::Unknown)
      return model;
  if (decl.isReplaceableGlobalAllocationFunction())
    return allocationContract(decl);
  if (const auto model = llvmRuntime(decl); model != M::Unknown)
    return model;
  return systemRuntime(decl);
}

ExternalModelResult
ExternalModules::query(const clang::FunctionDecl *decl, std::uint8_t formats,
                       bool readOnlyCallback,
                       FunctionManagerOperation operation) const {
  return query(enabled_ && decl ? recognize(*decl) : ExternalModel::Unknown,
               formats, readOnlyCallback, operation);
}

FunctionManagerOperation
ExternalModules::managerOperation(const clang::Expr &argument,
                                  clang::ASTContext &context) {
  using O = FunctionManagerOperation;
  const auto *type = argument.getType()->getAs<clang::EnumType>();
  if (!type ||
      type->getDecl()->getQualifiedNameAsString() !=
          "std::_Manager_operation" ||
      !context.getSourceManager().isInSystemHeader(
          type->getDecl()->getLocation()))
    return O::Unknown;
  clang::Expr::EvalResult value;
  if (!argument.EvaluateAsInt(value, context))
    return O::Unknown;
  for (const auto *constant : type->getDecl()->enumerators())
    if (constant->getInitVal() == value.Val.getInt())
      return llvm::StringSwitch<O>(constant->getName())
          .Case("__get_type_info", O::TypeInfo)
          .Case("__get_functor_ptr", O::Pointer)
          .Case("__clone_functor", O::Clone)
          .Case("__destroy_functor", O::Destroy)
          .Default(O::Unknown);
  return O::Unknown;
}

ExternalModelResult ExternalModules::query(ExternalModel model,
                                           std::uint8_t formats,
                                           bool readOnlyCallback,
                                           FunctionManagerOperation operation) {
  if (model == ExternalModel::Unknown)
    return {};
  if (const auto index = formatArgument(model); index >= 0)
    return {model, formattedSummary(model, static_cast<FormatEffect>(
                                               (formats >> (2 * index)) & 3u))};
  if (callbackArgument(model) >= 0)
    return {model, callbackSummary(model, readOnlyCallback)};
  if (model == ExternalModel::InlineFunctionManager) {
    using E = CallEffect;
    switch (operation) {
    case FunctionManagerOperation::Destroy:
      return {model, flags({E::EndLifetime})};
    case FunctionManagerOperation::Clone:
      return {model, summary(ExternalModel::InlineFunctionCopy)};
    case FunctionManagerOperation::Pointer:
      return {model,
              flags({E::ReadArguments, E::WriteArguments, E::InputToMemory})};
    case FunctionManagerOperation::TypeInfo:
      return {model, flags({E::ReadGlobals, E::WriteArguments})};
    case FunctionManagerOperation::Unknown:
      break;
    }
  }
  return {model, summary(model)};
}

const clang::FunctionDecl *
ExternalModules::delegate(const clang::FunctionDecl &decl,
                          ExternalModel model) {
  using namespace clang;
  if (model != ExternalModel::DelegatingDelete &&
      model != ExternalModel::DelegatingAllocate &&
      model != ExternalModel::StringCreate)
    return nullptr;
  auto &context = decl.getASTContext();
  auto op = OO_New;
  QualType alignment;
  unsigned extra = 0;
  if (model != ExternalModel::StringCreate) {
    op = decl.getOverloadedOperator();
    for (unsigned i = 1; i < decl.getNumParams(); ++i) {
      const auto type = decl.getParamDecl(i)->getType();
      if (type->isAlignValT())
        alignment = type;
      else
        ++extra; // Sized or nothrow wrappers keep the array/scalar operator.
    }
    if (!extra)
      op = op == OO_Array_New ? OO_New : OO_Delete;
  }
  const auto name = context.DeclarationNames.getCXXOperatorName(op);
  for (const auto *candidate : context.getTranslationUnitDecl()->lookup(name)) {
    const auto *function = dyn_cast<FunctionDecl>(candidate);
    if (!function || !function->isReplaceableGlobalAllocationFunction() ||
        function->getNumParams() != (alignment.isNull() ? 1u : 2u))
      continue;
    if (alignment.isNull() ||
        context.hasSameType(function->getParamDecl(1)->getType(), alignment))
      return function;
  }
  return nullptr;
}

namespace {
ExternalSummary callbackEffects(ExternalModel model, bool readOnly) {
  using E = CallEffect;
  // Only the runtime's own effects are known here. A caller-supplied callback
  // can change globals or reachable memory; its missing summary stays open.
  auto result = flags({E::ReadArguments, E::ReadGlobals, E::InputToReturn,
                       E::Callback, E::NoReturn});
  if (!readOnly)
    result.effects |= flags({E::Throw, E::Unknown}).effects;
  if (model == ExternalModel::LoaderIterate ||
      model == ExternalModel::UnwindTrace) {
    // Loader traversal holds its recursive lock and borrows module records;
    // DWARF backtracing borrows stack/register state. Neither unwinds frames
    // or runs personality routines. Only the supplied trace can mutate input.
    result.effects |=
        flags({E::ReadExternalState, E::WriteExternalState, E::Synchronize})
            .effects;
    if (model == ExternalModel::UnwindTrace)
      // libgcc can allocate/free an index for registered frame descriptions.
      result.effects |=
          flags({E::Allocate, E::BeginLifetime, E::EndLifetime, E::WriteErrno})
              .effects;
  } else if (model == ExternalModel::Once)
    result.effects |=
        flags({E::WriteArguments, E::InputToMemory, E::Synchronize}).effects;
  else if (model != ExternalModel::Search)
    result.effects |= flags({E::WriteArguments, E::InputToMemory, E::Allocate,
                             E::BeginLifetime, E::EndLifetime})
                          .effects;
  return result;
}

ExternalSummary runtimeStateSummary(ExternalModel model) {
  using E = CallEffect;
  switch (model) {
  case ExternalModel::InlineFunctionCopy:
  case ExternalModel::InlineFunctionManager: {
    auto effects = flags({E::ReadArguments, E::WriteArguments, E::InputToMemory,
                          E::BeginLifetime});
    if (model == ExternalModel::InlineFunctionManager)
      effects.effects |= static_cast<std::uint32_t>(E::EndLifetime) |
                         static_cast<std::uint32_t>(E::ReadGlobals) |
                         static_cast<std::uint32_t>(E::InputToReturn);
    return effects;
  }
  case ExternalModel::ReceiverPointerUpdate:
    return flags({E::ReadArguments, E::WriteArguments, E::InputToMemory});
  case ExternalModel::StreamGetline:
    return flags({E::ReadArguments, E::WriteArguments, E::ReadExternalState,
                  E::WriteStream, E::Allocate, E::BeginLifetime, E::EndLifetime,
                  E::InputToReturn, E::InputToMemory, E::Callback, E::Throw,
                  E::Unknown});
  case ExternalModel::DelegatingDelete:
    return flags({E::EndLifetime}); // Every selected delegate is checked.
  case ExternalModel::StringErase:
  case ExternalModel::StringSwap:
    return flags({E::ReadArguments, E::WriteArguments, E::InputToMemory});
  case ExternalModel::StringReturnSelf:
  case ExternalModel::StringMutate:
    return flags({E::ReadArguments, E::WriteArguments, E::Allocate,
                  E::BeginLifetime, E::EndLifetime, E::InputToReturn,
                  E::InputToMemory, E::Throw, E::Callback, E::Unknown});
  case ExternalModel::DefaultAllocate:
  case ExternalModel::DelegatingAllocate:
    return flags({E::Allocate, E::BeginLifetime, E::ReadExternalState,
                  E::WriteErrno, E::InputToReturn, E::Callback, E::Throw,
                  E::NoReturn, E::Unknown});
  case ExternalModel::StringCreate: {
    auto result = runtimeStateSummary(ExternalModel::DefaultAllocate);
    result.effects |=
        flags({E::ReadArguments, E::WriteArguments, E::InputToMemory}).effects;
    return result;
  }
  case ExternalModel::VarArgsStart:
    return flags({E::BeginLifetime, E::ReadArguments, E::WriteArguments,
                  E::InputToMemory});
  case ExternalModel::MapMemory:
    return flags({E::Allocate, E::BeginLifetime, E::EndLifetime,
                  E::ReadArguments, E::WriteArguments, E::ReadExternalState,
                  E::WriteExternalState, E::WriteErrno, E::InputToReturn,
                  E::InputToMemory});
  case ExternalModel::UnmapMemory:
    return flags({E::EndLifetime, E::ReadArguments, E::WriteExternalState,
                  E::WriteErrno, E::InputToReturn});
  case ExternalModel::ProtectMemory:
    return flags({E::ReadArguments, E::WriteExternalState, E::WriteErrno,
                  E::InputToReturn});
  case ExternalModel::SetNewHandler:
  case ExternalModel::SetTerminateHandler:
    return flags({E::ReadArguments, E::ReadExternalState, E::WriteExternalState,
                  E::Synchronize, E::Escape, E::InputToReturn});
  case ExternalModel::GetNewHandler:
  case ExternalModel::GetTerminateHandler:
    return flags({E::ReadExternalState, E::Synchronize, E::InputToReturn});
  case ExternalModel::ExitRegistration:
    return flags({E::ReadArguments, E::ReadExternalState, E::WriteExternalState,
                  E::Escape, E::Synchronize, E::Allocate, E::BeginLifetime,
                  E::WriteErrno, E::InputToReturn, E::InputToMemory});
  case ExternalModel::LockState:
    return flags({E::ReadArguments, E::WriteArguments, E::Escape,
                  E::Synchronize, E::ReadExternalState, E::WriteExternalState,
                  E::NoReturn, E::InputToReturn, E::InputToMemory});
  case ExternalModel::SignalStack:
    return flags({E::ReadArguments, E::WriteArguments, E::Escape,
                  E::ReadExternalState, E::WriteExternalState, E::WriteErrno,
                  E::InputToReturn, E::InputToMemory});
  default:
    return ExternalSummary::unknown();
  }
}

ExternalSummary directorySummary(ExternalModel model) {
  using E = CallEffect;
  // glibc uses noncancelling open/close and a direct getdents syscall.
  // Directory entries borrow the DIR buffer; this does not model arbitrary I/O
  // handles.
  auto result =
      flags({E::ReadArguments, E::ReadExternalState, E::WriteExternalState,
             E::WriteErrno, E::InputToReturn, E::InputToMemory});
  if (model == ExternalModel::DirectoryOpen)
    result.effects |= flags({E::Allocate, E::BeginLifetime}).effects;
  else if (model == ExternalModel::DirectoryClose)
    result.effects |= flags({E::WriteArguments, E::EndLifetime}).effects;
  else
    result.effects |= flags({E::WriteArguments, E::Synchronize}).effects;
  return result;
}

// Recognition alone does not resolve callbacks, cancellation paths or private
// runtime state. Keep those boundaries separate from fully reviewed effects.
ExternalSummary partialSummary(ExternalModel model) {
  using E = CallEffect;
  switch (model) {
  case ExternalModel::DirectoryOpen:
  case ExternalModel::DirectoryRead:
  case ExternalModel::DirectoryClose:
    return directorySummary(model);
  case ExternalModel::VFormat:
    return flags({E::ReadArguments, E::WriteArguments, E::WriteStream,
                  E::WriteErrno, E::InputToReturn, E::InputToMemory,
                  E::Unknown});
  case ExternalModel::VarArgs:
    return flags(
        {E::ReadArguments, E::WriteArguments, E::InputToMemory, E::Unknown});
  case ExternalModel::ThreadRuntime:
    return flags({E::Synchronize, E::ReadArguments, E::WriteArguments,
                  E::Escape, E::Callback, E::Unknown});
  case ExternalModel::CallbackRuntime:
    return flags({E::Callback, E::Escape, E::ReadArguments, E::WriteArguments,
                  E::Unknown});
  case ExternalModel::PasswordLookup:
    return flags({E::ReadArguments, E::WriteArguments, E::InputToReturn,
                  E::InputToMemory, E::ReadExternalState, E::WriteExternalState,
                  E::WriteErrno, E::Callback,
                  E::Unknown}); // NSS modules and cancellation remain open.
  case ExternalModel::StreamDelegate:
    // raw_ostream/streambuf virtual overrides may run arbitrary user code.
    // Record the forwarding dependency without inventing a delegate call.
    return flags({E::ReadArguments, E::WriteArguments, E::WriteStream,
                  E::InputToMemory, E::Callback, E::Throw, E::Unknown});
  case ExternalModel::StreamPosition:
    return flags({E::ReadArguments, E::InputToReturn, E::Callback, E::Throw,
                  E::Unknown});
  case ExternalModel::StaticCategory:
    return flags({E::ReadGlobals, E::ReadExternalState, E::WriteExternalState,
                  E::Synchronize, E::BeginLifetime, E::Escape,
                  E::InputToReturn});
  case ExternalModel::LoaderQuery:
    return flags({E::ReadArguments, E::WriteArguments, E::ReadGlobals,
                  E::ReadExternalState, E::Synchronize, E::InputToReturn,
                  E::InputToMemory});
  case ExternalModel::FutexWait:
    return flags({E::ReadArguments, E::ReadExternalState, E::WriteExternalState,
                  E::WriteErrno, E::Synchronize, E::NoReturn,
                  E::InputToReturn});
  case ExternalModel::TerminateRuntime:
    return flags({E::NoReturn, E::Throw, E::Callback, E::Unknown});
  case ExternalModel::ProcessRuntime:
    return flags({E::ReadExternalState, E::WriteExternalState, E::NoReturn,
                  E::Callback, E::Unknown});
  case ExternalModel::ProcessReplace:
    // execv delegates to the hidden execve syscall wrapper using environ.
    // Success replaces the process image; failure returns with errno. Neither
    // invokes exit handlers nor writes through the path/argument vectors.
    return flags({E::ReadArguments, E::ReadGlobals, E::ReadExternalState,
                  E::WriteExternalState, E::WriteErrno, E::NoReturn,
                  E::InputToReturn});
  case ExternalModel::MappingRuntime:
    return flags({E::Allocate, E::BeginLifetime, E::EndLifetime,
                  E::ReadExternalState, E::WriteExternalState, E::Unknown});
  case ExternalModel::IORead:
  case ExternalModel::IOWrite:
    return flags({E::ReadArguments, E::WriteArguments, E::ReadExternalState,
                  E::WriteExternalState, E::WriteErrno, E::Callback,
                  E::Unknown});
  default:
    return runtimeStateSummary(model);
  }
}

// Effects of stream, math and system-library operations. Argument positions
// and call-specific format validation remain in their existing catalog queries.
ExternalSummary librarySummary(ExternalModel model) {
  using E = CallEffect;
  switch (model) {
  case ExternalModel::Fprintf:
  case ExternalModel::Printf:
  case ExternalModel::Sprintf:
  case ExternalModel::Snprintf:
  case ExternalModel::VPrintf:
  case ExternalModel::VFprintf:
  case ExternalModel::VSprintf:
  case ExternalModel::VSnprintf:
    return ExternalModules::formattedSummary(model, FormatEffect::OutputOnly);
  case ExternalModel::StreamOutput:
    return flags({E::ReadArguments, E::ReadGlobals, E::WriteStream,
                  E::WriteErrno, E::InputToReturn, E::InputToMemory});
  case ExternalModel::OpenStream:
  case ExternalModel::CloseStream: {
    auto result = librarySummary(ExternalModel::StreamOutput);
    result.effects |= static_cast<std::uint32_t>(E::WriteExternalState);
    if (model == ExternalModel::OpenStream)
      result.effects |= static_cast<std::uint32_t>(E::Allocate) |
                        static_cast<std::uint32_t>(E::BeginLifetime);
    else
      result.effects |= static_cast<std::uint32_t>(E::EndLifetime);
    return result;
  }
  case ExternalModel::StreamInput:
    return flags({E::ReadExternalState, E::ReadGlobals, E::WriteStream,
                  E::WriteErrno, E::WriteArguments, E::InputToReturn,
                  E::InputToMemory});
  case ExternalModel::Math:
  case ExternalModel::FloatingIntrinsic:
    return flags({E::ReadGlobals, E::FloatingEnvironment, E::WriteErrno,
                  E::InputToReturn});
  case ExternalModel::MathOutput:
  case ExternalModel::NumericConversion:
    return flags({E::ReadArguments, E::WriteArguments, E::ReadGlobals,
                  E::WriteErrno, E::FloatingEnvironment, E::InputToReturn,
                  E::InputToMemory});
  case ExternalModel::ExternalPointer:
  case ExternalModel::FunctionTargetPointer:
    return flags({E::ReadArguments, E::ReadGlobals, E::InputToReturn});
  case ExternalModel::StaticBuffer:
    return flags(
        {E::ReadArguments, E::ReadGlobals, E::WriteGlobals, E::InputToReturn});
  case ExternalModel::SystemQuery:
    return flags({E::ReadArguments, E::ReadGlobals, E::ReadExternalState,
                  E::WriteErrno, E::InputToReturn});
  case ExternalModel::ScalarSystemOutput:
    return flags({E::ReadArguments, E::WriteArguments, E::ReadExternalState,
                  E::WriteErrno, E::InputToReturn, E::InputToMemory});
  case ExternalModel::SystemUpdate: {
    auto result = librarySummary(ExternalModel::SystemQuery);
    result.effects |= static_cast<std::uint32_t>(E::WriteExternalState);
    return result;
  }
  case ExternalModel::ConditionInitialize:
    return flags({E::BeginLifetime, E::ReadArguments, E::WriteArguments,
                  E::InputToReturn, E::InputToMemory});
  case ExternalModel::ConditionNotify:
  case ExternalModel::ConditionDestroy: {
    auto result = flags({E::ReadArguments, E::WriteArguments, E::Synchronize,
                         E::ReadExternalState, E::WriteExternalState,
                         E::InputToReturn, E::InputToMemory});
    if (model == ExternalModel::ConditionDestroy)
      result.effects |= static_cast<std::uint32_t>(E::EndLifetime);
    return result;
  }
  case ExternalModel::FutexWake:
    return flags({E::Synchronize, E::WriteExternalState, E::WriteErrno,
                  E::InputToMemory});
  case ExternalModel::Once:
  case ExternalModel::Sort:
  case ExternalModel::SortWithContext:
  case ExternalModel::Search:
  case ExternalModel::LoaderIterate:
  case ExternalModel::UnwindTrace:
    // Apply contracts at the existing site without expanding callback bodies.
    return callbackEffects(model, false);
  case ExternalModel::Buffer0:
  case ExternalModel::Buffer1:
  case ExternalModel::Buffer2:
  case ExternalModel::ReturnBuffer0:
  case ExternalModel::ReturnBuffer1:
    return flags({E::ReadArguments, E::WriteArguments, E::ReadGlobals,
                  E::WriteErrno, E::ReadExternalState, E::WriteExternalState,
                  E::Allocate, E::BeginLifetime, E::EndLifetime,
                  E::InputToReturn, E::InputToMemory});
  default:
    return partialSummary(model);
  }
}
} // namespace

int ExternalModules::callbackArgument(ExternalModel model) {
  switch (model) {
  case ExternalModel::LoaderIterate:
  case ExternalModel::UnwindTrace:
    return 0;
  case ExternalModel::Once:
    return 1;
  case ExternalModel::Sort:
  case ExternalModel::SortWithContext:
    return 3;
  case ExternalModel::Search:
    return 4;
  default:
    return -1;
  }
}

ExternalSummary ExternalModules::callbackSummary(ExternalModel model,
                                                 bool readOnly) {
  return callbackEffects(model, readOnly);
}

ExternalSummary ExternalModules::summary(ExternalModel model) {
  using E = CallEffect;
  switch (model) {
  case ExternalModel::ForwardReference:
  case ExternalModel::AddressOf:
  case ExternalModel::PointerIdentity:
    return flags({E::InputToReturn});
  case ExternalModel::ScalarIntrinsic:
    return flags({E::InputToReturn});
  case ExternalModel::ReadMemory:
  case ExternalModel::FindMemory:
  case ExternalModel::FunctionObjectPointer:
  case ExternalModel::InlineFunctionPointer:
  case ExternalModel::FunctionObjectQuery:
    return flags({E::ReadArguments, E::InputToReturn});
  case ExternalModel::Tokenize:
    return flags({E::ReadArguments, E::WriteArguments, E::InputToReturn,
                  E::InputToMemory});
  case ExternalModel::ScalarLibrary:
    return flags({E::ReadGlobals, E::InputToReturn});
  case ExternalModel::OverflowArithmetic:
    return flags({E::WriteArguments, E::InputToReturn, E::InputToMemory});
  case ExternalModel::StackAllocate:
    return flags({E::Allocate, E::BeginLifetime, E::InputToReturn});
  case ExternalModel::EmptyConstructor:
    return flags({E::BeginLifetime});
  case ExternalModel::TrivialScalarConstructor:
    // DFG retains possible scalar writes; recognition excludes pointer storage,
    // so this model needs no PTA write mask or receiver havoc.
    return flags({E::BeginLifetime, E::WriteArguments});
  case ExternalModel::TrivialDefaultConstructor:
    // May zero members; retain earlier may-targets without manufacturing a
    // definition for an uninitialized pointer. Its later use stays unknown.
    return flags({E::BeginLifetime, E::WriteArguments});
  case ExternalModel::NullObjectInitialization:
  case ExternalModel::EmptyFunctionObject:
    return flags({E::BeginLifetime, E::WriteArguments});
  case ExternalModel::PointerStructure:
    return flags({E::ReadArguments, E::WriteArguments, E::Escape,
                  E::InputToReturn, E::InputToMemory});
  case ExternalModel::AtomicFence:
    return flags({E::Synchronize});
  case ExternalModel::AtomicFlag:
    return flags({E::Synchronize, E::ReadArguments, E::WriteArguments,
                  E::InputToReturn, E::InputToMemory});
  case ExternalModel::AtomicClear:
    return flags({E::Synchronize, E::WriteArguments, E::InputToMemory});
  case ExternalModel::ScalarRehashPolicy:
    return flags({E::ReadArguments, E::WriteArguments, E::ReadGlobals,
                  E::FloatingEnvironment, E::InputToReturn, E::InputToMemory});
  case ExternalModel::ImmediateExit:
    return flags({E::NoReturn, E::WriteExternalState, E::InputToMemory});
  case ExternalModel::NoReturn:
    return flags({E::NoReturn});
  case ExternalModel::TrivialDestructor:
  case ExternalModel::RuntimeDestructor:
    return flags({E::EndLifetime});
  // Standard C library contracts, with supplied implementations taking
  // precedence. Byte writes retain unknown pointer contents in PTA rather
  // than pretending to know layout, extent or a null representation.
  case ExternalModel::CAllocate:
    return flags(
        {E::Allocate, E::BeginLifetime, E::WriteErrno, E::InputToReturn});
  case ExternalModel::DuplicateString:
    return flags({E::Allocate, E::BeginLifetime, E::ReadArguments,
                  E::WriteErrno, E::InputToReturn, E::InputToMemory});
  case ExternalModel::OutAllocate:
    return flags({E::Allocate, E::BeginLifetime, E::WriteArguments,
                  E::InputToReturn, E::InputToMemory});
  case ExternalModel::CFree:
    return flags({E::EndLifetime});
  case ExternalModel::Reallocate:
    return flags({E::Allocate, E::BeginLifetime, E::EndLifetime,
                  E::ReadArguments, E::WriteArguments, E::WriteErrno,
                  E::InputToReturn, E::InputToMemory});
  case ExternalModel::CopyMemory:
  case ExternalModel::CopyInterior:
    return flags({E::ReadArguments, E::WriteArguments, E::InputToReturn,
                  E::InputToMemory});
  case ExternalModel::SetMemory:
    return flags({E::WriteArguments, E::InputToReturn, E::InputToMemory});
  // Remaining C++ allocation/deallocation contracts retain handlers or
  // delegation to replaceable operators, pending their dedicated models.
  case ExternalModel::Allocate:
  case ExternalModel::Deallocate:
  case ExternalModel::Unknown:
    return ExternalSummary::unknown();
  default:
    return librarySummary(model);
  }
}

} // namespace wpgkit
