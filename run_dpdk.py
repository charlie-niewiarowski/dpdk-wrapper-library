#!/usr/bin/env python3
"""
DPDK needs three things from the host before any app can start: hugepage
memory for its mempools, a poll-mode-driver-capable kernel module loaded,
and a NIC/vdev to bind. This script handles the first two and attaches a
virtual NIC (net_tap0) instead of a physical one, so it's safe to run
without risking the host's real network connectivity (e.g. over SSH).

Binding a physical NIC to DPDK detaches it from the kernel and typically
kills any network connection using it -- that step is deliberately left
manual. See the --vdev help text below for how to point this at a real
port once you're ready.
"""

import argparse
import os
import shutil
import subprocess
import sys

REPO_ROOT = os.path.dirname(os.path.abspath(__file__))
BUILD_DIR = os.path.join(REPO_ROOT, "build")
BINARY_NAME = "kernel_bypass_networking_library"
HUGEPAGE_MOUNT = "/mnt/huge"
HUGEPAGE_SIZE_KB = 2048  # 2MB hugepages;

def require_linux():
    if sys.platform != "linux":
        sys.exit("DPDK kernel-bypass networking only runs on Linux.")


def require_root():
    if os.geteuid() != 0:
        sys.exit(
            "This needs root: hugepage reservation, kernel module loading, and "
            "/dev/vfio access all require it.\n"
            f"Re-run with: sudo python3 {os.path.basename(__file__)}"
        )


def check_dpdk_installed():
    if shutil.which("pkg-config") is None or subprocess.run(
        ["pkg-config", "--exists", "libdpdk"]
    ).returncode != 0:
        sys.exit(
            "libdpdk not found via pkg-config. Install DPDK first, e.g.:\n"
            "  sudo apt install dpdk dpdk-dev   (Debian/Ubuntu)\n"
            "or build from source: https://core.dpdk.org/download/"
        )


def setup_hugepages(num_pages: int):
    """Reserve 2MB hugepages and mount hugetlbfs -- the memory DPDK's mbuf pools live in."""
    dpdk_hugepages = shutil.which("dpdk-hugepages.py")
    if dpdk_hugepages:
        subprocess.run(
            [dpdk_hugepages, "-p", "2M", "--setup", f"{num_pages * 2}M"], check=True
        )
        return

    # Fall back to raw sysfs if the DPDK helper script isn't on PATH.
    nr_hugepages_path = f"/sys/kernel/mm/hugepages/hugepages-{HUGEPAGE_SIZE_KB}kB/nr_hugepages"
    with open(nr_hugepages_path, "w") as f:
        f.write(str(num_pages))
    with open(nr_hugepages_path) as f:
        allocated = int(f.read().strip())
    if allocated < num_pages:
        sys.exit(
            f"Only {allocated}/{num_pages} hugepages were allocated. "
            "Free up memory or lower --hugepages and retry."
        )

    os.makedirs(HUGEPAGE_MOUNT, exist_ok=True)
    already_mounted = any(
        line.split()[1] == HUGEPAGE_MOUNT for line in open("/proc/mounts")
    )
    if not already_mounted:
        subprocess.run(["mount", "-t", "hugetlbfs", "nodev", HUGEPAGE_MOUNT], check=True)


def load_kernel_modules():
    """vfio-pci is the modern IOMMU-backed poll-mode driver binding target."""
    subprocess.run(["modprobe", "vfio-pci"], check=True)
    # Dev/test VMs frequently lack IOMMU (VT-d/AMD-Vi) support; no-iommu mode trades
    # away DMA isolation so vfio-pci can still bind. Fine for a local demo, not for a
    # host you don't fully trust.
    noiommu_path = "/sys/module/vfio/parameters/enable_unsafe_noiommu_mode"
    if os.path.exists(noiommu_path):
        with open(noiommu_path, "w") as f:
            f.write("Y")


def restore_ownership(path: str):
    """Undo root ownership left behind by building under sudo, so CLion (as your user) still works."""
    sudo_uid, sudo_gid = os.environ.get("SUDO_UID"), os.environ.get("SUDO_GID")
    if not (sudo_uid and sudo_gid):
        return
    uid, gid = int(sudo_uid), int(sudo_gid)
    for dirpath, dirnames, filenames in os.walk(path):
        for name in dirnames + filenames:
            os.chown(os.path.join(dirpath, name), uid, gid)
    os.chown(path, uid, gid)


def build_binary():
    subprocess.run(["cmake", "--build", BUILD_DIR], check=True, cwd=REPO_ROOT)
    restore_ownership(BUILD_DIR)


def run_binary(vdev: str, extra_args: list[str]):
    binary_path = os.path.join(BUILD_DIR, BINARY_NAME)
    if not os.path.exists(binary_path):
        sys.exit(f"Binary not found at {binary_path}; did the build fail?")

    # --no-pci skips physical device probing entirely since we're only attaching a vdev.
    eal_args = ["-l", "0-1", "-n", "4", "--no-pci", f"--vdev={vdev}"]
    cmd = [binary_path, *eal_args, *extra_args]
    print("Running:", " ".join(cmd))
    subprocess.run(cmd, check=True)


def main():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument(
        "--hugepages", type=int, default=512,
        help="Number of 2MB hugepages to reserve (default: 512 = 1GB)",
    )
    parser.add_argument(
        "--vdev", default="net_tap0",
        help="Virtual device to attach in place of a physical NIC (default: net_tap0, "
             "which shows up as a real kernel interface you can watch with tcpdump; "
             "use net_null0 for a pure DPDK-internals test with no kernel interaction). "
             "Only pass a real PCI device (e.g. via --vdev=none and separately binding "
             "with dpdk-devbind.py) once you're ready to take a NIC offline.",
    )
    parser.add_argument(
        "--skip-setup", action="store_true",
        help="Skip hugepage reservation and kernel module loading (use if already configured)",
    )
    args = parser.parse_args()

    require_linux()
    require_root()
    check_dpdk_installed()

    if not args.skip_setup:
        print(f"Reserving {args.hugepages} x 2MB hugepages...")
        setup_hugepages(args.hugepages)
        print("Loading vfio-pci...")
        load_kernel_modules()

    build_binary()
    run_binary(args.vdev, [])


if __name__ == "__main__":
    main()
