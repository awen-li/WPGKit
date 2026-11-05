#include "ExternalEffects.h"

namespace wpgkit {

void ExternalEffects::addField(Andersen::Node field) {
  if (fieldsClobbered_)
    pta_.markUnknown(field);
  else
    fieldStorage_.push_back(field);
}

void ExternalEffects::unknownMemory() {
  unknownMemory_ = true;
  if (callbackContexts_)
    pta_.havoc(*callbackContexts_);
}

void ExternalEffects::clobberFields() {
  // Byte ranges cannot be mapped to field nodes yet. Fields are merged across
  // instances, so conservatively retain unknown contents for every field.
  // Runtime memory models run after imports end; no new fields can arrive.
  if (fieldsClobbered_)
    return;
  fieldsClobbered_ = true;
  for (const auto node : fieldStorage_)
    pta_.markUnknown(node);
  std::vector<Andersen::Node>().swap(fieldStorage_);
}

void ExternalEffects::writeObject(Andersen::Node pointer) {
  const auto written = pta_.addNode();
  pta_.markUnknown(written);
  pta_.store(pointer, written);
}

void ExternalEffects::writeMemory(Andersen::Node pointer) {
  writeObject(pointer);
  clobberFields();
}

void ExternalEffects::apply(ExternalCall site, ExternalModel model) {
  // Callback targets are checked at the pointer fixed point, before fallback.
  const auto modeling =
      ExternalModules::query(model, site.formats, true, site.managerOperation);
  site.externalEffects.effects |= modeling.summary.effects;
  const auto writes = ExternalModules::writtenArguments(model);
  for (std::size_t i = 0; i < site.args.size() && i < 32; ++i)
    if ((writes & (1u << i)) && site.pointerArgument(i))
      writeMemory(site.args[i].node);
  const auto alias = ExternalModules::returnedArgument(model);
  if (site.returnsPointer && alias >= 0 && site.pointerArgument(alias))
    pta_.copy(site.result, site.args[alias].node);
  if (model == ExternalModel::PointerStructure && site.receiver) {
    pta_.havoc(*site.receiver);
    clobberFields();
  }
  if (model == ExternalModel::FunctionTargetPointer ||
      model == ExternalModel::FunctionObjectPointer ||
      model == ExternalModel::InlineFunctionPointer ||
      model == ExternalModel::InlineFunctionCopy ||
      model == ExternalModel::InlineFunctionManager)
    applyFunctionObject(site, model);
  else if (ExternalModules::formatArgument(model) >= 0)
    applyFormatModel(site, model, modeling.summary);
  else
    applySpecialModel(site, model);
  if (model == ExternalModel::Once && site.pointerArgument(0))
    writeObject(site.args[0].node); // Typed control write, not byte havoc.
  if (model == ExternalModel::ExitRegistration && site.pointerArgument(1)) {
    if (!callbackContexts_) {
      callbackContexts_ = pta_.addNode();
      if (unknownMemory_)
        pta_.havoc(*callbackContexts_);
    }
    // on_exit/__cxa_atexit retain their void* context for future callbacks.
    // Keep its address precise; only an unknown execution weakens contents.
    pta_.copy(*callbackContexts_, site.args[1].node);
  }
}

void ExternalEffects::applyFormatModel(ExternalCall &site, ExternalModel model,
                                       ExternalSummary summary) {
  const auto formatIndex = ExternalModules::formatArgument(model);
  const auto format =
      static_cast<FormatEffect>((site.formats >> (2 * formatIndex)) & 3u);
  site.unresolved |= !summary.complete();
  const auto list = ExternalModules::variadicListArgument(model);
  if (list >= 0) {
    if (!site.pointerArgument(list)) {
      site.unresolved = true; // Unsupported va_list ABI, not an empty pack.
      return;
    }
    // The list cursor is consumed. For an unknown/%n format, count stores may
    // reach pointer arguments through its saved argument area. Havoc follows
    // those aliases; it does not invent calls or write unrelated globals.
    if (format != FormatEffect::OutputOnly) {
      pta_.havoc(site.args[list].node);
      clobberFields();
    }
    writeMemory(site.args[list].node);
  } else if (format != FormatEffect::OutputOnly) {
    for (std::size_t i = formatIndex + 1; i < site.args.size(); ++i)
      if (site.pointerArgument(i))
        writeMemory(site.args[i].node); // Possible %n destination.
  }
}

void ExternalEffects::applyHandlerModel(ExternalCall &site,
                                        ExternalModel model) {
  const bool terminate = model == ExternalModel::SetTerminateHandler ||
                         model == ExternalModel::GetTerminateHandler;
  auto &slot = handlers_[terminate ? 1 : 0];
  if (!slot) {
    slot = pta_.addNode();
    // Retain initial/runtime handlers and registrations outside our inputs.
    pta_.markUnknown(*slot);
  }
  pta_.copy(site.result, *slot);
  if (model == ExternalModel::SetNewHandler ||
      model == ExternalModel::SetTerminateHandler) {
    if (site.pointerArgument(0))
      pta_.copy(*slot, site.args[0].node);
    else
      site.unresolved = true;
  }
}

void ExternalEffects::allocateResult(ExternalCall &site, ExternalModel model) {
  pta_.address(
      site.result,
      pta_.addNode()); // Allocation-site abstraction; null adds no target.
  if (model == ExternalModel::CAllocate ||
      model == ExternalModel::DefaultAllocate ||
      model == ExternalModel::DelegatingAllocate ||
      model == ExternalModel::StringCreate ||
      model == ExternalModel::DuplicateString ||
      model == ExternalModel::DirectoryOpen ||
      model == ExternalModel::OpenStream)
    writeMemory(site.result);
  if (model == ExternalModel::StringCreate) {
    if (site.pointerArgument(0))
      writeObject(site.args[0].node); // Capacity is a typed size_t reference.
    else
      site.unresolved = true;
  }
  if (model == ExternalModel::Reallocate && site.pointerArgument(0)) {
    pta_.copy(
        site.result,
        site.args[0].node); // Includes in-place success and failure retention.
    const auto copied = pta_.addNode();
    pta_.load(copied, site.args[0].node);
    pta_.store(site.result, copied);
    writeMemory(
        site.result); // Unknown size, partial contents and new tail bytes.
  }
  if (model == ExternalModel::MapMemory) {
    if (site.pointerArgument(0))
      pta_.copy(site.result, site.args[0].node); // Fixed-address alias.
    pta_.markUnknown(site.result); // Unknown placement and MAP_FAILED.
    writeMemory(site.result);      // File/device contents or replaced mappings.
  }
}

void ExternalEffects::applyMemoryCopyModel(ExternalCall &site,
                                           ExternalModel model) {
  if (!site.pointerArgument(0) || !site.pointerArgument(1))
    return;
  const auto copied = pta_.addNode();
  pta_.load(copied, site.args[1].node);
  pta_.store(site.args[0].node, copied);
  pta_.copy(site.result, site.args[0].node);
  writeMemory(site.args[0].node);
  if (model == ExternalModel::CopyInterior)
    pta_.markUnknown(site.result);
}

void ExternalEffects::applyTokenizeModel(ExternalCall &site) {
  if (!site.pointerArgument(0) || !site.pointerArgument(2)) {
    site.unresolved = true;
    return;
  }
  const auto buffer = pta_.addNode();
  pta_.copy(buffer, site.args[0].node);
  pta_.load(buffer, site.args[2].node); // Continuation when the input is null.
  writeMemory(buffer); // Delimiter bytes can be replaced by NUL.
  pta_.copy(site.result, buffer);
  // Return and saved cursor can have different interior offsets. Their may
  // target sets overlap; this does not assert that their concrete values agree.
  pta_.markUnknown(site.result);
  pta_.store(site.args[2].node, site.result);
}

void ExternalEffects::applyOutputModel(ExternalCall &site,
                                       ExternalModel model) {
  switch (model) {
  case ExternalModel::PasswordLookup:
    if (site.pointerArgument(1) && site.pointerArgument(4))
      // Success stores the caller's record, failure can store null. Keep the
      // positive alias even when the separate NSS effect needs fallback.
      pta_.store(site.args[4].node, site.args[1].node);
    else
      site.unresolved = true;
    break;
  case ExternalModel::OutAllocate:
    if (site.pointerArgument(0)) {
      const auto allocated = pta_.addNode();
      pta_.address(allocated, pta_.addNode());
      writeMemory(allocated);
      pta_.store(site.args[0].node,
                 allocated); // Failure retains the old output value.
    } else
      site.unresolved = true;
    break;
  case ExternalModel::NumericConversion:
    if (site.pointerArgument(0) && site.pointerArgument(1)) {
      const auto end = pta_.addNode();
      pta_.copy(end, site.args[0].node);
      pta_.markUnknown(end); // End pointer may address any interior byte.
      pta_.store(site.args[1].node, end);
    }
    break;
  case ExternalModel::OverflowArithmetic:
  case ExternalModel::MathOutput: {
    const auto index = model == ExternalModel::MathOutput ? 1u : 2u;
    if (site.pointerArgument(index))
      writeObject(
          site.args[index].node); // Typed scalar output, not byte writes.
    else
      site.unresolved = true;
    break;
  }
  default:
    break;
  }
}

void ExternalEffects::updateReceiverPointers(ExternalCall &site) {
  if (!site.receiver || !site.pointerArgument(0)) {
    site.unresolved = true;
    return;
  }
  // Stream-buffer setters retain supplied base/current/end pointers. They
  // update the receiver only; the pointed-to character buffer is unchanged.
  const auto contents = pta_.addNode();
  for (const auto &argument : site.args)
    if (argument.kind != ParamKind::Other)
      pta_.copy(contents, argument.node);
  pta_.markUnknown(contents); // Interior addresses lack separate objects.
  pta_.store(*site.receiver, contents);
  clobberFields();
}

void ExternalEffects::applyFunctionObject(ExternalCall &site,
                                          ExternalModel model) {
  if (model == ExternalModel::FunctionTargetPointer ||
      model == ExternalModel::FunctionObjectPointer ||
      model == ExternalModel::InlineFunctionPointer) {
    const auto source =
        model == ExternalModel::FunctionTargetPointer
            ? site.receiver
            : (site.pointerArgument(0) ? std::optional{site.args[0].node}
                                       : std::nullopt);
    if (!source) {
      site.unresolved = true;
      return;
    }
    // The instantiated manager determines its storage. Only target<T>() still
    // needs both cases: its runtime callable type is not fixed here.
    if (model != ExternalModel::FunctionObjectPointer)
      pta_.copy(site.result, *source);
    if (model != ExternalModel::InlineFunctionPointer)
      pta_.load(site.result, *source);
    pta_.markUnknown(site.result);
    return;
  }
  const auto operation = model == ExternalModel::InlineFunctionCopy
                             ? FunctionManagerOperation::Clone
                             : site.managerOperation;
  if (operation == FunctionManagerOperation::Destroy)
    return; // The guarded inline functor has a trivial destructor.
  if (!site.pointerArgument(0) || !site.pointerArgument(1)) {
    site.unresolved = true;
    return;
  }
  if (operation != FunctionManagerOperation::TypeInfo) {
    const auto contents = pta_.addNode();
    if (operation != FunctionManagerOperation::Pointer)
      pta_.load(contents, site.args[1].node); // Trivial callable copy.
    if (operation != FunctionManagerOperation::Clone)
      pta_.copy(contents, site.args[1].node); // __get_functor_ptr.
    pta_.store(site.args[0].node, contents);
  }
  // Type-info addresses, byte layout and interior fields remain unknown. Only
  // destination storage is written; the source's pointees are not modified.
  writeMemory(site.args[0].node);
}

void ExternalEffects::applySpecialModel(ExternalCall &site,
                                        ExternalModel model) {
  switch (model) {
  case ExternalModel::ReceiverPointerUpdate:
    updateReceiverPointers(site);
    break;
  case ExternalModel::ForwardReference:
  case ExternalModel::AddressOf:
  case ExternalModel::PointerIdentity:
    if (site.pointerArgument(0))
      pta_.copy(site.result, site.args[0].node);
    else {
      site.unresolved = true;
      pta_.markUnknown(site.result);
    }
    break;
  case ExternalModel::Allocate:
  case ExternalModel::DefaultAllocate:
  case ExternalModel::DelegatingAllocate:
  case ExternalModel::StringCreate:
  case ExternalModel::MapMemory:
  case ExternalModel::Reallocate:
  case ExternalModel::StackAllocate:
  case ExternalModel::CAllocate:
  case ExternalModel::DuplicateString:
  case ExternalModel::OpenStream:
  case ExternalModel::DirectoryOpen:
    allocateResult(site, model);
    break;
  case ExternalModel::ReturnBuffer0:
  case ExternalModel::ReturnBuffer1:
    if (site.returnsPointer) {
      pta_.address(site.result,
                   pta_.addNode()); // GNU null-buffer allocation variants.
      pta_.markUnknown(
          site.result); // Static storage and interior/record addresses.
    }
    break;
  case ExternalModel::ExternalPointer:
  case ExternalModel::StaticCategory:
  case ExternalModel::StaticBuffer:
  case ExternalModel::PointerStructure:
  case ExternalModel::Search:
    if (site.returnsPointer)
      pta_.markUnknown(site.result);
    break;
  case ExternalModel::FindMemory:
  case ExternalModel::DirectoryRead:
    if (site.pointerArgument(0))
      pta_.copy(site.result, site.args[0].node);
    // Retain the base alias, but do not pretend every interior byte address
    // denotes the base object: offsets/subobjects are not modeled by this PTA.
    pta_.markUnknown(site.result);
    break;
  case ExternalModel::OutAllocate:
  case ExternalModel::PasswordLookup:
  case ExternalModel::NumericConversion:
  case ExternalModel::OverflowArithmetic:
  case ExternalModel::MathOutput:
    applyOutputModel(site, model);
    break;
  case ExternalModel::Tokenize:
    applyTokenizeModel(site);
    break;
  case ExternalModel::SetNewHandler:
  case ExternalModel::GetNewHandler:
  case ExternalModel::SetTerminateHandler:
  case ExternalModel::GetTerminateHandler:
    applyHandlerModel(site, model);
    break;
  case ExternalModel::StringMutate:
  case ExternalModel::StringErase:
  case ExternalModel::StringSwap:
  case ExternalModel::StringReturnSelf:
    applyStringModel(site, model);
    break;
  case ExternalModel::CopyMemory:
  case ExternalModel::CopyInterior:
    applyMemoryCopyModel(site, model);
    break;
  case ExternalModel::SetMemory:
    if (site.pointerArgument(0)) {
      pta_.copy(site.result, site.args[0].node);
      writeMemory(site.args[0].node);
    }
    // Arbitrary byte fills do not establish a null pointer representation.
    break;
  default:
    break; // Other models use the shared summary, write mask and return alias.
  }
}

void ExternalEffects::applyStringModel(ExternalCall &site,
                                       ExternalModel model) {
  if (!site.receiver) {
    site.unresolved = true;
    return;
  }
  pta_.havoc(*site.receiver);
  clobberFields();
  if (model == ExternalModel::StringReturnSelf)
    pta_.copy(site.result, *site.receiver);
  // Character storage can be exposed by data()/iterators. Its layout is not
  // reconstructed here, so a write retains an unknown-address store boundary.
  const auto buffer = pta_.addNode();
  pta_.markUnknown(buffer);
  writeMemory(buffer);
}

} // namespace wpgkit
