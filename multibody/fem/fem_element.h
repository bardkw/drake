#pragma once

#include <array>
#include <string>
#include <utility>

#include "drake/common/eigen_types.h"
#include "drake/multibody/fem/constitutive_model.h"
#include "drake/multibody/fem/damping_model.h"
#include "drake/multibody/fem/fem_indexes.h"
#include "drake/multibody/fem/fem_state.h"

namespace drake {
namespace multibody {
namespace fem {
namespace internal {

// TODO(xuchenhan-tri) Document the definition of quantities like "natural
//  dimension". See issue #14475.

/* Traits class for FemElement(see below). Specializations of concrete traits
 classes must at least define the fields listed in the example below.

   template <>
   struct FemElementTraits<ExampleElement> {
     // Define the scalar type.
     using T = ...;
     // Define element data for the concrete element.
     struct Data {
       // The Data class must be default constructible.
       Data() {...}
       ...
     };
     // The number of quadrature points in each element.
     static constexpr int num_quadrature_points = ...;
     // The natural dimension of the element.
     static constexpr int num_natural_dimension = ...;
     // The number of nodes in the element.
     static constexpr int num_nodes = ...;
     // The number of degrees of freedom in the element.
     static constexpr int num_dofs = ...;
     // The constitutive model used in the element.
     using ConstitutiveModel = ...;
   };

 @tparam Element The concrete FEM element that inherits from FemElement
 through CRTP. */
template <class Element>
struct FemElementTraits {};

/* FemElement is the base class for spatially discretized FEM elements for
 dynamic elasticity problems. It computes quantities such as the residual and
 the tangent matrix on a single FEM element given the data needed for the
 element's computation. These quantities are then assembled into their global
 counterparts by FemModel. The per-element, state-dependent data used in the
 computation of `DerivedElement` should be declared in the traits, along with
 the other responsibilities of the traits class detailed above.

 Since FEM elements are usually evaluated in computationally intensive inner
 loops of the simulation, the overhead caused by virtual methods and heap
 allocations may be significant. Therefore, this class uses CRTP to achieve
 compile-time polymorphism and avoids the overhead of virtual methods and
 facilitates inlining instead. The type information at compile time also helps
 eliminate all heap allocations. Derived FEM elements must inherit from this
 base class and implement the interface this class provides. The derived FEM
 elements must also be accompanied by a corresponding traits class that
 specializes FemElementTraits (see above).

 @tparam DerivedElement The concrete FEM element that inherits from FemElement
 through CRTP. */
template <class DerivedElement>
class FemElement {
 public:
  using Traits = FemElementTraits<DerivedElement>;
  using T = typename Traits::T;
  using Data = typename Traits::Data;
  using ConstitutiveModel = typename Traits::ConstitutiveModel;
  static constexpr int num_dofs = Traits::num_dofs;
  static constexpr int num_nodes = Traits::num_nodes;
  static constexpr int num_quadrature_points = Traits::num_quadrature_points;

  /* Indices of the nodes of this element within the model. */
  const std::array<FemNodeIndex, num_nodes>& node_indices() const {
    return node_indices_;
  }

  /* The FemElementIndex of this element within the model. */
  FemElementIndex element_index() const { return element_index_; }

  /* Computes the per-element, state-dependent data associated with this
   `DerivedElement` given the `state`. */
  Data ComputeData(const FemState<T>& state) const {
    return static_cast<const DerivedElement*>(this)->DoComputeData(state);
  }

  /* Calculates the tangent matrix for the element by combining the stiffness
   matrix, damping matrix, and the mass matrix according to the given `weights`.
   In particular, given a weight of (w₀, w₁, w₂), the tangent matrix is equal to
   w₀⋅K + w₁⋅D + w₂⋅M, where K, D, and M are stiffness, damping, and mass matrix
   respectively. */
  void CalcTangentMatrix(
      const Data& data, const Vector3<T>& weights,
      EigenPtr<Eigen::Matrix<T, num_dofs, num_dofs>> tangent_matrix) const {
    DRAKE_DEMAND(tangent_matrix != nullptr);
    tangent_matrix->setZero();
    AddScaledStiffnessMatrix(
        data, weights(0) + weights(1) * damping_model_.stiffness_coeff_beta(),
        tangent_matrix);
    AddScaledMassMatrix(
        data, weights(2) + weights(1) * damping_model_.mass_coeff_alpha(),
        tangent_matrix);
  }

  /* Calculates the element residual of this element evaluated at the input
   state. The residual equals Ma-fₑ(x)-fᵥ(x, v)-fₑₓₜ, where M is the mass
   matrix, fₑ(x) is the elastic force, fᵥ(x, v) is the damping force, and fₑₓₜ
   is the external force on the nodes of the element. Notice that the residual
   is "discrete" in space and "continuous" in time.
   @param[in]  data      The per-element FEM data to evaluate the residual.
   @param[out] residual  The residual vector. All values in `residual` will be
                         overwritten.
   @pre residual != nullptr */
  void CalcResidual(const Data& data,
                    EigenPtr<Vector<T, num_dofs>> residual) const {
    DRAKE_ASSERT(residual != nullptr);
    residual->setZero();
    static_cast<const DerivedElement*>(this)->DoCalcResidual(data, residual);
  }

  /* Accumulates the stiffness matrix (the derivative, or an approximation
   thereof, of the residual with respect to the generalized positions) of this
   element given the `state`.
   @param[in] data    The per-element FEM data to evaluate the stiffness matrix.
   @param[in] scale   The scaling factor applied to the stiffness matrix.
   @param[in, out] K  The matrix to which the scaled stiffness matrix will be
                      added.
   @pre K != nullptr */
  void AddScaledStiffnessMatrix(
      const Data& data, const T& scale,
      EigenPtr<Eigen::Matrix<T, num_dofs, num_dofs>> K) const {
    DRAKE_ASSERT(K != nullptr);
    static_cast<const DerivedElement*>(this)->DoAddScaledStiffnessMatrix(
        data, scale, K);
  }

  /* Accumulates the damping matrix (the derivative of the residual with
   respect to the time derivative of generalized positions) of this element
   given the `data`.
   @param[in] data    The per-element FEM data to evaluate the damping matrix.
   @param[in] scale   The scaling factor applied to the damping matrix.
   @param[in, out] D  The matrix to which the scaled damping matrix will be
                      added.
   @pre D != nullptr
   @note This function recomputes both the mass and the stiffness matrix and may
   be expensive. */
  void AddScaledDampingMatrix(
      const Data& data, const T& scale,
      EigenPtr<Eigen::Matrix<T, num_dofs, num_dofs>> D) const {
    DRAKE_ASSERT(D != nullptr);
    const T& alpha = damping_model_.mass_coeff_alpha();
    const T& beta = damping_model_.stiffness_coeff_beta();
    this->AddScaledMassMatrix(data, scale * alpha, D);
    this->AddScaledStiffnessMatrix(data, scale * beta, D);
  }

  /* Accumulates the mass matrix (the derivative of the residual with respect
   to the time second derivative of generalized positions) of this element
   given the `data`.
   @param[in] data  The FEM data to evaluate the mass matrix.
   @param[in] scale  The scaling factor applied to the mass matrix.
   @param[in, out] M  The matrix to which the scaled mass matrix will be added.
   @pre M != nullptr */
  void AddScaledMassMatrix(
      const Data& data, const T& scale,
      EigenPtr<Eigen::Matrix<T, num_dofs, num_dofs>> M) const {
    DRAKE_ASSERT(M != nullptr);
    static_cast<const DerivedElement*>(this)->DoAddScaledMassMatrix(data, scale,
                                                                    M);
  }

  /* Accumulates the total external force exerted on this element at the given
   `data` scaled by `scale` into the output parameter `external_force`.
   @pre external_force != nullptr. */
  void AddScaledExternalForce(
      const Data& data, const T& scale,
      EigenPtr<Vector<T, num_dofs>> external_force) const {
    DRAKE_ASSERT(external_force != nullptr);
    // The gravity force is always accounted for in the external forces.
    AddScaledGravityForce(data, scale, external_force);
    // Add element specific external forces.
    DoAddScaledExternalForce(data, scale, external_force);
  }

  /* Extracts the dofs corresponding to the nodes given by `node_indices` from
   the given `state_dofs`. */
  static Vector<T, 3 * num_nodes> ExtractElementDofs(
      const std::array<FemNodeIndex, num_nodes>& node_indices,
      const VectorX<T>& state_dofs) {
    constexpr int kDim = 3;
    Vector<T, kDim * num_nodes> element_dofs;
    for (int i = 0; i < num_nodes; ++i) {
      DRAKE_ASSERT((node_indices[i] + 1) * kDim <= state_dofs.size());
      element_dofs.template segment<kDim>(i * kDim) =
          state_dofs.template segment<kDim>(node_indices[i] * kDim);
    }
    return element_dofs;
  }

  /* Extracts the dofs corresponding to the nodes in this element from the given
   `state_dofs`. */
  Vector<T, 3 * num_nodes> ExtractElementDofs(
      const VectorX<T>& state_dofs) const {
    return ExtractElementDofs(this->node_indices(), state_dofs);
  }

  void set_gravity_vector(const Vector3<T>& gravity) { gravity_ = gravity; }

  const Vector3<T>& gravity_vector() const { return gravity_; }

 protected:
  /* Assignment and copy constructions are prohibited.
   Move constructor is allowed so that FemElement can be stored in
   `std::vector`. */
  FemElement(const FemElement&) = delete;
  FemElement(FemElement&&) = default;
  const FemElement& operator=(const FemElement&) = delete;
  FemElement&& operator=(const FemElement&&) = delete;

  /* Constructs a new FEM element. The constructor is made protected because
   FemElement should not be constructed directly. Use the constructor of the
   derived classes instead.
   @param[in] element_index  The index of the new element within the model.
   @param[in] node_indices   The node indices of the nodes of this element
                             within the model.
   @pre element_index is valid.
   @pre Entries in node_indices are valid. */
  FemElement(FemElementIndex element_index,
             const std::array<FemNodeIndex, num_nodes>& node_indices,
             ConstitutiveModel constitutive_model,
             DampingModel<T> damping_model)
      : element_index_(element_index),
        node_indices_(node_indices),
        constitutive_model_(std::move(constitutive_model)),
        damping_model_(std::move(damping_model)) {
    DRAKE_ASSERT(element_index.is_valid());
    for (int i = 0; i < num_nodes; ++i) {
      DRAKE_ASSERT(node_indices[i].is_valid());
    }
  }

  /* `DerivedElement` must provide an implementation for `DoComputeData()`.
   @throw std::exception if `DerivedElement` does not provide an
   implementation for `DoComputeData()`. */
  Data DoComputeData(const FemState<T>& state) const {
    ThrowIfNotImplemented(__func__);
  }

  /* `DerivedElement` must provide an implementation for `DoCalcResidual()` to
   provide the residual that is up to date given the `data`. The caller
   guarantees that `residual` is non-null and contains all zeros; the
   implementation in the derived class does not have to test for this.
   @throw std::exception if `DerivedElement` does not provide an implementation
   for `DoCalcResidual()`. */
  void DoCalcResidual(const Data& data,
                      EigenPtr<Vector<T, num_dofs>> residual) const {
    ThrowIfNotImplemented(__func__);
  }

  /* `DerivedElement` must provide an implementation for
   `DoAddScaledStiffnessMatrix()` to provide the stiffness matrix that is up to
   date given the `data`. The caller guarantees that `K` is non-null; the
   implementation in the derived class does not have to test for this.
   @throw std::exception if `DerivedElement` does not provide an implementation
   for `DoAddScaledStiffnessMatrix()`. */
  void DoAddScaledStiffnessMatrix(
      const Data& data, const T& scale,
      EigenPtr<Eigen::Matrix<T, num_dofs, num_dofs>> K) const {
    ThrowIfNotImplemented(__func__);
  }

  /* `DerivedElement` must provide an implementation for
   `DoAddScaledMassMatrix()` to provide the mass matrix that is up-to-date given
   the `data`. The caller guarantees that `M` is non-null; the implementation
   in the derived class does not have to test for this.
   @throw std::exception if `DerivedElement` does not provide an implementation
   for `DoAddScaledMassMatrix()`. */
  void DoAddScaledMassMatrix(
      const Data& data, const T& scale,
      EigenPtr<Eigen::Matrix<T, num_dofs, num_dofs>> M) const {
    ThrowIfNotImplemented(__func__);
  }

  /* `DerivedElement` may override this method to include _non-gravity_
   external forces specific to the derived element. Default implementation is
   no-op. */
  void DoAddScaledExternalForce(
      const Data& data, const T& scale,
      EigenPtr<Vector<T, num_dofs>> external_force) const {}

  /* Adds the gravity force acting on each node in the element scaled by
   `scale` into `force`. Derived elements may choose to override this method
   to provide a more efficient implementation for specific elements. */
  void AddScaledGravityForce(const Data& data, const T& scale,
                             EigenPtr<Vector<T, num_dofs>> force) const {
    Eigen::Matrix<T, num_dofs, num_dofs> mass_matrix =
        Eigen::Matrix<T, num_dofs, num_dofs>::Zero();
    AddScaledMassMatrix(data, 1, &mass_matrix);
    constexpr int kDim = 3;
    Vector<T, num_dofs> stacked_gravity;
    for (int i = 0; i < num_nodes; ++i) {
      stacked_gravity.template segment<kDim>(kDim * i) = gravity_;
    }
    *force += scale * mass_matrix * stacked_gravity;
  }

  const ConstitutiveModel& constitutive_model() const {
    return constitutive_model_;
  }

  const DampingModel<T>& damping_model() const { return damping_model_; }

 private:
  /* Helper to throw a descriptive exception when a given function is not
   implemented. */
  void ThrowIfNotImplemented(const char* source_method) const {
    throw std::runtime_error("The DerivedElement from " +
                             NiceTypeName::Get(*this) +
                             " must provide an implementation for " +
                             std::string(source_method) + "().");
  }

  /* The index of this element within the model. */
  FemElementIndex element_index_;
  /* The node indices of this element within the model. */
  std::array<FemNodeIndex, num_nodes> node_indices_;
  /* The constitutive model that describes the stress-strain relationship
   for this element. */
  ConstitutiveModel constitutive_model_;
  DampingModel<T> damping_model_;
  /* Gravity vector. */
  Vector3<T> gravity_{0, 0, -9.81};
};

}  // namespace internal
}  // namespace fem
}  // namespace multibody
}  // namespace drake
