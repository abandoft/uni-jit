# Portable IR Package

## Purpose

The UniJIT portable IR package is an architecture-neutral, deterministic representation of verified straight-line and control-flow IR. It is intended for persistent caches, build systems, and untrusted transport boundaries. It is not an object-memory dump, does not expose C++ layout, and never contains executable code or process addresses.

Portable IR and AOT objects are separate trust domains. A portable IR package may be decoded and verified on any supported 64-bit host. A future AOT object will additionally bind the exact compiler identity, target profile, code model, and portable-IR digest before executable memory can be created.

## Version 1 wire contract

Every integer is encoded in little-endian order. No host padding, enum representation, pointer, `size_t`, or native object layout appears on the wire. Version 1 begins with a fixed 64-byte header:

| Offset | Size | Field |
| ---: | ---: | --- |
| 0 | 8 | magic `UNIJIR\0\1` |
| 8 | 2 | format version, currently `1` |
| 10 | 2 | minimum reader version, currently `1` |
| 12 | 1 | package kind: straight-line function or CFG function |
| 13 | 1 | flags, zero in version 1 |
| 14 | 2 | header size, exactly `64` |
| 16 | 8 | exact total package length |
| 24 | 8 | exact payload length |
| 32 | 32 | SHA-256 of the payload |

The payload starts with a fixed 96-byte manifest containing all allocation-driving counts. Remaining data is emitted in one canonical order: parameter types, nodes, call arguments, fast-call descriptors and flattened signature types, memory descriptors, atomic descriptors, frame slots, trusted-object layouts, patch cells, vector constants, vector shuffles, vector-select arguments, and CFG block records. Reserved fields and unused manifest words must be zero. There is exactly one encoding for a given in-memory IR graph.

Node records are fixed-width and contain stable numeric opcode/type values, SSA indexes, side-table indexes, and the exact two's-complement bits of the immediate. CFG records preserve block instruction order, terminators, and edge argument order. Decode reconstructs the public IR semantics and then runs the normal IR verifier; successful decode never bypasses compilation verification.

## Fail-closed boundary

The decoder accepts a pointer plus an explicit byte length. It rejects null/nonzero input pairs, truncated headers and records, trailing bytes, unknown versions/kinds/flags, nonzero reserved fields, length arithmetic overflow, count disagreement, SHA-256 mismatch, invalid enum values, invalid indexes, malformed SSA/CFG structure, and any configured resource-limit violation.

The default limits cap a package at 64 MiB, 256 parameters, 65,536 nodes, 1,024 blocks, 262,144 call or edge arguments, 64 fast calls, 16,384 flattened fast-call signature values, 64 memory regions, 65,536 memory/atomic/vector side-table entries, 256 frame slots or patch cells, and 64 trusted objects. Applications can only tighten or deliberately raise these limits through an explicit limits value.

Runtime helper calls are rejected during encoding and decoding because a function pointer has no portable or trustworthy wire meaning. Generation-safe `FastCall` descriptors are portable: only their scalar signatures and slot indexes are stored, and every target must be rebound after compilation. Trusted-object entries carry only their non-addressable layout identity and bounded byte size. Patch-cell entries carry typed initial data, never executable addresses.

Encoding verifies the source IR before allocating the package and returns a bounded status instead of a partial artifact. Decoding validates the header and manifest before allocating IR tables, checks the payload digest before interpreting records, constructs into temporary storage, and publishes a function only after complete structural verification. Allocation failure and unexpected implementation exceptions are contained and returned as status failures.

## Determinism and qualification

Re-encoding a decoded package must be byte-for-byte identical, including its SHA-256 digest. Straight-line and CFG packages are qualified through semantic round trips in the interpreter and native compiler, all supported side-table categories, corruption at every header region, systematic truncation, count/length overflow probes, unknown enum/opcode probes, resource-limit negatives, and deterministic mutation fuzzing.

The installed-package consumer creates an IR package, decodes it through the installed headers and library, verifies byte identity after re-encoding, compiles the reconstructed function, and invokes it. Hosted Ubuntu and Windows x86-64 plus Apple AArch64 jobs therefore exercise the same public artifact boundary used by external customers. Real RISC-V 64 qualification uses the identical committed corpus and scalar oracle.

## AOT boundary

Portable IR versioning is independent of source releases. A reader may accept a package only when its format and minimum-reader versions are supported. The AOT format will not embed an unchecked native blob in this container. Its trust envelope must include at least the portable-IR SHA-256 digest, exact compiler build identity, complete target profile and feature policy, ABI/code model, relocation vocabulary, immutable-code/data split, executable mapping bounds, and an application-supplied authenticity decision. Until that contract and its three-architecture loader qualification are delivered, portable IR is always recompiled locally and no serialized byte is mapped executable.
