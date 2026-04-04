# MARK: Status endpoint — cgroup-aware for containers
"""Status, health check, and server info."""

import os
import time

import psutil
from pathlib import Path
from pydantic import BaseModel
from fastapi import APIRouter

from api.tunnel import tunnels

router = APIRouter(tags=["Root"])

_start = time.time()


# MARK: Read cgroup values (v2 first, v1 fallback)
def _read_cgroup(v2_path: str, v1_path: str, is_cpu_max_period: bool = False) -> int | None:
    """Read value from cgroups (v2 first, then v1)."""
    for path in [v2_path, v1_path]:
        p = Path(path)
        if p.exists():
            val = p.read_text().strip()
            # cpu.max contains "quota period"
            if path == "/sys/fs/cgroup/cpu.max" and " " in val:
                parts = val.split()
                val = parts[1] if is_cpu_max_period and len(parts) > 1 else parts[0]
            if val not in ("max", ""):
                try:
                    v = int(val)
                    if v < 9223372036854771712:  # Not "unlimited"
                        return v
                except ValueError:
                    pass
    return None


class Memory(BaseModel):
    limit_mb: float
    used_mb: float
    free_mb: float
    process_mb: float
    percent: float


class Status(BaseModel):
    ok: bool
    tunnels: int
    uptime: int
    memory: Memory
    cpu_count: float


@router.get("/status", response_model=Status, summary="System status")
async def status():
    """Container/system memory and health status."""
    proc = psutil.Process(os.getpid())
    rss = proc.memory_info().rss

    # MARK: Container limits (cgroups)
    limit = _read_cgroup("/sys/fs/cgroup/memory.max",
                         "/sys/fs/cgroup/memory/memory.limit_in_bytes")
    used = _read_cgroup("/sys/fs/cgroup/memory.current",
                        "/sys/fs/cgroup/memory/memory.usage_in_bytes")

    # MARK: CPU quota from cgroups
    cpu_quota = _read_cgroup("/sys/fs/cgroup/cpu.max",
                             "/sys/fs/cgroup/cpu/cpu.cfs_quota_us")
    cpu_period = _read_cgroup("/sys/fs/cgroup/cpu.max",
                              "/sys/fs/cgroup/cpu/cpu.cfs_period_us",
                              is_cpu_max_period=True) or 100000

    if limit and used:
        limit_mb = limit / (1024 * 1024)
        used_mb = used / (1024 * 1024)
    else:
        mem = psutil.virtual_memory()
        limit_mb = mem.total / (1024 * 1024)
        used_mb = mem.used / (1024 * 1024)

    # MARK: Effective CPU count (0.1 vCPU = 0.1)
    if cpu_quota and cpu_quota > 0:
        effective_cpus = cpu_quota / cpu_period
    else:
        effective_cpus = psutil.cpu_count() or 1

    free_mb = limit_mb - used_mb
    process_mb = rss / (1024 * 1024)
    percent = (used_mb / limit_mb * 100) if limit_mb > 0 else 0

    return Status(
        ok=percent < 95,
        tunnels=len(tunnels),
        uptime=int(time.time() - _start),
        memory=Memory(
            limit_mb=round(limit_mb, 1),
            used_mb=round(used_mb, 1),
            free_mb=round(free_mb, 1),
            process_mb=round(process_mb, 1),
            percent=round(percent, 1),
        ),
        cpu_count=round(effective_cpus, 2),
    )
