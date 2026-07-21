#include <cstdlib>
#include <iostream>

extern "C" {
#include "lauxlib.h"
#include "lualib.h"
}

#include "unijit_lua.h"

namespace {

constexpr char kTestProgram[] = R"lua(
local unijit = require("unijit")

local function checkpoint(name)
  io.stderr:write("Lua frontend checkpoint: " .. name .. "\n")
  io.stderr:flush()
end

checkpoint("loaded")

local function compare(original, arguments)
  local native = unijit.compile(original)
  for _, values in ipairs(arguments) do
    assert(native(table.unpack(values)) == original(table.unpack(values)))
  end
  return native
end

local function promote_loop(native, original, argument)
  for _ = 1, 64 do
    assert(native(argument) == original(argument))
  end
  assert(unijit.wait(native, 5000))
  assert(unijit.stats(native).active_tier == "optimized")
end

local recurrence = function(a, b, c)
  local difference = a - b
  local sum = b + c
  return difference * sum
end

local cases = {}
for index = -256, 256 do
  cases[#cases + 1] = {index * 17, index - 31, index * index + 9}
end
cases[#cases + 1] = {math.maxinteger, -1, 1}
cases[#cases + 1] = {math.mininteger, 1, -1}
local native_recurrence = compare(recurrence, cases)
checkpoint("integer recurrence")
assert(unijit.wait(native_recurrence, 5000))
local recurrence_stats = unijit.stats(native_recurrence)
assert(recurrence_stats.tierable)
assert(not recurrence_stats.loop)
assert(recurrence_stats.active_tier == "optimized")
assert(recurrence_stats.invocations == #cases)
assert(recurrence_stats.backedges == 0)
assert(recurrence_stats.compilation_attempts == 1,
       "unexpected compilation attempt count: " ..
       tostring(recurrence_stats.compilation_attempts))
assert(recurrence_stats.successful_compilations == 1,
       "unexpected successful compilation count: " ..
       tostring(recurrence_stats.successful_compilations))
assert(recurrence_stats.failed_compilations == 0)
assert(recurrence_stats.promotions == 1)
assert(recurrence_stats.compilation_state == "succeeded")
assert(recurrence_stats.scheduler_available)
assert(recurrence_stats.code_size > 0)
assert(not unijit.cancel(native_recurrence))
local cached_recurrence = unijit.compile(recurrence)
assert(cached_recurrence(9, 3, 5) == 48)
checkpoint("integer tiering")

local function compare_float(original, arguments)
  local native = unijit.compile_float(original)
  for _, values in ipairs(arguments) do
    local expected = original(table.unpack(values))
    local actual = native(table.unpack(values))
    assert(math.type(actual) == "float")
    assert(actual == expected)
  end
  return native
end

local float_recurrence = function(a, b, c)
  local difference = a - b
  local sum = b + c
  return difference * sum
end
local float_cases = {
  {9.0, 3.0, 5.0},
  {-17.25, 4.5, 0.25},
  {1.0e12, -0.5, 3.25},
  {0.0, -0.0, 7.5},
}
local native_float_recurrence = compare_float(float_recurrence, float_cases)
checkpoint("float recurrence")
for _ = 1, 64 do
  assert(native_float_recurrence(9.0, 3.0, 5.0) == 48.0)
end
assert(unijit.wait(native_float_recurrence, 5000))
local float_stats = unijit.stats(native_float_recurrence)
assert(float_stats.active_tier == "optimized")
assert(float_stats.compilation_attempts == 1)
assert(float_stats.successful_compilations == 1)
local cached_float_recurrence = unijit.compile_float(float_recurrence)
assert(cached_float_recurrence(9.0, 3.0, 5.0) == 48.0)
checkpoint("float tiering")

compare_float(function(value)
  return (value + 7) * 0.5 - 3.0
end, {{0.0}, {-100.25}, {123456.5}})

compare_float(function(lhs, rhs)
  return (lhs / rhs) / 2
end, {{9.0, 3.0}, {-17.25, 4.5}, {1.0e12, -0.5}})

compare_float(function()
  return 17.5
end, {{}})

compare_float(function()
  return 17 / 2
end, {{}})

local float_negate = function(value)
  return -value
end
local native_float_negate = compare_float(
    float_negate, {{0.0}, {-0.0}, {1.25}, {-17.5}, {math.huge}, {-math.huge}})
assert(1.0 / native_float_negate(0.0) == -math.huge)
assert(1.0 / native_float_negate(-0.0) == math.huge)
for _ = 1, 64 do
  assert(native_float_negate(17.5) == -17.5)
end
assert(unijit.wait(native_float_negate, 5000))
assert(unijit.stats(native_float_negate).active_tier == "optimized")
assert(1.0 / native_float_negate(0.0) == -math.huge)
assert(1.0 / native_float_negate(-0.0) == math.huge)
checkpoint("float arithmetic")

local float_ok, float_message = pcall(native_float_recurrence, 1.0, 2, 3.0)
assert(not float_ok and tostring(float_message):find("Float64"))
checkpoint("float invocation rejection")

float_ok, float_message = pcall(unijit.compile_float, function()
  return 17
end)
assert(not float_ok and tostring(float_message):find("not Float64"))
checkpoint("float rejections")

local float_loop_recurrence = function(start, limit, step, seed)
  local value = seed
  for index = start, limit, step do
    value = (value + index) * 0.5 + 1.0
  end
  return value
end
local native_float_loop_recurrence =
    unijit.compile_float(float_loop_recurrence)
local float_loop_cases = {
  {1.0, 20.0, 1.0, 0.25},
  {20.0, 1.0, -1.0, -7.5},
  {-4.5, 7.5, 0.25, 3.0},
  {7.5, -4.5, -0.5, -3.0},
  {7.0, -7.0, 0.5, 11.0},
  {-7.0, 7.0, -0.5, 11.0},
  {0.0, 10.0, math.huge, 2.0},
  {10.0, 0.0, -math.huge, 2.0},
}
for _, values in ipairs(float_loop_cases) do
  local expected = float_loop_recurrence(table.unpack(values))
  local actual = native_float_loop_recurrence(table.unpack(values))
  assert(math.type(actual) == "float" and actual == expected,
         table.concat(values, ",") .. ": " .. tostring(actual) ..
         " != " .. tostring(expected))
end
assert(native_float_loop_recurrence(1.0, 12000.0, 1.0, 0.25) ==
       float_loop_recurrence(1.0, 12000.0, 1.0, 0.25))
assert(unijit.wait(native_float_loop_recurrence, 5000))
local float_loop_stats = unijit.stats(native_float_loop_recurrence)
assert(float_loop_stats.loop)
assert(float_loop_stats.active_tier == "optimized")
assert(float_loop_stats.backedges >= 12000)
assert(float_loop_stats.successful_compilations == 1)
for _, values in ipairs(float_loop_cases) do
  assert(native_float_loop_recurrence(table.unpack(values)) ==
         float_loop_recurrence(table.unpack(values)))
end

local float_loop_visits = function(start, limit, step)
  local visits = 0.0
  for _ = start, limit, step do
    visits = visits + 1.0
  end
  return visits
end
local native_float_loop_visits = unijit.compile_float(float_loop_visits)
local nan = 0.0 / 0.0
local float_loop_boundary_cases = {
  {nan, 10.0, 1.0},
  {1.0, nan, 1.0},
  {1.0, 10.0, nan},
  {10.0, 1.0, nan},
  {math.huge, math.huge, -math.huge},
  {-math.huge, -math.huge, math.huge},
}
for _, values in ipairs(float_loop_boundary_cases) do
  assert(native_float_loop_visits(table.unpack(values)) ==
         float_loop_visits(table.unpack(values)))
end
local float_zero_invocations =
    unijit.stats(native_float_loop_visits).invocations
for _, zero_step in ipairs({0.0, -0.0}) do
  local original_ok, original_message =
      pcall(float_loop_visits, 1.0, 10.0, zero_step)
  local native_ok, native_message =
      pcall(native_float_loop_visits, 1.0, 10.0, zero_step)
  assert(not original_ok and
         tostring(original_message):find("'for' step is zero"))
  assert(not native_ok and
         tostring(native_message):find("'for' step is zero"))
end
assert(unijit.stats(native_float_loop_visits).invocations ==
       float_zero_invocations)

local constant_zero_float_loop = function()
  local visits = 0.0
  for _ = 1.0, 3.0, 0.0 do
    visits = visits + 1.0
  end
  return visits
end
local native_constant_zero_float_loop =
    unijit.compile_float(constant_zero_float_loop)
local constant_original_ok, constant_original_message =
    pcall(constant_zero_float_loop)
local constant_native_ok, constant_native_message =
    pcall(native_constant_zero_float_loop)
assert(not constant_original_ok and
       tostring(constant_original_message):find("'for' step is zero"))
assert(not constant_native_ok and
       tostring(constant_native_message):find("'for' step is zero"))

local computed_zero_float_loop = function(value)
  local step = value - value
  local visits = 0.0
  for _ = 1.0, 3.0, step do
    visits = visits + 1.0
  end
  return visits
end
local native_computed_zero_float_loop =
    unijit.compile_float(computed_zero_float_loop)
local computed_original_ok, computed_original_message =
    pcall(computed_zero_float_loop, 7.5)
local computed_native_ok, computed_native_message =
    pcall(native_computed_zero_float_loop, 7.5)
assert(not computed_original_ok and
       tostring(computed_original_message):find("'for' step is zero"))
assert(not computed_native_ok and
       tostring(computed_native_message):find("'for' step is zero"))
checkpoint("Float64 numeric loops")

local immediate = compare(function(value)
  return value + 7
end, {{0}, {-100}, {math.maxinteger}})
checkpoint("integer addition")

compare(function(value)
  return value * 400 - 900
end, {{0}, {17}, {-9123456}, {math.maxinteger}})
checkpoint("integer multiplication")

compare(function(value)
  local large = 9223372036854770000
  return value + large
end, {{0}, {807}, {-123456789}})
checkpoint("integer large constant")

local integer_unary_cases = {
  {0}, {1}, {-1}, {42}, {-97}, {math.mininteger}, {math.maxinteger},
}
compare(function(value)
  return -value
end, integer_unary_cases)
compare(function(value)
  return ~value
end, integer_unary_cases)
compare(function(value)
  return -(-value) + ~(~value)
end, integer_unary_cases)
checkpoint("integer unary arithmetic")

local bitwise_cases = {
  {0, 0}, {-1, 0}, {1, -1}, {42, 21}, {-97, 85},
  {math.mininteger, math.maxinteger},
  {0x5555555555555555, -0x5555555555555556},
}
compare(function(lhs, rhs)
  return ((lhs & rhs) | (lhs ~ rhs)) ~ (lhs | rhs)
end, bitwise_cases)
compare(function(value)
  return ((value & 0x55555555) | 0x100000000) ~ 0x12345678
end, integer_unary_cases)
checkpoint("integer binary bitwise arithmetic")

local shift_cases = {
  {0, 0}, {1, 1}, {-1, -1}, {0x123456789abcdef, 31},
  {math.mininteger, 63}, {math.maxinteger, -63},
  {-1, 64}, {-1, -64}, {-1, 65}, {-1, -65},
  {math.mininteger, math.mininteger},
  {math.maxinteger, math.maxinteger},
}
local shift_left = function(value, amount)
  return value << amount
end
local native_shift_left = compare(shift_left, shift_cases)
compare(function(value, amount)
  return value >> amount
end, shift_cases)
compare(function(_, amount)
  return 7 << amount
end, shift_cases)
compare(function(value, _)
  return value >> 7
end, shift_cases)
for _ = 1, 64 do
  assert(native_shift_left(-1, 63) == shift_left(-1, 63))
end
assert(unijit.wait(native_shift_left, 5000))
assert(unijit.stats(native_shift_left).active_tier == "optimized")
for _, values in ipairs(shift_cases) do
  assert(native_shift_left(table.unpack(values)) ==
         shift_left(table.unpack(values)))
end
checkpoint("integer shifts")

local floor_cases = {
  {0, 1}, {1, 1}, {-1, 1}, {17, 5}, {-17, 5},
  {17, -5}, {-17, -5}, {math.mininteger, -1},
  {math.mininteger, 3}, {math.maxinteger, -7},
}
local floor_divide = function(lhs, rhs)
  return lhs // rhs
end
local floor_modulo = function(lhs, rhs)
  return lhs % rhs
end
local native_floor_divide = compare(floor_divide, floor_cases)
local native_floor_modulo = compare(floor_modulo, floor_cases)
compare(function(value)
  return value // -7
end, integer_unary_cases)
compare(function(value)
  return value % -7
end, integer_unary_cases)
for _ = 1, 64 do
  assert(native_floor_divide(-17, 5) == -4)
  assert(native_floor_modulo(-17, 5) == 3)
end
assert(unijit.wait(native_floor_divide, 5000))
assert(unijit.wait(native_floor_modulo, 5000))
assert(unijit.stats(native_floor_divide).active_tier == "optimized")
assert(unijit.stats(native_floor_modulo).active_tier == "optimized")
for _, values in ipairs(floor_cases) do
  assert(native_floor_divide(table.unpack(values)) ==
         floor_divide(table.unpack(values)))
  assert(native_floor_modulo(table.unpack(values)) ==
         floor_modulo(table.unpack(values)))
end
local floor_ok, floor_message = pcall(native_floor_divide, 7, 0)
assert(not floor_ok and tostring(floor_message):find("divide by zero"))
floor_ok, floor_message = pcall(native_floor_modulo, 7, 0)
assert(not floor_ok and tostring(floor_message):find("n%%0"))
local native_constant_zero = unijit.compile(function(value)
  return value // 0
end)
floor_ok, floor_message = pcall(native_constant_zero, 7)
assert(not floor_ok and tostring(floor_message):find("divide by zero"))
checkpoint("integer floor arithmetic")

compare(function()
  return 17
end, {{}})

checkpoint("scalar arithmetic")

local counted_sum = function(count)
  local sum = 0
  for index = 1, count do
    sum = sum + index
  end
  return sum
end
local backedge_counted_sum = unijit.compile(counted_sum)
local baseline_loop_stats = unijit.stats(backedge_counted_sum)
assert(baseline_loop_stats.active_tier == "baseline")
assert(baseline_loop_stats.loop)
assert(backedge_counted_sum(10000) == counted_sum(10000))
assert(unijit.wait(backedge_counted_sum, 5000))
local optimized_loop_stats = unijit.stats(backedge_counted_sum)
assert(optimized_loop_stats.active_tier == "optimized")
assert(optimized_loop_stats.invocations == 1)
assert(optimized_loop_stats.backedges == 10000)
assert(optimized_loop_stats.compilation_attempts == 1)
assert(optimized_loop_stats.successful_compilations == 1)
assert(optimized_loop_stats.promotions == 1)
assert(optimized_loop_stats.input_ir_nodes > baseline_loop_stats.input_ir_nodes)
local native_counted_sum = unijit.compile(counted_sum)
for count = -7, 128 do
  assert(native_counted_sum(count) == counted_sum(count))
end
assert(native_counted_sum(10000) == counted_sum(10000))

local offset_sum = function(count)
  local sum = 5
  for index = 3, count do
    sum = sum + index * 2
  end
  return sum
end
local native_offset_sum = unijit.compile(offset_sum)
for _, count in ipairs({-1, 0, 2, 3, 19, 1000}) do
  assert(native_offset_sum(count) == offset_sum(count))
end

local near_max_loop = function(limit)
  local visits = 0
  for index = 9223372036854775800, limit do
    visits = visits + 1
  end
  return visits
end
local native_near_max_loop = unijit.compile(near_max_loop)
for _, limit in ipairs({math.maxinteger - 8, math.maxinteger - 7,
                        math.maxinteger - 6, math.maxinteger}) do
  assert(native_near_max_loop(limit) == near_max_loop(limit))
end

local positive_stride_sum = function(limit)
  local sum = 0
  for index = 1, limit, 3 do
    sum = sum + index
  end
  return sum
end
local native_positive_stride_sum = unijit.compile(positive_stride_sum)
local positive_stride_baseline_stats = unijit.stats(native_positive_stride_sum)
assert(native_positive_stride_sum(30001) == positive_stride_sum(30001))
assert(unijit.wait(native_positive_stride_sum, 5000))
local positive_stride_stats = unijit.stats(native_positive_stride_sum)
assert(positive_stride_stats.active_tier == "optimized")
assert(positive_stride_stats.backedges == 10001)
assert(positive_stride_stats.input_ir_nodes >
       positive_stride_baseline_stats.input_ir_nodes)
for _, limit in ipairs({-7, 0, 1, 2, 3, 4, 17, 1000, 30002}) do
  assert(native_positive_stride_sum(limit) == positive_stride_sum(limit))
end

local negative_stride_sum = function(limit)
  local sum = 0
  for index = 30001, limit, -3 do
    sum = sum + index
  end
  return sum
end
local native_negative_stride_sum = unijit.compile(negative_stride_sum)
local negative_stride_baseline_stats = unijit.stats(native_negative_stride_sum)
assert(native_negative_stride_sum(1) == negative_stride_sum(1))
assert(unijit.wait(native_negative_stride_sum, 5000))
local negative_stride_stats = unijit.stats(native_negative_stride_sum)
assert(negative_stride_stats.active_tier == "optimized")
assert(negative_stride_stats.backedges == 10001)
assert(negative_stride_stats.input_ir_nodes >
       negative_stride_baseline_stats.input_ir_nodes)
for _, limit in ipairs({30008, 30002, 30001, 30000, 29998, 17, 0, -1000}) do
  assert(native_negative_stride_sum(limit) == negative_stride_sum(limit))
end

local parameter_start_sum = function(start, limit)
  local sum = 0
  for index = start, limit, 3 do
    sum = sum + index
  end
  return sum
end
local native_parameter_start_sum = unijit.compile(parameter_start_sum)
local parameter_start_baseline_stats = unijit.stats(native_parameter_start_sum)
assert(native_parameter_start_sum(0, 30000) ==
       parameter_start_sum(0, 30000))
assert(unijit.wait(native_parameter_start_sum, 5000))
local parameter_start_stats = unijit.stats(native_parameter_start_sum)
assert(parameter_start_stats.active_tier == "optimized")
assert(parameter_start_stats.backedges == 10001)
assert(parameter_start_stats.input_ir_nodes >
       parameter_start_baseline_stats.input_ir_nodes)
for _, bounds in ipairs({{7, -7}, {7, 7}, {7, 8}, {-17, 19},
                          {math.maxinteger - 17, math.maxinteger}}) do
  assert(native_parameter_start_sum(bounds[1], bounds[2]) ==
         parameter_start_sum(bounds[1], bounds[2]))
end

local reverse_parameter_start_sum = function(start, limit)
  local sum = 0
  for index = start, limit, -3 do
    sum = sum + index
  end
  return sum
end
local native_reverse_parameter_start_sum =
    unijit.compile(reverse_parameter_start_sum)
local reverse_parameter_start_baseline_stats =
    unijit.stats(native_reverse_parameter_start_sum)
assert(native_reverse_parameter_start_sum(30000, 0) ==
       reverse_parameter_start_sum(30000, 0))
assert(unijit.wait(native_reverse_parameter_start_sum, 5000))
local reverse_parameter_start_stats =
    unijit.stats(native_reverse_parameter_start_sum)
assert(reverse_parameter_start_stats.active_tier == "optimized")
assert(reverse_parameter_start_stats.backedges == 10001)
assert(reverse_parameter_start_stats.input_ir_nodes >
       reverse_parameter_start_baseline_stats.input_ir_nodes)
for _, bounds in ipairs({{-7, 7}, {-7, -7}, {-7, -8}, {19, -17},
                          {math.mininteger + 17, math.mininteger}}) do
  assert(native_reverse_parameter_start_sum(bounds[1], bounds[2]) ==
         reverse_parameter_start_sum(bounds[1], bounds[2]))
end

local parameter_step_sum = function(start, limit, step)
  local sum = 0
  for index = start, limit, step do
    sum = sum + index
  end
  return sum
end
local native_parameter_step_sum = unijit.compile(parameter_step_sum)
local parameter_step_baseline_stats = unijit.stats(native_parameter_step_sum)
assert(native_parameter_step_sum(0, 30000, 3) ==
       parameter_step_sum(0, 30000, 3))
assert(unijit.wait(native_parameter_step_sum, 5000))
local parameter_step_stats = unijit.stats(native_parameter_step_sum)
assert(parameter_step_stats.active_tier == "optimized")
assert(parameter_step_stats.backedges == 10001)
assert(parameter_step_stats.input_ir_nodes >
       parameter_step_baseline_stats.input_ir_nodes)
local parameter_step_cases = {
  {7, -7, 3},
  {7, 7, 3},
  {-17, 19, 3},
  {19, -17, -3},
  {-7, 7, -3},
  {math.maxinteger - 17, math.maxinteger, 3},
  {math.mininteger + 17, math.mininteger, -3},
  {math.mininteger, math.maxinteger, math.maxinteger},
  {math.maxinteger, math.mininteger, math.mininteger},
}
for _, values in ipairs(parameter_step_cases) do
  assert(native_parameter_step_sum(values[1], values[2], values[3]) ==
         parameter_step_sum(values[1], values[2], values[3]))
end
for remaining = 0, 20 do
  local positive_limit = remaining * 3
  assert(native_parameter_step_sum(0, positive_limit, 3) ==
         parameter_step_sum(0, positive_limit, 3))
  assert(native_parameter_step_sum(positive_limit, 0, -3) ==
         parameter_step_sum(positive_limit, 0, -3))
end
local invocations_before_zero_step =
    unijit.stats(native_parameter_step_sum).invocations
local original_zero_ok, original_zero_message =
    pcall(parameter_step_sum, 1, 10, 0)
local native_zero_ok, native_zero_message =
    pcall(native_parameter_step_sum, 1, 10, 0)
assert(not original_zero_ok and
       tostring(original_zero_message):find("'for' step is zero"))
assert(not native_zero_ok and
       tostring(native_zero_message):find("'for' step is zero"))
assert(unijit.stats(native_parameter_step_sum).invocations ==
       invocations_before_zero_step)

local unary_loop = function(count, seed)
  local value = seed
  for index = 1, count do
    value = -value
    value = ~value
  end
  return value
end
local native_unary_loop = unijit.compile(unary_loop)
for _, values in ipairs({{0, 0}, {1, 0}, {2, 1}, {17, -97},
                          {31, math.mininteger}, {32, math.maxinteger}}) do
  assert(native_unary_loop(table.unpack(values)) ==
         unary_loop(table.unpack(values)))
end
assert(native_unary_loop(10000, math.mininteger) ==
       unary_loop(10000, math.mininteger))
assert(unijit.wait(native_unary_loop, 5000))
assert(unijit.stats(native_unary_loop).active_tier == "optimized")
for _, values in ipairs({{0, math.mininteger}, {1, math.mininteger},
                          {9999, math.maxinteger}, {10000, -1}}) do
  assert(native_unary_loop(table.unpack(values)) ==
         unary_loop(table.unpack(values)))
end

local bitwise_loop = function(count, seed, mask)
  local value = seed
  for index = 1, count do
    value = (value ~ index) & mask
    value = value | (seed & index)
  end
  return value
end
local native_bitwise_loop = unijit.compile(bitwise_loop)
local bitwise_loop_cases = {
  {0, 0, -1}, {1, 0, -1}, {17, -97, 0x55555555},
  {31, math.mininteger, math.maxinteger},
  {32, math.maxinteger, -1},
}
for _, values in ipairs(bitwise_loop_cases) do
  assert(native_bitwise_loop(table.unpack(values)) ==
         bitwise_loop(table.unpack(values)))
end
assert(native_bitwise_loop(10000, math.mininteger, math.maxinteger) ==
       bitwise_loop(10000, math.mininteger, math.maxinteger))
assert(unijit.wait(native_bitwise_loop, 5000))
assert(unijit.stats(native_bitwise_loop).active_tier == "optimized")
for _, values in ipairs(bitwise_loop_cases) do
  assert(native_bitwise_loop(table.unpack(values)) ==
         bitwise_loop(table.unpack(values)))
end

local shift_loop = function(count, seed, amount)
  local value = seed
  for index = 1, count do
    value = (value << amount) ~ (value >> index)
    value = value | (7 << (amount - index))
    value = value >> -3
  end
  return value
end
local native_shift_loop = unijit.compile(shift_loop)
local shift_loop_cases = {
  {0, 0, 0}, {1, -1, 63}, {2, -1, 64}, {17, -97, -3},
  {31, math.mininteger, -64}, {32, math.maxinteger, 65},
  {33, -1, math.mininteger},
}
for _, values in ipairs(shift_loop_cases) do
  assert(native_shift_loop(table.unpack(values)) ==
         shift_loop(table.unpack(values)))
end
assert(native_shift_loop(10000, math.mininteger, 7) ==
       shift_loop(10000, math.mininteger, 7))
assert(unijit.wait(native_shift_loop, 5000))
assert(unijit.stats(native_shift_loop).active_tier == "optimized")
for _, values in ipairs(shift_loop_cases) do
  assert(native_shift_loop(table.unpack(values)) ==
         shift_loop(table.unpack(values)))
end

local floor_loop = function(count, seed, divisor)
  local value = seed
  for index = 1, count do
    value = (value // divisor) + (index % 7)
  end
  return value
end
local native_floor_loop = unijit.compile(floor_loop)
local floor_loop_cases = {
  {0, -123, 5}, {1, -123, 5}, {17, -1234567, 5},
  {33, math.mininteger, -7}, {71, math.maxinteger, 11},
}
for _, values in ipairs(floor_loop_cases) do
  assert(native_floor_loop(table.unpack(values)) ==
         floor_loop(table.unpack(values)))
end
assert(native_floor_loop(10000, math.mininteger, 7) ==
       floor_loop(10000, math.mininteger, 7))
assert(unijit.wait(native_floor_loop, 5000))
assert(unijit.stats(native_floor_loop).active_tier == "optimized")
for _, values in ipairs(floor_loop_cases) do
  assert(native_floor_loop(table.unpack(values)) ==
         floor_loop(table.unpack(values)))
end
floor_ok, floor_message = pcall(native_floor_loop, 1, 7, 0)
assert(not floor_ok and tostring(floor_message):find("divide by zero"))
checkpoint("integer floor loop")

checkpoint("numeric loops")

)lua"
R"lua(

local guarded_body_sum = function(start, limit, step, threshold)
  local sum = 7
  for index = start, limit, step do
    if index < threshold then
      sum = sum + index * 2
    end
    sum = sum + 1
  end
  return sum
end
local native_guarded_body_sum = unijit.compile(guarded_body_sum)
local guarded_body_cases = {
  {1, 20, 1, 9},
  {20, 1, -1, 9},
  {-20, 20, 3, 0},
  {20, -20, -3, 0},
  {7, -7, 3, 100},
  {7, -7, -3, -100},
  {math.maxinteger - 6, math.maxinteger, 2, math.maxinteger},
  {math.mininteger + 6, math.mininteger, -2, math.mininteger + 3},
}
for _, values in ipairs(guarded_body_cases) do
  assert(native_guarded_body_sum(table.unpack(values)) ==
         guarded_body_sum(table.unpack(values)))
end
assert(native_guarded_body_sum(1, 12000, 1, 6000) ==
       guarded_body_sum(1, 12000, 1, 6000))
assert(unijit.wait(native_guarded_body_sum, 5000))
local guarded_body_stats = unijit.stats(native_guarded_body_sum)
assert(guarded_body_stats.active_tier == "optimized")
assert(guarded_body_stats.loop)
assert(guarded_body_stats.successful_compilations == 1)
for _, values in ipairs(guarded_body_cases) do
  assert(native_guarded_body_sum(table.unpack(values)) ==
         guarded_body_sum(table.unpack(values)))
end

local guarded_break_sum = function(start, limit, step, stop)
  local sum = 0
  for index = start, limit, step do
    sum = sum + 3
    if index >= stop then
      break
    end
    sum = sum + index
  end
  return sum
end
local native_guarded_break_sum = unijit.compile(guarded_break_sum)
local precise_guarded_break_sum = unijit.compile(guarded_break_sum)
assert(precise_guarded_break_sum(1, 20000, 1, 9) ==
       guarded_break_sum(1, 20000, 1, 9))
local precise_break_stats = unijit.stats(precise_guarded_break_sum)
assert(precise_break_stats.invocations == 1)
assert(precise_break_stats.backedges == 9)
assert(precise_break_stats.active_tier == "baseline")
local guarded_break_cases = {
  {1, 20, 1, 9},
  {1, 20, 3, 10},
  {20, 1, -1, 9},
  {20, 1, -3, 10},
  {-20, 20, 3, 0},
  {7, -7, 3, 0},
  {math.maxinteger - 6, math.maxinteger, 2, math.maxinteger - 1},
}
for _, values in ipairs(guarded_break_cases) do
  assert(native_guarded_break_sum(table.unpack(values)) ==
         guarded_break_sum(table.unpack(values)))
end

local guarded_return_sum = function(start, limit, step, stop)
  local sum = 11
  for index = start, limit, step do
    if index == stop then
      return sum
    end
    sum = sum + index
  end
  return sum
end
local native_guarded_return_sum = unijit.compile(guarded_return_sum)
local precise_guarded_return_sum = unijit.compile(guarded_return_sum)
assert(precise_guarded_return_sum(1, 20000, 1, 9) ==
       guarded_return_sum(1, 20000, 1, 9))
local precise_return_stats = unijit.stats(precise_guarded_return_sum)
assert(precise_return_stats.invocations == 1)
assert(precise_return_stats.backedges == 9)
assert(precise_return_stats.active_tier == "baseline")
local guarded_return_cases = {
  {1, 20, 1, 9},
  {1, 20, 3, 10},
  {20, 1, -1, 9},
  {20, 1, -3, 10},
  {-20, 20, 3, 1},
  {7, -7, 3, 0},
  {math.mininteger + 6, math.mininteger, -2, math.mininteger + 2},
}
for _, values in ipairs(guarded_return_cases) do
  local actual = native_guarded_return_sum(table.unpack(values))
  local expected = guarded_return_sum(table.unpack(values))
  assert(actual == expected,
         table.concat(values, ",") .. ": " .. tostring(actual) ..
         " != " .. tostring(expected))
end

local immediate_guard_sum = function(count)
  local sum = 0
  for index = 1, count do
    if index <= 4 then
      sum = sum + index
    end
  end
  return sum
end
local native_immediate_guard_sum = unijit.compile(immediate_guard_sum)
for count = -3, 20 do
  assert(native_immediate_guard_sum(count) == immediate_guard_sum(count))
end

local not_equal_guard_sum = function(count, skipped)
  local sum = 0
  for index = 1, count do
    if index ~= skipped then
      sum = sum + index
    end
  end
  return sum
end
local native_not_equal_guard_sum = unijit.compile(not_equal_guard_sum)
for _, values in ipairs({{0, 4}, {1, 1}, {10, 4}, {10, 1000}}) do
  assert(native_not_equal_guard_sum(table.unpack(values)) ==
         not_equal_guard_sum(table.unpack(values)))
end

local greater_immediate_sum = function(count)
  local sum = 0
  for index = 1, count do
    if index > 4 then
      sum = sum + index
    end
  end
  return sum
end
local native_greater_immediate_sum = unijit.compile(greater_immediate_sum)
for count = -3, 20 do
  assert(native_greater_immediate_sum(count) == greater_immediate_sum(count))
end

local less_than_break = function(start, limit, step, stop)
  local visits = 0
  for index = start, limit, step do
    if index < stop then
      break
    end
    visits = visits + 1
  end
  return visits
end
local native_less_than_break = unijit.compile(less_than_break)
for _, values in ipairs({{10, 1, -1, 4}, {1, 10, 1, 4},
                          {10, 1, -3, -100}, {1, 10, 3, 100}}) do
  assert(native_less_than_break(table.unpack(values)) ==
         less_than_break(table.unpack(values)))
end

local guarded_zero_ok, guarded_zero_message =
    pcall(native_guarded_body_sum, 1, 10, 0, 5)
assert(not guarded_zero_ok and
       tostring(guarded_zero_message):find("'for' step is zero"))

checkpoint("guarded loops")

local near_max_stride = function(limit)
  local visits = 0
  for index = 9223372036854775790, limit, 3 do
    visits = visits + 1
  end
  return visits
end
local native_near_max_stride = unijit.compile(near_max_stride)
promote_loop(native_near_max_stride, near_max_stride, math.maxinteger)
for _, limit in ipairs({math.maxinteger - 18, math.maxinteger - 17,
                        math.maxinteger - 16, math.maxinteger - 2,
                        math.maxinteger}) do
  assert(native_near_max_stride(limit) == near_max_stride(limit))
end

local near_min_stride = function(limit)
  local visits = 0
  for index = -9223372036854775791, limit, -3 do
    visits = visits + 1
  end
  return visits
end
local native_near_min_stride = unijit.compile(near_min_stride)
promote_loop(native_near_min_stride, near_min_stride, math.mininteger)
for _, limit in ipairs({math.mininteger + 18, math.mininteger + 17,
                        math.mininteger + 16, math.mininteger + 2,
                        math.mininteger}) do
  assert(native_near_min_stride(limit) == near_min_stride(limit))
end

local huge_positive_stride = function(limit)
  local visits = 0
  for index = (-9223372036854775807 - 1), limit, 9223372036854775807 do
    visits = visits + 1
  end
  return visits
end
local native_huge_positive_stride = unijit.compile(huge_positive_stride)
promote_loop(native_huge_positive_stride, huge_positive_stride,
             math.maxinteger)
for _, limit in ipairs({math.mininteger, -2, -1,
                        math.maxinteger - 1, math.maxinteger}) do
  assert(native_huge_positive_stride(limit) == huge_positive_stride(limit))
end

local minimum_stride = function(limit)
  local visits = 0
  for index = 9223372036854775807, limit, (-9223372036854775807 - 1) do
    visits = visits + 1
  end
  return visits
end
local native_minimum_stride = unijit.compile(minimum_stride)
promote_loop(native_minimum_stride, minimum_stride, math.mininteger)
for _, limit in ipairs({math.maxinteger, 0, -1, math.mininteger}) do
  assert(native_minimum_stride(limit) == minimum_stride(limit))
end

checkpoint("boundary loops")

assert(native_recurrence(9, 3, 5, "extra arguments are ignored") == 48)

local ok, message = pcall(native_recurrence, 1, 2.5, 3)
assert(not ok and tostring(message):find("integer"))

ok, message = pcall(native_recurrence, 1, 2)
assert(not ok and tostring(message):find("requires"))

ok, message = pcall(unijit.compile, function(value)
  if value > 0 then
    return value
  end
  return -value
end)
assert(not ok and tostring(message):find("unsupported Lua 5.5 opcode"))

ok, message = pcall(unijit.compile, function(value)
  return value / 2
end)
assert(not ok and tostring(message):find("unsupported Lua 5.5 opcode"))

ok, message = pcall(unijit.compile, function(...)
  return 1
end)
assert(not ok and tostring(message):find("vararg"))

ok, message = pcall(unijit.compile, function(count)
  local sum = 0
  for index = 1, count, 0 do
    sum = sum + index
  end
  return sum
end)
assert(not ok and tostring(message):find("step cannot be zero"))

ok, message = pcall(unijit.compile, function(count)
  local sum = 0
  for index = 1, count do
    sum = sum + index
  end
  for index = 1, count do
    sum = sum + index
  end
  return sum
end)
assert(not ok and tostring(message):find("only one numeric for loop"))

ok, message = pcall(unijit.compile, function(count, first, second)
  local sum = 0
  for index = 1, count do
    if index < first then
      sum = sum + index
    end
    if index > second then
      break
    end
  end
  return sum
end)
assert(not ok and tostring(message):find("only one guarded condition"))

local conditional_branch_sum = function(start, limit, step, threshold)
  local sum = 7
  for index = start, limit, step do
    if index < threshold then
      sum = sum + index * 3
    else
      sum = sum - index * 2
    end
    sum = sum + 1
  end
  return sum
end
local native_conditional_branch_sum = unijit.compile(conditional_branch_sum)
local conditional_branch_cases = {
  {1, 20, 1, 9},
  {20, 1, -1, 9},
  {-20, 20, 3, 0},
  {20, -20, -3, 0},
  {7, -7, 3, 100},
  {7, -7, -3, -100},
  {math.maxinteger - 6, math.maxinteger, 2, math.maxinteger},
  {math.mininteger + 6, math.mininteger, -2, math.mininteger + 3},
}
for _, values in ipairs(conditional_branch_cases) do
  assert(native_conditional_branch_sum(table.unpack(values)) ==
         conditional_branch_sum(table.unpack(values)))
end
assert(native_conditional_branch_sum(1, 12000, 1, 6000) ==
       conditional_branch_sum(1, 12000, 1, 6000))
assert(unijit.wait(native_conditional_branch_sum, 5000))
assert(unijit.stats(native_conditional_branch_sum).active_tier == "optimized")
for _, values in ipairs(conditional_branch_cases) do
  assert(native_conditional_branch_sum(table.unpack(values)) ==
         conditional_branch_sum(table.unpack(values)))
end

local conditional_equality_sum = function(count, skipped)
  local sum = 0
  for index = 1, count do
    if index ~= skipped then
      sum = sum + index
    else
      sum = sum - 100
    end
  end
  return sum
end
local native_conditional_equality_sum =
    unijit.compile(conditional_equality_sum)
for _, values in ipairs({{0, 1}, {1, 1}, {10, 4}, {10, 1000}}) do
  assert(native_conditional_equality_sum(table.unpack(values)) ==
         conditional_equality_sum(table.unpack(values)))
end
assert(native_conditional_equality_sum(12000, 6000) ==
       conditional_equality_sum(12000, 6000))
assert(unijit.wait(native_conditional_equality_sum, 5000))
assert(unijit.stats(native_conditional_equality_sum).active_tier == "optimized")
for _, values in ipairs({{0, 1}, {1, 1}, {10, 4}, {10, 1000}}) do
  assert(native_conditional_equality_sum(table.unpack(values)) ==
         conditional_equality_sum(table.unpack(values)))
end
checkpoint("integer conditional branches")

ok = pcall(unijit.compile, math.abs)
assert(not ok)

ok = pcall(unijit.compile, 42)
assert(not ok)

checkpoint("rejections")

local disposable = unijit.compile(function(value)
  return value + 1
end)
local _, owner = debug.getupvalue(disposable, 1)
local owner_metatable = debug.getmetatable(owner)
owner_metatable.__gc(owner)
owner_metatable.__gc(owner)
ok, message = pcall(disposable, 1)
assert(not ok and tostring(message):find("invalid UniJIT compiled function"))

checkpoint("explicit finalization")

immediate = nil
native_recurrence = nil
cached_recurrence = nil
native_float_recurrence = nil
cached_float_recurrence = nil
backedge_counted_sum = nil
native_counted_sum = nil
native_offset_sum = nil
native_near_max_loop = nil
native_positive_stride_sum = nil
native_negative_stride_sum = nil
native_parameter_start_sum = nil
native_reverse_parameter_start_sum = nil
native_parameter_step_sum = nil
native_shift_left = nil
native_shift_loop = nil
native_floor_divide = nil
native_floor_modulo = nil
native_constant_zero = nil
native_floor_loop = nil
native_guarded_body_sum = nil
native_guarded_break_sum = nil
precise_guarded_break_sum = nil
native_guarded_return_sum = nil
precise_guarded_return_sum = nil
native_immediate_guard_sum = nil
native_not_equal_guard_sum = nil
native_greater_immediate_sum = nil
native_less_than_break = nil
native_conditional_branch_sum = nil
native_conditional_equality_sum = nil
native_near_max_stride = nil
native_near_min_stride = nil
native_huge_positive_stride = nil
native_minimum_stride = nil
disposable = nil
collectgarbage("collect")
checkpoint("garbage collection")
)lua";

} // namespace

int main() {
  lua_State *state = luaL_newstate();
  if (state == nullptr) {
    std::cerr << "unable to create Lua state\n";
    return EXIT_FAILURE;
  }

  luaL_openlibs(state);
  luaL_requiref(state, "unijit", luaopen_unijit, 1);
  lua_pop(state, 1);

  int status = luaL_loadbuffer(state, kTestProgram, sizeof(kTestProgram) - 1,
                               "@lua_frontend_test.lua");
  if (status == LUA_OK) {
    status = lua_pcall(state, 0, 0, 0);
  }
  if (status != LUA_OK) {
    const char *message = lua_tostring(state, -1);
    std::cerr << (message == nullptr ? "unknown Lua test failure" : message)
              << '\n';
  }

  lua_close(state);
  if (status != LUA_OK) {
    return EXIT_FAILURE;
  }
  std::cout << "Lua 5.5 frontend tests passed\n";
  return EXIT_SUCCESS;
}
