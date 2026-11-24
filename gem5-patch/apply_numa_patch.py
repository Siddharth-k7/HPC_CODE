#!/usr/bin/env python3
"""
Adds NUMA page placement to gem5 SE (syscall-emulation) mode.

What it does:
  * Each memory range of the system becomes one NUMA node (gem5 already
    makes one physical page pool per range).
  * First-touch: a page is placed on the node of the core that first touches it
    (node = core_id / numa_cores_per_node), like Linux.
  * mbind()        : MPOL_BIND / MPOL_PREFERRED / MPOL_INTERLEAVE on a range.
  * set_mempolicy(): process-wide policy (so "numactl --interleave" can be mimicked).
  * get_mempolicy(MPOL_F_NODE|MPOL_F_ADDR): returns the node a page lives on.
  * getcpu()       : returns the real node instead of always 0.
  * Prints how many pages ended up on each node when the process exits.

Usage:  python3 apply_numa_patch.py /path/to/gem5
Run once on a clean gem5 checkout. Safe to re-run (it checks a marker).
"""
import sys, pathlib

root = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else ".")
MARK = "NUMA-SE-PATCH"

def edit(rel, old, new, count=1):
    p = root / rel
    s = p.read_text()
    if MARK in s and new.strip() in s:
        return
    if s.count(old) < 1:
        sys.exit(f"[patch] could not find anchor in {rel}:\n{old}")
    s = s.replace(old, new, count)
    p.write_text(s)
    print(f"[patch] edited {rel}")

# ---------------------------------------------------------------- Process.py
edit("src/sim/Process.py",
'    maxStackSize = Param.MemorySize("64MiB", "maximum size of the stack")\n',
'    maxStackSize = Param.MemorySize("64MiB", "maximum size of the stack")\n'
'    # NUMA-SE-PATCH\n'
'    numa_cores_per_node = Param.Unsigned(\n'
'        0, "Cores per NUMA node. 0 disables NUMA page placement. "\n'
'        "Node i = memory range i of the system.")\n')

# ---------------------------------------------------------------- mem_pool.hh
edit("src/sim/mem_pool.hh",
'    Addr memSize(int pool_id=0) const;\n',
'    Addr memSize(int pool_id=0) const;\n'
'    int numPools() const { return pools.size(); } // NUMA-SE-PATCH\n'
'    Addr poolStart(int pool_id) const { return pools[pool_id].startAddr(); }\n'
'    Addr poolBytes(int pool_id) const { return pools[pool_id].totalBytes(); }\n')

# ---------------------------------------------------------------- se_workload.hh
edit("src/sim/se_workload.hh",
'    Addr memSize(int pool_id=0) const;\n',
'    Addr memSize(int pool_id=0) const;\n'
'    // NUMA-SE-PATCH\n'
'    int numMemPools() const { return memPools.numPools(); }\n'
'    int poolOf(Addr paddr) const\n'
'    {\n'
'        for (int i = 0; i < memPools.numPools(); i++) {\n'
'            Addr s = memPools.poolStart(i);\n'
'            if (paddr >= s && paddr < s + memPools.poolBytes(i))\n'
'                return i;\n'
'        }\n'
'        return -1;\n'
'    }\n')

# ---------------------------------------------------------------- process.hh
edit("src/sim/process.hh",
'    bool fixupFault(Addr vaddr);\n',
'    bool fixupFault(Addr vaddr);\n'
'\n'
'    // ---------------- NUMA-SE-PATCH ----------------\n'
'    struct NumaPolicy\n'
'    {\n'
'        Addr start;              // page aligned\n'
'        Addr end;                // exclusive\n'
'        int mode;                // Linux MPOL_* value\n'
'        std::vector<int> nodes;  // nodes from the nodemask\n'
'    };\n'
'    /// NUMA state shared by all threads of a process (CLONE_VM).\n'
'    struct NumaShared\n'
'    {\n'
'        unsigned coresPerNode = 0;   // 0 = NUMA placement disabled\n'
'        int faultCtx = -1;           // core whose access is being fixed up\n'
'        std::vector<NumaPolicy> policies;      // mbind ranges, newest last\n'
'        NumaPolicy defaultPolicy{0, 0, 0, {}}; // set_mempolicy\n'
'        std::vector<uint64_t> pagesPerNode;    // printed at exit\n'
'    };\n'
'    std::shared_ptr<NumaShared> numa = std::make_shared<NumaShared>();\n'
'    /// Pick the node for a page about to be allocated.\n'
'    int numaNodeForPage(Addr page_addr);\n'
'    int numaNumNodes();\n'
'    void numaReport();\n'
'    /// Remember which core is faulting while in scope.\n'
'    struct NumaFaultScope\n'
'    {\n'
'        std::shared_ptr<NumaShared> n; int old;\n'
'        NumaFaultScope(Process *p, int ctx) : n(p->numa), old(n->faultCtx)\n'
'        { n->faultCtx = ctx; }\n'
'        ~NumaFaultScope() { n->faultCtx = old; }\n'
'    };\n')

# ---------------------------------------------------------------- process.cc
edit("src/sim/process.cc",
'    fatal_if(!seWorkload, "Couldn\'t find appropriate workload object.");\n',
'    fatal_if(!seWorkload, "Couldn\'t find appropriate workload object.");\n'
'    numa->coresPerNode = params.numa_cores_per_node; // NUMA-SE-PATCH\n')

edit("src/sim/process.cc",
'        np->memState = memState;\n',
'        np->memState = memState;\n'
'        np->numa = numa;  // NUMA-SE-PATCH: threads share NUMA policy\n')

edit("src/sim/process.cc",
'    const int npages = divCeil(size, page_size);\n'
'    const Addr paddr = seWorkload->allocPhysPages(npages);\n',
'    const int npages = divCeil(size, page_size);\n'
'    // NUMA-SE-PATCH: choose the physical pool (= NUMA node) for the page.\n'
'    int pool = 0;\n'
'    if (numa->coresPerNode > 0 && seWorkload->numMemPools() > 1) {\n'
'        pool = numaNodeForPage(page_addr);\n'
'        if ((int)numa->pagesPerNode.size() < seWorkload->numMemPools())\n'
'            numa->pagesPerNode.resize(seWorkload->numMemPools(), 0);\n'
'        numa->pagesPerNode[pool] += npages;\n'
'    }\n'
'    const Addr paddr = seWorkload->allocPhysPages(npages, pool);\n')

edit("src/sim/process.cc",
'bool\nProcess::fixupFault(Addr vaddr)\n',
'''// ---------------- NUMA-SE-PATCH ----------------
int
Process::numaNumNodes()
{
    return seWorkload->numMemPools();
}

int
Process::numaNodeForPage(Addr page_addr)
{
    const int n = numaNumNodes();
    const Addr psize = pTable->pageSize();
    auto pick = [&](const NumaPolicy &pol) -> int {
        // Linux: MPOL_PREFERRED=1, MPOL_BIND=2, MPOL_INTERLEAVE=3
        if (pol.nodes.empty())
            return -1;
        if (pol.mode == 1 || pol.mode == 2)
            return pol.nodes[0] % n;
        if (pol.mode == 3) {
            Addr idx = (page_addr - pol.start) / psize;
            return pol.nodes[idx % pol.nodes.size()] % n;
        }
        return -1;   // MPOL_DEFAULT / MPOL_LOCAL -> first touch
    };
    // Newest mbind() range wins.
    for (auto it = numa->policies.rbegin(); it != numa->policies.rend(); ++it) {
        if (page_addr >= it->start && page_addr < it->end) {
            int node = pick(*it);
            if (node >= 0)
                return node;
            break;
        }
    }
    if (numa->defaultPolicy.mode != 0) {
        int node = pick(numa->defaultPolicy);
        if (node >= 0)
            return node;
    }
    // First touch: node of the faulting core.
    if (numa->faultCtx >= 0)
        return (numa->faultCtx / (int)numa->coresPerNode) % n;
    return 0;
}

void
Process::numaReport()
{
    if (numa->pagesPerNode.empty())
        return;
    std::string s;
    for (size_t i = 0; i < numa->pagesPerNode.size(); i++)
        s += csprintf(" node%d=%d", i, numa->pagesPerNode[i]);
    inform("NUMA pages placed:%s\\n", s);
}

bool
Process::fixupFault(Addr vaddr)
''')

# ---------------------------------------------------------------- fault paths: record faulting core
edit("src/arch/x86/faults.cc",
'    } else if (!tc->getProcessPtr()->fixupFault(addr)) {\n',
'    } else if (![&]{ Process::NumaFaultScope ns(tc->getProcessPtr(),\n'
'                         tc->contextId());  // NUMA-SE-PATCH\n'
'                     return tc->getProcessPtr()->fixupFault(addr); }()) {\n')

edit("src/arch/x86/tlb.cc",
'            if (process->fixupFault(vaddr)) {\n',
'            Process::NumaFaultScope ns(process, tc->contextId()); // NUMA-SE-PATCH\n'
'            if (process->fixupFault(vaddr)) {\n')

edit("src/arch/x86/linux/se_workload.cc",
'    Process *p = tc->getProcessPtr();\n'
'    if (!p->fixupFault(tc->readMiscReg(misc_reg::Cr2))) {\n',
'    Process *p = tc->getProcessPtr();\n'
'    Process::NumaFaultScope ns(p, tc->contextId()); // NUMA-SE-PATCH\n'
'    if (!p->fixupFault(tc->readMiscReg(misc_reg::Cr2))) {\n')

edit("src/mem/se_translating_port_proxy.cc",
'    auto *process = _tc->getProcessPtr();\n',
'    auto *process = _tc->getProcessPtr();\n'
'    Process::NumaFaultScope ns(process, _tc->contextId()); // NUMA-SE-PATCH\n')

# ---------------------------------------------------------------- syscalls
edit("src/sim/syscall_emul.hh",
'SyscallReturn getcpuFunc(SyscallDesc *desc, ThreadContext *tc,\n',
'// NUMA-SE-PATCH\n'
'SyscallReturn mbindFunc(SyscallDesc *desc, ThreadContext *tc,\n'
'                        Addr start, uint64_t len, int mode,\n'
'                        VPtr<uint64_t> nmask, uint64_t maxnode,\n'
'                        unsigned flags);\n'
'SyscallReturn setMempolicyFunc(SyscallDesc *desc, ThreadContext *tc,\n'
'                               int mode, VPtr<uint64_t> nmask,\n'
'                               uint64_t maxnode);\n'
'SyscallReturn getMempolicyFunc(SyscallDesc *desc, ThreadContext *tc,\n'
'                               VPtr<int> mode, VPtr<uint64_t> nmask,\n'
'                               uint64_t maxnode, Addr addr,\n'
'                               uint64_t flags);\n'
'SyscallReturn getcpuFunc(SyscallDesc *desc, ThreadContext *tc,\n')

edit("src/sim/syscall_emul.cc",
'''    // Set a fixed NUMA node 0.
    if (node)
        *node = 0;
''',
'''    // NUMA-SE-PATCH: report the node of this core.
    if (node) {
        Process *p = tc->getProcessPtr();
        uint32_t nd = 0;
        if (p->numa->coresPerNode > 0)
            nd = (tc->contextId() / p->numa->coresPerNode) % p->numaNumNodes();
        *node = htog(nd, tc->getSystemPtr()->getGuestByteOrder());
    }
''')

edit("src/sim/syscall_emul.cc",
'SyscallReturn\nsched_getparamFunc(',
'''// ---------------- NUMA-SE-PATCH ----------------
static std::vector<int>
numaReadMask(ThreadContext *tc, VPtr<uint64_t> &nmask, uint64_t maxnode)
{
    std::vector<int> nodes;
    if (!nmask || maxnode == 0)
        return nodes;
    uint64_t m = gtoh((uint64_t)*nmask,
                      tc->getSystemPtr()->getGuestByteOrder());
    for (int i = 0; i < 64 && (uint64_t)i < maxnode; i++)
        if (m & (1ULL << i))
            nodes.push_back(i);
    return nodes;
}

SyscallReturn
mbindFunc(SyscallDesc *desc, ThreadContext *tc, Addr start, uint64_t len,
          int mode, VPtr<uint64_t> nmask, uint64_t maxnode, unsigned flags)
{
    Process *p = tc->getProcessPtr();
    const Addr psize = p->pTable->pageSize();
    if (start % psize)
        return -EINVAL;
    Process::NumaPolicy pol;
    pol.start = start;
    pol.end = roundUp(start + len, psize);
    pol.mode = mode & 0xff;          // strip MPOL_F_* mode flags
    pol.nodes = numaReadMask(tc, nmask, maxnode);
    p->numa->policies.push_back(pol);
    return 0;
}

SyscallReturn
setMempolicyFunc(SyscallDesc *desc, ThreadContext *tc, int mode,
                 VPtr<uint64_t> nmask, uint64_t maxnode)
{
    Process *p = tc->getProcessPtr();
    p->numa->defaultPolicy.mode = mode & 0xff;
    p->numa->defaultPolicy.start = 0;
    p->numa->defaultPolicy.end = 0;
    p->numa->defaultPolicy.nodes = numaReadMask(tc, nmask, maxnode);
    return 0;
}

SyscallReturn
getMempolicyFunc(SyscallDesc *desc, ThreadContext *tc, VPtr<int> mode,
                 VPtr<uint64_t> nmask, uint64_t maxnode, Addr addr,
                 uint64_t flags)
{
    Process *p = tc->getProcessPtr();
    const uint64_t MPOL_F_NODE = 1, MPOL_F_ADDR = 2;
    if ((flags & MPOL_F_NODE) && (flags & MPOL_F_ADDR)) {
        Addr paddr;
        if (!p->pTable->translate(addr, paddr)) {
            // Like Linux: fault the page in first (first touch by caller).
            Process::NumaFaultScope ns(p, tc->contextId());
            if (!p->fixupFault(addr) || !p->pTable->translate(addr, paddr))
                return -EFAULT;
        }
        int node = p->seWorkload->poolOf(paddr);
        if (node < 0)
            node = 0;
        if (mode)
            *mode = htog(node, tc->getSystemPtr()->getGuestByteOrder());
        return 0;
    }
    if (mode)
        *mode = htog(p->numa->defaultPolicy.mode,
                     tc->getSystemPtr()->getGuestByteOrder());
    return 0;
}

SyscallReturn
sched_getparamFunc(''')

edit("src/arch/x86/linux/syscall_tbl64.cc",
'''    {237, "mbind", ignoreFunc},
    {238, "set_mempolicy"},
    {239, "get_mempolicy", ignoreFunc},''',
'''    {237, "mbind", mbindFunc},                 // NUMA-SE-PATCH
    {238, "set_mempolicy", setMempolicyFunc},
    {239, "get_mempolicy", getMempolicyFunc},''')

edit("src/sim/syscall_emul.cc",
'#include "sim/process.hh"\n',
'#include "sim/process.hh"\n'
'#include "sim/se_workload.hh" // NUMA-SE-PATCH\n')

edit("src/sim/syscall_emul.cc",
'    if (group)\n        *p->exitGroup = true;\n',
'    if (group)\n        *p->exitGroup = true;\n'
'    if (group || p->pid() == p->tgid())\n'
'        p->numaReport();  // NUMA-SE-PATCH\n')

print("[patch] done")
