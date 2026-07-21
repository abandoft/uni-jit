local warmup = assert(tonumber(arg[1]), "warmup invocation count required")
local iterations = assert(tonumber(arg[2]), "measurement invocation count required")
local samples = assert(tonumber(arg[3]), "sample count required")
local mode = arg[4] or "reference"
local loop_iterations = 1000
local loop_start = 0.25
local loop_step = 0.5
local loop_limit = loop_start + (loop_iterations - 1) * loop_step
local loop_seed = 3.75

local function kernel(start, limit, step, seed)
    local value = seed
    for index = start, limit, step do
        value = (value + index) * 0.5 + 1.0
    end
    return value
end

local engine = _VERSION
if mode == "unijit" then
    kernel = require("unijit").compile_float(kernel)
    engine = "UniJIT/" .. _VERSION
elseif jit and jit.version then
    engine = jit.version
end

local function execute(count)
    local checksum = 0.0
    for _ = 1, count do
        checksum = checksum +
            kernel(loop_start, loop_limit, loop_step, loop_seed)
    end
    return checksum
end

local function median(values)
    table.sort(values)
    local middle = math.floor(#values / 2) + 1
    if #values % 2 == 1 then
        return values[middle]
    end
    return (values[middle - 1] + values[middle]) / 2
end

execute(warmup)

local timings = {}
local checksum = 0.0
for sample = 1, samples do
    local started = os.clock()
    checksum = execute(iterations)
    timings[sample] = (os.clock() - started) * 1000000000.0 /
        (iterations * loop_iterations)
end

io.write(string.format(
    '{"schema":"unijit.lua-benchmark.v1",' ..
    '"workload":"float_parameter_loop",' ..
    '"engine":"%s",' ..
    '"warmup_iterations":%d,' ..
    '"measurement_iterations":%d,' ..
    '"inner_loop_iterations":%d,' ..
    '"samples":%d,' ..
    '"median_ns":%.6f,' ..
    '"checksum":%.17g}\n',
    engine, warmup, iterations, loop_iterations, samples,
    median(timings), checksum))
