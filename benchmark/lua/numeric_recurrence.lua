local warmup = assert(tonumber(arg[1]), "warmup iteration count required")
local iterations = assert(tonumber(arg[2]), "measurement iteration count required")
local samples = assert(tonumber(arg[3]), "sample count required")

local function kernel(count)
    local x = 0.125
    local y = 0.75
    for index = 1, count do
        x = x * 1.0000001192092896 + y + index * 0.0000001
        y = y * 0.9999999403953552 + x * 0.0000003
        if x > 1000000.0 then
            x = x - 1000000.0
        end
        if y > 1000000.0 then
            y = y - 1000000.0
        end
    end
    return x + y
end

local function median(values)
    table.sort(values)
    local middle = math.floor(#values / 2) + 1
    if #values % 2 == 1 then
        return values[middle]
    end
    return (values[middle - 1] + values[middle]) / 2
end

kernel(warmup)

local timings = {}
local checksum = 0.0
for sample = 1, samples do
    local started = os.clock()
    checksum = kernel(iterations)
    timings[sample] = (os.clock() - started) * 1000000000.0 / iterations
end

local engine = _VERSION
if jit and jit.version then
    engine = jit.version
end

io.write(string.format(
    '{"schema":"unijit.lua-benchmark.v1",' ..
    '"workload":"numeric_recurrence",' ..
    '"engine":"%s",' ..
    '"warmup_iterations":%d,' ..
    '"measurement_iterations":%d,' ..
    '"samples":%d,' ..
    '"median_ns":%.6f,' ..
    '"checksum":%.17g}\n',
    engine, warmup, iterations, samples, median(timings), checksum))
