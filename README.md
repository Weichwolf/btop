# btop — Debian 13 / Intel GPU

A fork of [aristocratos/btop](https://github.com/aristocratos/btop), based on
**v1.4.7**, with Debian 13 (trixie) packaging and Intel i915 GPU memory statistics.
The upstream resource monitor tracks CPU, memory, disks, network and processes.

## Intel GPU memory

Press **5** to toggle the GPU box. `GPU RAM (user)` adds:

- **Alloc:** allocated graphics buffers.
- **Resident:** the portion currently backed by memory, as reported by i915.

Values cover readable DRM clients owned by the current user, including system
and stolen-memory regions. Repeated client IDs are counted once per GPU. Shared
buffer identities are unavailable, so shared memory is shown as lower/upper
bounds rather than a potentially double-counted total. Equal bounds show one
value. `*` means incomplete visibility; `N/A` means no usable records.
Other users and kernel-only allocations are excluded. Values refresh once per
second; this is neither total system RAM nor a fixed VRAM capacity.

Tested on Debian 13 amd64 with Intel Iris Xe (Tiger Lake, i915). Other Intel
generations and the xe driver have not been validated for this addition.

## Debian package

Current package: **1.4.7-1~bpo13+1**, a local backport outside the Debian archive.
See [build instructions and focused tests](debian/README.source) for the isolated
`sbuild` workflow. Sources are published here; generated packages and logs are
excluded from Git.

After building, install from the repository root:

```sh
sudo apt install --no-install-recommends ./.deb-build/btop_1.4.7-1~bpo13+1_amd64.deb
```

The package grants `/usr/bin/btop` **CAP_PERFMON** for Intel utilization counters;
btop runs as your normal user. On Debian kernels with
`kernel.perf_event_paranoid=3`, an administrator must also allow these counters:

```sh
sudo sysctl -w kernel.perf_event_paranoid=2
```

This also permits unprivileged profiling of the caller's own userspace processes.
To persist it, put `kernel.perf_event_paranoid = 2` in
`/etc/sysctl.d/90-btop-intel-gpu.conf`. The package does not change this setting.
The memory collector itself needs no additional capabilities.

## Development

The Intel collector has 11 focused parser/accounting/procfs tests, tested with
ASan/UBSan, plus a local hardware check using a shared 64 MiB i915 buffer.
Debian compiler flags are exported for C and C++. Known build limitation:
upstream's later `-fstack-protector` overrides Debian's stronger setting for C++.

Full usage, dependencies and other platforms:
[upstream v1.4.7 documentation](https://github.com/aristocratos/btop/blob/v1.4.7/README.md).
License: [Apache-2.0](LICENSE); third-party notices remain in their source files
and [debian/copyright](debian/copyright).
