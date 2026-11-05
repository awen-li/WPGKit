#pragma once

#include <compare>
#include <cstdint>

namespace wpgkit {

// Numeric IDs are owned values, scoped to one analysis session. Zero is
// invalid. The owning session supplies uniqueness; IDs do not encode a session
// token. The tag prevents accidental comparisons between different identity
// domains.
template <typename Tag> class Id final {
public:
  constexpr Id() = default;

  explicit constexpr Id(std::uint64_t value) : value_(value) {}

  [[nodiscard]] constexpr std::uint64_t value() const noexcept {
    return value_;
  }

  [[nodiscard]] constexpr bool isValid() const noexcept {
    return value_ != 0;
  }

  auto operator<=>(const Id &) const = default;

private:
  std::uint64_t value_ = 0;
};

using FunctionId = Id<class FunctionIdTag>;

class PointId final {
public:
  PointId(FunctionId function, std::uint64_t position)
      : function_(function), position_(position) {}

  [[nodiscard]] FunctionId function() const noexcept {
    return function_;
  }

  [[nodiscard]] std::uint64_t position() const noexcept {
    return position_;
  }

  [[nodiscard]] bool isValid() const noexcept {
    return function_.isValid();
  }

  auto operator<=>(const PointId &) const = default;

private:
  FunctionId function_;
  std::uint64_t position_;
};

} // namespace wpgkit
