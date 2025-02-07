#include "drake/multibody/parsing/detail_urdf_parser.h"

#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <variant>

#include <Eigen/Dense>
#include <fmt/format.h>
#include <tinyxml2.h>

#include "drake/common/sorted_pair.h"
#include "drake/math/rotation_matrix.h"
#include "drake/multibody/parsing/detail_path_utils.h"
#include "drake/multibody/parsing/detail_tinyxml.h"
#include "drake/multibody/parsing/detail_tinyxml2_diagnostic.h"
#include "drake/multibody/parsing/detail_urdf_geometry.h"
#include "drake/multibody/parsing/package_map.h"
#include "drake/multibody/parsing/scoped_names.h"
#include "drake/multibody/plant/multibody_plant.h"
#include "drake/multibody/tree/ball_rpy_joint.h"
#include "drake/multibody/tree/fixed_offset_frame.h"
#include "drake/multibody/tree/planar_joint.h"
#include "drake/multibody/tree/prismatic_joint.h"
#include "drake/multibody/tree/revolute_joint.h"
#include "drake/multibody/tree/universal_joint.h"
#include "drake/multibody/tree/weld_joint.h"

namespace drake {
namespace multibody {
namespace internal {

using Eigen::Matrix3d;
using Eigen::Vector3d;
using Eigen::Vector4d;
using math::RigidTransformd;
using tinyxml2::XMLNode;
using tinyxml2::XMLDocument;
using tinyxml2::XMLElement;

namespace {

// Helper class to share infrastructure among parsing methods.
class UrdfParser {
 public:
  // Note that @p data_source, @p xml_doc, and @p w are aliased for the
  // lifetime of this object. Their lifetimes must exceed that of the created
  // object.
  UrdfParser(
      const DataSource* data_source,
      const std::string& model_name,
      const std::optional<std::string>& parent_model_name,
      const std::string& root_dir,
      XMLDocument* xml_doc,
      const ParsingWorkspace& w)
      : model_name_(model_name),
        parent_model_name_(parent_model_name),
        root_dir_(root_dir),
        xml_doc_(xml_doc),
        w_(w),
        diagnostic_(&w.diagnostic, data_source) {
    DRAKE_DEMAND(data_source != nullptr);
    DRAKE_DEMAND(xml_doc != nullptr);
  }

  // @return a model instance index, if one was created during parsing.
  // @throw std::exception on parse error.
  // @note: see AddModelFromUrdf for a full account of diagnostics, error
  // reporting, and return values.
  std::optional<ModelInstanceIndex> Parse();
  // TODO(rpoyner-tri): assimilate all the remaining file-private parsing
  // functions in this file into methods of this class, to properly plumb the
  // error/warning channels.

  void Warning(const XMLNode& location, std::string message) const {
    diagnostic_.Warning(location, std::move(message));
  }

  void Error(const XMLNode& location, std::string message) const {
    diagnostic_.Error(location, std::move(message));
  }

 private:
  const std::string model_name_;
  const std::optional<std::string> parent_model_name_;
  const std::string root_dir_;
  XMLDocument* const xml_doc_;
  const ParsingWorkspace& w_;
  TinyXml2Diagnostic diagnostic_;

  ModelInstanceIndex model_instance_{};
};

const char* kWorldName = "world";

SpatialInertia<double> ExtractSpatialInertiaAboutBoExpressedInB(
    XMLElement* node) {
  RigidTransformd X_BBi;

  XMLElement* origin = node->FirstChildElement("origin");
  if (origin) {
    X_BBi = OriginAttributesToTransform(origin);
  }

  double body_mass = 0;
  XMLElement* mass = node->FirstChildElement("mass");
  if (mass) {
    ParseScalarAttribute(mass, "value", &body_mass);
  }

  double ixx = 0;
  double ixy = 0;
  double ixz = 0;
  double iyy = 0;
  double iyz = 0;
  double izz = 0;

  XMLElement* inertia = node->FirstChildElement("inertia");
  if (inertia) {
    ParseScalarAttribute(inertia, "ixx", &ixx);
    ParseScalarAttribute(inertia, "ixy", &ixy);
    ParseScalarAttribute(inertia, "ixz", &ixz);
    ParseScalarAttribute(inertia, "iyy", &iyy);
    ParseScalarAttribute(inertia, "iyz", &iyz);
    ParseScalarAttribute(inertia, "izz", &izz);
  }

  const RotationalInertia<double> I_BBcm_Bi(ixx, iyy, izz, ixy, ixz, iyz);

  // If this is a massless body, return a zero SpatialInertia.
  if (body_mass == 0. && I_BBcm_Bi.get_moments().isZero() &&
      I_BBcm_Bi.get_products().isZero()) {
    return SpatialInertia<double>(body_mass, {0., 0., 0.}, {0., 0., 0});
  }
  // B and Bi are not necessarily aligned.
  const math::RotationMatrix<double> R_BBi(X_BBi.rotation());

  // Re-express in frame B as needed.
  const RotationalInertia<double> I_BBcm_B = I_BBcm_Bi.ReExpress(R_BBi);

  // Bi's origin is at the COM as documented in
  // http://wiki.ros.org/urdf/XML/link#Elements
  const Vector3d p_BoBcm_B = X_BBi.translation();

  return SpatialInertia<double>::MakeFromCentralInertia(
      body_mass, p_BoBcm_B, I_BBcm_B);
}

void ParseBody(const multibody::PackageMap& package_map,
               const std::string& root_dir,
               ModelInstanceIndex model_instance,
               XMLElement* node,
               MaterialMap* materials,
               MultibodyPlant<double>* plant) {
  std::string drake_ignore;
  if (ParseStringAttribute(node, "drake_ignore", &drake_ignore) &&
      drake_ignore == std::string("true")) {
    return;
  }

  std::string body_name;
  if (!ParseStringAttribute(node, "name", &body_name)) {
    throw std::runtime_error(
        "ERROR: link tag is missing name attribute.");
  }

  const RigidBody<double>* body_pointer{};
  if (body_name == kWorldName) {
    // TODO(SeanCurtis-TRI): We have no documentation about what our parsers
    //  support and not. The fact that we allow this behavior should be
    //  discoverable *somewhere*. But this function is in an anonymous
    //  namespace; where would the documentation go that supports this
    //  implementation?
    body_pointer = &plant->world_body();
    if (node->FirstChildElement("inertial") != nullptr) {
      // TODO(SeanCurtis-TRI): It would be good to report file name and line
      //  number in this error.
      static const logging::Warn log_once(
          "A URDF file declared the \"world\" link and then attempted to "
          "assign mass properties (via the <inertial> tag). Only geometries, "
          "<collision> and <visual>, can be assigned to the world link. The "
          "<inertial> tag is being ignored.");
    }
  } else {
    SpatialInertia<double> M_BBo_B;
    XMLElement* inertial_node = node->FirstChildElement("inertial");
    if (!inertial_node) {
      M_BBo_B = SpatialInertia<double>(0, Vector3d::Zero(),
                                       UnitInertia<double>(0, 0, 0));
    } else {
      M_BBo_B = ExtractSpatialInertiaAboutBoExpressedInB(inertial_node);
    }

    // Add a rigid body to model each link.
    body_pointer = &plant->AddRigidBody(body_name, model_instance, M_BBo_B);
  }

  if (plant->geometry_source_is_registered()) {
    const RigidBody<double>& body = *body_pointer;
    std::unordered_set<std::string> geometry_names;

    for (XMLElement* visual_node = node->FirstChildElement("visual");
         visual_node;
         visual_node = visual_node->NextSiblingElement("visual")) {
      geometry::GeometryInstance geometry_instance =
          ParseVisual(body_name, package_map, root_dir, visual_node, materials,
                      &geometry_names);
      // The parsing should *always* produce an IllustrationProperties
      // instance, even if it is empty.
      DRAKE_DEMAND(geometry_instance.illustration_properties() != nullptr);
      plant->RegisterVisualGeometry(
          body, geometry_instance.pose(), geometry_instance.shape(),
          geometry_instance.name(),
          *geometry_instance.illustration_properties());
    }

    for (XMLElement* collision_node = node->FirstChildElement("collision");
         collision_node;
         collision_node = collision_node->NextSiblingElement("collision")) {
      geometry::GeometryInstance geometry_instance =
          ParseCollision(body_name, package_map, root_dir, collision_node,
          &geometry_names);
      DRAKE_DEMAND(geometry_instance.proximity_properties() != nullptr);
      plant->RegisterCollisionGeometry(
          body, geometry_instance.pose(), geometry_instance.shape(),
          geometry_instance.name(),
          std::move(*geometry_instance.mutable_proximity_properties()));
    }
  }
}

void ParseCollisionFilterGroup(ModelInstanceIndex model_instance,
                               XMLElement* node,
                               MultibodyPlant<double>* plant) {
  auto next_child_element = [](const ElementNode& data_element,
                               const char* element_name) {
    return std::get<tinyxml2::XMLElement*>(data_element)
        ->FirstChildElement(element_name);
  };
  auto next_sibling_element = [](const ElementNode& data_element,
                                 const char* element_name) {
    return std::get<tinyxml2::XMLElement*>(data_element)
        ->NextSiblingElement(element_name);
  };
  auto has_attribute = [](const ElementNode& data_element,
                          const char* attribute_name) {
    std::string attribute_value;
    return ParseStringAttribute(std::get<tinyxml2::XMLElement*>(data_element),
                                attribute_name, &attribute_value);
  };
  auto get_string_attribute = [](const ElementNode& data_element,
                                 const char* attribute_name) {
    std::string attribute_value;
    if (!ParseStringAttribute(std::get<tinyxml2::XMLElement*>(data_element),
                              attribute_name, &attribute_value)) {
      throw std::runtime_error(fmt::format(
          "{}:{}:{} The tag <{}> does not specify the required attribute"
          " \"{}\" at line {}.", __FILE__, __func__, __LINE__,
          std::get<tinyxml2::XMLElement*>(data_element)->Value(),
          attribute_name,
          std::get<tinyxml2::XMLElement*>(data_element)->GetLineNum()));
    }
    return attribute_value;
  };
  auto get_bool_attribute = [](const ElementNode& data_element,
                               const char* attribute_name) {
    std::string attribute_value;
    ParseStringAttribute(std::get<tinyxml2::XMLElement*>(data_element),
                         attribute_name, &attribute_value);
    return attribute_value == std::string("true") ? true : false;
  };
  ParseCollisionFilterGroupCommon(model_instance, node, plant,
                                  next_child_element, next_sibling_element,
                                  has_attribute, get_string_attribute,
                                  get_bool_attribute, get_string_attribute);
}

// Parses a joint URDF specification to obtain the names of the joint, parent
// link, child link, and the joint type. An exception is thrown if any of these
// names cannot be determined.
//
// @param[in] node The XML node parsing the URDF joint description.
// @param[out] name A reference to a string where the name of the joint
// should be saved.
// @param[out] type A reference to a string where the joint type should be
// saved.
// @param[out] parent_link_name A reference to a string where the name of the
// parent link should be saved.
// @param[out] child_link_name A reference to a string where the name of the
// child link should be saved.
void ParseJointKeyParams(XMLElement* node,
                         std::string* name,
                         std::string* type,
                         std::string* parent_link_name,
                         std::string* child_link_name) {
  if (!ParseStringAttribute(node, "name", name)) {
    throw std::runtime_error(
        "ERROR: joint tag is missing name attribute");
  }

  if (!ParseStringAttribute(node, "type", type)) {
    throw std::runtime_error(
        "ERROR: joint " + *name + " is missing type attribute");
  }

  // Obtains the name of the joint's parent link.
  XMLElement* parent_node = node->FirstChildElement("parent");
  if (!parent_node) {
    throw std::runtime_error(
        "ERROR: joint " + *name + " doesn't have a parent node!");
  }
  if (!ParseStringAttribute(parent_node, "link", parent_link_name)) {
    throw std::runtime_error(
        "ERROR: joint " + *name + "'s parent does not have a link attribute!");
  }

  // Obtains the name of the joint's child link.
  XMLElement* child_node = node->FirstChildElement("child");
  if (!child_node) {
    throw std::runtime_error(
        "ERROR: joint " + *name + " doesn't have a child node");
  }
  if (!ParseStringAttribute(child_node, "link", child_link_name)) {
    throw std::runtime_error(
        "ERROR: joint " + *name + "'s child does not have a link attribute!");
  }
}

void ParseJointLimits(XMLElement* node, double* lower, double* upper,
                      double* velocity, double* acceleration, double* effort) {
  *lower = -std::numeric_limits<double>::infinity();
  *upper = std::numeric_limits<double>::infinity();
  *velocity = std::numeric_limits<double>::infinity();
  *acceleration = std::numeric_limits<double>::infinity();
  *effort = std::numeric_limits<double>::infinity();

  XMLElement* limit_node = node->FirstChildElement("limit");
  if (limit_node) {
    ParseScalarAttribute(limit_node, "lower", lower);
    ParseScalarAttribute(limit_node, "upper", upper);
    ParseScalarAttribute(limit_node, "velocity", velocity);
    ParseScalarAttribute(limit_node, "drake:acceleration", acceleration);
    ParseScalarAttribute(limit_node, "effort", effort);
  }
}

void ParseJointDynamics(XMLElement* node, double* damping) {
  *damping = 0.0;
  double coulomb_friction = 0.0;
  double coulomb_window = std::numeric_limits<double>::epsilon();

  XMLElement* dynamics_node = node->FirstChildElement("dynamics");
  if (dynamics_node) {
    ParseScalarAttribute(dynamics_node, "damping", damping);
    if (ParseScalarAttribute(dynamics_node, "friction", &coulomb_friction) &&
        coulomb_friction != 0.0) {
      static const logging::Warn log_once(
          "At least one of your URDF files has specified a non-zero value for "
          "the 'friction' attribute of a joint/dynamics tag. MultibodyPlant "
          "does not currently support non-zero joint friction.");
    }
    if (ParseScalarAttribute(dynamics_node, "coulomb_window",
                             &coulomb_window) &&
        coulomb_window != std::numeric_limits<double>::epsilon()) {
      static const logging::Warn log_once(
          "At least one of your URDF files has specified a value for  the "
          "'coulomb_window' attribute of a <joint> tag. Drake no longer makes "
          "use of that attribute and all instances will be ignored.");
    }
  }
}

const Body<double>& GetBodyForElement(
    const std::string& element_name,
    const std::string& link_name,
    ModelInstanceIndex model_instance,
    MultibodyPlant<double>* plant) {
  if (link_name == kWorldName) {
    return plant->world_body();
  }

  if (!plant->HasBodyNamed(link_name, model_instance)) {
    throw std::runtime_error(
        "ERROR: Could not find link named\"" +
        link_name + "\" with model instance ID " +
        std::to_string(model_instance) + " for element " + element_name + ".");
  }
  return plant->GetBodyByName(link_name, model_instance);
}

void ParseJoint(ModelInstanceIndex model_instance,
                std::map<std::string, double>* joint_effort_limits,
                XMLElement* node,
                MultibodyPlant<double>* plant) {
  std::string drake_ignore;
  if (ParseStringAttribute(node, "drake_ignore", &drake_ignore) &&
      drake_ignore == std::string("true")) {
    return;
  }

  // Parses the parent and child link names.
  std::string name, type, parent_name, child_name;
  ParseJointKeyParams(node, &name, &type, &parent_name, &child_name);

  const Body<double>& parent_body = GetBodyForElement(
      name, parent_name, model_instance, plant);
  const Body<double>& child_body = GetBodyForElement(
      name, child_name, model_instance, plant);

  RigidTransformd X_PJ;
  XMLElement* origin = node->FirstChildElement("origin");
  if (origin) {
    X_PJ = OriginAttributesToTransform(origin);
  }

  Vector3d axis(1, 0, 0);
  XMLElement* axis_node = node->FirstChildElement("axis");
  if (axis_node && type.compare("fixed") != 0 &&
      type.compare("floating") != 0 && type.compare("ball") != 0) {
    ParseVectorAttribute(axis_node, "xyz", &axis);
    if (axis.norm() < 1e-8) {
      throw std::runtime_error(
          "ERROR: Joint " + name + "axis is zero.  Don't do that.");
    }
    axis.normalize();
  }

  // Joint properties -- these are only used by some joint types.

  // Dynamic properties
  double damping = 0;

  // Limits
  double upper = std::numeric_limits<double>::infinity();
  double lower = -std::numeric_limits<double>::infinity();
  double velocity = std::numeric_limits<double>::infinity();
  double acceleration = std::numeric_limits<double>::infinity();

  // In MultibodyPlant, the effort limit is a property of the actuator, which
  // isn't created until the transmission element is parsed.  Stash a value
  // for all joints when parsing the joint element so that we can look it up
  // later if/when an actuator is created.
  double effort = std::numeric_limits<double>::infinity();

  auto throw_on_custom_joint = [node, name, type](bool want_custom_joint) {
    const std::string node_name(node->Name());
    const bool is_custom_joint = node_name == "drake:joint";
    if (want_custom_joint && !is_custom_joint) {
      throw std::runtime_error(
          "ERROR: Joint " + name + " of type " + type +
          " is a custom joint type, and should be a <drake:joint>");
    } else if (!want_custom_joint && is_custom_joint) {
      throw std::runtime_error(
          "ERROR: Joint " + name + " of type " + type +
          " is a standard joint type, and should be a <joint>");
    }
  };

  if (type.compare("revolute") == 0 || type.compare("continuous") == 0) {
    throw_on_custom_joint(false);
    ParseJointLimits(node, &lower, &upper, &velocity, &acceleration, &effort);
    ParseJointDynamics(node, &damping);
    const JointIndex index = plant->AddJoint<RevoluteJoint>(
        name, parent_body, X_PJ,
        child_body, std::nullopt, axis, lower, upper, damping).index();
    Joint<double>& joint = plant->get_mutable_joint(index);
    joint.set_velocity_limits(Vector1d(-velocity), Vector1d(velocity));
    joint.set_acceleration_limits(
        Vector1d(-acceleration), Vector1d(acceleration));
  } else if (type.compare("fixed") == 0) {
    throw_on_custom_joint(false);
    plant->AddJoint<WeldJoint>(name, parent_body, X_PJ,
                               child_body, std::nullopt,
                               RigidTransformd::Identity());
  } else if (type.compare("prismatic") == 0) {
    throw_on_custom_joint(false);
    ParseJointLimits(node, &lower, &upper, &velocity, &acceleration, &effort);
    ParseJointDynamics(node, &damping);
    const JointIndex index = plant->AddJoint<PrismaticJoint>(
        name, parent_body, X_PJ,
        child_body, std::nullopt, axis, lower, upper, damping).index();
    Joint<double>& joint = plant->get_mutable_joint(index);
    joint.set_velocity_limits(Vector1d(-velocity), Vector1d(velocity));
    joint.set_acceleration_limits(
        Vector1d(-acceleration), Vector1d(acceleration));
  } else if (type.compare("floating") == 0) {
    throw_on_custom_joint(false);
    drake::log()->warn("Joint {} specified as type floating which is not "
                       "supported by MultibodyPlant.  Leaving {} as a "
                       "free body.", name, child_name);
  } else if (type.compare("ball") == 0) {
    throw_on_custom_joint(true);
    ParseJointDynamics(node, &damping);
    plant->AddJoint<BallRpyJoint>(name, parent_body, X_PJ,
                                  child_body, std::nullopt, damping);
  } else if (type.compare("planar") == 0) {
    throw_on_custom_joint(true);
    Vector3d damping_vec(0, 0, 0);
    XMLElement* dynamics_node = node->FirstChildElement("dynamics");
    if (dynamics_node) {
      ParseVectorAttribute(dynamics_node, "damping", &damping_vec);
    }
    plant->AddJoint<PlanarJoint>(name, parent_body, X_PJ,
                                 child_body, std::nullopt, damping_vec);
  } else if (type.compare("universal") == 0) {
    throw_on_custom_joint(true);
    ParseJointDynamics(node, &damping);
    plant->AddJoint<UniversalJoint>(name, parent_body, X_PJ,
                                    child_body, std::nullopt, damping);
  } else {
    throw std::runtime_error(
        "ERROR: Joint " + name + " has unrecognized type: " + type);
  }

  joint_effort_limits->emplace(name, effort);
}

void ParseTransmission(
    ModelInstanceIndex model_instance,
    const std::map<std::string, double>& joint_effort_limits,
    XMLElement* node,
    MultibodyPlant<double>* plant) {
  // Determines the transmission type.
  std::string type;
  XMLElement* type_node = node->FirstChildElement("type");
  if (type_node) {
    type = type_node->GetText();
  } else {
    // Old URDF format, kept for convenience
    if (!ParseStringAttribute(node, "type", &type)) {
      throw std::runtime_error(
          std::string(__FILE__) + ": " + __func__ +
          "ERROR: Transmission element is missing a type.");
    }
  }

  // Checks if the transmission type is not SimpleTransmission. If it is not,
  // print a warning and then abort this method call since only simple
  // transmissions are supported at this time.
  if (type.find("SimpleTransmission") == std::string::npos) {
    static const logging::Warn log_once(
        "At least one of your URDF files has <transmission> type that isn't "
        "'SimpleTransmission'. Drake only supports 'SimpleTransmission'; all "
        "other transmission types will be ignored.");
    return;
  }

  // Determines the actuator's name.
  XMLElement* actuator_node = node->FirstChildElement("actuator");
  if (!actuator_node) {
    throw std::runtime_error(
        "ERROR: Transmission is missing an actuator element.");
  }

  std::string actuator_name;
  if (!ParseStringAttribute(actuator_node, "name", &actuator_name)) {
    throw std::runtime_error(
        "ERROR: Transmission is missing an actuator name.");
  }

  // Determines the name of the joint to which the actuator is attached.
  XMLElement* joint_node = node->FirstChildElement("joint");
  if (!joint_node) {
    throw std::runtime_error(
        "ERROR: Transmission is missing a joint element.");
  }

  std::string joint_name;
  if (!ParseStringAttribute(joint_node, "name", &joint_name)) {
    throw std::runtime_error(
        "ERROR: Transmission is missing a joint name.");
  }

  if (!plant->HasJointNamed(joint_name, model_instance)) {
    throw std::runtime_error(
        "ERROR: Transmission specifies joint " + joint_name +
        " which does not exist.");
  }
  const Joint<double>& joint = plant->GetJointByName(
      joint_name, model_instance);

  // Checks if the actuator is attached to a fixed joint. If so, abort this
  // method call.
  if (joint.num_positions() == 0) {
    drake::log()->warn(
        "WARNING: Skipping transmission since it's attached to "
        "a fixed joint \"" + joint_name + "\".");
    return;
  }

  const auto effort_iter = joint_effort_limits.find(joint_name);
  DRAKE_DEMAND(effort_iter != joint_effort_limits.end());
  if (effort_iter->second < 0) {
    throw std::runtime_error(
        "ERROR: Transmission specifies joint " + joint_name +
        " which has a negative effort limit.");
  }

  if (effort_iter->second <= 0) {
    drake::log()->warn(
        "WARNING: Skipping transmission since it's attached to "
        "joint \"" + joint_name + "\" which has a zero "
        "effort limit {}.", effort_iter->second);
    return;
  }

  const JointActuator<double>& actuator =
      plant->AddJointActuator(actuator_name, joint, effort_iter->second);

  // Parse and add the optional drake:rotor_inertia parameter.
  XMLElement* rotor_inertia_node =
      actuator_node->FirstChildElement("drake:rotor_inertia");
  if (rotor_inertia_node) {
    double rotor_inertia;
    if (!ParseScalarAttribute(rotor_inertia_node, "value", &rotor_inertia)) {
      throw std::runtime_error(
          "ERROR: joint actuator " + actuator_name +
          "'s drake:rotor_inertia does not have a \"value\" attribute!");
    }
    plant->get_mutable_joint_actuator(actuator.index())
        .set_default_rotor_inertia(rotor_inertia);
  }

  // Parse and add the optional drake:gear_ratio parameter.
  XMLElement* gear_ratio_node =
      actuator_node->FirstChildElement("drake:gear_ratio");
  if (gear_ratio_node) {
    double gear_ratio;
    if (!ParseScalarAttribute(gear_ratio_node, "value", &gear_ratio)) {
      throw std::runtime_error(
          "ERROR: joint actuator " + actuator_name +
          "'s drake:gear_ratio does not have a \"value\" attribute!");
    }
    plant->get_mutable_joint_actuator(actuator.index())
        .set_default_gear_ratio(gear_ratio);
  }
}

void ParseFrame(ModelInstanceIndex model_instance,
                XMLElement* node,
                MultibodyPlant<double>* plant) {
  std::string name;
  if (!ParseStringAttribute(node, "name", &name)) {
    throw std::runtime_error("ERROR parsing frame name.");
  }

  std::string body_name;
  if (!ParseStringAttribute(node, "link", &body_name)) {
    throw std::runtime_error(
        "ERROR: missing link name for frame " + name + ".");
  }

  const Body<double>& body =
      GetBodyForElement(name, body_name, model_instance, plant);

  RigidTransformd X_BF = OriginAttributesToTransform(node);
  plant->AddFrame(std::make_unique<FixedOffsetFrame<double>>(
      name, body.body_frame(), X_BF));
}

void ParseBushing(ModelInstanceIndex model_instance,
                  XMLElement* node,
                  MultibodyPlant<double>* plant) {
  // Functor to read a child element with a vector valued `value` attribute
  // Throws an error if unable to find the tag or if the value attribute is
  // improperly formed.
  auto read_vector = [node](const char* element_name) -> Eigen::Vector3d {
    const XMLElement* value_node = node->FirstChildElement(element_name);
    if (value_node != nullptr) {
      Eigen::Vector3d value;
      if (ParseVectorAttribute(value_node, "value", &value)) {
        return value;
      } else {
        throw std::runtime_error(
            fmt::format("Unable to read the 'value' attribute for the <{}> "
                        "tag on line {}",
                        element_name, value_node->GetLineNum()));
      }
    } else {
      throw std::runtime_error(
          fmt::format("Unable to find the <{}> "
                      "tag on line {}",
                      element_name, node->GetLineNum()));
    }
  };

  // Functor to read a child element with a string valued `name` attribute.
  // Throws an error if unable to find the tag of if the name attribute is
  // improperly formed.
  auto read_frame = [model_instance, node, plant]
                    (const char* element_name) -> const Frame<double>* {
    XMLElement* value_node = node->FirstChildElement(element_name);

    if (value_node != nullptr) {
      std::string frame_name;
      if (ParseStringAttribute(value_node, "name", &frame_name)) {
        if (!plant->HasFrameNamed(frame_name, model_instance)) {
          throw std::runtime_error(fmt::format(
              "Frame: {} specified for <{}> does not exist in the model.",
              frame_name, element_name));
        }
        return &plant->GetFrameByName(frame_name, model_instance);

      } else {
        throw std::runtime_error(
            fmt::format("Unable to read the 'name' attribute for the <{}> "
                        "tag on line {}",
                        element_name, value_node->GetLineNum()));
      }

    } else {
      throw std::runtime_error(
          fmt::format("Unable to find the <{}> tag on line {}", element_name,
                      node->GetLineNum()));
    }
  };

  ParseLinearBushingRollPitchYaw(read_vector, read_frame, plant);
}

std::optional<ModelInstanceIndex> UrdfParser::Parse() {
  XMLElement* node = xml_doc_->FirstChildElement("robot");
  if (!node) {
    Error(*xml_doc_, "URDF does not contain a robot tag.");
    return {};
  }

  std::string model_name = model_name_;
  if (model_name.empty() && !ParseStringAttribute(node, "name", &model_name)) {
    Error(*node, "Your robot must have a name attribute or a model name "
          "must be specified.");
    return {};
  }

  model_name = parsing::PrefixName(
      parent_model_name_.value_or(""), model_name);

  model_instance_ = w_.plant->AddModelInstance(model_name);

  // Parses the model's material elements. Throws an exception if there's a
  // material name clash regardless of whether the associated RGBA values are
  // the same.
  MaterialMap materials;
  for (XMLElement* material_node = node->FirstChildElement("material");
       material_node;
       material_node = material_node->NextSiblingElement("material")) {
    ParseMaterial(material_node, true /* name_required */,
                  w_.package_map, root_dir_, &materials);
  }

  // Parses the model's link elements.
  for (XMLElement* link_node = node->FirstChildElement("link");
       link_node;
       link_node = link_node->NextSiblingElement("link")) {
    ParseBody(w_.package_map, root_dir_, model_instance_, link_node,
              &materials, w_.plant);
  }

  // Parses the collision filter groups only if the scene graph is registered.
  if (w_.plant->geometry_source_is_registered()) {
    ParseCollisionFilterGroup(model_instance_, node, w_.plant);
  }

  // Joint effort limits are stored with joints, but used when creating the
  // actuator (which is done when parsing the transmission).
  std::map<std::string, double> joint_effort_limits;

  // Parses the model's joint elements.  While it may not be strictly required
  // to be true in MultibodyPlant, generally the JointIndex for any given
  // joint follows the declaration order in the model (and users probably
  // should avoid caring about the ordering of JointIndex), we still parse the
  // joints in model order regardless of the element type so that the results
  // are consistent with an SDF version of the same model.
  for (XMLElement* joint_node = node->FirstChildElement(); joint_node;
       joint_node = joint_node->NextSiblingElement()) {
    const std::string node_name(joint_node->Name());
    if (node_name == "joint" || node_name == "drake:joint") {
      ParseJoint(model_instance_, &joint_effort_limits, joint_node, w_.plant);
    }
  }

  // Parses the model's transmission elements.
  for (XMLElement* transmission_node = node->FirstChildElement("transmission");
       transmission_node;
       transmission_node =
           transmission_node->NextSiblingElement("transmission")) {
    ParseTransmission(model_instance_, joint_effort_limits,
                      transmission_node, w_.plant);
  }

  if (node->FirstChildElement("loop_joint")) {
    Error(*node, "loop joints are not supported in MultibodyPlant");
    return model_instance_;
  }

  // Parses the model's Drake frame elements.
  for (XMLElement* frame_node = node->FirstChildElement("frame"); frame_node;
       frame_node = frame_node->NextSiblingElement("frame")) {
    ParseFrame(model_instance_, frame_node, w_.plant);
  }

  // Parses the model's custom Drake bushing tags.
  for (XMLElement* bushing_node =
           node->FirstChildElement("drake:linear_bushing_rpy");
       bushing_node; bushing_node = bushing_node->NextSiblingElement(
                         "drake:linear_bushing_rpy")) {
    ParseBushing(model_instance_, bushing_node, w_.plant);
  }

  return model_instance_;
}

}  // namespace

std::optional<ModelInstanceIndex> AddModelFromUrdf(
    const DataSource& data_source,
    const std::string& model_name_in,
    const std::optional<std::string>& parent_model_name,
    const ParsingWorkspace& workspace) {
  MultibodyPlant<double>* plant = workspace.plant;
  DRAKE_THROW_UNLESS(plant != nullptr);
  DRAKE_THROW_UNLESS(!plant->is_finalized());
  TinyXml2Diagnostic diag(&workspace.diagnostic, &data_source);

  // Opens the URDF file and feeds it into the XML parser.
  XMLDocument xml_doc;
  if (data_source.IsFilename()) {
    xml_doc.LoadFile(data_source.filename().c_str());
    if (xml_doc.ErrorID()) {
      diag.Error(xml_doc, fmt::format("Failed to parse XML file: {}",
                                      xml_doc.ErrorName()));
      return std::nullopt;
    }
  } else {
    xml_doc.Parse(data_source.contents().c_str());
    if (xml_doc.ErrorID()) {
      diag.Error(xml_doc, fmt::format("Failed to parse XML string: {}",
                                      xml_doc.ErrorName()));
      return std::nullopt;
    }
  }

  UrdfParser parser(&data_source, model_name_in, parent_model_name,
                    data_source.GetRootDir(), &xml_doc, workspace);
  return parser.Parse();
}

}  // namespace internal
}  // namespace multibody
}  // namespace drake
