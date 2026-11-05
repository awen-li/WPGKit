#pragma once

#include "Ids.h"
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace wpgkit {

using TextId = Id<class TextIdTag>;

struct SourceLocation final {
  TextId file;
  std::uint64_t offset = 0;
  auto operator<=>(const SourceLocation &) const = default;
};

struct CallLocation final {
  SourceLocation expansion;
  SourceLocation spelling;
  auto operator<=>(const CallLocation &) const = default;
};

// Own each distinct text once. Graph records hold IDs, never strings or views.
// Copies preserve IDs and own independent storage for result snapshots.
class GraphMetadata final {
public:
  GraphMetadata() = default;

  GraphMetadata(const GraphMetadata &other)
      : byText_(other.byText_), byId_(other.byId_.size()) {
    for (const auto &[text, id] : byText_)
      byId_[id.value() - 1] = &text;
  }

  GraphMetadata &operator=(const GraphMetadata &other) {
    if (this != &other) {
      GraphMetadata copy(other);
      byText_.swap(copy.byText_);
      byId_.swap(copy.byId_);
    }
    return *this;
  }

  GraphMetadata(GraphMetadata &&) noexcept = default;
  GraphMetadata &operator=(GraphMetadata &&) noexcept = default;

  TextId intern(std::string_view text) {
    if (text.empty())
      return {};
    if (const auto found = byText_.find(text); found != byText_.end())
      return found->second;
    const TextId id(byId_.size() + 1);
    const auto found = byText_.emplace(text, id).first;
    try {
      byId_.push_back(&found->first);
    } catch (...) {
      byText_.erase(found);
      throw;
    }
    return id;
  }

  [[nodiscard]] TextId find(std::string_view text) const {
    const auto found = byText_.find(text);
    return found == byText_.end() ? TextId{} : found->second;
  }

  [[nodiscard]] const std::string &text(TextId id) const {
    static const std::string unavailable;
    return id.isValid() ? *byId_.at(id.value() - 1) : unavailable;
  }

  [[nodiscard]] std::size_t size() const noexcept {
    return byId_.size();
  }

  [[nodiscard]] std::string format(CallLocation location) const {
    return text(location.expansion.file) + ":" +
           std::to_string(location.expansion.offset) + ":" +
           text(location.spelling.file) + ":" +
           std::to_string(location.spelling.offset);
  }

private:
  std::map<std::string, TextId, std::less<>> byText_;
  // Map keys have stable addresses and are never erased or changed.
  std::vector<const std::string *> byId_;
};

} // namespace wpgkit
