# numa_machine.py
#
# gem5 SE-mode config for the paper's machine:
#   "eight NUMA node, 48-core system with four AMD Opteron 6172 processors"
#   (Muddukrishna, Jonsson, Brorsson, Scientific Programming 2015, Fig. 1)
#
#   * 8 NUMA nodes x 6 cores = 48 x86 cores (TimingSimpleCPU by default)
#   * Ruby MOESI_hammer coherence (AMD Hammer / Opteron family), with the
#     full-bit directory turned on (like Opteron "HT Assist")
#   * Private L1I / L1D / L2 per core (L2 is the last-level cache here)
#   * One directory + one DDR3 memory controller per node. Node i owns the
#     contiguous physical range [i*S, (i+1)*S). With the NUMA-SE patch,
#     gem5 places pages in these ranges by first touch or mbind().
#   * 8 routers, one per node. Router links follow a 4-socket layout:
#       - the two dies of a socket are linked        (1 hop)
#       - even dies are fully linked, odd dies too    (1 hop)
#       - everything else is 2 hops
#     which gives the OS-style distance table 10 / 16 / 22 like the paper.
#
# Needs: gem5 built with PROTOCOL=MOESI_hammer and the NUMA-SE patch.
#
# Example:
#   build/X86_MOESI_hammer/gem5.opt numa_machine.py \
#       --cmd=bin/map --options="--sched=locality --dist=coarse"

import argparse
import os
import sys
import types

import m5
from m5.defines import buildEnv
from m5.objects import *
from m5.util import addToPath, fatal

# ---------------------------------------------------------------- gem5 configs
GEM5_CONFIGS = os.environ.get("GEM5_CONFIGS")
if not GEM5_CONFIGS:
    # assume binary lives in <gem5>/build/<X>/gem5.opt
    GEM5_CONFIGS = os.path.join(
        os.path.dirname(os.path.dirname(os.path.dirname(sys.executable))),
        "configs",
    )
addToPath(GEM5_CONFIGS)

from common import Options, ObjectList
from ruby import Ruby
import ruby.MOESI_hammer as Hammer
from topologies.BaseTopology import SimpleTopology

# ---------------------------------------------------------------- arguments
parser = argparse.ArgumentParser(description="48-core 8-node NUMA machine")
Options.addCommonOptions(parser)
Options.addSEOptions(parser)
Ruby.define_options(parser)

g = parser.add_argument_group("NUMA machine")
g.add_argument("--nodes", type=int, default=8, help="NUMA nodes")
g.add_argument("--cores-per-node", type=int, default=6)
g.add_argument("--node-mem", default="256MiB", help="memory per node")
g.add_argument(
    "--hop-latency",
    type=int,
    default=45,
    help="Ruby cycles per router-to-router link (one direction)",
)
g.add_argument(
    "--local-latency",
    type=int,
    default=1,
    help="Ruby cycles for core/directory <-> own router links",
)
g.add_argument(
    "--first-touch-off",
    action="store_true",
    help="disable NUMA page placement (all pages on node 0)",
)

# Defaults for the scaled-down Opteron machine
parser.set_defaults(
    ruby=True,
    protocol="MOESI_hammer",
    network="simple",
    cpu_type="X86TimingSimpleCPU",
    cpu_clock="2.1GHz",       # Opteron 6172 clock
    ruby_clock="2.1GHz",
    sys_clock="1GHz",
    l1i_size="32KiB",
    l1i_assoc=2,
    l1d_size="16KiB",         # paper: 64 KiB, scaled down
    l1d_assoc=2,
    l2_size="64KiB",          # paper: 512 KiB L2 + 5 MiB L3, scaled down
    l2_assoc=16,
    dir_on=True,              # full-bit directory (no broadcast probes)
    mem_type="DDR3_1600_8x8",
    topology="NumaOpteron",
)
args = parser.parse_args()

N = args.nodes
C = args.cores_per_node
args.num_cpus = N * C
args.num_dirs = N
if buildEnv["PROTOCOL"] != "MOESI_hammer":
    fatal("Build gem5 with PROTOCOL=MOESI_hammer (build/X86_MOESI_hammer)")
if not args.cmd:
    fatal("Give a program with --cmd")


# ---------------------------------------------------------------- topology
def node_links(n):
    """Router links between nodes (undirected). Nodes 2s and 2s+1 = socket s."""
    links = set()
    for a in range(n):
        for b in range(a + 1, n):
            same_socket = a // 2 == b // 2
            same_parity = (a % 2) == (b % 2)
            if same_socket or same_parity:
                links.add((a, b))
    return sorted(links)


def hop_matrix(n):
    """All-pairs hop counts (BFS) over node_links."""
    adj = {i: set() for i in range(n)}
    for a, b in node_links(n):
        adj[a].add(b)
        adj[b].add(a)
    hops = []
    for s in range(n):
        dist = {s: 0}
        q = [s]
        while q:
            u = q.pop(0)
            for v in adj[u]:
                if v not in dist:
                    dist[v] = dist[u] + 1
                    q.append(v)
        hops.append([dist[j] for j in range(n)])
    return hops


class NumaOpteron(SimpleTopology):
    description = "NumaOpteron"

    def __init__(self, controllers):
        self.nodes = controllers

    def makeTopology(self, options, network, IntLink, ExtLink, Router):
        l1s = [c for c in self.nodes if "L1Cache" in c.type]
        dirs = [c for c in self.nodes if "Directory" in c.type]
        others = [c for c in self.nodes if c not in l1s and c not in dirs]

        routers = [Router(router_id=i) for i in range(N)]
        network.routers = routers

        ext_links = []
        lid = 0
        for c in l1s:  # core i lives on node i // C
            ext_links.append(
                ExtLink(link_id=lid, ext_node=c,
                        int_node=routers[int(c.version) // C],
                        latency=options.local_latency)
            )
            lid += 1
        for d in dirs:  # directory i lives on node i
            ext_links.append(
                ExtLink(link_id=lid, ext_node=d,
                        int_node=routers[int(d.version) % N],
                        latency=options.local_latency)
            )
            lid += 1
        for o in others:  # DMA etc. (none in SE mode)
            ext_links.append(
                ExtLink(link_id=lid, ext_node=o, int_node=routers[0],
                        latency=options.local_latency)
            )
            lid += 1
        network.ext_links = ext_links

        int_links = []
        for a, b in node_links(N):
            for s, d in ((a, b), (b, a)):
                int_links.append(
                    IntLink(link_id=lid, src_node=routers[s],
                            dst_node=routers[d],
                            latency=options.hop_latency, weight=1)
                )
                lid += 1
        network.int_links = int_links


# make "topologies.NumaOpteron" importable for Ruby.create_topology()
_mod = types.ModuleType("topologies.NumaOpteron")
_mod.NumaOpteron = NumaOpteron
sys.modules["topologies.NumaOpteron"] = _mod


# ---------------------------------------------------------------- memory
node_bytes = MemorySize(args.node_mem).value
mem_ranges = [AddrRange(i * node_bytes, size=node_bytes) for i in range(N)]


def numa_setup_memory_controllers(system, ruby, dir_cntrls, options):
    """One memory controller per directory, each on its node's own range."""
    ruby.block_size_bytes = options.cacheline_size
    ruby.memory_size_bits = 48
    mem_ctrls = []
    for d in dir_cntrls:
        r = mem_ranges[int(d.version)]
        dram = DDR3_1600_8x8(range=r)
        mc = MemCtrl(dram=dram)
        mc.port = d.memory_out_port
        d.addr_ranges = [r]
        mem_ctrls.append(mc)
    system.mem_ctrls = mem_ctrls


Ruby.setup_memory_controllers = numa_setup_memory_controllers

# Directory cache (probe filter storage for the full-bit directory):
# large enough for every line any core can hold, so it never forces evictions.
_orig_create = Hammer.create_system


def _create_system(options, full_system, system, dma_ports, bootmem,
                   ruby_system, cpus):
    seqs, dirs, topo = _orig_create(options, full_system, system, dma_ports,
                                    bootmem, ruby_system, cpus)
    for d in dirs:
        d.probeFilter.size = "4MiB"
        d.probeFilter.assoc = 16
        d.probeFilter.start_index_bit = 6
    return seqs, dirs, topo


Hammer.create_system = _create_system

# ---------------------------------------------------------------- system
CPUClass = ObjectList.cpu_list.get(args.cpu_type)
system = System(
    cpu=[CPUClass(cpu_id=i) for i in range(args.num_cpus)],
    mem_mode="timing",
    mem_ranges=mem_ranges,
    cache_line_size=args.cacheline_size,
)
system.multi_thread = False
system.voltage_domain = VoltageDomain(voltage=args.sys_voltage)
system.clk_domain = SrcClockDomain(clock=args.sys_clock,
                                   voltage_domain=system.voltage_domain)
system.cpu_voltage_domain = VoltageDomain()
system.cpu_clk_domain = SrcClockDomain(
    clock=args.cpu_clock, voltage_domain=system.cpu_voltage_domain)
for cpu in system.cpu:
    cpu.clk_domain = system.cpu_clk_domain

# One process; every core can run one of its threads.
process = Process(pid=100)
process.executable = args.cmd
process.cmd = [args.cmd] + (args.options.split() if args.options else [])
process.cwd = os.getcwd()
process.numa_cores_per_node = 0 if args.first_touch_off else C
env = [
    f"NUMA_NODES={N}",
    f"NUMA_CORES_PER_NODE={C}",
    "NUMA_GEM5=1",
    # last-level cache of one node = private L2 of each of its cores
    f"RT_LLC_BYTES={MemorySize(args.l2_size).value * C}",
]
if args.env:
    with open(args.env) as f:
        env += [line.rstrip() for line in f if line.strip()]
process.env = env
if args.output:
    process.output = args.output
if args.errout:
    process.errout = args.errout

for cpu in system.cpu:
    cpu.workload = process
    cpu.createThreads()

Ruby.create_system(args, False, system)
system.ruby.clk_domain = SrcClockDomain(clock=args.ruby_clock,
                                        voltage_domain=system.voltage_domain)
for i in range(args.num_cpus):
    system.cpu[i].createInterruptController()
    system.ruby._cpu_ports[i].connectCpuPorts(system.cpu[i])

system.workload = SEWorkload.init_compatible(args.cmd)

# ---------------------------------------------------------------- report
hops = hop_matrix(N)
dist = [[10 if h == 0 else 10 + 6 * h for h in row] for row in hops]
print("NUMA machine: %d nodes x %d cores = %d cores" % (N, C, N * C))
print("Node distance table (10 + 6*hops):")
print("      " + " ".join("%3d" % j for j in range(N)))
for i, row in enumerate(dist):
    print("  %2d: " % i + " ".join("%3d" % v for v in row))
with open(os.path.join(m5.options.outdir, "numa_distance.txt"), "w") as f:
    for row in dist:
        f.write(" ".join(str(v) for v in row) + "\n")

root = Root(full_system=False, system=system)
m5.instantiate()
print("Starting simulation ...")
ev = m5.simulate()
print("Exiting @ tick %i because %s" % (m5.curTick(), ev.getCause()))
