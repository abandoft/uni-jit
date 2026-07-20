# Compilation resource limits

UniJIT applies explicit resource budgets before expensive verification and
again before executable publication. The defaults are intended for untrusted
embedding inputs while remaining far above the current language frontend
regions.

| Resource | Default maximum |
|---|---:|
| Native parameters | 256 |
| SSA/CFG nodes | 65,536 |
| CFG blocks | 1,024 |
| Call or CFG edge arguments | 262,144 |
| Stack maps and exit records | 4,096 |
| Recovery and stack-map values | 262,144 |
| Native code bytes per function | 16 MiB |

Every configured maximum must be positive. `CompilationOptions::limits`
controls both straight-line and CFG compilation. The CFG compiler also retains
the direct `CompilationLimits` overloads, including variants with
deoptimization and assumption metadata:

```cpp
unijit::jit::CompilationOptions options(
    unijit::jit::OptimizationLevel::kOptimized);
options.limits.maximum_ir_nodes = 8192;
options.limits.maximum_code_bytes = 2U * 1024U * 1024U;
auto result = unijit::jit::Compiler::compile(function, options);

unijit::jit::CompilationOptions cfg_options(
    unijit::jit::OptimizationLevel::kBaseline);
cfg_options.limits.maximum_cfg_blocks = 256;
auto cfg_result = unijit::jit::Compiler::compile(cfg, cfg_options);
```

## Enforcement order

The compiler rejects excessive parameters, nodes, CFG blocks, flattened
straight-line or CFG runtime-call arguments, CFG edge arguments, requested exit
records, and recovery operations before the IR verifier runs. CFG call and edge
arguments share the aggregate `maximum_ir_arguments` budget. This ordering
bounds verifier work, including CFG dominance state, rather than merely
rejecting an oversized result afterward.

After optimization and recovery preparation, UniJIT bounds stack-map record
and live-value requirements before native lowering. It checks emitted stack
maps and machine-code bytes again after lowering and before allocating or
publishing W^X executable memory. A limit violation returns
`StatusCode::kResourceExhausted`; an invalid zero budget returns
`StatusCode::kInvalidArgument`. Where one input site or observed count caused
the rejection, `Status::location()` identifies it.

QuickJS and PocketPy additionally reject source larger than 1 MiB before
retaining or translating it. Lua compilation reads an already allocated stock
`Proto` and is governed by the core bytecode-to-IR and compilation limits.
Scheduler queue budgets and code-cache resident-byte budgets remain separate
global controls because they govern concurrent and long-lived resources rather
than one compilation.

The values are defaults, not immutable platform constants. A trusted embedder
may raise them deliberately, but doing so also accepts the verifier, optimizer,
lowering, metadata, and executable-memory cost of the larger region.
