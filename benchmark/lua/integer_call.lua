local warmup = assert(tonumber(arg[1]), "warmup iteration count required")
local iterations = assert(tonumber(arg[2]), "measurement iteration count required")
local samples = assert(tonumber(arg[3]), "sample count required")
local mode = arg[4] or "reference"

local function kernel(a, b, c)
    local left = (a - b) * (b + c)
    local right = a * 7 - c * 11
    return left + right + 19
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
    for index = 1, count do
        local a = index % 1021
        local b = index % 509
        local c = index % 251
        checksum = checksum + kernel(a, b, c)
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
    timings[sample] = (os.clock() - started) * 1000000000.0 / iterations
end

io.write(string.format(
    '{"schema":"unijit.lua-benchmark.v1",' ..
    '"workload":"integer_call",' ..
    '"engine":"%s",' ..
    '"warmup_iterations":%d,' ..
    '"measurement_iterations":%d,' ..
    '"samples":%d,' ..
    '"median_ns":%.6f,' ..
    '"checksum":%.0f}\n',
    engine, warmup, iterations, samples, median(timings), checksum))
