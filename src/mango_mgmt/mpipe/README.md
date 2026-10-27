# mpipe/ — Host↔SoC data path

A point-to-point IP link (`ARPHRD_NONE`, no Ethernet header or ARP;
`mango_mgmt%d` on the Host, `mango_mgmt` on the SoC) over the 256-word
MPIPE HW FIFO.  This is the transport TCP rides on; upper
layers (term console, api RPC) speak kernel sockets over it.

## Architecture (SPLIT_ARCH)

```
Host xmit                                             SoC xmit
  │                                                     │
softirq: NAT + pick frame_key                         softirq: NAT (noop) + pick frame_key
  │                                                     │
  └─ tx_llist ─┐                                   ┌─ tx_llist
               ▼                                   ▼
         TX kthread  ──────── MPIPE FIFO ──────  TX kthread
         (MMIO writes)                            (MMIO writes)
                           (same silicon)
         RX kthread  ──────── MPIPE FIFO ──────  RX kthread
         (MMIO reads +                            (MMIO reads +
          decode)                                  decode)
  │                                                     │
  └─ rx_llist ─┐                                   ┌─ rx_llist
               ▼                                   ▼
         NAPI: NAT + GRO → stack              NAPI: NAT (noop) + GRO → stack
```

Protocol work runs in softirq; MMIO runs in kthreads, one per
direction.  The two layers are joined by lockless `llist` queues
(SPSC, one producer / one consumer).

## Wire format

Per-frame, discovery-domain view (after `hw.c`'s wire-XOR is folded out):

```
[0, 0, ...]  [frame_key]  [w0^key  w1^key  ...  wN^key]  [0]
 sentinels     header                payload            sentinel
```

- **Sentinel = 0** — marks frame boundaries on the wire.
- **Frame key K** — picked so `K ≠ 0` and `K ≠ W` for every payload
  word `W`.  That invariant makes every internal word non-zero, so
  no payload word can be mistaken for a sentinel.  Key selection is
  a bitset gap-search (`mpipe_pick_frame_key` in `netdev.c`): build
  a bitset of `W & mask`, find the first unset bit, pigeonhole guarantees
  a gap whenever `2^M > N`.
- **Wire-XOR 0xDEADBEEF** — `hw.c` folds this in/out of every FIFO
  word.  The hardware returns 0xDEADBEEF on empty-FIFO reads; XOR'ing
  makes those reads decode as 0 (the discovery-domain sentinel), so a
  stale read on an empty FIFO looks like a frame boundary, not data
  corruption.  Userspace tools that mmap the raw registers bypass this
  fold.

Reliability and ordering are delegated to kernel TCP on top of the
netdev; resync on frame-decode errors just bumps `rx_resyncs` and
scans to the next sentinel.

## NAT (Host only)

The SoC has a hard-coded internal IP (default 192.168.10.2).  Host
picks its own IP via `ip addr`; the inetaddr notifier in `netdev.c`
auto-derives the SoC's external IP as Host±1 (or accepts a
user-supplied override via the writable `soc_ip` sysfs attribute,
validated against host_ip's subnet — in-subnet, not network /
broadcast, not equal to host_ip).  The pair lives in
`struct mango_mgmt_mpipe_nat`; the xmit / NAPI paths rewrite
dst/src IPs + incrementally patch L3/L4 checksums.

`host_ip` itself can be anything in the subnet *except* the SoC's
hardcoded internal IP — that one collision is rejected by the
inetaddr validator (TX NAT would otherwise rewrite our own packets'
dst to ourselves).

xmit drops packets whose destination isn't the SoC's external IP
(counted as TX drops) rather than silently bridging them to the SoC and
getting the `from <soc_ext>: DIFFERENT ADDRESS!` ICMP reply that leaks
the internal/external split.

Because NAT rewrites checksums, no HW checksum offload — the xmit
side relies on kernel-computed checksums arriving fully formed.

See `nat.h` for the NAT struct and packer helpers.

## Files

| File | Role |
|---|---|
| `hw.{c,h}` | MPIPE register bank selection (Host/SoC swapped), word send/recv, watermark-driven IRQ arming, polling-fallback kthread, wire-XOR fold. |
| `netdev.{c,h}` | Virtual netdev: lifecycle (`mango_mgmt_mpipe_{init,exit}`), xmit / NAPI / TX+RX kthreads / frame-key selection / RX state machine / skb pool / carrier / inetaddr notifier (NAT auto-derive). |
| `nat.h` | NAT struct + pack/unpack helpers, and `mango_mgmt_mpipe_get_nat` prototype.  Deliberately does **not** include `<linux/netdevice.h>` so aux modules that can't tolerate that header (libsmi conflict) can still read NAT state. |

## Key data structures

- `struct mango_mgmt_mpipe_netdev_priv` (netdev.h) — the netdev's
  private data: `tx_llist` / `rx_llist`, `tx_thread` / `rx_thread`,
  soft-cache `tx_avail` / `rx_avail`, RX state machine
  (`rx_skb` / `rx_frame_key` / `rx_payload_words` / `rx_draining`),
  per-CPU `encode_bitset_pcpu` for frame-key selection, NAT state.
- `struct mpipe_skb_cb` (netdev.c) — sits in `skb->cb`: per-frame
  `frame_key` and the `llist_node` used to thread the skb through
  tx_llist / rx_llist.
- `struct mango_mgmt_mpipe_hw_config` (hw.h) — IRQ callback table
  registered by `netdev.c`; the TX callback wakes the TX kthread,
  the RX callback wakes the RX kthread.

## Soft-cache pattern (hot path)

Both kthreads keep a local estimate of FIFO space (`tx_avail`) or
fill level (`rx_avail`) and only read the HW status register when
the estimate hits zero.  The HW value only moves monotonically in the
safe direction between our reads (peer drains freed slots for TX,
peer fills words for RX), so `cached <= actual` is always true;
we just underestimate.  This keeps status-register reads off the
inner loop.

## Carrier model

No keepalive, no pingpong.  `netif_carrier_ok` flips based on observed
FIFO progress, with one optimistic bootstrap:

- `ndo_open` → `netif_carrier_on` (otherwise the kernel's linkdown
  route gate would block the first packet — and there's no first
  packet without a route — so the model could never start).
- On any TX success → `netif_carrier_on` (recovers from stall).
- On any RX word received → `netif_carrier_on` (same).
- If the TX kthread can't make progress for `MPIPE_CARRIER_DOWN_JIFFIES`
  (2 s) → `netif_carrier_off`.

The effect after bootstrap is that `NO-CARRIER` in `ip link` reports a
stalled transmit path with no protocol overhead; an idle link keeps its
carrier even if the peer is gone.

## Debug

Counters (sysfs, `/sys/class/mango-mgmt/mango-mgmt%d/mpipe*/`):

| attr | field | meaning |
|---|---|---|
| `mpipe/tx_words` | `tx_words` | Wire words emitted by TX kthread |
| `mpipe/rx_words` | `rx_words` | Wire words consumed by RX kthread |
| `mpipe/rx_resyncs` | `rx_resyncs` | Times the RX state machine drained to the next sentinel after a decode error (oversize frame or skb alloc failure) |
| `mpipe_debug/*_fill` | HW read | Live FIFO fill level |
| `mpipe_debug/*_irq_count` | HW counter | IRQs observed (TX / RX / total) |
| `mpipe/host_ip` / `mpipe/soc_ip` / `mpipe/soc_internal_ip` | NAT | Current NAT state (Host-only; hidden on SoC) |

Define `DEBUG_MANGO_MGMT_MPIPE` at build time to enable first-page
TX/RX word tracing in `hw.c`.
