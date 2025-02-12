#include "drake/multibody/parsing/detail_tinyxml2_diagnostic.h"

#include "drake/common/drake_assert.h"

namespace drake {
namespace multibody {
namespace internal {

using drake::internal::DiagnosticDetail;
using tinyxml2::XMLNode;

TinyXml2Diagnostic::TinyXml2Diagnostic(
    const drake::internal::DiagnosticPolicy* diagnostic,
    const DataSource* data_source,
    const std::string& file_extension)
    : diagnostic_(diagnostic), data_source_(data_source),
      file_extension_(file_extension) {
  DRAKE_DEMAND(diagnostic != nullptr);
  DRAKE_DEMAND(data_source != nullptr);
}

DiagnosticDetail TinyXml2Diagnostic::MakeDetail(
    const XMLNode& location, const std::string& message) const {
  DiagnosticDetail detail;
  if (data_source_->IsFilename()) {
    detail.filename = data_source_->GetAbsolutePath();
  } else {
    detail.filename = data_source_->GetStem() + "." + file_extension_;
  }
  detail.line = location.GetLineNum();
  detail.message = message;
  return detail;
}

void TinyXml2Diagnostic::Warning(
    const XMLNode& location, const std::string& message) const {
  diagnostic_->Warning(MakeDetail(location, message));
}

void TinyXml2Diagnostic::Error(
    const XMLNode& location, const std::string& message) const {
  diagnostic_->Error(MakeDetail(location, message));
}

}  // namespace internal
}  // namespace multibody
}  // namespace drake
