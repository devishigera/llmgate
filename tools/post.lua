-- Cache-HIT benchmark: every request has the same body, so after the first
-- one the answer comes from the cache.
wrk.method = "POST"
wrk.headers["Content-Type"] = "application/json"
wrk.headers["Authorization"] = "Bearer lg_live_testkey"
wrk.body = '{"model":"mock","messages":[{"role":"user","content":"hello"}]}'