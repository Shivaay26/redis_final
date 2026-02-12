import subprocess
import time
import matplotlib.pyplot as plt
import os
import signal

# Configuration
SERVER_CMD = "../server"
CLIENT_SRC = "swarm.cpp"
CLIENT_EXE = "./swarm_bench"
TOTAL_REQS = 1000000

concurrency_levels = [1, 10, 50, 100, 200, 500, 800, 1000]
results = []  # List of (rps, p99) tuples

def compile_client():
    print("Compiling benchmark client...")
    subprocess.run([
    "g++", 
    CLIENT_SRC, 
    "-o", CLIENT_EXE,
    "-O3",                # Maximum general optimization
    "-march=native",      # CRITICAL: Uses your CPU's specific instructions (AVX/AVX2)
    "-flto",              # Link Time Optimization (inlines across boundaries)
    "-DNDEBUG",           # CRITICAL: Removes all assert() checks
    "-fno-exceptions",    # Removes exception handling overhead (if not using try/catch)
    "-fno-rtti",          # Removes runtime type info overhead
    "-std=c++17"          # Use modern C++ optimizations
], check=True)

def run_test(clients):
    print(f"Testing {clients} clients...", end="", flush=True)
    
    server = subprocess.Popen([SERVER_CMD], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(1)  # Increased startup time

    try:
        output = subprocess.check_output([CLIENT_EXE, str(clients), str(TOTAL_REQS)], 
                                          timeout=60)  # Add timeout
        rps_str, p99_str = output.decode().strip().split(',')
        rps = float(rps_str)
        p99 = float(p99_str)
        print(f" RPS: {rps:.0f}, P99: {p99:.2f}ms")
        return (rps, p99)
    except (subprocess.CalledProcessError, subprocess.TimeoutExpired, ValueError) as e:
        print(f" Failed! ({e})")
        return (0, 0)
    finally:
        server.terminate()
        server.wait()
        time.sleep(1)  # Increased cleanup time

if __name__ == "__main__":
    if not os.path.exists(SERVER_CMD):
        print(f"Error: {SERVER_CMD} not found. Compile your server first!")
        exit(1)

    compile_client()

    print(f"\n--- Starting Benchmark (Total Load: {TOTAL_REQS} reqs) ---\n")
    
    for c in concurrency_levels:
        result = run_test(c)
        results.append(result)

    # Extract metrics
    rps_values = [r[0] for r in results]
    p99_values = [r[1] for r in results]

    # Plot RPS
    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(16, 6))
    
    ax1.plot(concurrency_levels, rps_values, marker='o', linestyle='-', color='b')
    ax1.set_title(f'Throughput vs Concurrency (Total: {TOTAL_REQS} reqs)')
    ax1.set_xlabel('Concurrent Clients')
    ax1.set_ylabel('Requests Per Second (RPS)')
    ax1.grid(True)
    ax1.set_xscale('log')
    for x, y in zip(concurrency_levels, rps_values):
        if y > 0:
            ax1.annotate(f"{y/1000:.0f}k", (x, y), textcoords="offset points", 
                        xytext=(0,10), ha='center', fontsize=8)

    # Plot P99 Latency
    ax2.plot(concurrency_levels, p99_values, marker='s', linestyle='-', color='r')
    ax2.set_title('P99 Latency vs Concurrency')
    ax2.set_xlabel('Concurrent Clients')
    ax2.set_ylabel('P99 Latency (ms)')
    ax2.grid(True)
    ax2.set_xscale('log')
    for x, y in zip(concurrency_levels, p99_values):
        if y > 0:
            ax2.annotate(f"{y:.1f}", (x, y), textcoords="offset points", 
                        xytext=(0,10), ha='center', fontsize=8)

    plt.tight_layout()
    output_file = "benchmark_result.png"
    plt.savefig(output_file, dpi=150)
    print(f"\nGraph saved to {output_file}")
    plt.show()