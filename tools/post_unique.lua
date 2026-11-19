-- Cache-MISS benchmark: every request has a different prompt, so every
-- request goes through the circuit breaker to the provider.
-- wrk runs this script once per thread, so the thread number is part of the
-- prompt to keep prompts unique across threads.
local counter = 0
local next_id = 0

-- Runs once per thread in the main script; gives each thread its own number.
-- `thread_id` becomes a global variable inside that thread.
function setup(thread)
  thread:set("thread_id", next_id)
  next_id = next_id + 1
end

request = function()
  counter = counter + 1
  local body = string.format(
    '{"model":"mock","messages":[{"role":"user","content":"hello %d-%d"}]}',
    thread_id, counter)
  return wrk.format("POST", "/v1/chat", {
    ["Content-Type"] = "application/json",
    ["Authorization"] = "Bearer lg_live_testkey",
  }, body)
end