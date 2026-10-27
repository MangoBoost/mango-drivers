# mango_mgmt — MangoBoost Management Driver

Kernel module (`mango-mgmt.ko`) that owns the Host↔SoC management
link on MangoBoost BoostX cards (Xilinx Versal).  One instance per
card on the Host (attached via PCIe), one instance on the SoC itself
(attached via platform/OF MMIO).  The two instances are peers — they
talk to each other through the card's hardware FIFO.

On probe the module:

- brings up the MPIPE FIFO and registers a point-to-point IP netdev
  (`mango_mgmt%d` on the Host, `mango_mgmt` on the SoC),
- starts an RPC client (Host) or service (SoC) on top of that netdev,
- wires `/dev/mango-mgmt%d` to a terminal session into the SoC's
  console,
- attaches the SMI command plane (GCQ) when the function carries it,
  so `/dev/mango_smi%d`, hwmon and the SMI sysfs attributes come up
  from the same probe.

Which functions it binds, and what each carries, is one table:
`struct mango_mgmt_desc` rows in `transport/pcie.c` (PCI ID `1f52:2100`,
which carries MPIPE and SMI together) and `transport/mmio.c` (DT
compatible `mangoboost,mgmt-v2`).  An empty `mango_smi` PCI driver is
also registered so `/sys/bus/pci/drivers/mango_smi/` lists the SMI
devices where userspace looks for them.

Other MangoBoost drivers load after this module and use the exports
declared in [`mango_mgmt_api.h`](mango_mgmt_api.h) to call the SoC over
RPC.

## Layering

```
             ┌────────────────────────────────────────────┐
             │   other drivers        /     userspace     │
             └──────┬─────────────────┬─────────────────┬─┘
                    │ mango_mgmt_api.h│ /sys/class      │ /dev/mango-mgmt%d
                    │ (RPC, EEPROM,   │  /mango-mgmt/…  │  (terminal + mmap)
                    │  SoC IP, …)     │                 │
                    ▼                 ▼                 ▼
             ┌──────────────┬──────────────┬──────────────┐
             │  api/        │  sysfs.c     │  cdev.c      │
             │  (SunRPC)    │              │  term/       │
             └──────┬───────┴──────┬───────┴──────┬───────┘
                    │              │              │
                    │ kernel TCP   │              │
                    ▼              ▼              ▼
             ┌─────────────────────────────────────────────┐
             │             mpipe/  (virtual netdev)        │
             └──────────────────────┬──────────────────────┘
                                    │ MMIO via regmap
                                    ▼
             ┌─────────────────────────────────────────────┐
             │          transport/  (PCIe | MMIO)          │
             └──────────────────────┬──────────────────────┘
                                    │
                                    ▼
                                 hardware
```

## Subdirectories

| Dir | What it is | README |
|---|---|---|
| `transport/` | Bus backends (PCIe for Host, platform MMIO for SoC).  Provides the `mango_mgmt_transport` interface consumed by everything above. | [transport/README.md](transport/README.md) |
| `mpipe/` | The Host↔SoC data path: virtual netdev over the MPIPE HW FIFO, with frame-key-picked XOR framing, soft-cache hot path, NAT, and carrier model. | [mpipe/README.md](mpipe/README.md) |
| `api/` | Host↔SoC SunRPC.  The public contract (`mango_mgmt_api.h`) plus the RPC client/server/XDR machinery that binds to it. | [api/README.md](api/README.md) |
| `term/` | SoC-console access: `/dev/mango-mgmt%d` on Host tunnels through TCP:1022 to a virtio-console on the SoC.  Minicom-compatible ioctls. | [term/README.md](term/README.md) |
| `monitor/` | Power/temperature/board-info readouts for other drivers — dispatch glue; the implementation lives in `smi/`. | — |
| `smi/` | SMI core (GCQ/RPMsg command plane, sensors, hwmon, cdev).  The PCIe function attaches it from the mgmt probe via `smi_glue.c`; the SoC platform glue is `smi/smi_platform.c`. | — |

## Top-level files

| File | Role |
|---|---|
| `mango_mgmt_api.h` | **Public header.**  The contract other drivers bind against — breaking it means breaking every downstream driver.  Defines project/op descriptor, lifecycle (`register_project`, `attach_{devid,port}`), `rpc_call`, and device-lookup utilities. |
| `mod.c` | `module_init` / `module_exit`, module params (`irq_mode`, `rpc_port`, `term_port`). |
| `dev.c` / `dev.h` | `struct mango_mgmt_dev` (one per card), probe-time transport and monitor setup.  Holds what the function shares plus its two components: `struct mango_mgmt_service mgmt` (cdev, devid, ready, `hw`/`netdev`/`term`) and `smi`. |
| `frontend.c` / `frontend.h` | Composes what the function carries: the MGMT component (class device → `mpipe → api → term`) and the SMI component, each brought up and torn down on its own; `frontend_quiesce/resume` for AER and shutdown.  Called from the transport probe/remove. |
| `smi_glue.c` / `smi_glue.h` | The SMI component of a management device: attach/detach/quiesce on the function's GCQ blocks and the reads answered by the SMI serving the device, own or sibling.  The attach's devres is libsmi's own, so this file holds no token for it.  The one file that knows both mgmt and libsmi. |
| `cdev.c` / `cdev.h` | Default `file_operations` for `/dev/mango-mgmt%d`.  Just `open` + `mmap` of the MPIPE register window.  `term/client.c` overlays its own fops on Host; SoC keeps these defaults. |
| `sysfs.c` / `sysfs.h` | sysfs class setup, attribute groups: `info/` (power/temp/serial/macaddr), `mpipe/` (NAT IPs, counters), `mpipe_debug/` (HW fill level, IRQ counts). |
| `regmap.c` / `regmap.h` | Per-card table of IP block bases (MPIPE, GCQ, GCQ payload) plus one intra-block register layout.  The source of truth for every offset `mpipe/hw.c` and the SMI attach use. |
| `hw_contract.h` | PCI IDs and block offsets/lengths the regmap is built from. |
| `mango_dev.c` / `mango_dev_health.c` | `/sys/class/mango_dev/` hardware-presence inventory and devlink health reporting, used by the data-path drivers. |

## User-facing interfaces

| Interface | Who exposes it | For |
|---|---|---|
| `mango_mgmt%d` (Host) / `mango_mgmt` (SoC) netdev | `mpipe/` | TCP/IP between Host and SoC (MPIPE NAT auto-derives the SoC external IP once Host gets an IP). |
| `/dev/mango-mgmt%d` | `cdev.c` + `term/` | Terminal into SoC console (Host), or raw MPIPE mmap (both sides). |
| `/dev/mango_smi%d`, hwmon | `smi/` | Card sensors, flash, EEPROM and QSFP access. |
| `/sys/class/mango-mgmt/mango-mgmt%d/` | `sysfs.c` | Board info + MPIPE counters / NAT state / FIFO debug. |
| `EXPORT_SYMBOL_GPL` exports | `api/api.c`, `api/rpc_client.c` | Access to the SoC side for other drivers — see `mango_mgmt_api.h`. |

## Module params

```
modprobe mango-mgmt [irq_mode=N] [rpc_port=P] [term_port=P]
```

### `irq_mode` (int, default `-1`)

SoC side only; the host side always polls.

| N | Meaning |
|---|---|
| -1 | the descriptor's source (default): the platform IRQ, else polling |
| 0 | no IRQ (polling) |
| 2 | the platform IRQ |
| 3 | polling kthread |

See [transport/README.md](transport/README.md).

### `rpc_port` (ushort, default `1023`)

Port the SoC-side SunRPC server listens on (and the Host-side client
connects to).  The default lands in the privileged-port range for
historical reasons — if it clashes with another service on the box
(notably an NFS client, which uses port 1023 as a source port),
override with a non-privileged alternative.

### `term_port` (ushort, default `1022`)

Port the SoC-side terminal console listens on for the Host's
`/dev/mango-mgmt%d` tunnel.  Same override rationale as `rpc_port`.

## Load order

`mango-mgmt` loads before any driver that uses its exports.  Callers of
`mango_mgmt_rpc_call()` and related exports must tolerate the SoC side
not being ready yet (`mango_mgmt_device_is_ready` / `-ENOLINK` from
RPC).
