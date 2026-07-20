local warmup = assert(tonumber(arg[1]), "warmup invocation count required")
local iterations = assert(tonumber(arg[2]), "measurement invocation count required")
local samples = assert(tonumber(arg[3]), "sample count required")
local mode = arg[4] or "reference"
local loop_iterations = 1000

local function kernel(count)
    local sum = 0
    for index = 1, count do
        sum = sum + index
    end
    return sum
end

local engine = _VERSION
if mode == "unijit" then
    kernel = require("unijit").compile(kernel)
    engine = "UniJIT/" .. _VERSION
elseif jit and jit.version then
    engine = jit.version
end

local function execute(count)
    local checksum = 0
    for _ = 1, count do
        checksum = checksum + kernel(loop_iterations)
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
local checksum = 0
for sample = 1, samples do
    local started = os.clock()
    checksum = execute(iterations)
    timings[sample] = (os.clock() - started) * 1000000000.0 /
        (iterations * loop_iterations)
end

io.write(string.format(
    '{"schema":"unijit.lua-benchmark.v1",' ..
    '"workload":"integer_loop",' ..
    '"engine":"%s",' ..
    '"warmup_iterations":%d,' ..
    '"measurement_iterations":%d,' ..
    '"inner_loop_iterations":%d,' ..
    '"samples":%d,' ..
    '"median_ns":%.6f,' ..
    '"checksum":%.0f}\n',
    engine, warmup, iterations, loop_iterations, samples,
    median(timings), checksum))
