# term/ — Console (terminal) access to the SoC

Exposes the SoC's kernel console at `/dev/mango-mgmt%d` on the Host.
`minicom -D /dev/mango-mgmt0` (or `screen`, `picocom`, …) opens a live
session into the SoC's hvc0.

## Flow

```
Host user                     Host kernel                    SoC kernel
  │                               │                              │
  │ open /dev/mango-mgmt0         │                              │
  ├──────────────────────────────▶│                              │
  │                               │  TCP :1022  (over mpipe)     │
  │                               │ ◀───────────────────────────▶│
  │                               │                              │
  │ read/write/ioctl              │                              │  virtio-console
  │◀─────────────────────────────▶│                              │    hvc0
```

### Host side — `client.c` + `socket_tty.{c,h}`

`client.c` wires the chardev to `socket_tty_fops()` at probe time.
On `open(2)`:

- looks up the SoC external IP from the MPIPE NAT struct,
- creates a TCP socket bound to the MPIPE netdev,
- connects to SoC:1022,
- stashes the raw `struct socket *` in `filp->private_data`.

The `file_operations` is synthesised once at module init by opening a
throwaway socket and copying its `file_operations`, then overriding
`open` / `release` / `ioctl`.  The inherited `read_iter` / `write_iter`
/ `poll` run against userspace directly — no bounce buffers, no
`kernel_recvmsg` / `kernel_sendmsg` reimplementation.

`ioctl` emulates just enough terminal ABI (TCGETS, TIOCMGET, etc.) to
keep minicom happy; unknown ioctls fall through to the socket's own
handler.

If the netdev isn't up or the connect fails, `open` still succeeds and
leaves `private_data = NULL` — the inherited socket shims (read / write
/ splice / fasync) and the `ioctl` fall-through then return `-ENOTCONN`,
and `poll` reports `EPOLLERR | EPOLLHUP`, instead of dereferencing the
NULL `struct socket *`.  That
path is what lets a userspace tool `mmap` the cdev while the netdev
is down.

### SoC side — `server.c` + `pa-kiov.{c,h}`

`server.c` registers a `virtio_device` on a fake VirtIO bus, which the
SoC's `virtio-console` driver binds to.  That driver creates `hvc0`.

The backend for that VirtIO device is a TCP listener on port 1022.
A kthread `accept()`s one connection at a time and pumps bytes between
the connection and the two virtqueues (`RX` / `TX` from the SoC's
perspective).  The device side of each ring is driven by
`vring_host.{c,h}`, an in-tree cut of the kernel's `vringh` restricted
to what the console negotiates (split ring, no indirect descriptors, no
event index): the distros the SoC runs on do not all ship `vringh.ko`
(Debian builds none of `CONFIG_VHOST_RING`'s selectors), and the console
never needed the rest.  Payloads are Physical-Address kiovs
(`pa-kiov.{c,h}` helpers) because the descriptors carry PAs, not kernel
virtual addresses.

When the Host disconnects, the listener just waits for the next
connect — no state on the SoC side between sessions.

## Files

| File | Role |
|---|---|
| `client.c/h` | Host: wire `/dev/mango-mgmt%d` to `socket_tty_fops()`. |
| `server.c/h` | SoC: virtio-console device + TCP listener on :1022 + byte pump. |
| `socket_tty.c/h` | Kernel-socket-inherited `file_operations`, terminal-ioctl emulation. |
| `vring_host.c/h` | Device side of a split virtio ring: take the next chain, publish it used, ask whether to notify. |
| `pa-kiov.c/h` | Copy helpers between flat kernel buffers and PA-addressed chains. |

## Lifecycle

`frontend.c` picks client vs server based on `transport_is_soc_device()`:

- Host → `mango_mgmt_term_client_init()` — one line, just sets `cdev->cdev.ops`.
- SoC → `mango_mgmt_term_server_init()` — registers the virtio device,
  stashes it in `mgmt_dev->term`, starts the listener kthread.

Both ends use the `term_port` module parameter (default 1022).
