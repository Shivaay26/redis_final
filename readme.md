# ⚡ High-Performance Redis Clone (C++)

A high-throughput, event-driven key-value store built from scratch in C++.
Designed to demonstrate **asynchronous I/O**, **custom event loop**, **pipelining**, **intrusive data structures**, and **binary protocol parsing** at scale.

![Throughput vs Latency](./benchmark_result.png)
*Figure 1: Benchmark results showing 1.65M requests per second (RPS) peak throughput at 100 concurrent clients on WSL2, with sub-2ms P99 latency.*

---

## 🚀 Key Features

* **Event-Driven Architecture:** Uses `epoll` for O(1) event notification on Linux
* **Custom Intrusive Hashtable:** Scratch-built hashtable using intrusive linked lists for zero-allocation lookups and better cache locality
* **Progressive Resizing:** Incremental hashtable expansion to avoid stop-the-world latency spikes during growth
* **Efficient I/O Batching:** Fully asynchronous socket handling with custom state machines for reading/writing
* **Binary-Safe Protocol:** Custom serialization protocol supporting pipelined requests without parsing overhead
* **Command Pipelining:** Batches multiple commands per TCP packet for 10-50x throughput gains

---

## 🛠️ Technology Stack

* **Language:** C++17
* **System APIs:** `socket`, `bind`, `listen`, `accept`, `read`/`write`, `fcntl` (non-blocking mode)
* **Multiplexing:** `epoll` (Level-Triggered mode)
* **Data Structures:**
  * Custom Intrusive Hashtable (O(1) lookups, progressive resizing)
  * `std::vector` (I/O buffers)
  * `std::deque` (Latency tracking in benchmark client)

---

## 📊 Performance Benchmarks

To verify server efficiency, I built a custom C++ benchmarking tool (`swarm`) with **nanosecond-precision latency tracking** and support for thousands of concurrent pipelined connections.

### Platform Comparison

I tested on both WSL2 and native Linux (dual-boot, identical hardware) to measure platform differences:

**Key Results:**
- **Peak Throughput:** 1.65M requests/second (WSL2)
- **Best-Case Latency:** 60-90 microseconds P99
- **At Peak Load:** 1.9ms P99 @ 1.65M RPS

| Platform | Peak RPS | P99 Latency | Performance Window |
|:---------|:---------|:------------|:-------------------|
| **WSL2** | **1,650,592** | **0.09ms – 1.9ms** | 1-100 concurrent clients |
| **Linux (Fedora)** | **1,277,770** | **0.06ms – 2.7ms** | 1-100 concurrent clients |

**Key Insight:** The server maintains **sub-2ms P99 latency** up to peak throughput (100 clients, 1.65M RPS). Beyond this point, CPU saturation causes latency to degrade as the system enters overload - reaching 7ms @ 500 clients and 41ms @ 1000 clients.

### Why is WSL2 Faster for Localhost?

This surprising result is due to **WSL2's optimized loopback implementation**:

- **WSL2 localhost:** Uses an optimized inter-process communication path between Windows and Linux, potentially bypassing parts of the traditional TCP/IP stack
- **Linux localhost:** Goes through the full TCP/IP network stack, even for `127.0.0.1`

**Real-World Implications:**
- ✅ For **development and testing** on localhost, WSL2 provides excellent performance
- ⚠️ For **production benchmarks**, these numbers may not represent real network performance
- 📊 For **realistic measurements**, testing over actual network interfaces is recommended

**Key Insight:** Both platforms hit the same fundamental limit - **single-core CPU saturation**. The difference is in localhost overhead, not the server architecture itself.

### Conservative Performance Estimate

For production deployments over real networks, use the **Linux numbers as a baseline:**
- **Peak Throughput:** 1.28M RPS
- **P99 Latency:** 0.06ms – 2.7ms (1-100 clients)

---

### Test Environment
* **Platform:** WSL2 / Fedora Linux (dual boot)
* **Hardware:** Consumer-grade laptop (Intel/AMD x86_64, 8-16GB RAM)
* **Compilation:** `g++ -O3 -march=native -flto -DNDEBUG`
* **Methodology:** Open-loop stress test with pipelined requests over TCP localhost

---

## 📦 Building & Running

### Prerequisites
* **OS:** Linux (or WSL2 on Windows)
* **Compiler:** g++ with C++17 support
* **Python:** 3.7+ with matplotlib (for benchmark visualization)

### 1. Compile the Server
```bash
g++ -O3 -march=native -flto -DNDEBUG -std=c++17 server_epoll.cpp hashtable.cpp -o server
```

### 2. Start the Server
```bash
./server
```
*Server listens on `localhost:1234` by default.*

### 3. Compile the Benchmark Client
```bash
cd benchmark
g++ -O3 -march=native -flto -DNDEBUG -std=c++17 swarm.cpp -o swarm_bench
```

### 4. Generate Performance Graphs
```bash
python3 plot_benchmark.py
```
*This runs the server across various concurrency levels (1, 10, 50, 100, ..., 10000) and generates `benchmark_result.png`.*

---

## 🧠 Core Engineering Concepts

### 1. Custom Intrusive Hashtable (The "Secret Sauce")
Standard containers like `std::unordered_map` hit a performance ceiling because they're designed for convenience, not raw speed.

**The Problem:**
- Every insertion requires a `malloc` for the node
- Every lookup requires pointer chasing
- At 1M+ RPS, the memory allocator becomes the bottleneck

**Our Solution - Intrusive Data Structure:**
- **Embedded Pointers:** The `next` pointer is embedded *inside* the `Entry` struct itself
- **Zero Allocation:** We can move nodes between lists (e.g., during resizing) without allocating or freeing memory
- **Progressive Resizing:** Instead of freezing the server to resize a massive table (O(N) latency spike), we migrate a small batch of keys incrementally per request. This keeps latency deterministic (O(1))

### 2. The Event Loop (`epoll`)
As we scaled past 10,000 connections, standard polling failed. We moved to an **Event-Driven Architecture**.

**Why Event Loop?**
- **Thread-per-Client:** Consumes ~1MB stack per client. Caps at ~10k clients due to RAM
- **Event Loop:** Uses **one thread** for all clients. The Kernel (`epoll`) notifies us only when data is ready. This allows CPU usage to stay focused on *processing* data, not *waiting* for it

**Implementation:**
- **Single-threaded event loop:** One thread handles 10,000+ connections
- **State machines:** Each connection is a state machine (`STATE_REQ` → `STATE_RES` → `STATE_END`)
- **epoll multiplexing:** Kernel notifies the app only when sockets are ready, eliminating busy-polling CPU waste

### 3. Pipelining & Batching
In the early versions (v1-v3), the bottleneck was the sheer number of system calls.

**Without Pipelining:**
- 1 Request = 2 Syscalls (Write + Read)
- At 1M RPS = 2M syscalls/second

**With Pipelining:**
- 32 Requests = 2 Syscalls
- At 1M RPS = ~62K syscalls/second

By batching commands, we saturated the CPU cache rather than the Kernel's interrupt handler.

**Optimization:**
1. **Batch reads:** Read as much data as possible into a buffer in one syscall
2. **Parse multiple commands:** Process all pipelined requests in the buffer
3. **Batch writes:** Send multiple responses in one `write()` call

**Result:** 10-50x throughput improvement over traditional request-response loops.

### 4. Binary Protocol Design
Custom protocol avoids the overhead of text parsing (like Redis RESP):
```
[4-byte length][4-byte nargs][arg1_len][arg1_data][arg2_len][arg2_data]...
```
- **Zero string allocations** during parsing
- **Binary-safe:** Supports arbitrary bytes (nulls, newlines, etc.)
- **Pipelining-friendly:** Fixed-length prefixes enable fast framing

---

## 🔬 Optimization Journey: From 306K to 1.65M RPS

This server wasn't built in one shot—it's the result of systematic, iterative optimization. Through 11 distinct versions, each addressing specific bottlenecks, performance improved from 306K to 1.65M RPS.

**Total improvement: 5.4x from v1 to v11** 🚀

### Key Learnings

Each stage taught something valuable about systems programming:

#### 1. **Syscalls Are Expensive** (v1 → v3: +49%)
- Blocking on individual requests kills throughput
- Batching (pipelining) reduced syscalls by 80%
- **Lesson**: Minimize kernel crossings at all costs

#### 2. **Generic Containers Have Overhead** (v4 → v7: +163%)
- `std::unordered_map` has hidden allocations
- Custom intrusive structures eliminate malloc from the hot path
- **Lesson**: When performance matters, build your own data structures

#### 3. **Predictability > Raw Speed** (v8: -17% RPS, but stable)
- Unlimited pipelining hit 1.79M RPS but had 100ms+ tail latency
- Limiting to 32 requests dropped to 1.48M but kept P99 under 10ms
- **Lesson**: Production systems need predictable latency, not just max throughput

#### 4. **Platform Matters** (v11: WSL2 vs Linux)
- WSL2's optimized localhost gave 29% higher throughput
- Native Linux more representative of production networks
- **Lesson**: Always benchmark on target platform

---

### The 80/20 Rule in Action

| Effort | RPS Gain | Cumulative |
|:-------|:---------|:-----------|
| **First 20% effort** (pipelining + epoll) | +127% | **680K RPS** |
| **Next 60% effort** (hashtable + advanced tuning) | +163% | **1.79M RPS** |
| **Last 20% effort** (memory opts + tuning) | -8% | **1.65M RPS** (but stable) |

The biggest gains came from architectural changes (pipelining, epoll). Micro-optimizations helped, but with diminishing returns.

---

### What Didn't Work

Not every optimization succeeded. Here's what we tried and abandoned:

❌ **Unlimited pipeline depth** → Caused latency spikes  
❌ **Connection pooling beyond 10K objects** → Memory overhead outweighed gains  

**Lesson**: Measure everything. Intuition is often wrong.

---

## 📈 Architecture Limits

Both platforms reached the **single-threaded ceiling**. The bottleneck is CPU instruction throughput on one core, not I/O or syscalls.

**To exceed 1.65M RPS, architectural changes are required:**

| Optimization | Expected Gain | Complexity |
|:-------------|:--------------|:-----------|
| **io_uring** (Linux 5.1+) | +30-50% | Medium |
| **Multi-threaded I/O** (Redis 6.0 style) | +200-400% | High |
| **Lock-free hashtable** + thread-per-core | +400-800% | Very High |
| **SIMD hash functions** (xxHash) | +5-10% | Low |

These would be production-grade improvements requiring 3-7 days implementation each.

---

## 🔧 Project Structure
```
.
├── server_epoll.cpp         # Main server implementation
├── hashtable.h              # Hashtable interface
├── hashtable.cpp            # Hashtable implementation
├── benchmark/
│   ├── swarm.cpp            # High-performance benchmark client (C++)
│   └── plot_benchmark.py    # Automated benchmark runner & visualizer
├── benchmark_result.png     # Performance graph output (generated)
└── README.md                # This file
```

---

## 🎓 Learning Resources

Recommended reading:
- *[Beej's Guide to Network Programming](https://beej.us/guide/bgnet/)*
- *[Build Your Own Redis](https://build-your-own.org/redis/)* (the guide I followed)
- *Redis source code* (`networking.c`, `dict.c`)

**Unexpected Finding:** This project revealed that WSL2's localhost is significantly faster than native Linux's loopback interface for TCP benchmarks—a valuable lesson about the importance of testing on target platforms.

---

## 📝 License

MIT License. Free to use for educational and commercial purposes.

---

## 📧 Contact

Questions? Open an issue or reach out at praveenshahi26@gmail.com.

---

*Built with ☕ and C++ for maximum performance.*