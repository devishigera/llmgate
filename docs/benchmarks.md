# Benchmarks

All numbers were measured on the machine below and describe this setup only. They are not a claim about other hardware.

## Machine

- Host: MacBook Air M2, 8 GB RAM
- VM: Ubuntu 24.04 ARM64 in VMware Fusion, 4 vCPUs, 3.8 GB RAM
- Build: Release (`-DCMAKE_BUILD_TYPE=Release`)
- Server: LLMGate commit `fc1f43b`, httplib (one worker thread per connection), mock provider with 0 ms latency
- Config: `config/bench.example.json` (rate limit 1,000,000 per second, cache capacity 10,000)
- Load generator: wrk, running on the same VM as the server, so both compete for the same 4 CPUs
- Date: 2026-11-19

## Method

    tools/loadtest.sh

Each run is two tests of 30 seconds, using 2 wrk threads and 50 connections:

    wrk -t2 -c50 -d30s --latency -s tools/post.lua http://127.0.0.1:8080/v1/chat
    wrk -t2 -c50 -d30s --latency -s tools/post_unique.lua http://127.0.0.1:8080/v1/chat

- `post.lua` sends the same prompt every time, so after the first request the answer comes from the cache (cache hit).
- `post_unique.lua` sends a different prompt every time, so every request goes to the provider (cache miss).

The whole script was run 3 times. "Non-2xx or 3xx responses" did not appear in any run, so all requests succeeded and the numbers are valid.

## Results: cache hit

| Run | Requests/sec | p50 | p75 | p90 | p99 |
|-----|--------------|-----|-----|-----|-----|
| 1 | 132,233 | 82 µs | 14.33 ms | 24.91 ms | 32.70 ms |
| 2 | 124,058 | 89 µs | 15.34 ms | 26.87 ms | 53.92 ms |
| 3 | 130,629 | 85 µs | 14.74 ms | 25.20 ms | 33.88 ms |

Mean about 129,000 requests/sec; range 124,058 to 132,233 (about 6%).

## Results: cache miss

| Run | Requests/sec | p50 | p75 | p90 | p99 |
|-----|--------------|-----|-----|-----|-----|
| 1 | 124,369 | 85 µs | 14.86 ms | 26.26 ms | 35.22 ms |
| 2 | 122,209 | 89 µs | 15.59 ms | 26.75 ms | 35.76 ms |
| 3 | 124,563 | 89 µs | 15.58 ms | 26.56 ms | 37.23 ms |

Mean about 123,700 requests/sec; range 122,209 to 124,563 (about 2%).

## Notes

- **Cache hit vs miss.** Cache hits were about 4% faster than misses, which is within the run-to-run spread of the hit test. The mock provider answers instantly, so a miss costs only a few extra microseconds. These results do not show a speed benefit from the cache. That benefit appears only when the provider is slow, and it has not been measured yet.
- **Latency shape.** The median is about 85 µs, but p75 is about 15 ms and p99 is 33 to 54 ms. A likely explanation is the thread-per-connection design: the server has far fewer worker threads than the 50 connections, so some connections wait for a free worker. This has not been confirmed yet. A run with fewer connections (`CONNECTIONS=8 tools/loadtest.sh`) should show whether the tail latency goes away.
- **Outlier.** Run 2 of the cache-hit test had a maximum latency of 1.75 s and the highest p99 (53.92 ms). The cause was not investigated; it is probably other activity on the VM.
- **Shared CPUs.** wrk and the server ran on the same VM, so the real capacity of the server alone is likely somewhat higher than these numbers.
- **Baseline for Phase 3.** These are the numbers for the httplib server. The epoll server planned for Phase 3 will be measured with the same script on the same VM.