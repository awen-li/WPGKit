#pragma once

namespace wpgkit {

// Fixed when a session is created, before any facts are extracted.
struct AnalysisOptions final {
  // Distinguish record fields and constant array elements by base object.
  // Otherwise all subobjects share their containing object's storage.
  bool fieldSensitive = false;
};

} // namespace wpgkit
