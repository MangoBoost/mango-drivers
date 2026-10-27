# mango-drivers

Linux kernel drivers for MangoBoost BoostX cards built on Xilinx Versal.

| Module | Source | Description |
|---|---|---|
| `mango-mgmt.ko` | [`src/mango_mgmt`](src/mango_mgmt/README.md) | Host↔SoC management link (MPIPE netdev, RPC, SoC console) and card management (SMI: sensors, hwmon, flash, EEPROM, QSFP) |

The same module runs on both ends of the card: on the host it binds the
PCIe management function (`1f52:2100`), on the card's SoC it binds the
`mangoboost,mgmt-v2` and `mangoboost,smi` device-tree nodes.

## Supported platforms

- Arch: x86_64, AArch64
- Linux 5.15 and later

| Distribution | Kernel |
|---|---|
| Ubuntu 22.04 LTS (Jammy) | `5.15`, `6.8` (HWE) |
| Ubuntu 24.04 LTS (Noble) | `6.8`, `6.14` (HWE), `6.17` (HWE) |
| Ubuntu 26.04 LTS (Resolute) | `7.0` |
| Red Hat Enterprise Linux 9 / 10 | `5.14`, `6.12` |

Kernel API differences are detected from the target kernel's headers at
build time (see [`src/libcompat`](src/libcompat/README.md)), never from
its version number.

## Build

Requires the headers of the target kernel. Run from the repository root:

```
$ make -C src -j$(nproc)                            # running kernel
$ make -C src -j$(nproc) KERNEL_BUILD=/path/to/kernel  # another kernel tree
$ ls src/*.ko
src/mango-mgmt.ko
```

`MANGO_VERSION=<x.y.z>` sets the version reported by `modinfo` and
sysfs (default `1.0.0`).

## Install and load

```
$ sudo make -C src modules_install
$ sudo depmod -a
$ sudo modprobe mango-mgmt
```

For another kernel, pass the same `KERNEL_BUILD` to `modules_install` and
run `depmod -a <kernel-release>`.

Module parameters are described in
[`src/mango_mgmt/README.md`](src/mango_mgmt/README.md#module-params).

## Contributing

Format the sources before sending changes (needs `clang-format-14`, or set
`CLANG_FORMAT`):

```
$ ./script/apply_clang_format.sh
```

## License

GPL-2.0 ([LICENSE](LICENSE)). Some files carry a different license; see
[NOTICE](NOTICE).
