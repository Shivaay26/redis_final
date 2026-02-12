import subprocess
import time
import matplotlib.pyplot as plt
import os
import signal

# Configuration
SERVER_CMD = "./server"     # Path to your server executable
CLIENT_SRC = "swarm.cpp"    # Path to the C++ client source
CLIENT_EXE = "./swarm_bench"
TOTAL_REQS = 1000000        # Constant load: 1 Million requests

# Define the concurrency levels to test
concurrency_levels = [1, 10, 50, 100, 200, 500, 800, 1000,5000, 10000]
results = []

def compile_client():
    print("Compiling benchmark client...")
    subprocess.run(["g++", "-O3", CLIENT_SRC, "-o", CLIENT_EXE], check=True)

def run_test(clients):
    print(f"Testing {clients} clients...", end="", flush=True)
    
    # 1. Start Server
    server = subprocess.Popen([SERVER_CMD], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(0.5) # Give it time to start

    try:
        # 2. Run Benchmark
        # cmd: ./swarm_bench <clients> <total_requests>
        output = subprocess.check_output([CLIENT_EXE, str(clients), str(TOTAL_REQS)])
        rps = float(output.strip())
        print(f" RPS: {rps:.0f}")
        return rps
    except subprocess.CalledProcessError as e:
        print(" Failed!")
        return 0
    finally:
        # 3. Kill Server
        server.terminate()
        server.wait()
        time.sleep(0.5) # Wait for socket cleanup (TIME_WAIT)

if __name__ == "__main__":
    if not os.path.exists(SERVER_CMD):
        print(f"Error: {SERVER_CMD} not found. Compile your server first!")
        exit(1)

    compile_client()

    print(f"\n--- Starting Benchmark (Total Load: {TOTAL_REQS} reqs) ---\n")
    
    for c in concurrency_levels:
        rps = run_test(c)
        results.append(rps)

    # Plotting
    plt.figure(figsize=(10, 6))
    plt.plot(concurrency_levels, results, marker='o', linestyle='-', color='b')
    plt.title(f'Throughput vs Concurrency (Total Requests: {TOTAL_REQS})')
    plt.xlabel('Number of Concurrent Clients')
    plt.ylabel('Requests Per Second (RPS)')
    plt.grid(True)
    plt.xscale('log') # Log scale helps visualize 1 vs 1000 better
    
    # Annotate points
    for x, y in zip(concurrency_levels, results):
        plt.annotate(f"{y/1000:.0f}k", (x, y), textcoords="offset points", xytext=(0,10), ha='center')

    output_file = "benchmark_result.png"
    plt.savefig(output_file)
    print(f"\nGraph saved to {output_file}")
    plt.show()