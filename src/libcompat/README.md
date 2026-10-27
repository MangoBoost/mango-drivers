# libcompat — shared kernel-compat layer

Everything the drivers need to build across the kernel support matrix
(Linux 5.15 – 7.0; Ubuntu LTS/HWE, RHEL 9/10, PetaLinux) lives here, so
driver code never tests kernel versions itself.

## Layout

- `compat.mk` — feature probes. Each probe greps/seds the *target
  kernel's own headers* (or `Module.symvers`) for a declaration and
  emits a `-DMANGO_COMPAT_*` flag into `MANGO_FLAGS`. Included by the
  top-level `src/Makefile`.
- `kheaders/` — empty stubs for headers some kernel-headers packages
  reference from modpost without shipping; `compat.mk` adds them to the
  include path only when the real header is missing.
- `include/mango_compat/<area>.h` — header-only shims (`static inline`
  wrappers, macros), partitioned by kernel area (`netdev.h`, `sunrpc.h`,
  `sysfs.h`, …). Driver code includes what it needs.

## Rules

1. **Probe declarations, not versions.** `LINUX_VERSION_CODE` /
   `RHEL_VERSION_CODE` gates are forbidden in driver code: distro
   kernels backport APIs, so a kernel's API set never follows from its
   base version (RHEL 9.x is nominally 5.14 but tracks much newer APIs).
2. **Match declaration forms.** Probe for `(*member)(`, a typed
   function prefix, or a full declaration fragment — never a bare token.
   Bare tokens false-positive on comments and backport residue.
3. **Namespace.** Everything this layer exposes is prefixed
   `mango_compat_` / `MANGO_COMPAT_`, so a reader can tell a
   kernel-compat shim from MangoBoost product code at any call site.
4. **Shims must not lie.** If an API's semantics cannot be preserved
   across kernels, narrow the wrapper to the portable subset instead of
   accepting arguments it silently ignores (`mango_compat_eventfd_signal`
   takes no counter: v6.8+ always increments by 1).
5. **Self-contained documentation.** Every probe in `compat.mk` and
   every shim header documents, in place, which kernel release changed
   the API and which distro kernels are known to backport it. A reader
   must never need any external resource to understand a shim.
6. **Exceptions are visible.** Where no header probe can express a
   condition (e.g. distro-specific quirks not tied to an API shape),
   the version gate stays in the driver as a clearly commented
   `DOCUMENTED EXCEPTION` block explaining why — greppable, never
   hidden inside this layer.

## Adding a shim

1. Identify the exact declaration difference in the kernel headers and
   which release introduced it; check the distros in the support matrix
   for backports.
2. Add the probe to `compat.mk` under the matching section, following
   rule 2, with the version/backport note (rule 5).
3. Add the wrapper or flag documentation to the matching
   `mango_compat/<area>.h`.
4. Convert the driver call sites; verify
   `grep -rn "LINUX_VERSION_CODE" <component>` stays clean.
5. Build across the kernel matrix before merging.
