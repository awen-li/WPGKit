#pragma once

#include <cstdint>
#include <initializer_list>

namespace wpgkit {

enum class ParamKind { Other, Pointer, ReferenceToPointer, Reference };

// Numeric pointer-value ID; Other has no pointer node. Shared by actual/formal
// binding and runtime effects, without exposing a solver or compiler type.
struct CallArgument final {
  std::uint32_t node;
  ParamKind kind;
};

// May-effects: in particular, NoReturn means a call may fail to return. It
// never justifies removing a normal return edge from an unknown call.
enum class CallEffect : std::uint32_t {
  ReadArguments = 1 << 0,
  WriteArguments = 1 << 1,
  ReadGlobals = 1 << 2,
  WriteGlobals = 1 << 3,
  Escape = 1 << 4,
  Callback = 1 << 5,
  Allocate = 1 << 6,
  EndLifetime = 1 << 7,
  Throw = 1 << 8,
  NoReturn = 1 << 9,
  InputToReturn = 1 << 10,
  InputToMemory = 1 << 11,
  Unknown = 1 << 12,
  // Standard stdio output/state, including the stream's buffer. This does not
  // mean arbitrary writes through the other pointer arguments or to globals.
  WriteStream = 1 << 13,
  WriteErrno = 1 << 14,
  BeginLifetime = 1 << 15,
  ReadExternalState = 1 << 16,
  WriteExternalState = 1 << 17,
  FloatingEnvironment = 1 << 18,
  Synchronize = 1 << 19
};

struct ExternalSummary final {
  std::uint32_t effects = 0;
  bool operator==(const ExternalSummary &) const = default;

  bool complete() const {
    return !has(CallEffect::Unknown);
  }

  bool has(CallEffect effect) const {
    return (effects & static_cast<std::uint32_t>(effect)) != 0;
  }

  static ExternalSummary unknown() {
    using E = CallEffect;
    ExternalSummary result;
    for (const auto effect : {E::ReadArguments,
                              E::WriteArguments,
                              E::ReadGlobals,
                              E::WriteGlobals,
                              E::Escape,
                              E::Callback,
                              E::Allocate,
                              E::EndLifetime,
                              E::Throw,
                              E::NoReturn,
                              E::InputToReturn,
                              E::InputToMemory,
                              E::Unknown,
                              E::WriteStream,
                              E::WriteErrno,
                              E::BeginLifetime,
                              E::ReadExternalState,
                              E::WriteExternalState,
                              E::FloatingEnvironment,
                              E::Synchronize})
      result.effects |= static_cast<std::uint32_t>(effect);
    return result;
  }
};

} // namespace wpgkit
