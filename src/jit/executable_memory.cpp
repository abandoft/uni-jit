#include "jit/executable_memory.h"

#include <cstring>
#include <limits>
#include <string>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace unijit::jit::detail {
namespace {

// Clang's function-type sanitizer probes eight bytes before an indirect-call
// target. Dynamic code has no sanitizer signature, so keep a readable,
// zero-filled prefix while preserving 16-byte entry alignment.
constexpr std::size_t kCodePrefixSize = 16;

#if defined(_WIN32)
std::size_t page_size() noexcept {
  SYSTEM_INFO info{};
  GetSystemInfo(&info);
  return static_cast<std::size_t>(info.dwPageSize);
}
#else
std::size_t page_size() noexcept {
  const long result = sysconf(_SC_PAGESIZE);
  return result > 0 ? static_cast<std::size_t>(result) : 4096U;
}
#endif

bool round_to_pages(std::size_t size, std::size_t* rounded) noexcept {
  const std::size_t page = page_size();
  if (size > std::numeric_limits<std::size_t>::max() - (page - 1U)) {
    return false;
  }
  *rounded = ((size + page - 1U) / page) * page;
  return true;
}

}  // namespace

ExecutableMemory::~ExecutableMemory() { release(); }

ExecutableMemory::ExecutableMemory(ExecutableMemory&& other) noexcept
    : mapping_address_(std::exchange(other.mapping_address_, nullptr)),
      address_(std::exchange(other.address_, nullptr)),
      mapping_size_(std::exchange(other.mapping_size_, 0)),
      code_size_(std::exchange(other.code_size_, 0)) {}

ExecutableMemory& ExecutableMemory::operator=(ExecutableMemory&& other) noexcept {
  if (this != &other) {
    release();
    mapping_address_ = std::exchange(other.mapping_address_, nullptr);
    address_ = std::exchange(other.address_, nullptr);
    mapping_size_ = std::exchange(other.mapping_size_, 0);
    code_size_ = std::exchange(other.code_size_, 0);
  }
  return *this;
}

void ExecutableMemory::release() noexcept {
  if (mapping_address_ == nullptr) {
    return;
  }
#if defined(_WIN32)
  VirtualFree(mapping_address_, 0, MEM_RELEASE);
#else
  munmap(mapping_address_, mapping_size_);
#endif
  mapping_address_ = nullptr;
  address_ = nullptr;
  mapping_size_ = 0;
  code_size_ = 0;
}

Status ExecutableMemory::publish(const std::uint8_t* code,
                                 std::size_t code_size,
                                 ExecutableMemory* output) {
  if (code == nullptr || code_size == 0 || output == nullptr) {
    return {StatusCode::kInvalidArgument,
            "native code and output storage must be non-empty"};
  }

  if (code_size >
      std::numeric_limits<std::size_t>::max() - kCodePrefixSize) {
    return {StatusCode::kResourceExhausted,
            "native code size overflows its entry prefix"};
  }
  std::size_t mapping_size = 0;
  if (!round_to_pages(code_size + kCodePrefixSize, &mapping_size)) {
    return {StatusCode::kResourceExhausted,
            "native code size overflows an address-space page"};
  }

#if defined(_WIN32)
  void* mapping = VirtualAlloc(nullptr, mapping_size, MEM_COMMIT | MEM_RESERVE,
                               PAGE_READWRITE);
  if (mapping == nullptr) {
    return {StatusCode::kResourceExhausted,
            "VirtualAlloc failed for the native code mapping"};
  }
  auto* code_address =
      static_cast<std::uint8_t*>(mapping) + kCodePrefixSize;
  std::memset(mapping, 0, kCodePrefixSize);
  std::memcpy(code_address, code, code_size);
  FlushInstructionCache(GetCurrentProcess(), code_address, code_size);
  DWORD old_protection = 0;
  if (VirtualProtect(mapping, mapping_size, PAGE_EXECUTE_READ,
                     &old_protection) == 0) {
    VirtualFree(mapping, 0, MEM_RELEASE);
    return {StatusCode::kMemoryProtectionFailed,
            "VirtualProtect could not publish native code read-execute"};
  }
#else
  void* mapping = mmap(nullptr, mapping_size, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANON, -1, 0);
  if (mapping == MAP_FAILED) {
    return {StatusCode::kResourceExhausted,
            std::string("mmap failed for native code: ") +
                std::strerror(errno)};
  }
  auto* code_address =
      static_cast<std::uint8_t*>(mapping) + kCodePrefixSize;
  std::memset(mapping, 0, kCodePrefixSize);
  std::memcpy(code_address, code, code_size);
  auto* begin = reinterpret_cast<char*>(code_address);
  __builtin___clear_cache(begin, begin + code_size);
  if (mprotect(mapping, mapping_size, PROT_READ | PROT_EXEC) != 0) {
    const std::string message =
        std::string("mprotect could not publish native code: ") +
        std::strerror(errno);
    munmap(mapping, mapping_size);
    return {StatusCode::kMemoryProtectionFailed, message};
  }
#endif

  output->release();
  output->mapping_address_ = mapping;
  output->address_ = code_address;
  output->mapping_size_ = mapping_size;
  output->code_size_ = code_size;
  return Status::ok_status();
}

}  // namespace unijit::jit::detail
