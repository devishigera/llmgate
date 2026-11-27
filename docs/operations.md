# Operating LLMGate (Phase 2)

This page covers what you need to run the gateway and read what it tells you.

## Endpoints

| Route | Auth | Purpose |
|---|---|---|
| `POST /v1/chat` | Bearer key | Send a chat request |
| `GET /v1/usage` | Bearer key | `{"used_today": N, "daily_budget": M}` for the calling key |
| `GET /health` | none | Liveness check |
| `GET /metrics` | none | Prometheus text format |

**`/metrics` and `/health` are unauthenticated.** Keep the gateway on a private network, or put a proxy in front that only lets your Prometheus server reach `/metrics`. Metric labels only ever contain config values (team names, provider names) and status codes, never keys or prompts.

## Request pipeline

auth (401) -> validate (400) -> rate limit (429 + `Retry-After`) -> daily budget (429 `budget_exceeded`, retry after UTC midnight) -> cache -> provider router.

The router tries providers in priority order. Each provider has its own circuit breaker, and transient failures are retried with exponential backoff and jitter. If a provider keeps failing, the next one is used (failover).

| Status | Meaning |
|---|---|
| 502 `provider_unavailable` | Every provider is down or its breaker is open |
| 502 `provider_error` | A provider returned a permanent error |
| 504 `provider_timeout` | The provider timed out |

Cache hits cost 0 tokens. Tokens are counted against the budget only after a provider succeeds. Budgets reset at UTC midnight and are in memory only until the write-ahead log arrives in Phase 3.

## Logging

One JSON object per line on **stdout**. Startup messages go to **stderr**.

Set the level with `LLMGATE_LOG_LEVEL`: `debug`, `info` (default), `warn`, `error` or `off`. Anything else exits with code 2.

Each `/v1/chat` request writes one line:

```json
{"ts":"2026-11-27T08:46:31.385Z","level":"info","msg":"request","request_id":"f65540aa87b06025","team":"claims-team","route":"/v1/chat","status":200,"latency_ms":3.733,"cache_hit":false,"provider":"mock"}
```

- `level` is `info` below 400, `warn` for 4xx and `error` for 5xx.
- `team` and `provider` are `null` when they do not apply (for example a wrong key).
- The same `request_id` is returned to the client in the `x-request-id` response header, so a user can quote it when reporting a problem.
- A failing provider call logs `provider_failed` with the provider name, `transient` and `timeout`. The error text is never logged.
- API keys, key hashes and prompts are never logged. `/health` and `/metrics` are not logged.

Useful one-liners:

```
./build-macos/llmgate --config config/ollama.local.json > gw.log
grep -c lg_live gw.log
```

The second command must print `0`.

## Metrics

| Metric | Labels |
|---|---|
| `llmgate_requests_total` | `status` |
| `llmgate_request_duration_seconds` | histogram |
| `llmgate_cache_hits_total`, `llmgate_cache_misses_total` | |
| `llmgate_rate_limited_total`, `llmgate_budget_rejected_total` | |
| `llmgate_provider_duration_seconds` | `provider` (histogram) |
| `llmgate_provider_errors_total` | `provider` |
| `llmgate_breaker_state` | `provider` (0 closed, 1 open, 2 half-open) |
| `llmgate_failovers_total` | |
| `llmgate_tokens_total` | `team` |

## Load testing

`tools/loadtest.sh` starts the server with `LLMGATE_LOG_LEVEL=off`, because writing a line per request at benchmark speed would distort the numbers.
