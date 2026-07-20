#ifndef UNIJIT_RUNTIME_MATERIALIZATION_H
#define UNIJIT_RUNTIME_MATERIALIZATION_H

#include <cstddef>
#include <cstdint>
#include <vector>

#include "unijit/runtime/deoptimization.h"

namespace unijit::runtime {

using ObjectHandle = std::uint64_t;

enum class MaterializationSource : std::uint8_t {
  kRecoveredSlot = 0,
  kConstant,
  kObject,
};

struct MaterializationInput final {
  MaterializationSource source{MaterializationSource::kConstant};
  ir::ValueType type{ir::ValueType::kWord};
  std::size_t recovered_slot{0};
  ir::Word constant{0};
  std::size_t object_id{0};

  static MaterializationInput recovered(std::size_t slot,
                                        ir::ValueType type) noexcept;
  static MaterializationInput constant_value(ir::ValueType type,
                                             ir::Word value) noexcept;
  static MaterializationInput object(std::size_t object_id) noexcept;
};

struct ObjectRecipe final {
  std::size_t id{0};
  std::size_t destination_slot{0};
  std::uint64_t kind{0};
  std::vector<MaterializationInput> fields;
};

class MaterializationPlan final {
 public:
  static constexpr std::size_t kMaximumObjects = 4096;
  static constexpr std::size_t kMaximumFields = 256U * 1024U;

  MaterializationPlan(std::size_t site, std::size_t resume_offset) noexcept
      : site_(site), resume_offset_(resume_offset) {}

  Status add(const ObjectRecipe& recipe);
  Status validate() const;

  bool empty() const noexcept { return recipes_.empty(); }
  std::size_t size() const noexcept { return recipes_.size(); }
  std::size_t site() const noexcept { return site_; }
  std::size_t resume_offset() const noexcept { return resume_offset_; }
  const std::vector<ObjectRecipe>& recipes() const noexcept {
    return recipes_;
  }

 private:
  std::size_t site_{0};
  std::size_t resume_offset_{0};
  std::vector<ObjectRecipe> recipes_;
};

struct MaterializationCallbacks final {
  void* state{nullptr};
  Status (*begin)(void* state, DeoptimizationReason reason, std::size_t site,
                  std::size_t resume_offset, std::size_t object_count,
                  std::size_t frame_value_count) noexcept = nullptr;
  Status (*allocate)(void* state, std::size_t object_id, std::uint64_t kind,
                     std::size_t field_count,
                     ObjectHandle* handle) noexcept = nullptr;
  Status (*store_primitive)(void* state, ObjectHandle object,
                            std::size_t field_index, ir::ValueType type,
                            ir::Word value) noexcept = nullptr;
  Status (*store_object)(void* state, ObjectHandle object,
                         std::size_t field_index,
                         ObjectHandle value) noexcept = nullptr;
  Status (*store_frame_primitive)(void* state, std::size_t slot,
                                  ir::ValueType type,
                                  ir::Word value) noexcept = nullptr;
  Status (*store_frame_object)(void* state, std::size_t slot,
                               ObjectHandle value) noexcept = nullptr;
  Status (*commit)(void* state) noexcept = nullptr;
  void (*rollback)(void* state) noexcept = nullptr;
};

enum class MaterializedValueKind : std::uint8_t {
  kWord = 0,
  kFloat64,
  kObject,
};

struct MaterializedValue final {
  std::size_t slot{0};
  MaterializedValueKind kind{MaterializedValueKind::kWord};
  ir::Word value{0};
  ObjectHandle object{0};
};

struct MaterializedFrame final {
  DeoptimizationReason reason{DeoptimizationReason::kGuardFailed};
  std::size_t site{0};
  std::size_t resume_offset{0};
  std::vector<MaterializedValue> values;

  const MaterializedValue* find(std::size_t slot) const noexcept;
};

struct MaterializationResult final {
  Status status;
  MaterializedFrame frame;

  bool ok() const noexcept { return status.ok(); }
};

MaterializationResult materialize_frame(
    const ReconstructedFrame& frame, const MaterializationPlan& plan,
    const MaterializationCallbacks& callbacks);

}  // namespace unijit::runtime

#endif  // UNIJIT_RUNTIME_MATERIALIZATION_H
