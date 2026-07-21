#include "unijit/jit/code_cache.h"

#include <algorithm>
#include <limits>
#include <mutex>
#include <new>
#include <unordered_map>
#include <utility>

namespace unijit::jit {
namespace {

ir::EvaluationResult invalid_handle() {
  return {{StatusCode::kInvalidArgument, "code handle is not valid"}, 0};
}

}  // namespace

std::size_t CodeHandle::parameter_count() const noexcept {
  return function_ == nullptr ? 0 : function_->parameter_count();
}

ir::ValueType CodeHandle::parameter_type(std::size_t index) const noexcept {
  return function_ == nullptr ? ir::ValueType::kWord
                              : function_->parameter_type(index);
}

ir::ValueType CodeHandle::return_type() const noexcept {
  return function_ == nullptr ? ir::ValueType::kWord
                              : function_->return_type();
}

const TargetProfile* CodeHandle::target_profile() const noexcept {
  return function_ == nullptr ? nullptr : &function_->target_profile();
}

std::uint64_t CodeHandle::target_profile_key() const noexcept {
  return function_ == nullptr ? 0 : function_->target_profile_key();
}

bool CodeHandle::requires_context() const noexcept {
  return function_ != nullptr && function_->requires_context();
}

std::size_t CodeHandle::assumption_count() const noexcept {
  return function_ == nullptr ? 0 : function_->assumptions().size();
}

bool CodeHandle::assumptions_valid() const noexcept {
  return function_ != nullptr &&
         function_->assumptions().first_invalid() == nullptr;
}

const CompilationStats* CodeHandle::compilation_stats() const noexcept {
  return function_ == nullptr ? nullptr : &function_->stats();
}

const CapabilityReport *CodeHandle::capabilities() const noexcept {
  return function_ == nullptr ? nullptr : &function_->capabilities();
}

const StackMapTable* CodeHandle::stack_maps() const noexcept {
  return function_ == nullptr ? nullptr : &function_->stack_maps();
}

const StackMapRecord* CodeHandle::stack_map(std::size_t site) const noexcept {
  return function_ == nullptr ? nullptr : function_->stack_map(site);
}

StackMapCaptureResult CodeHandle::reconstruct_stack_map(
    const runtime::ExecutionContext& context) const {
  if (function_ == nullptr) {
    return {{StatusCode::kInvalidArgument, "code handle is not valid"}, {}};
  }
  return function_->reconstruct_stack_map(context);
}

const runtime::DeoptimizationRecord* CodeHandle::deoptimization_record(
    std::size_t site) const noexcept {
  return function_ == nullptr ? nullptr
                              : function_->deoptimization_record(site);
}

runtime::ReconstructionResult CodeHandle::reconstruct_deoptimization(
    std::size_t site, const ir::Word* args, std::size_t arg_count,
    const runtime::ExecutionContext& context) const {
  if (function_ == nullptr) {
    return {{StatusCode::kInvalidArgument, "code handle is not valid"}, {}};
  }
  return function_->reconstruct_deoptimization(site, args, arg_count,
                                               context);
}

runtime::MaterializationResult CodeHandle::materialize_deoptimization(
    std::size_t site, const ir::Word* args, std::size_t arg_count,
    const runtime::ExecutionContext& context,
    const runtime::MaterializationPlan& plan,
    const runtime::MaterializationCallbacks& callbacks) const {
  if (function_ == nullptr) {
    return {{StatusCode::kInvalidArgument, "code handle is not valid"}, {}};
  }
  return function_->materialize_deoptimization(
      site, args, arg_count, context, plan, callbacks);
}

runtime::OsrEntryResult CodeHandle::enter_osr(
    const runtime::OsrFrame& frame, const runtime::OsrEntryPlan& plan,
    runtime::ExecutionContext* context) const {
  if (function_ == nullptr) {
    runtime::OsrArguments arguments;
    arguments.status = {StatusCode::kInvalidArgument,
                        "code handle is not valid"};
    return {arguments, {arguments.status, 0}};
  }
  return function_->enter_osr(frame, plan, context);
}

NativeEntry CodeHandle::native_entry() const noexcept {
  return function_ == nullptr ? nullptr : function_->native_entry();
}

ir::EvaluationResult CodeHandle::invoke(
    const ir::Word* args, std::size_t arg_count,
    runtime::ExecutionContext* context) const {
  return function_ == nullptr ? invalid_handle()
                              : function_->invoke(args, arg_count, context);
}

struct CodeCache::Impl final {
  struct Entry final {
    std::uint64_t fingerprint{0};
    std::uint64_t generation{0};
    std::uint64_t last_access{0};
    std::size_t code_bytes{0};
    std::shared_ptr<const CompiledFunction> function;
  };

  explicit Impl(CodeCacheLimits configured_limits,
                TargetProfile configured_target) noexcept
      : limits(configured_limits), target_profile(configured_target) {}

  std::uint64_t next_stamp() noexcept {
    if (access_clock == std::numeric_limits<std::uint64_t>::max()) {
      for (auto& item : entries) {
        item.second.last_access = 0;
      }
      access_clock = 0;
    }
    return ++access_clock;
  }

  std::uint64_t next_code_generation() noexcept {
    if (generation == std::numeric_limits<std::uint64_t>::max()) {
      generation = 0;
    }
    return ++generation;
  }

  void erase(std::unordered_map<std::string, Entry>::iterator entry) noexcept {
    statistics.resident_code_bytes -= entry->second.code_bytes;
    entries.erase(entry);
    statistics.resident_entries = entries.size();
  }

  bool evict_one() noexcept {
    if (entries.empty()) {
      return false;
    }
    const auto victim = std::min_element(
        entries.begin(), entries.end(), [](const auto& lhs, const auto& rhs) {
          return lhs.second.last_access < rhs.second.last_access;
        });
    erase(victim);
    ++statistics.evictions;
    return true;
  }

  CodeCacheLimits limits;
  TargetProfile target_profile;
  mutable std::mutex mutex;
  std::unordered_map<std::string, Entry> entries;
  CodeCacheStats statistics;
  std::uint64_t access_clock{0};
  std::uint64_t generation{0};
};

CodeCache::CodeCache(CodeCacheLimits limits, TargetProfile target_profile)
    : impl_(std::make_unique<Impl>(limits, target_profile)) {}

CodeCache::~CodeCache() = default;
CodeCache::CodeCache(CodeCache&&) noexcept = default;
CodeCache& CodeCache::operator=(CodeCache&&) noexcept = default;

CodeHandle CodeCache::find(std::string_view key, std::uint64_t fingerprint) {
  if (impl_ == nullptr) {
    return {};
  }
  try {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    ++impl_->statistics.lookups;
    const auto entry = impl_->entries.find(std::string(key));
    if (entry == impl_->entries.end() ||
        entry->second.fingerprint != fingerprint) {
      ++impl_->statistics.misses;
      return {};
    }
    if (entry->second.function->assumptions().first_invalid() != nullptr) {
      impl_->erase(entry);
      ++impl_->statistics.misses;
      ++impl_->statistics.assumption_invalidations;
      return {};
    }
    ++impl_->statistics.hits;
    entry->second.last_access = impl_->next_stamp();
    return {entry->second.function, entry->second.generation};
  } catch (const std::bad_alloc&) {
    return {};
  }
}

CodeCachePublication CodeCache::publish(
    std::string_view key, std::uint64_t fingerprint,
    std::unique_ptr<CompiledFunction> function) {
  if (impl_ == nullptr) {
    return {{StatusCode::kInvalidArgument, "code cache was moved from"}, {},
            false, false};
  }
  if (key.empty()) {
    return {{StatusCode::kInvalidArgument, "code cache key cannot be empty"},
            {}, false, false};
  }
  if (function == nullptr) {
    return {{StatusCode::kInvalidArgument,
             "cannot publish a null compiled function"},
            {}, false, false};
  }
  if (!validate_target_profile(impl_->target_profile).ok()) {
    return {{StatusCode::kInvalidArgument,
             "code cache has an invalid target profile"},
            {}, false, false};
  }
  if (!target_profiles_equal(function->target_profile(),
                             impl_->target_profile)) {
    return {{StatusCode::kInvalidArgument,
             "compiled function target does not match the code cache"},
            {}, false, false};
  }

  const std::size_t code_bytes = function->stats().executable_mapping_size;
  try {
    std::string owned_key(key);
    std::shared_ptr<const CompiledFunction> shared(std::move(function));
    std::lock_guard<std::mutex> lock(impl_->mutex);
    ++impl_->statistics.publications;

    auto existing = impl_->entries.find(owned_key);
    if (existing != impl_->entries.end() &&
        existing->second.fingerprint == fingerprint) {
      if (existing->second.function->assumptions().first_invalid() == nullptr) {
        ++impl_->statistics.publication_reuses;
        existing->second.last_access = impl_->next_stamp();
        return {Status::ok_status(),
                {existing->second.function, existing->second.generation}, true,
                true};
      }
      impl_->erase(existing);
      ++impl_->statistics.assumption_invalidations;
      existing = impl_->entries.end();
    }

    const std::uint64_t code_generation = impl_->next_code_generation();
    CodeHandle uncached(shared, code_generation);
    if (shared->assumptions().first_invalid() != nullptr ||
        impl_->limits.maximum_entries == 0 ||
        code_bytes > impl_->limits.maximum_code_bytes) {
      ++impl_->statistics.uncached_publications;
      return {Status::ok_status(), std::move(uncached), false, false};
    }

    if (existing != impl_->entries.end()) {
      impl_->erase(existing);
      ++impl_->statistics.replacements;
    }
    while (impl_->entries.size() >= impl_->limits.maximum_entries ||
           impl_->statistics.resident_code_bytes >
               impl_->limits.maximum_code_bytes - code_bytes) {
      if (!impl_->evict_one()) {
        ++impl_->statistics.uncached_publications;
        return {Status::ok_status(), std::move(uncached), false, false};
      }
    }

    Impl::Entry entry{fingerprint, code_generation, impl_->next_stamp(),
                      code_bytes, shared};
    const auto inserted =
        impl_->entries.emplace(std::move(owned_key), std::move(entry));
    if (!inserted.second) {
      ++impl_->statistics.uncached_publications;
      return {Status::ok_status(), std::move(uncached), false, false};
    }
    impl_->statistics.resident_entries = impl_->entries.size();
    impl_->statistics.resident_code_bytes += code_bytes;
    return {Status::ok_status(),
            {inserted.first->second.function,
             inserted.first->second.generation},
            true, false};
  } catch (const std::bad_alloc&) {
    return {{StatusCode::kResourceExhausted,
             "unable to allocate code-cache publication state"},
            {}, false, false};
  }
}

bool CodeCache::invalidate(std::string_view key) {
  if (impl_ == nullptr) {
    return false;
  }
  try {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto entry = impl_->entries.find(std::string(key));
    if (entry == impl_->entries.end()) {
      return false;
    }
    impl_->erase(entry);
    ++impl_->statistics.invalidations;
    return true;
  } catch (const std::bad_alloc&) {
    return false;
  }
}

bool CodeCache::invalidate(std::string_view key, std::uint64_t fingerprint) {
  if (impl_ == nullptr) {
    return false;
  }
  try {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto entry = impl_->entries.find(std::string(key));
    if (entry == impl_->entries.end() ||
        entry->second.fingerprint != fingerprint) {
      return false;
    }
    impl_->erase(entry);
    ++impl_->statistics.invalidations;
    return true;
  } catch (const std::bad_alloc&) {
    return false;
  }
}

void CodeCache::clear() {
  if (impl_ == nullptr) {
    return;
  }
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->entries.clear();
  impl_->statistics.resident_entries = 0;
  impl_->statistics.resident_code_bytes = 0;
  ++impl_->statistics.clears;
}

CodeCacheLimits CodeCache::limits() const noexcept {
  if (impl_ == nullptr) {
    return {0, 0};
  }
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->limits;
}

TargetProfile CodeCache::target_profile() const noexcept {
  if (impl_ == nullptr) {
    return {};
  }
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->target_profile;
}

CodeCacheStats CodeCache::stats() const noexcept {
  if (impl_ == nullptr) {
    return {};
  }
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->statistics;
}

}  // namespace unijit::jit
