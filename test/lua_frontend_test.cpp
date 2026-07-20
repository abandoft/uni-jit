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

local function compare(original, arguments)
  local native = unijit.compile(original)
  for _, values in ipairs(arguments) do
    assert(native(table.unpack(values)) == original(table.unpack(values)))
  end
  return native
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

local float_ok, float_message = pcall(native_float_recurrence, 1.0, 2, 3.0)
assert(not float_ok and tostring(float_message):find("Float64"))

float_ok, float_message = pcall(unijit.compile_float, function()
  return 17
end)
assert(not float_ok and tostring(float_message):find("not Float64"))

local immediate = compare(function(value)
  return value + 7
end, {{0}, {-100}, {math.maxinteger}})

compare(function(value)
  return value * 400 - 900
end, {{0}, {17}, {-9123456}, {math.maxinteger}})

compare(function(value)
  local large = 9223372036854770000
  return value + large
end, {{0}, {807}, {-123456789}})

compare(function()
  return 17
end, {{}})

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
  for index = 1, count, 2 do
    sum = sum + index
  end
  return sum
end)
assert(not ok and tostring(message):find("step 1"))

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

ok = pcall(unijit.compile, math.abs)
assert(not ok)

ok = pcall(unijit.compile, 42)
assert(not ok)

local disposable = unijit.compile(function(value)
  return value + 1
end)
local _, owner = debug.getupvalue(disposable, 1)
local owner_metatable = debug.getmetatable(owner)
owner_metatable.__gc(owner)
owner_metatable.__gc(owner)
ok, message = pcall(disposable, 1)
assert(not ok and tostring(message):find("invalid UniJIT compiled function"))

immediate = nil
native_recurrence = nil
cached_recurrence = nil
native_float_recurrence = nil
cached_float_recurrence = nil
backedge_counted_sum = nil
native_counted_sum = nil
native_offset_sum = nil
native_near_max_loop = nil
disposable = nil
collectgarbage("collect")
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
