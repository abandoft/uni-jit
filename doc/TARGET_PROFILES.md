# Target profiles

UniJIT describes every native compilation with an immutable
`unijit::jit::TargetProfile`. The profile is a compatibility requirement, not
an informal processor name: it records the architecture, ABI, endianness,
required instruction features, vector-width policy, and maximum vector width.

## Baseline and host profiles

`baseline_target_profile()` returns the portable requirement emitted by the
configured backend:

| Architecture | ABI | Required baseline |
|---|---|---|
| AArch64 | AAPCS64 | FP64 and Advanced SIMD/NEON |
| x86-64 | System V or Windows x64 | FP64 and SSE2 |
| RISC-V 64 | ELF psABI | RV64IMD |

`host_target_profile()` reports the features that the current process may
actually use. x86-64 discovery combines CPUID with XGETBV, so AVX, AVX2, and
FMA are not reported unless the operating system has enabled the required
extended register state. Linux RISC-V uses `AT_HWCAP` for the optional vector
extension. Unknown optional features stay disabled.

The current AArch64 backend targets the architectural 128-bit Advanced SIMD
floor, and x86-64 targets mandatory SSE2 with bounded scalar legalization for
operations absent from that baseline. RISC-V vector code is not emitted yet;
the discovered RVV bit is retained for the forthcoming fixed-width vector
lowering and does not by itself widen the portable profile.

## Compilation and execution rules

`CompilationOptions::target_profile` defaults to the portable backend
baseline. Compilation rejects:

- malformed architecture, ABI, endianness, feature, or vector-width
  combinations;
- a profile for a backend other than the one configured into the library;
- a required feature set not contained by the current host profile.

The accepted profile is stored in `CompiledFunction` and exposed through the
compiled function and `CodeHandle`. Host compatibility is captured before W^X
publication, and `native_entry()` remains unavailable when that check fails;
there is no CPUID or operating-system capability query on the invocation hot
path. Cross-target code generation is not silently inferred from the build
host; until object-level cross compilation is delivered, a different
architecture or ABI is rejected.

## Cache identity

A `CodeCache` is scoped to one exact target profile. It rejects publication of
a compiled function carrying any different profile, even when the host could
execute both. This keeps portable and optional-feature generations separate
without relying on callers to fold the profile into an ad-hoc source
fingerprint. `target_profile_key()` supplies a stable non-cryptographic key for
serialized-artifact and telemetry identity.

Profile validation is a compatibility boundary, not a trust boundary. Future
portable IR and native AOT loaders must still authenticate and structurally
validate their input before using the profile key.
