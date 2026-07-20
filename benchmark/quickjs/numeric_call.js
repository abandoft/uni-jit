"use strict";

function numericKernel(a, b) {
  return (a + b) * (a - 3.25) + b * 0.75;
}

function executeNumericKernel(kernel, count) {
  let lhs = 1.25;
  let rhs = -7.5;
  let checksum = 0.0;
  for (let iteration = 0; iteration < count; ++iteration) {
    checksum += kernel(lhs, rhs);
    lhs += 0.125;
    rhs -= 0.0625;
    if (lhs > 4096.0) {
      lhs = 1.25;
    }
    if (rhs < -4096.0) {
      rhs = -7.5;
    }
  }
  return checksum;
}

globalThis.unijitBenchmark = {
  kernel: numericKernel,
  execute: executeNumericKernel,
};

function benchmarkMedian(values) {
  values.sort((left, right) => left - right);
  const middle = Math.floor(values.length / 2);
  return values.length % 2 === 1
    ? values[middle]
    : (values[middle - 1] + values[middle]) / 2;
}

function checksumBits(value) {
  const view = new DataView(new ArrayBuffer(8));
  view.setFloat64(0, value, false);
  return `0x${view.getBigUint64(0, false).toString(16).padStart(16, "0")}`;
}

function parsePositiveInteger(value, name) {
  const parsed = Number(value);
  if (!Number.isSafeInteger(parsed) || parsed <= 0) {
    throw new Error(`${name} must be a positive safe integer`);
  }
  return parsed;
}

function runNodeBenchmark() {
  const warmup = parsePositiveInteger(process.argv[2] ?? "100000", "warmup");
  const iterations = parsePositiveInteger(
    process.argv[3] ?? "1000000",
    "iterations",
  );
  const sampleCount = parsePositiveInteger(process.argv[4] ?? "7", "samples");

  executeNumericKernel(numericKernel, warmup);
  const timings = [];
  let checksum;
  for (let sample = 0; sample < sampleCount; ++sample) {
    const started = process.hrtime.bigint();
    const current = executeNumericKernel(numericKernel, iterations);
    const elapsed = process.hrtime.bigint() - started;
    if (checksum !== undefined && checksumBits(current) !== checksumBits(checksum)) {
      throw new Error("JavaScript benchmark samples produced different checksums");
    }
    checksum = current;
    timings.push(Number(elapsed) / iterations);
  }

  const jitless = process.execArgv.includes("--jitless");
  process.stdout.write(`${JSON.stringify({
    schema: "unijit.javascript-numeric-call.v1",
    engine: "V8",
    mode: jitless ? "jitless" : "jit",
    node_version: process.version,
    v8_version: process.versions.v8,
    warmup_iterations: warmup,
    measurement_iterations: iterations,
    samples: sampleCount,
    median_ns: benchmarkMedian(timings),
    checksum: checksumBits(checksum),
  })}\n`);
}

if (typeof process === "object" && process.release?.name === "node") {
  runNodeBenchmark();
}
