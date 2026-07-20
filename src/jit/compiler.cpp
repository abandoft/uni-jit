#include "unijit/jit/compiler.h"

#include <cstring>
#include <memory>
#include <new>
#include <utility>

#include "jit/executable_memory.h"

#if defined(UNIJIT_TARGET_AARCH64)
#include "jit/backend/aarch64/lower.h"
#endif

namespace unijit::jit {

struct CompiledFunction::Impl final {
  using Entry = ir::Word (*)(const ir::Word*);

  detail::ExecutableMemory memory;

  Entry entry() const noexcept {
    static_assert(sizeof(Entry) == sizeof(void*),
                  "native entry and data pointers must have equal size");
    Entry result = nullptr;
    void* address = memory.address();
    std::memcpy(&result, &address, sizeof(result));
    return result;
  }
};

CompiledFunction::CompiledFunction(std::unique_ptr<Impl> impl,
                                   std::size_t parameter_count,
                                   CompilationStats stats) noexcept
    : impl_(std::move(impl)),
      parameter_count_(parameter_count),
      stats_(stats) {}

CompiledFunction::~CompiledFunction() = default;
CompiledFunction::CompiledFunction(CompiledFunction&&) noexcept = default;
CompiledFunction& CompiledFunction::operator=(CompiledFunction&&) noexcept =
    default;

ir::EvaluationResult CompiledFunction::invoke(const ir::Word* args,
                                              std::size_t arg_count) const {
  if (arg_count != parameter_count_) {
    return {{StatusCode::kInvalidArgument,
             "argument count does not match compiled function signature"},
            0};
  }
  if (arg_count != 0 && args == nullptr) {
    return {{StatusCode::kInvalidArgument,
             "argument storage is null for a non-empty signature"},
            0};
  }
  if (impl_ == nullptr || impl_->entry() == nullptr) {
    return {{StatusCode::kCodeGenerationFailed,
             "compiled function has no published entry point"},
            0};
  }
  return {Status::ok_status(), impl_->entry()(args)};
}

CompilationResult Compiler::compile(const ir::Function& function) {
  const Status verification = ir::verify(function);
  if (!verification.ok()) {
    return {verification, nullptr};
  }

#if defined(UNIJIT_TARGET_AARCH64)
  detail::aarch64::LoweringResult lowering =
      detail::aarch64::lower(function);
  if (!lowering.status.ok()) {
    return {lowering.status, nullptr};
  }

  try {
    auto implementation = std::make_unique<CompiledFunction::Impl>();
    const Status publication = detail::ExecutableMemory::publish(
        lowering.code.data(), lowering.code.size(), &implementation->memory);
    if (!publication.ok()) {
      return {publication, nullptr};
    }

    CompilationStats stats{lowering.code.size(), lowering.spill_slots};
    auto compiled = std::unique_ptr<CompiledFunction>(new CompiledFunction(
        std::move(implementation), function.parameter_count(), stats));
    return {Status::ok_status(), std::move(compiled)};
  } catch (const std::bad_alloc&) {
    return {{StatusCode::kResourceExhausted,
             "unable to allocate compiled-function metadata"},
            nullptr};
  }
#else
  (void)function;
  return {{StatusCode::kUnsupportedArchitecture,
           "UniJIT has no native backend for this architecture yet"},
          nullptr};
#endif
}

}  // namespace unijit::jit
