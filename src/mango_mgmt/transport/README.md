# transport/ — Bus backends

Abstracts the underlying bus (PCIe or MMIO) behind a common
`mango_mgmt_transport` interface.  Upper layers see uniform regmap
reads/writes; the transport picks the IRQ mode and register window for
the bus it's on.

## Backends

| Backend | File | Binds | Device model |
|---|---|---|---|
| PCIe | `pcie.{c,h}` | Host side, PCI ID `1f52:2100` | PCI device |
| MMIO | `mmio.{c,h}` | SoC side, DT compatible `mangoboost,mgmt-v2` | platform device (OF) |

`pcie.c` also registers an empty `mango_smi` PCI driver.  It binds no
device; it exists so `/sys/bus/pci/drivers/mango_smi/devices` lists the
SMI devices for userspace.

## IRQ modes

| Mode | PCIe | MMIO |
|---|---|---|
| Interrupt | n/a | single platform IRQ → shared `hw_irq_handler` (TX+RX) |
| Polling | `mpipe/hw.c` polling kthread | same, when the device tree has no IRQ |

The PCIe function wires no MPIPE interrupt, so the host side always
polls.  On the SoC the `irq_mode` module parameter (`-1` = the
descriptor's choice, the default) can force polling.

MPIPE owns the IRQ sources: `mango_mgmt_mpipe_init()` calls the
transport's `irq_setup` once its HW context exists and `irq_teardown`
before that context goes, so a handler never runs without it.
`irq_setup` returns the number of IRQs in service; 0 makes mpipe start
its polling kthread instead.  AER quiesce/resume and shutdown go through
`mango_mgmt_frontend_quiesce()`/`mango_mgmt_frontend_resume()`.

## SoC vs Host

`mango_mgmt_transport_is_soc_device()` reads the descriptor bound to the
device ID / OF compatible (`desc->side`): the MMIO descriptor is SoC,
the PCIe descriptor is Host.  `mpipe/hw.c` uses it to pick its register
bank (TX/RX are swapped on the two sides) and `frontend.c` to choose
client vs server for term + api.

## Files

| File | Purpose |
|---|---|
| `transport.{c,h}` | Driver registration, SoC/Host detection. |
| `pcie.{c,h}` | PCI driver, BAR mapping, AER handlers, mgmt_addr strings (`"pcie:0000:01:00.0"`). |
| `mmio.{c,h}` | Platform driver (OF), MMIO resource mapping, IRQ setup, mgmt_addr strings (`"mmio:<dev_name>"`). |

## Extension points used by upper layers

| Callback | Caller | Purpose |
|---|---|---|
| `region_get` | transport | Mapped BAR / MMIO region by index; `regmap.c` resolves block base + offset and does the access. |
| `irq_setup` / `irq_teardown` | `mpipe/netdev.c` | Request / free the MPIPE IRQ sources around the HW context's lifetime; setup returns the IRQ count (0 = poll). |
| `get_mgmt_addr` / `get_mgmt_addr_bin` | `mpipe/netdev.c`, `api/api.c` | Stable string/binary identifier for the device. |
