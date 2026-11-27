# LLMGate: A High-Performance LLM Gateway in C++

> A small, fast, secure proxy that sits between applications (or AI agents) and Large Language Model providers. It decides **who** may call the model, **how often**, **how much they can spend**, and **what happens when the provider fails**.

This README is written for you, the builder. It starts from the very basics and walks through what the project is, why it exists, how every part works, which technologies we use, and the exact order to build it in. Read it top to bottom once, then use it as your build manual.

---

## Table of Contents

1. [The Basics: Concepts You Need First](#1-the-basics-concepts-you-need-first)
2. [What Is LLMGate?](#2-what-is-llmgate)
3. [Why Build This?](#3-why-build-this)
4. [Features](#4-features)
5. [Architecture](#5-architecture)
6. [Life of a Request](#6-life-of-a-request)
7. [Core Components in Depth](#7-core-components-in-depth)
8. [Technology Stack](#8-technology-stack)
9. [Project Structure](#9-project-structure)
10. [API Specification](#10-api-specification)
11. [Configuration](#11-configuration)
12. [Development Environment Setup](#12-development-environment-setup)
13. [Build Roadmap (12 Weeks)](#13-build-roadmap-12-weeks)
14. [Testing Strategy](#14-testing-strategy)
15. [DevOps: Docker, CI/CD, Kubernetes, Terraform](#15-devops-docker-cicd-kubernetes-terraform)
16. [Observability and SLOs](#16-observability-and-slos)
17. [Security Design](#17-security-design)
18. [Benchmarking](#18-benchmarking)
19. [Common Pitfalls](#19-common-pitfalls)
20. [Resume Bullets and Interview Prep](#20-resume-bullets-and-interview-prep)
21. [Glossary](#21-glossary)
22. [Learning Resources](#22-learning-resources)

---

## 1. The Basics: Concepts You Need First

If you already know these, skip ahead. If not, spend an hour here. Everything later builds on them.

### 1.1 What is an LLM?
A **Large Language Model** (ChatGPT, Claude, Llama, Gemini) is a program that takes text in (a **prompt**) and produces text out (a **completion**). You usually use one by sending a request over the internet to a company's server (a **provider**), or by running an open model on your own machine (for example with **Ollama**).

### 1.2 What is an API?
An **API** (Application Programming Interface) is a way for one program to talk to another. Most LLM providers expose a **REST API over HTTP**: you send a JSON message to a URL, and you get a JSON message back.

Example request body:
```json
{
  "model": "llama3",
  "messages": [{"role": "user", "content": "Explain TCP in one sentence."}]
}
```

### 1.3 What are tokens?
LLMs do not read characters or words. They read **tokens** (chunks of text, roughly 3 to 4 characters in English). Providers **bill by tokens** and enforce limits in tokens. So "how much did this request cost?" means "how many tokens did it use?".

### 1.4 What is HTTP, and what sits under it?
HTTP is a text protocol: a client sends a request line (`POST /v1/chat HTTP/1.1`), headers, and a body; the server replies with a status code (`200`, `401`, `429`, `502`), headers, and a body. HTTP rides on **TCP**, which gives a reliable byte stream between two machines. Each connection is a **socket**. DNS turns names like `api.example.com` into IP addresses.

### 1.5 What is a proxy / gateway?
A **gateway** is a server that stands in front of other servers. Clients talk to the gateway; the gateway talks to the real service. Because everything flows through it, the gateway is the perfect place to add **authentication, limits, caching, logging, and safety**, once, for everyone.

### 1.6 What is DevOps?
DevOps is the practice (and tooling) of **building, testing, shipping, and running software reliably**. Core ideas:
- **Containers (Docker):** package a program with everything it needs so it runs the same everywhere.
- **CI/CD:** automatically build, test, and deploy every code change.
- **Kubernetes (K8s):** a system that runs many containers, restarts them when they crash, and scales them.
- **Infrastructure as Code (Terraform):** describe cloud servers in files instead of clicking in a console.
- **Observability:** metrics, logs, and traces that tell you what the system is doing.
- **SRE / SLOs:** measure reliability with numbers (for example "99% of requests finish in under 500 ms").

---

## 2. What Is LLMGate?

**LLMGate is a C++ service that every LLM request in an organization passes through.**

Imagine a company where 50 teams and many AI agents all want to call an LLM:

- Without a gateway: each team stores its own provider key, nobody knows who is spending what, one buggy loop can burn the whole budget in an hour, and if the provider goes down every team's app breaks.
- With LLMGate: teams get their own gateway keys, each key has a **rate limit** and a **daily token budget**, repeated questions are **served from cache** for free, and if one provider fails the gateway **fails over** to another.

In one sentence: **LLMGate is to LLM calls what a bouncer, accountant, and backup generator are to a nightclub.**

### Deployment model: one hosted server, any client

LLMGate is **not** a program that users install on their own computers. It is a **centrally hosted server application**:

- It is **developed** primarily on macOS (Apple Silicon), with Linux-specific parts built and tested in Ubuntu (ARM64).
- It is **deployed** on a **Linux server** and exposed as an **HTTP/HTTPS API**.
- It is **used** over the network by clients running any operating system.

```
 Windows client ──┐
 macOS client  ───┼──  HTTP / HTTPS  ──►  LLMGate (Linux server)  ──►  LLM providers
 Linux client  ───┘
```

Clients do **not** need a native LLMGate build for their operating system. They only send HTTP requests, and HTTP is the same on every OS: `curl`, Python `requests`, a browser, or an AI-agent framework all work identically from Windows, macOS and Linux. The only operating system LLMGate itself must support is **Linux**; macOS is simply the development workstation. For that reason this project contains no native Windows or macOS application work.

---

## 3. Why Build This?

| Reason | Detail |
|---|---|
| **Real industry problem** | Companies running AI at scale all build "LLM gateways" (cost control, routing, safety). It is infrastructure, not a toy app. |
| **Exercises the fundamentals** | Threads, sockets, locks, data structures, networking, and the Linux process model are used for real, not just read about. |
| **Covers the full stack of platform skills** | Backend service + containers + CI/CD + Kubernetes + IaC + observability + security + AI. |
| **Backend only** | No frontend needed. You test it with `curl`. |
| **Measurable** | You can benchmark it and put real numbers on a resume (requests/sec, p95 latency, cache hit rate). |

---

## 4. Features

### Core (MVP)
- REST endpoint `POST /v1/chat` that is OpenAI-style compatible.
- **API key authentication** (keys stored hashed, compared in constant time).
- **Per-key rate limiting** (token bucket, written from scratch).
- **Per-key daily token budget** with usage tracking.
- **Response cache** (LRU with TTL, written from scratch).
- **Provider abstraction** with a **mock provider** (for tests) and a **real provider** (Ollama or a hosted API).
- **Circuit breaker** and **failover** between providers.
- `/health` and `/metrics` (Prometheus text format, written by hand).
- Unit tests, Dockerfile, CI pipeline.

### Production-grade (later phases)
- Event-driven server using **epoll** (Linux-only; developed and tested in Ubuntu) plus worker thread pool, benchmarked against a simpler threaded design.
- **Streaming responses** (Server-Sent Events).
- Timeouts, **retries with exponential backoff and jitter**, **graceful shutdown**.
- **Write-ahead log (WAL)** so budgets survive crashes.
- **Structured logging**, Grafana dashboards, **SLOs and alerts**.
- **Chaos tests** (kill provider, inject latency).
- **Kubernetes, Helm, Terraform**, canary deploys with auto-rollback.
- **Sanitizers (ASan/TSan/UBSan)**, vulnerability scanning, image signing.
- **Semantic cache** (embedding similarity search).
- **Prompt-injection and PII filter**.
- **Evaluation harness** for scoring model responses.

---

## 5. Architecture

```
                         +--------------------------------------------------+
                         |                    LLMGate                       |
  Client / AI agent      |                                                  |
  -------------------->  |  [1] HTTP Server (epoll + worker thread pool)    |
   POST /v1/chat         |        |                                         |
   Authorization: Bearer |        v                                         |
                         |  [2] Auth  ---- invalid ----> 401                |
                         |        |                                         |
                         |        v                                         |
                         |  [3] Rate Limiter (token bucket) -- over --> 429 |
                         |        |                                         |
                         |        v                                         |
                         |  [4] Budget Check (daily tokens) -- over --> 429 |
                         |        |                                         |
                         |        v                                         |
                         |  [5] Safety Filter (PII / injection)  (phase 9)  |
                         |        |                                         |
                         |        v                                         |
                         |  [6] Cache (LRU + TTL)  -- hit --> return        |
                         |        | miss                                    |
                         |        v                                         |
                         |  [7] Router + Circuit Breakers                   |
                         |        |             |                           |
                         |        v             v                           |
                         |   Provider A     Provider B (failover)           |
                         |   (Ollama)       (hosted API / mock)             |
                         |        |             |                           |
                         |        +------+------+                           |
                         |               v                                  |
                         |  [8] Usage accounting + WAL + Metrics + Logs     |
                         +---------------|----------------------------------+
                                         v
                                  Response to client

  Side endpoints: GET /health   GET /metrics
  Operated by:    Docker -> GitHub Actions -> Kubernetes -> Prometheus/Grafana
```

---

## 6. Life of a Request

Follow one request from start to finish. This is also a great way to explain the project in an interview.

1. **Connection accepted.** The server accepts a TCP connection and parses the HTTP request (method, path, headers, body). Oversized bodies are rejected early (security).
2. **Authentication.** Read the `Authorization: Bearer <key>` header. Hash the key (SHA-256) and look it up in the key store. Unknown key returns `401`.
3. **Rate limit.** Each key owns a **token bucket**: it holds up to N tokens and refills at R per second. A request needs 1 token. If the bucket is empty, return `429 Too Many Requests` with a `Retry-After` header.
4. **Budget check.** Each key has a daily LLM-token allowance. If it is already exhausted, return `429` with an explanatory error code.
5. **Safety filter** (later phase). Redact or block PII; flag obvious prompt-injection patterns.
6. **Cache lookup.** Build a cache key from `hash(model + normalized messages + parameters)`. On a hit, return immediately (no provider cost, microsecond latency).
7. **Route to a provider.** Pick the primary provider whose **circuit breaker** is closed. If it is open (provider known to be failing), skip it and use the next one.
8. **Call the provider** with a timeout. On transient failure, **retry with exponential backoff + jitter**, up to a limit. On repeated failure, trip the breaker and **fail over**.
9. **Account and respond.** Count tokens used, subtract from the key's budget, append to the WAL, store the response in the cache, update metrics, write a structured log line, and return JSON to the client.

---

## 7. Core Components in Depth

Each subsection explains **what it is**, **why it is needed**, **how to build it**, and **what to test**. The data structures are deliberately hand-written; that is the point of the project.

**Which parts need Linux?** Most of LLMGate is portable C++ and builds on both macOS and Ubuntu. Only a few parts are tied to Linux:

| Component | Platform requirement | Where to develop and test |
|---|---|---|
| epoll event-loop server (7.1, Phase 3) | **Linux only** (`<sys/epoll.h>`) | Edit on macOS if you like; compile, test and benchmark in Ubuntu |
| WAL durability and crash recovery (7.5) | Code is portable, but results must be verified on Linux: on macOS `fsync` does not guarantee data reaches disk (`F_FULLFSYNC` is needed) | Final crash-recovery tests in Ubuntu |
| Graceful shutdown (7.12) | POSIX signals work on both, but Kubernetes behaviour is Linux | Verify in Ubuntu |
| Sanitizers, `strace`, `ss`, `perf`, `tcpdump`, `valgrind`, cgroups and namespaces | Linux tooling and kernel features | Ubuntu |
| Docker, `kind`/Kubernetes, container resource limits | Linux (cgroups) | Ubuntu |
| Auth, rate limiter, cache, budgets, circuit breaker, retries, providers, metrics formatter, safety filter, semantic cache, config, logging, thread pool, `cpp-httplib` server | Portable C++17/20 + POSIX | macOS primary; confirm on Ubuntu |

### 7.1 HTTP Server

**What:** Accepts connections, parses HTTP, dispatches to handlers.

**Phase 1 (simple):** Use the header-only `cpp-httplib` library with its built-in thread pool. Gets you working fast.

**Phase 3 (advanced):** Replace with your own **event-driven server**:
- Non-blocking sockets + **epoll** (Linux-only, see the platform rule below) to watch thousands of connections with one thread.
- An **event loop** thread accepts and reads; a **worker thread pool** runs handlers.
- Benchmark the simple server against the epoll server and report the difference. That comparison is a strong interview story.

**Concepts to understand:** blocking vs non-blocking I/O, file descriptors, `epoll_create1 / epoll_ctl / epoll_wait`, level-triggered vs edge-triggered, `SO_REUSEADDR`, backlog, keep-alive.

**Platform rule: `epoll` is Linux-only.** It is a Linux kernel API (`<sys/epoll.h>`). It does not exist on macOS (whose equivalent is `kqueue`) or on Windows. Because LLMGate is deployed on a Linux server, we implement **only the epoll backend**; there is no kqueue or Windows backend. In practice:
- Write the epoll code on macOS if you like, but **compile, run, test and benchmark it only in Ubuntu**.
- Keep it in its own files (`epoll_server.h/.cpp`) behind a guard so the macOS build never sees it:
  ```cmake
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_sources(llmgate PRIVATE src/server/epoll_server.cpp)
    target_compile_definitions(llmgate PRIVATE LLMGATE_HAS_EPOLL=1)
  endif()
  ```
  and wrap the code itself in `#ifdef __linux__`. Your editor may grey out that code on macOS; that is expected.
- On macOS the gateway runs on the portable `cpp-httplib` server. Handlers, auth, limiter, cache and the rest must not depend on which server backend is active (they sit behind `http_server.h`).

### 7.2 Authentication

**What:** Verify that the caller owns a valid key.

**How:**
- Keys look like `lg_live_<32 random chars>`.
- Never store raw keys. Store `SHA-256(key)` in a hash map (loaded from a config file at start).
- Compare hashes with a **constant-time comparison** so response timing cannot leak information.
- Each key record holds: `team name`, `rate limit settings`, `daily token budget`, `allowed models`.

**Test:** valid key, wrong key, missing header, malformed header.

### 7.3 Token Bucket Rate Limiter (DSA)

**What:** A classic algorithm that allows short bursts but enforces an average rate.

**How it works:**
- Bucket capacity `C` (burst size), refill rate `R` tokens/second.
- State per key: `tokens` (double) and `last_refill` timestamp.
- On each request: `tokens = min(C, tokens + (now - last_refill) * R)`; if `tokens >= 1`, subtract 1 and allow; else reject and compute `Retry-After = (1 - tokens) / R`.

**Implementation notes:**
- Use `std::chrono::steady_clock` (never wall-clock time, which can jump).
- One mutex per bucket (or sharded locks) to avoid a global bottleneck.
- Store buckets in an `unordered_map<KeyHash, Bucket>`.

**Test:** burst allowed, then rejected; refill after waiting; concurrent requests from multiple threads never exceed the limit (use ThreadSanitizer).

### 7.4 LRU Cache with TTL (DSA)

**What:** A bounded cache that evicts the **Least Recently Used** entry when full, and expires entries after a **Time To Live**.

**How:**
- **Hash map** (`key -> list node`) for O(1) lookup.
- **Doubly linked list** ordered by recency (front = most recent). On access, move node to the front. On insert when full, remove the tail.
- Each entry stores an expiry timestamp. Expired entries are treated as misses on read.
- **Min-heap** keyed by expiry time lets a background thread purge expired entries efficiently without scanning everything.
- Thread safety: a mutex (later, optionally sharded into N segments by key hash to reduce contention).

**Write it yourself.** Do not use `std::list` shortcuts blindly; implement the node links by hand at least once to truly understand it.

**Test:** capacity eviction order, TTL expiry, update-existing-key moves to front, concurrency stress test.

### 7.5 Token Counting and Budgets

**What:** Track how many LLM tokens each key spends per day.

**How:**
- Providers usually return token usage in the response; use that as truth.
- Before calling, estimate input tokens (approximation: `characters / 4`) to reject obviously over-budget requests early.
- Keep `used_today` per key in memory; reset at midnight UTC.
- **Persistence (phase 3):** append each usage event to a **write-ahead log** file (`fsync` policy configurable). On startup, replay the log to rebuild counters. Periodically write a **snapshot** and truncate the log.

**Concepts:** durability, `fsync`, crash recovery, log compaction.

**Test:** budget exhaustion, midnight reset, crash-and-recover (kill the process mid-run, restart, verify the counters).

### 7.6 Provider Abstraction

**What:** A common C++ interface so the gateway does not care which model backend it uses.

```cpp
class Provider {
public:
    virtual ~Provider() = default;
    virtual ChatResponse chat(const ChatRequest&, std::chrono::milliseconds timeout) = 0;
    virtual std::string name() const = 0;
};
```

**Implementations:**
- `MockProvider`: returns canned answers, can be told to fail or be slow (essential for tests and chaos testing).
- `OllamaProvider`: calls a local Ollama server over plain HTTP (no TLS pain).
- `HostedProvider` (optional): calls a hosted API over HTTPS (needs OpenSSL).

### 7.7 Circuit Breaker (computer-science state machine)

**What:** Stops hammering a provider that is clearly failing, so the gateway fails fast and recovers.

**States:**
- **Closed:** normal; requests pass. Count consecutive/recent failures.
- **Open:** after `failure_threshold` failures, reject immediately for `cool_down` seconds.
- **Half-open:** after cool-down, allow a few trial requests. If they succeed, go to Closed; if they fail, go back to Open.

**Test:** drive it with the mock provider through every transition.

### 7.8 Retries, Backoff, Timeouts

- Every outbound call has a **timeout** (never wait forever).
- Retry only **transient** errors (timeouts, `5xx`, connection reset), never `4xx`.
- **Exponential backoff with jitter:** wait `random(0, base * 2^attempt)` so many clients do not retry in lockstep.
- Total retry budget is capped so a request cannot hang for minutes.

### 7.9 Streaming (SSE)

**What:** Send the model's answer piece by piece as it is generated, like ChatGPT typing.

**How:** Respond with `Content-Type: text/event-stream` and write `data: {...}\n\n` chunks as they arrive from the provider; keep the connection open; handle client disconnects (stop calling the provider to save tokens).

### 7.10 Metrics (Prometheus format)

Expose plain text at `/metrics`. Write the formatter by hand:

```
# HELP llmgate_requests_total Total requests
# TYPE llmgate_requests_total counter
llmgate_requests_total{status="200"} 1042
llmgate_cache_hits_total 311
llmgate_request_duration_seconds_bucket{le="0.1"} 900
```

Metric types to implement: **counter**, **gauge**, **histogram** (latency buckets). Use atomics so counting is lock-free.

### 7.11 Structured Logging

Log **JSON lines** (one JSON object per line): timestamp, request id, key id (never the key), route, status, latency, cache hit, provider used. Machine-parseable logs are the foundation of debugging production.

### 7.12 Graceful Shutdown

On `SIGTERM` (what Kubernetes sends): stop accepting new connections, finish in-flight requests (with a deadline), flush the WAL, then exit. Install a signal handler that sets an atomic flag; never do heavy work inside the handler itself.

### 7.13 Semantic Cache (AI + DSA, later phase)

**What:** Cache hits for prompts that mean the same thing but are worded differently ("What is TCP?" vs "Explain TCP").

**How:**
- Turn each prompt into an **embedding** (a vector of numbers) using an embedding model (Ollama can serve one).
- Store vectors in an index; for a new prompt, find the nearest stored vector using **cosine similarity**.
- If similarity > threshold (e.g. 0.95), reuse the cached answer.
- Start with brute-force search; then implement **LSH** (locality-sensitive hashing) or a small **HNSW** graph for fast approximate nearest-neighbor lookup.

### 7.14 Safety Filter (security + AI, later phase)

- **PII redaction:** regexes for emails, phone numbers, Aadhaar-like and card-like numbers; replace with placeholders before the text leaves your network.
- **Prompt-injection heuristics:** flag phrases such as "ignore previous instructions"; log and optionally block.
- Honest limitation to document: regex filters reduce risk but do not eliminate it.

### 7.15 Evaluation Harness (AI, later phase)

A command-line tool that sends a fixed set of test prompts through the gateway and scores answers (exact match, keyword match, or "LLM-as-judge"). Tracks quality, latency, and cost across providers. This is the foundation of "agent evals".

---

## 8. Technology Stack

| Layer | Technology | Why |
|---|---|---|
| Language | **C++17/20** (Apple clang on macOS, GCC/Clang on Ubuntu) | Your strongest language; performance; real systems concepts |
| Build | **CMake** + Makefile wrapper | Industry-standard C++ build system |
| HTTP (early) | **cpp-httplib** | Header-only, fast to start, portable to macOS and Linux |
| HTTP (later) | Own **epoll** server (Linux-only) | Deep understanding of I/O |
| JSON | **nlohmann/json** | De-facto standard, easy |
| Hashing | **OpenSSL** (SHA-256) | Hashing keys; HTTPS to providers |
| Concurrency | `std::thread`, `std::mutex`, `std::condition_variable`, `std::atomic` | The core of the project |
| Testing | **GoogleTest** | Standard C++ unit testing |
| Load testing | **wrk** or **hey**, plus a Bash script | Produce real numbers |
| Local LLM | **Ollama** | Free, offline, plain HTTP |
| Containers | **Docker** (multi-stage, non-root) | Packaging |
| CI/CD | **GitHub Actions** | Free and popular |
| Orchestration | **Kubernetes** (`kind` / `minikube` locally) | Industry standard |
| Packaging | **Helm** | Templated K8s deploys |
| IaC | **Terraform** | Cloud provisioning |
| Metrics | **Prometheus** + **Grafana** | Observability standard |
| Security scanning | **Trivy**, ASan/TSan/UBSan, `clang-tidy` | Catch bugs and vulnerabilities |
| Development OS | **macOS** (MacBook M2, ARM64) as primary, plus **Ubuntu ARM64** in VMware Fusion | Write code on macOS; build and test Linux behaviour in Ubuntu |
| Production OS | **Linux server** | LLMGate is hosted centrally; clients on any OS connect over HTTP/HTTPS |
| Source control | **Git + GitHub** | Source of truth that syncs macOS and Ubuntu |
| Debug tools | `lldb` on macOS; `gdb`, `strace`, `ss`, `tcpdump`, `perf`, `valgrind` in Ubuntu | Real Linux skills |

---

## 9. Project Structure

```
llmgate/
├── CMakeLists.txt
├── Makefile                  # make build / test / run / docker / bench
├── README.md
├── .gitignore                # build*/, .DS_Store, config/*.local.json (never commit builds or secrets)
├── config/
│   ├── llmgate.example.json  # keys (hashed), limits, providers
│   └── prometheus.yml
├── include/llmgate/
│   ├── server/               # http_server.h, router.h, epoll_server.h (epoll: Linux-only)
│   ├── auth/                 # key_store.h
│   ├── limiter/              # token_bucket.h
│   ├── cache/                # lru_cache.h, semantic_cache.h
│   ├── budget/               # budget_tracker.h, wal.h
│   ├── provider/             # provider.h, mock.h, ollama.h, hosted.h
│   ├── resilience/           # circuit_breaker.h, retry.h
│   ├── metrics/              # metrics.h
│   ├── safety/               # pii_filter.h, injection_detector.h
│   └── util/                 # logger.h, sha256.h, time.h, thread_pool.h
├── src/                      # .cpp implementations + main.cpp
├── tests/                    # GoogleTest files, one per component
├── tools/
│   ├── loadtest.sh
│   ├── chaos.sh
│   └── eval/                 # evaluation harness
├── deploy/
│   ├── Dockerfile
│   ├── k8s/                  # deployment.yaml, service.yaml, secret.yaml, configmap.yaml
│   ├── helm/llmgate/
│   └── terraform/
├── docs/
│   ├── architecture.md
│   ├── design-note.md        # design decisions and trade-offs
│   ├── slo.md
│   └── benchmarks.md
└── .github/workflows/ci.yml
```

---

## 10. API Specification

### `POST /v1/chat`
Headers: `Authorization: Bearer <gateway-key>`, `Content-Type: application/json`

Request:
```json
{
  "model": "llama3",
  "messages": [{"role": "user", "content": "What is a mutex?"}],
  "max_tokens": 256,
  "stream": false
}
```

Response `200`:
```json
{
  "id": "req_8f3a...",
  "model": "llama3",
  "provider": "ollama",
  "cached": false,
  "choices": [{"message": {"role": "assistant", "content": "A mutex is ..."}}],
  "usage": {"prompt_tokens": 12, "completion_tokens": 80, "total_tokens": 92}
}
```

### Error responses

| Status | Meaning |
|---|---|
| `400` | Malformed JSON or missing fields |
| `401` | Missing or invalid API key |
| `413` | Request body too large |
| `429` | Rate limit or daily budget exceeded (`Retry-After` header set) |
| `502` | All providers failed |
| `503` | Gateway shutting down or overloaded |
| `504` | Provider timed out |

### `GET /health`
Returns `200 {"status":"ok"}`. Split later into `/healthz` (liveness: process is alive) and `/readyz` (readiness: ready to take traffic), which Kubernetes probes use.

### `GET /metrics`
Prometheus text format (see 7.10).

Try it:
```bash
curl -s localhost:8080/v1/chat \
  -H "Authorization: Bearer lg_live_testkey" \
  -H "Content-Type: application/json" \
  -d '{"model":"llama3","messages":[{"role":"user","content":"hi"}]}'
```

The same request works from a Windows, macOS or Linux client: replace `localhost:8080` with the hosted gateway's HTTPS address. No LLMGate software is installed on the client.

---

## 11. Configuration

`config/llmgate.json`:
```json
{
  "listen": "0.0.0.0:8080",
  "max_body_bytes": 1048576,
  "cache": { "capacity": 10000, "ttl_seconds": 600 },
  "providers": [
    { "name": "ollama", "type": "ollama", "url": "http://localhost:11434", "timeout_ms": 30000, "priority": 1 },
    { "name": "mock", "type": "mock", "priority": 2 }
  ],
  "circuit_breaker": { "failure_threshold": 5, "cool_down_seconds": 20 },
  "keys": [
    {
      "team": "claims-team",
      "key_sha256": "<hash>",
      "rate_limit": { "burst": 20, "per_second": 5 },
      "daily_token_budget": 200000
    }
  ]
}
```

Secrets (provider API keys) come from **environment variables** (in Kubernetes: a `Secret`), never from files committed to Git.

---

## 12. Development Environment Setup

### 12.1 The environments

| Environment | Role | OS / CPU |
|---|---|---|
| **MacBook M2** | **Primary development.** Write and edit all code here. Build and run the portable parts and their unit tests (most of Phases 1-2). | macOS, ARM64 |
| **Ubuntu in VMware Fusion** | **Linux build, test and benchmark.** Compile and test everything Linux-specific: epoll server, sanitizers, WAL crash tests, Docker/`kind`, benchmarks. | Ubuntu, ARM64 (`aarch64`) |
| **GitHub** | **Source of truth.** The only way code moves between the Mac and Ubuntu. | n/a |
| **Linux server** | Deployment target. Runs the final LLMGate and exposes it over HTTP/HTTPS. | Linux (CPU architecture to be confirmed, see section 15) |

Check where you are at any time with `uname -sm`: macOS prints `Darwin arm64`, Ubuntu prints `Linux aarch64`.

### 12.2 One-time setup: macOS

```bash
xcode-select --install          # Apple clang, git, lldb
# Install Homebrew from https://brew.sh (on Apple Silicon it lives in /opt/homebrew)
brew install cmake openssl@3
```

Build and run the portable tests on macOS:
```bash
cmake -B build-macos -DCMAKE_BUILD_TYPE=Debug -DOPENSSL_ROOT_DIR="$(brew --prefix openssl@3)"
cmake --build build-macos -j
ctest --test-dir build-macos --output-on-failure
```
- The Linux-only epoll server is skipped automatically on macOS (see 7.1).
- Debug with `lldb` (installed with the command line tools). `valgrind` is not supported on Apple Silicon macOS; use AddressSanitizer instead.

### 12.3 One-time setup: Ubuntu ARM64 in VMware Fusion

1. **Create the VM from an Ubuntu 24.04 LTS ARM64 ISO** (Server is fine; a desktop environment is optional). An x86-64 ISO will not work on an M-series Mac.
2. **Resources** (the Mac has 8 GB RAM shared with macOS): suggest 2 vCPUs, 3 to 4 GB RAM and a 40 GB disk.
3. **Install tools:**
   ```bash
   sudo apt update
   sudo apt install -y build-essential cmake git gdb valgrind clang clang-tidy \
        libssl-dev curl jq strace tcpdump linux-tools-generic
   ```
4. **GoogleTest, nlohmann/json, cpp-httplib:** fetched with CMake `FetchContent` on both machines, so nothing is installed by hand.
5. **Docker and Kubernetes locally:** install Docker Engine, `kubectl` and `kind` **inside Ubuntu** (choose the arm64 builds).
6. **Load tester:** `sudo apt install wrk` (or install `hey`). Final benchmarks run in Ubuntu.
7. **Verify:** `uname -m` prints `aarch64`; `g++ --version` and `cmake --version` work.

Build and test in Ubuntu:
```bash
cmake -B build-linux -DCMAKE_BUILD_TYPE=Debug
cmake --build build-linux -j"$(nproc)"
ctest --test-dir build-linux --output-on-failure
```

### 12.4 Syncing macOS and Ubuntu through GitHub

GitHub is the **single source of truth**. Code moves between the Mac and Ubuntu only through `git push` and `git pull`. Do **not** use VMware shared folders, drag-and-drop or copy-paste to move source files.

**Rules**
1. Pull before you start editing; push before you switch machines.
2. Only one machine holds uncommitted work at any time.
3. Never commit build output (`build-macos/`, `build-linux/`). A macOS binary will not run on Linux and vice versa, so each machine builds its own.
4. Each machine has its own SSH key registered with GitHub.
5. Secrets never go into Git (see section 11).

**One-time Git setup, on BOTH macOS and Ubuntu**
```bash
git config --global user.name "Your Name"
git config --global user.email "you@example.com"
git config --global init.defaultBranch main
git config --global pull.rebase true             # keeps history linear when you alternate machines

ssh-keygen -t ed25519 -C "llmgate-macos"         # on Ubuntu use a different label, e.g. "llmgate-ubuntu"
cat ~/.ssh/id_ed25519.pub                        # copy this into GitHub: Settings > SSH and GPG keys
ssh -T git@github.com                            # should greet you by username
```

**One-time repository creation, on macOS only** (first create an empty repository named `llmgate` on GitHub, without a README)
```bash
mkdir -p ~/dev/llmgate && cd ~/dev/llmgate
git init -b main
printf 'build*/\n.DS_Store\nconfig/*.local.json\n' > .gitignore
git add README.md .gitignore
git commit -m "Initial commit"
git remote add origin git@github.com:<your-username>/llmgate.git
git push -u origin main
```

**One-time clone, on Ubuntu only**
```bash
git clone git@github.com:<your-username>/llmgate.git ~/llmgate
```

**Daily loop**

*Step 1: write code and commit, on macOS*
```bash
cd ~/dev/llmgate
git pull                                   # start from the latest
git switch -c feat/token-bucket            # new branch for a new task (first time only)
# ... edit code, build and run portable tests (12.2) ...
git status                                 # confirm no build directories or secrets are staged
git add -A
git commit -m "Add token bucket rate limiter"
git push -u origin feat/token-bucket       # -u only the first time on a new branch
```

*Step 2: compile and test, in Ubuntu*
```bash
cd ~/llmgate
git fetch origin
git switch feat/token-bucket               # first time: creates a local branch tracking origin
git pull                                   # every later time
cmake -B build-linux -DCMAKE_BUILD_TYPE=Debug
cmake --build build-linux -j"$(nproc)"
ctest --test-dir build-linux --output-on-failure
```
Compare `git rev-parse --short HEAD` on both machines to confirm they are on the same commit.

*Step 3 (only when needed): push a Linux-specific fix from Ubuntu*
```bash
# edit, e.g. src/server/epoll_server.cpp
git add -A
git commit -m "Fix epoll read loop"
git push                                   # on a branch created in Ubuntu: git push -u origin <branch>
```
Then on macOS:
```bash
git pull
```

*Step 4: merge to main when Ubuntu is green, on macOS*
```bash
git switch main
git pull
git merge --no-ff feat/token-bucket
git push
git branch -d feat/token-bucket
```
Then in Ubuntu: `git switch main && git pull`.

**If something goes wrong**
```bash
git push                      # rejected as "non-fast-forward"? the other machine pushed first:
git pull                      # rebases your commits on top of theirs (pull.rebase is on)
git push

# on a conflict: fix the files, then
git add <file>
git rebase --continue         # or: git rebase --abort
```

**Which machine runs what**

| Task | Machine |
|---|---|
| Write and edit code | **macOS** (Ubuntu only for small Linux fixes) |
| `git pull`, `git commit`, `git push` | Both (follow the rules above) |
| `cmake -B build-macos ...`, portable unit tests | **macOS** |
| `cmake -B build-linux ...`, full test suite | **Ubuntu** |
| epoll server, sanitizer runs, WAL crash tests, `strace`/`ss`/`perf` | **Ubuntu** |
| Benchmarks (`wrk`/`hey`) | **Ubuntu** |
| Docker, `kind`, Kubernetes | **Ubuntu** |

### 12.5 Local LLM (Ollama) and the mock provider

Use the **mock provider** for all unit and integration tests and for most day-to-day development; it works identically on macOS and Ubuntu and needs no extra memory. Ollama is optional. With 8 GB RAM, use a very small model (for example `ollama pull llama3.2:1b`) and do not run it together with a `kind` cluster. If you run Ollama on macOS and call it from Ubuntu, the provider URL in Ubuntu's config must point at the Mac's address as seen from the VM, and Ollama must be set to listen beyond localhost; only do that on the private VMware network.

### 12.6 ARM64 and portability rules

- **Dependencies are ARM64-compatible:** cpp-httplib and nlohmann/json are header-only, GoogleTest is built from source by `FetchContent`, and OpenSSL comes from Homebrew (macOS) or `apt` (Ubuntu). Never hard-code `x86_64` in CMake files, scripts or Dockerfiles. When a tool's download page offers several CPU architectures, choose `arm64` / `aarch64`.
- **No x86-specific code:** no inline assembly and no SSE/AVX intrinsics (`<immintrin.h>`).
- **Memory ordering:** ARM64 reorders memory operations more freely than x86-64, so concurrency bugs can appear on your machines that would hide on x86. Use `std::atomic` with the default (sequentially consistent) ordering unless you can justify weaker, and run ThreadSanitizer in Ubuntu.
- **Keep Linux-only headers and calls** (`<sys/epoll.h>` and similar) inside the guarded epoll files; everything else stays portable.
- **Deployment architecture:** confirm whether the production Linux server is ARM64 or x86-64 before the first release (see section 15).

### 12.7 Hardware note

Your Mac has 8 GB RAM, shared between macOS and the VM. Keep the VM at 3 to 4 GB, close heavy apps while it runs, default to the mock provider, and do not run the VM, a local model and a `kind` cluster all at the same time.

---

## 13. Build Roadmap (12 Weeks)

Rule for every phase: **finish with something that runs, is tested, and is pushed to GitHub.** Never go a week without a working build. Before calling a phase done, pull it into Ubuntu (section 12.4) and confirm it builds and its tests pass there.

### Week 0: Learning prep (a few days)
- Read: Beej's Guide to Network Programming (sockets), basic HTTP (MDN), CMake tutorial.
- Write a tiny TCP echo server and a tiny multi-threaded version.

### Phase 1: Solid Core (Weeks 1-2)
**Goal:** a working gateway with mock provider.
- [ ] CMake project, GoogleTest wired in, `make test` works.
- [ ] HTTP server with `/health` and `POST /v1/chat`.
- [ ] Config loader (JSON).
- [ ] API key auth (SHA-256, constant-time compare).
- [ ] Token bucket limiter + tests.
- [ ] LRU cache with TTL + tests.
- [ ] Provider interface + `MockProvider` (+ failure/latency injection).
- [ ] Circuit breaker + tests.
- [ ] Load-test script; record first benchmark numbers in `docs/benchmarks.md`.

**Done when:** `curl` with a valid key returns a mock answer; invalid key gets 401; hammering gets 429; repeated prompt is served from cache. The unit tests pass on both macOS and Ubuntu.

### Phase 2: Real Provider + Budgets (Week 3)
- [x] `OllamaProvider` over HTTP.
- [x] Token usage accounting and daily budgets.
- [x] Retries with backoff + jitter; per-call timeouts.
- [x] Failover Ollama -> mock (or second provider).
- [x] `/metrics` with counters and a latency histogram.
- [x] Structured JSON logging with request IDs.

### Phase 3: Production-Grade Server (Week 4)
**Environment:** `epoll` is Linux-only, so the epoll server, its benchmarks and the WAL crash-recovery test are compiled and run in Ubuntu. You can edit on macOS, but the macOS build skips this code (see 7.1).
- [ ] Event-driven **epoll** server + worker pool.
- [ ] Benchmark threaded vs epoll; document results and trade-offs.
- [ ] SSE streaming.
- [ ] Graceful shutdown on SIGTERM.
- [ ] Write-ahead log + snapshot + crash recovery test.

### Phase 4: Containers + CI (Week 5)
- [ ] Multi-stage **Dockerfile**: build stage compiles; final stage is minimal, runs as **non-root**.
- [ ] GitHub Actions: build, run tests, `clang-tidy`, build image.
- [ ] Add **ASan/UBSan** and **TSan** test jobs.
- [ ] **Trivy** scan of the image; fail on high-severity findings.

### Phase 5: Observability + Reliability (Week 6)
- [ ] Prometheus scrape config; Grafana dashboard (rate, errors, latency percentiles, cache hit rate, per-team spend).
- [ ] Define **SLOs** in `docs/slo.md` (e.g. availability 99.5%, p95 latency target) and alert rules.
- [ ] **Chaos script:** kill provider, inject latency, flood requests; capture screenshots showing breaker + failover.

### Phase 6: Kubernetes (Week 7)
- [ ] `kind` cluster; Deployment (replicas, resource requests/limits), Service, ConfigMap, Secret.
- [ ] **Liveness and readiness probes**; graceful termination (`terminationGracePeriodSeconds`).
- [ ] Horizontal Pod Autoscaler (optional).
- [ ] Note design issue to document: rate limits are per-pod in memory; discuss how a shared store (e.g. Redis) or consistent hashing would fix it.

### Phase 7: IaC + Delivery (Week 8)
- [ ] **Terraform:** provision a small cloud environment (free tier / credits).
- [ ] **Helm chart** for the app.
- [ ] GitOps-style deploy (e.g. Argo CD) and a **canary rollout with auto-rollback** (e.g. Argo Rollouts), driven by error-rate metrics.
- [ ] Image signing (cosign).

### Phase 8: AI Layer (Weeks 9-10)
- [ ] Embedding client + **semantic cache** (brute-force first, then LSH/HNSW).
- [ ] PII redaction + prompt-injection heuristics.
- [ ] **Evaluation harness**: dataset of prompts, scoring, comparison report across providers/models.

### Phase 9: Polish (Weeks 11-12)
- [ ] `docs/architecture.md` with diagrams.
- [ ] `docs/design-note.md`: problem, options considered, decision, trade-offs (practice writing like a real engineering design review).
- [ ] Final benchmark report with charts.
- [ ] 3-minute demo video/GIF.
- [ ] Clean README, badges (CI status), tidy commit history, tagged release `v1.0.0`.

---

## 14. Testing Strategy

| Level | What | Tools |
|---|---|---|
| **Unit** | Every data structure and algorithm in isolation (cache, limiter, breaker, parsers) | GoogleTest |
| **Concurrency** | Many threads hammering limiter/cache/budget | TSan, stress tests |
| **Integration** | Start the real server; send HTTP requests with the mock provider | GoogleTest + `curl`/script |
| **Fault injection** | Provider failing/slow; corrupt WAL; kill -9 mid-write | Mock provider flags, chaos script |
| **Load** | Throughput and latency under pressure | `wrk` / `hey` |
| **Security** | Fuzz the HTTP parser and JSON input; scan image | libFuzzer (optional), Trivy |
| **Memory** | Leaks and invalid access | ASan, Valgrind |

Aim for tests that **explain behavior**, for example `RateLimiter_RejectsBurstBeyondCapacity`.

Linux-only tests (epoll server and similar) are compiled only when building on Linux, using the same CMake guard as in 7.1, so `ctest` stays green on macOS. Run the full suite in Ubuntu before merging to `main`.

---

## 15. DevOps: Docker, CI/CD, Kubernetes, Terraform

### Docker (multi-stage, non-root)
```dockerfile
# build stage
FROM ubuntu:24.04 AS build
RUN apt-get update && apt-get install -y build-essential cmake git libssl-dev
WORKDIR /src
COPY . .
RUN cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j

# runtime stage
FROM ubuntu:24.04
RUN apt-get update && apt-get install -y libssl3 ca-certificates && rm -rf /var/lib/apt/lists/*
RUN useradd -r -u 10001 llmgate
COPY --from=build /src/build/llmgate /usr/local/bin/llmgate
USER 10001
EXPOSE 8080
ENTRYPOINT ["llmgate", "--config", "/etc/llmgate/llmgate.json"]
```
Why multi-stage: the final image has no compiler, so it is smaller and has a smaller attack surface. Why non-root: if the process is compromised, the attacker does not get root.

**CPU architecture note:** nothing in this Dockerfile is architecture-specific, so an image built inside your Ubuntu ARM64 VM is an **ARM64** image, which will not run on an x86-64 server (and the reverse). The image that gets deployed is the one pushed by CI, and the default GitHub-hosted Linux runners are x86-64 (ARM64 runners also exist). Confirm the CPU architecture of the production server and make sure CI builds for it. A side benefit: CI on x86-64 tests your ARM64-developed code on a second architecture.

### CI/CD pipeline (GitHub Actions)
On every push/PR:
1. Checkout and cache dependencies.
2. Configure and build (Release).
3. Run unit tests.
4. Run sanitizer builds (ASan/UBSan, TSan).
5. Static analysis (`clang-tidy`).
6. Build Docker image.
7. **Trivy** vulnerability scan.
8. On `main`: push image to a registry (GHCR), tagged with the commit SHA.

### Kubernetes concepts used
- **Pod:** smallest unit, runs your container.
- **Deployment:** keeps N copies of a Pod running and handles rolling updates.
- **Service:** stable network name/IP in front of the Pods (uses cluster DNS).
- **ConfigMap / Secret:** configuration and sensitive values injected into Pods.
- **Probes:** liveness (restart me if I hang), readiness (don't send me traffic yet).
- **Resource requests/limits:** how much CPU/memory a Pod needs and may use (implemented by Linux **cgroups**).
- **HPA:** scale replicas by load.

### Terraform
Declare cloud resources in `.tf` files; `terraform plan` shows changes, `terraform apply` creates them, and the state file tracks reality. Keep secrets out of the files and out of Git.

---

## 16. Observability and SLOs

**Three pillars:** metrics (numbers over time), logs (events), traces (request journeys; optional extension with OpenTelemetry).

**Golden signals to dashboard:** traffic (req/s), errors (% of 5xx), latency (p50/p95/p99), saturation (CPU, memory, queue depth, thread pool utilization).

**Gateway-specific metrics:** cache hit rate, tokens consumed per team, rate-limit rejections, breaker state per provider, failover count, provider latency.

**Example SLOs** (write yours in `docs/slo.md`):
- Availability: 99.5% of requests return a non-5xx response over 30 days.
- Latency: 95% of cache-miss requests complete under the target you measure; cache-hit p95 under 20 ms.
- An **error budget** is the allowed failure (0.5% here); alert when you burn it too fast.

---

## 17. Security Design

| Threat | Mitigation |
|---|---|
| Stolen or leaked keys from storage | Store only SHA-256 hashes; never log keys |
| Timing attacks on key comparison | Constant-time compare |
| Oversized/malicious requests | Body size limit, header limit, read timeouts (slowloris defense) |
| Request flooding | Per-key rate limit; global connection cap |
| API keys intercepted in transit | Clients reach the hosted gateway over **HTTPS** only; plain HTTP is for local development |
| Budget abuse | Per-key daily token budget |
| Secrets in source control | Env vars / Kubernetes Secrets; `.gitignore`; secret scanning in CI |
| Vulnerable base image or deps | Trivy scans, minimal runtime image, pinned versions |
| Container breakout impact | Non-root user, read-only filesystem, drop Linux capabilities |
| Memory bugs in C++ | ASan/UBSan/TSan in CI, bounds-checked parsing, fuzzing |
| Sensitive data leaving the org | PII redaction before forwarding |
| Prompt injection | Heuristic detection plus documented limitations |

Write a short **threat model** in `docs/` (assets, attackers, threats, mitigations). It plays directly to a cybersecurity background.

---

## 18. Benchmarking

Record results honestly in `docs/benchmarks.md`. Example procedure:

```bash
# 100 connections, 4 threads, 30 seconds
wrk -t4 -c100 -d30s --latency -s post.lua http://localhost:8080/v1/chat
```

Measure and report:
- Requests/second and latency percentiles (p50, p95, p99).
- Cache-hit vs cache-miss latency.
- Threaded server vs epoll server.
- Behavior under provider failure (does the breaker keep latency low?).
- Hardware and settings used (for example: MacBook M2, Ubuntu ARM64 VM in VMware Fusion with N vCPUs and N GB RAM), so results are reproducible. Run final benchmarks in Ubuntu, because the deployment target is Linux, and label them as VM results; a dedicated Linux server will give different numbers.

**Never invent numbers.** Put only measured values on your resume.

---

## 19. Common Pitfalls

- **Over-scoping early.** Finish Phase 1 completely before touching Kubernetes.
- **Locking too coarsely.** One global mutex will cap throughput; measure before optimizing.
- **Deadlocks.** Always acquire multiple locks in a fixed order; prefer one lock per structure.
- **Using wall-clock time** for rate limiting or TTLs. Use `steady_clock`.
- **Forgetting timeouts.** A single hung provider call can exhaust all worker threads.
- **Retry storms.** Retries without backoff and jitter make outages worse.
- **Blocking inside the event loop.** In epoll mode, never do slow work on the loop thread.
- **Signal handler misuse.** Only set an atomic flag inside handlers.
- **Logging secrets.** Review every log statement.
- **No tests for concurrency.** Run TSan regularly.
- **Letting the README rot.** Update docs in the same commit as the code.
- **Committing build directories or binaries.** macOS and Linux binaries are incompatible; keep `build*/` in `.gitignore`.
- **Editing on both machines without syncing.** Push before switching, pull before editing (section 12.4).
- **Linux headers leaking into portable code.** Keep `<sys/epoll.h>` and similar behind the guard so the macOS build stays clean.
- **Trusting macOS for durability results.** On macOS, `fsync` does not guarantee data reaches disk; verify WAL and crash behaviour in Ubuntu.

---

## 20. Resume Bullets and Interview Prep

### Resume bullets (replace X, Y, Z with your measured numbers)
- Built **LLMGate**, a multi-threaded C++ LLM gateway enforcing per-team rate limits, token budgets, response caching, and provider failover; sustained **X req/s with p95 latency under Y ms** in load tests.
- Implemented token-bucket rate limiter, LRU+TTL cache, circuit breaker, and write-ahead log from scratch; cache achieved **Z% hit rate** on repeated workloads, cutting provider calls accordingly.
- Replaced thread-per-request design with an **epoll** event loop and worker pool, improving throughput by **X%** (benchmarked).
- Containerized (multi-stage, non-root) and deployed to **Kubernetes** with probes and resource limits; CI/CD on **GitHub Actions** with ASan/TSan, Trivy scanning, and image signing; infrastructure via **Terraform/Helm**.
- Added Prometheus/Grafana observability, defined SLOs with alerting, and validated resilience with chaos tests.

### Questions you should be able to answer
1. Walk me through a request from `curl` to response.
2. How does a token bucket work? Why not a fixed window counter?
3. How does your LRU cache achieve O(1)? What happens under concurrency?
4. Explain the circuit breaker states and why half-open exists.
5. Threads vs epoll: when is each better? What did your benchmark show?
6. What is a race condition? Where could one occur in your code, and how did you test for it?
7. What does `fsync` do and why does your WAL need it?
8. What happens on `SIGTERM`? How does Kubernetes use it?
9. How do liveness and readiness probes differ?
10. How do containers isolate processes (namespaces, cgroups)?
11. Rate limits are per-pod. What breaks when you scale to 5 replicas, and how would you fix it?
12. How did you secure API keys? What is a timing attack?
13. What are the limits of your prompt-injection filter?
14. What is an SLO, an SLI, and an error budget?
15. Why is LLMGate a hosted server instead of a program every user installs, and why do Windows and macOS clients need no special build?
16. Why is epoll Linux-only, and how did you keep that out of the portable code?

If you can answer all of these from memory using your own project as the example, you will stand out in almost any systems or backend interview.

---

## 21. Glossary

| Term | Meaning |
|---|---|
| **API** | Interface through which programs talk to each other |
| **Backoff** | Waiting longer between each retry |
| **Bucket (token bucket)** | Rate-limiting algorithm allowing bursts at a steady average rate |
| **Cache** | Stored answers reused to avoid repeated work |
| **cgroups** | Linux feature limiting CPU/memory/etc. for process groups |
| **Circuit breaker** | Pattern that stops calling a failing dependency temporarily |
| **CI/CD** | Automated build, test, and deploy on every change |
| **Container** | Isolated, packaged process (via namespaces + cgroups) |
| **Embedding** | Vector of numbers representing the meaning of text |
| **epoll** | Linux API to monitor many file descriptors efficiently |
| **Failover** | Switching to a backup when the primary fails |
| **Gateway** | Server fronting other services, adding common controls |
| **HMAC / SHA-256** | Cryptographic hashing / message authentication |
| **IaC** | Infrastructure as Code (Terraform) |
| **Jitter** | Randomness added to timing to avoid synchronized retries |
| **LLM** | Large Language Model |
| **LRU** | Least Recently Used eviction policy |
| **Mutex** | Lock allowing one thread at a time into a critical section |
| **Namespace** | Linux feature isolating what a process can see |
| **Observability** | Understanding system behavior from metrics, logs, traces |
| **p95 / p99** | 95th / 99th percentile latency |
| **Prometheus** | Metrics collection and alerting system |
| **Provider** | Service that runs the LLM (hosted API or Ollama) |
| **Race condition** | Bug where results depend on unlucky thread timing |
| **Rate limit** | Cap on how often something can be done |
| **SLI / SLO** | Measured reliability indicator / target for it |
| **SSE** | Server-Sent Events, one-way streaming over HTTP |
| **Token (LLM)** | Chunk of text used for billing and limits |
| **TTL** | Time To Live: how long a cached item stays valid |
| **WAL** | Write-Ahead Log: append-only log for crash recovery |

---

## 22. Learning Resources

- **Sockets and networking:** *Beej's Guide to Network Programming*
- **C++ concurrency:** *C++ Concurrency in Action* (Anthony Williams)
- **Linux internals:** *The Linux Programming Interface* (Michael Kerrisk); `man 7 epoll`, `man 7 namespaces`, `man 7 cgroups`
- **Systems design:** *Designing Data-Intensive Applications* (chapters on logs and replication)
- **SRE:** Google's free *Site Reliability Engineering* book (SLOs, error budgets)
- **Kubernetes:** official Kubernetes tutorials; *Kubernetes the Hard Way* for depth
- **Terraform:** HashiCorp Learn tutorials
- **CMake:** *Professional CMake* (Craig Scott)
- **Algorithms:** implement every structure here yourself before looking at reference code

---

## License
MIT (or your choice).

## Author
Your name, with links to GitHub and LinkedIn.

---

**Final advice:** build in thin vertical slices. Get one request flowing end to end on day one, even with fake everything, and then deepen each layer. A project you can run, explain, and measure is worth far more than a bigger one that only exists in plans.
