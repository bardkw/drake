#pragma once

#include <string>
#include <vector>

#include <Eigen/Dense>
#include <tinyxml2.h>

#include "drake/math/rigid_transform.h"

namespace drake {
namespace multibody {
namespace internal {

// Parses a string containing double values in the XML attribute format into a
// vector of doubles.
std::vector<double> ConvertToDoubles(const std::string& str);

// Parses a string attribute of @p node named @p attribute_name into @p val.
// If the attribute is not present, @p val will be cleared.
//
// @returns false if the attribute is not present
bool ParseStringAttribute(const tinyxml2::XMLElement* node,
                          const char* attribute_name, std::string* val);

// Parses a scalar attribute of @p node named @p attribute_name into @p val.
//
// @returns false if the attribute is not present
//
// @throws std::exception if the attribute doesn't contain a
// single numeric value.
bool ParseScalarAttribute(const tinyxml2::XMLElement* node,
                          const char* attribute_name, double* val);

// Parses an attribute of @p node named @p attribute_name consisting of scalar
// values into @p val.
//
// @returns false if the attribute is not present, and the output is left
// untouched.
// @tparam rows is indicates the size of the vector `val`.
//
// @throws std::exception if the attribute doesn't contain `rows` numeric
// values.
template <int rows>
bool ParseVectorAttribute(const tinyxml2::XMLElement* node,
                          const char* attribute_name,
                          Eigen::Matrix<double, rows, 1>* val) {
  const char* attr = node->Attribute(attribute_name);
  if (attr) {
    std::vector<double> vals = ConvertToDoubles(attr);
    if (vals.size() != rows) {
      throw std::invalid_argument(
          fmt::format("Expected {} values for attribute {} got {}", rows,
                      attribute_name, attr));
    }
    *val = Eigen::Matrix<double, rows, 1>(vals.data());
    return true;
  }
  return false;
}

// Parses "xyz" and "rpy" attributes from @p node and returns a
// RigidTransformd created from them.  If either the "xyz" or "rpy"
// attributes are omitted they will be initialized with zero values.
//
// @throws std::exception if the "xyz" or "rpy" attributes are
// malformed.
math::RigidTransformd OriginAttributesToTransform(
    const tinyxml2::XMLElement* node);

// Parses a three vector value from parameter @p node, which is an
// XML node. The value is specified by an attribute within the XML
// whose name is specified by parameter @p attribute_name.  This
// method also supports a three vector specified by a single scalar
// value, which it automatically converts into a three vector by
// using the same scalar value for all three dimensions.
//
// @param[in] node A pointer to the XML element node that contains an
// attribute with a three vector or a scalar value.
// @param[in] attribute_name The name of the attribute containing the three
// vector or scalar value.
// @param[out] val The three vector where the results should be stored.
//
// @returns false if the attribute is not present
//
// @throws std::exception If any problem is encountered parsing the
// three vector value.
bool ParseThreeVectorAttribute(const tinyxml2::XMLElement* node,
                               const char* attribute_name,
                               Eigen::Vector3d* val);

}  // namespace internal
}  // namespace multibody
}  // namespace drake
