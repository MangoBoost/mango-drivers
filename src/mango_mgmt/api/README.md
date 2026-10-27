# api/ — Host↔SoC RPC

SunRPC-based forward channel from Host to SoC.  The public contract
lives outside this directory — `../mango_mgmt_api.h` and
`../mango_mgmt_rpc_macros.h` hold the framework; each project keeps its
own op schemas.  Everything in this directory is the
implementation.

## Channel model

One TCP connection per mgmt device:

```
Host  mango_mgmt_rpc_call()
        │    (typed wrappers <name>_rpc() in each project's *_rpc.h
        │     are the intended callers — auto-generated from the
        │     same X-macro schema the server uses)
        ▼
      rpc_client.c  ── generic p_encode/p_decode ──►  TCP :1023  ──►  SoC
                                                                        │
                                                       rpc_server.c (svc thread)
                                                                        │
                                                       rpc_xdr.c dispatcher
                                                                        │
                                              project op handler        │
                                              (any registered project — │
                                              mgmt's own `meta` or a    │
                                              client-module project)   ──┘
```

TCP rides on the `mango_mgmt%d` netdev (see `../mpipe/`).  The client
connects to the SoC's external IP (auto-derived from host_ip, or
user-overridden via the `soc_ip` sysfs attribute — see `../mpipe/nat.h`);
Host waits for NAT-ready before creating the client and reconnects on
IP change via the `on_ip_change` callback
(`api.c:mango_mgmt_rpc_on_ip_change`).

The back direction (SoC→Host) is **not implemented**.  If it's ever
needed, it would add a second `struct mango_mgmt_rpc` per device and a
mirror-image client/server pair.

## Wire format

Two SunRPC procs total:

| Proc | Number | Body |
|---|---|---|
| `MANGO_MGMT_RPC_NULL` | 0 | empty (ping) |
| `MANGO_MGMT_RPC_CALL` | 1 | `u32 prj_id` + `u32 proj_op` + `<project-defined>` |

Every project op multiplexes onto `RPC_CALL`.  Mgmt's own meta project
(`MANGO_MGMT_PRJ_META = 0`, op `MANGO_MGMT_META_OP_INTROSPECT`) is
registered at mgmt's `module_init` via the same public API client modules
use — the dispatcher has no hardcoded prj_id cases.

## Files

| File | Role |
|---|---|
| `api.c` | Device registry (xarray by devid), init/exit lifecycle, RPC startup kthread, project / instance registry (`__mango_mgmt_register_project`, `attach/detach_{devid,port}`), EEPROM access, `get_devid` / `get_soc_ip` / `device_is_ready` utilities. |
| `rpc_client.c` | Forward-channel RPC client (`rpc_create` + `rpc_call_sync`) and the generic `mango_mgmt_rpc_call()` entry point.  No project-specific logic. |
| `rpc_server.c` | Forward-channel RPC service (SoC side): `svc_create` + `svc_set_num_threads`.  Kernel compat lives in `mango_compat/sunrpc.h`. |
| `rpc_xdr.c` | Client-side generic encode/decode, server-side generic dispatcher (`pc_decode` does routing + body decode + module pin; `pc_func` dispatches the handler; `pc_encode` runs the stashed `res_encode`; `pc_release` frees bodies + `module_put`), client `rpc_procinfo` and server `svc_procedure` tables (two entries each). |
| `meta.c` / `meta.h` | mgmt's own project — minimal working reference for project registration.  Holds the `mango_mgmt_meta_introspect` op (an introspection query: "is (prj, devid [, port]) attached?"), the `check_soc_prj_{devid,port}` Host-side helpers that wrap it, and the mgmt-side register / attach / detach bookkeeping. |
| `api.h` | Internal lifecycle entry points for `mod.c` / `frontend.c`. |
| `internal.h` | Shared types across the `.c` files.  Deliberately does **not** pull in `linux/sunrpc/*.h` — api.c co-exists with libsmi which conflicts on `dev_open`. |
| `rpc.h` | Pulls in SunRPC headers; included only by `rpc_*.c`.  RPC program/version constants, `MANGO_MGMT_RPC_{NULL,CALL,NPROC}`, forward-channel procedure-table extern. |

## Adding a new op to an existing project

Every op's arg/res struct, XDR codec, and Host-side `_rpc()` wrapper
is generated from an X-macro schema.  No edits to `mango_mgmt/api/`
are needed.

1. Pick an op id in the project's local op enum (next slot before
   `<PRJ>_OP_MAX`).
2. Declare the arg/res schema as X-macros:
   ```c
   #define MY_OP_ARG(X) X(u32, key) X(u32, flags)
   #define MY_OP_RES(X) X(u32, result)
   ```
3. Emit struct + codec + Host wrapper in one shot:
   ```c
   MANGO_MGMT_RPC_DECLARE_CALL_PORT(my_op,
                                    MANGO_MGMT_PRJ_<NAME>,
                                    MY_OP,
                                    MY_OP_ARG,
                                    MY_OP_RES)
   ```
   Use `MANGO_MGMT_RPC_DECLARE_CALL` for device-scope ops;
   `_CALL_PORT` additionally prepends `u32 port` to both the arg
   struct and the generated wrapper's signature.
4. Write the SoC handler in the client module's `.c` file:
   ```c
   MANGO_MGMT_RPC_DEFINE(my_op, MANGO_MGMT_SCOPE_PORT)
   {
       /* priv, args, res are typed */
       return 0;
   }
   ```
5. Add a row to the project's ops table:
   ```c
   [MY_OP] = MANGO_MGMT_OP_ENTRY(my_op),
   ```

The generated wrapper is `int my_op_rpc(int devid, u32 port, u32 key,
u32 flags, u32 *result_out)`.  Pass `NULL` for an output pointer to
ignore that field.  If an op needs a non-trivial wrapper (transformed
fields, opaque return type, state tracking), use
`MANGO_MGMT_RPC_DECLARE_OP[_PORT]` (struct + codec only) and
hand-write it — reserve `_CALL_*` for the common case.

See `meta.h` + `meta.c` for mgmt's own minimal project.

## Adding a new project

1. Allocate a new enum value in `mango_mgmt_api.h` before `PRJ_MAX`:
   ```c
   enum mango_mgmt_prj {
       MANGO_MGMT_PRJ_META = 0,
       /* ... existing projects ... */
       MANGO_MGMT_PRJ_FOO,        /* ← new */
       MANGO_MGMT_PRJ_MAX,
   };
   ```
   Array sizes (`mango_mgmt_projects[]`, `inst_devid[]`,
   `inst_port[][]`) grow automatically.
2. Create a `<project>_rpc.h` header in the project's module with the op enum,
   X-macro schemas, and `MANGO_MGMT_RPC_DECLARE_CALL[_PORT]`
   invocations (one per op).
3. Register at module init:
   ```c
   mango_mgmt_register_project(MANGO_MGMT_PRJ_FOO, foo_ops, FOO_OP_MAX);
   ```
   This is a macro that captures the caller's `THIS_MODULE`; the
   dispatcher `try_module_get`s it per request so the stashed
   handler / codec pointers stay valid across the
   pc_decode → pc_func → pc_encode → pc_release callback chain.
4. Bind priv at probe:
   ```c
   mango_mgmt_attach_port(MANGO_MGMT_PRJ_FOO, devid, port, foo);
   /* or attach_devid for device-scope ops */
   ```
5. Unbind on remove, unregister on exit (reverse order).  Both
   synchronize RCU before returning, so it's safe to free priv
   immediately after.

Host-side presence check:
```c
if (!mango_mgmt_check_soc_prj_port(devid, MANGO_MGMT_PRJ_FOO, port))
    return -EAGAIN;   /* SoC FOO not attached yet */
```

## Dispatch path (SoC side)

The **full** body decode happens in `pc_decode` — this is the only
callback that's reliably handed an `xdr_stream` on both modern and
legacy kernels.  `pc_func` is handler-only, `pc_encode` runs the
stashed `res_encode`, `pc_release` frees bodies and module_put's.

```
svc thread → mgmt_svc_dec_rpc_call (pc_decode):
              read prj_id, proj_op off the wire
              rcu_read_lock();
                proj = projects[prj_id];
                desc = &proj->ops[proj_op];
                try_module_get(proj->owner);      // pin client module
                stash handler/res_encode/scope/owner into rq_argp
              rcu_read_unlock();
              arg_body = kzalloc(desc->arg_size);
              res_body = kzalloc(desc->res_size);
              desc->arg_decode(xdr, arg_body);
              // logical failure → arg->decode_status = rpc_*_err

          → mgmt_svc_proc_rpc_call (pc_func):
              if (decode_status != rpc_success) return decode_status;
              rcu_read_lock();
                priv = scope == DEVID ? entry->inst_devid[prj]
                                       : entry->inst_port[prj][port];
                status = arg->handler(priv, arg_body, res_body);
              rcu_read_unlock();
              res->status = status;        // fallback for encode
              return rpc_success;

          → mgmt_svc_enc_rpc_call (pc_encode):
              if (res_body allocated): arg->res_encode(xdr, res_body);
              else:                    mango_mgmt_xdr_encode_s32(xdr,
                                                        res->status);

          → mgmt_svc_release_rpc_call (pc_release):
              kfree(arg_body); kfree(res_body);
              module_put(arg->owner);
```

`SCOPE_PORT` arg structs begin with `u32 port` (enforced by
`MANGO_MGMT_RPC_DECLARE_OP_PORT`), so `*(u32 *)arg->body` is the port.

Body sizes follow `desc->arg_size` / `desc->res_size`.  The
`DECLARE_OP` macros cap each op's arg/res struct at the wire limit
`MANGO_MGMT_RPC_MAX_BODY_WORDS` with a `static_assert`, so an oversize
op fails the build with the op's name in the error message.

## RCU / lifetime / module pin

| Action | Write | Sync |
|---|---|---|
| `register_project` | `rcu_assign_pointer(projects[prj], p)`; stores `THIS_MODULE` as `p->owner` | `projects_lock` |
| `unregister_project` | auto-detach any remaining instances (WARN), clear `projects[prj]`, then `synchronize_rcu()` | `projects_lock` |
| `attach_devid/port` | `rcu_assign_pointer(inst_*, priv)` | `projects_lock` |
| `detach_devid/port` | `rcu_assign_pointer(..., NULL)` + `synchronize_rcu()` | `projects_lock` |

Two levels of protection across a request:

- **RCU** protects reads of `projects[prj_id]` and
  `inst_{devid,port}[prj][...]`.  `pc_decode` holds it only while
  reading the project entry and grabbing the module ref; `pc_func`
  holds it only around the priv lookup + handler call.  Dropping RCU
  between callbacks is what lets us `kzalloc(GFP_KERNEL)` the body
  buffers without sleeping under the lock.
- **Module ref** (`try_module_get(proj->owner)` in pc_decode,
  `module_put` in pc_release) keeps the client module — and hence its
  handler / `res_encode` function pointers — resident across the full
  dispatch.  Without it, an client module could unload between the two
  RCU sections and leave stashed pointers dangling.

After detach or unregister returns, priv can be freed safely.  Aux
modules with in-flight dispatches block `rmmod` on the module refcount
until pc_release runs.

## Kernel compat

SunRPC compat lives in `src/libcompat/include/mango_compat/sunrpc.h`
(probes in `src/libcompat/compat.mk`); the few signature differences that
need a different function body stay as `#ifdef MANGO_COMPAT_*` blocks in
`rpc_server.c` and `rpc_xdr.c`.  Feature
detection is grep-based, not `LINUX_VERSION_CODE`, because vendor
kernels (e.g. Xilinx 6.6) backport later signatures.  Each define's
meaning and the wrapper that consumes it is documented inline there.
