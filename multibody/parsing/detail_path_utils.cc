#include "drake/multibody/parsing/detail_path_utils.h"

#include <optional>
#include <regex>
#include <string>
#include <vector>

#include "drake/common/drake_assert.h"
#include "drake/common/filesystem.h"
#include "drake/common/never_destroyed.h"
#include "drake/common/text_logging.h"

namespace drake {
namespace multibody {
namespace internal {

using std::string;

namespace {

// Searches for key package in package_map. If the key exists, returns the
// associated value; else prints a warning and returns nullopt.
std::optional<string> GetPackagePath(
    const string& package, const PackageMap& package_map) {
  if (package_map.Contains(package)) {
    return package_map.GetPath(package);
  } else {
    drake::log()->warn("Couldn't find package '{}' in the supplied package"
                       "path: {}", package, package_map);
    return std::nullopt;
  }
}
}  // namespace

string ResolveUri(const string& uri, const PackageMap& package_map,
                  const string& root_dir) {
  filesystem::path result;

  // Parse the given URI into pieces.
  static const never_destroyed<std::regex> uri_matcher{
      "^([a-z0-9+.-]+)://([^/]*)/+(.*)"};
  std::smatch match;
  if (std::regex_match(uri, match, uri_matcher.access())) {
    // The `uri` was actually a URI (not a bare filename).
    DRAKE_DEMAND(match.size() == 4);
    const auto& uri_scheme = match[1];
    const auto& uri_package = match[2];
    const auto& uri_path = match[3];
    if (uri_scheme == "file") {
      result = "/" + uri_path.str();
    } else if ((uri_scheme == "model") || (uri_scheme == "package")) {
      std::optional<string> package_path =
          GetPackagePath(uri_package, package_map);
      if (!package_path) { return {}; }
      result = filesystem::path(*package_path) / std::string(uri_path);
    } else {
      drake::log()->warn(
          "URI '{}' specifies an unsupported scheme; supported schemes are "
          "'file://', 'model://', and 'package://'.", uri);
      return {};
    }
  } else {
    if (root_dir.empty()) {
      drake::log()->warn(
          "URI '{}' is invalid when parsing a string instead of a filename.",
          uri);
      return {};
    }
    // Strictly speaking a URI should not just be a bare filename, but we allow
    // this for backward compatibility and user convenience.
    const string& filename = uri;
    if (filesystem::path(filename).is_absolute()) {
      result = filename;
    } else if (filesystem::path(root_dir).is_absolute()) {
      result = root_dir;
      result.append(filename);
    } else {
      result = filesystem::current_path() / root_dir / filename;
    }
  }

  result = result.lexically_normal();

  if (!filesystem::exists(result)) {
    drake::log()->warn("URI '{}' resolved to '{}' which could not be found.",
                       uri, result.string());
    return {};
  }

  return result.string();
}

}  // namespace internal
}  // namespace multibody
}  // namespace drake
