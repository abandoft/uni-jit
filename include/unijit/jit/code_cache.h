#ifndef UNIJIT_JIT_CODE_CACHE_H
#define UNIJIT_JIT_CODE_CACHE_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "unijit/jit/compiler.h"
#include "unijit/status.h"

namespace unijit::jit {

struct CodeCacheLimits final {
  std::size_t maximum_entries{1024};
  std::size_t maximum_code_bytes{64U * 1024U * 1024U};
};

struct CodeCacheStats final {
  std::uint64_t lookups{0};
  std::uint64_t hits{0};
  std::uint64_t misses{0};
  std::uint64_t publications{0};
  std::uint64_t publication_reuses{0};
  std::uint64_t uncached_publications{0};
  std::uint64_t replacements{0};
  std::uint64_t invalidations{0};
  std::uint64_t evictions{0};
  std::uint64_t clears{0};
  std::size_t resident_entries{0};
  std::size_t resident_code_bytes{0};
};

class CodeHandle final {
 public:
  CodeHandle() noexcept = default;

  bool valid() const noexcept { return function_ != nullptr; }
  explicit operator bool() const noexcept { return valid(); }

  std::uint64_t generation() const noexcept { return generation_; }
  std::size_t parameter_count() const noexcept;
  bool requires_context() const noexcept;
  const CompilationStats* compilation_stats() const noexcept;
  NativeEntry native_entry() const noexcept;

  ir::EvaluationResult invoke(
      const ir::Word* args, std::size_t arg_count,
      runtime::ExecutionContext* context = nullptr) const;

 private:
  friend class CodeCache;

  CodeHandle(std::shared_ptr<const CompiledFunction> function,
             std::uint64_t generation) noexcept
      : function_(std::move(function)), generation_(generation) {}

  std::shared_ptr<const CompiledFunction> function_;
  std::uint64_t generation_{0};
};

struct CodeCachePublication final {
  Status status;
  CodeHandle handle;
  bool cached{false};
  bool reused{false};

  bool ok() const noexcept { return status.ok() && handle.valid(); }
};

class CodeCache final {
 public:
  explicit CodeCache(CodeCacheLimits limits = {});
  ~CodeCache();

  CodeCache(CodeCache&&) noexcept;
  CodeCache& operator=(CodeCache&&) noexcept;

  CodeCache(const CodeCache&) = delete;
  CodeCache& operator=(const CodeCache&) = delete;

  CodeHandle find(std::string_view key, std::uint64_t fingerprint);

  CodeCachePublication publish(
      std::string_view key, std::uint64_t fingerprint,
      std::unique_ptr<CompiledFunction> function);

  bool invalidate(std::string_view key);
  bool invalidate(std::string_view key, std::uint64_t fingerprint);
  void clear();

  CodeCacheLimits limits() const noexcept;
  CodeCacheStats stats() const noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace unijit::jit

#endif  // UNIJIT_JIT_CODE_CACHE_H
