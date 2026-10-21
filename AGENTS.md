# AGENTS.md — rvcache

RV subject cache with interest-driven tick forwarding. `rv_cache` sits
between a feed side (publishers on `_TIC.<subject>`, SASS2/SASS3) and a
consumer side (native RV listeners, rv7 clients, OMM clients), keeps the
latest image per subject in a raikv shm map (or heap), answers snapshot
requests from cache, and forwards ticks only where there is interest
(bloom-gated). Namespace `rvcache`. Depends on raikv, raimd, sassrv, omm,
libdecnumber.

**`SPEC.md` and `TODO.md` are the canonical design documents.** Read
SPEC's "Milestone 3 design notes" (bloom-gated forwarding, batched shm
merges, prefetch, ordering invariants, RWF nets) before touching the tick
path, and "Milestone 4" before touching OMM. Update both in the same
session as any design or implementation change — they are the project's
memory. `IMPLEMENTATION_NOTES.md` records implementation decisions.

## Workspace layout (injinj fleet convention)

- This repo is one of the `injinj` siblings. Every dependency is a sibling
  checkout (`../raikv`, `../raimd`, `../sassrv`, `../omm`, `../libdecnumber`); headers come from
  `-I../<dep>/include` and libraries from
  `../<dep>/FC43_x86_64/lib64/lib<dep>.a`. There is no install step between
  siblings — build the dependency, then build this repo.
- Dependencies are declared once, in `build_depends.mak`
  (`<dep>_dep := MAJOR.MINOR`, the rpm BuildRequires minimum). `./deps.sh`
  clones missing siblings from it, `./deps.sh -b` also builds them in
  dependency order, `./deps.sh -s` prints the commit of this repo and every
  sibling (the only record of what a release was built from).
- Whole-fleet build order lives in `~/injinj/GNUmakefile` (`make build`):
  openpgm → raikv → libdecnumber → raimd → linecook → lzf → rdbparser → h3 →
  HdrHistogram_c → sassrv → raids → natsmd → omm → raims → rvcache →
  raimdapi → ossl.

## Build

- `make` → `FC43_x86_64/bin`, `FC43_x86_64/lib64`. The build dir is
  `$(short_dist)$(lsb_dist_ver)_$(uname_m)$(port_extra)`: Fedora 43 →
  `FC43_x86_64`, Rocky 9.5 → `ROCKY9.5_x86_64`.
- `make port_extra=-g` → debug build in `FC43_x86_64-g`.
  `make port_extra=-mingw` → Windows cross build in `FC43_x86_64-mingw`
  (mingw64 toolchain; links the siblings' *master* checkouts).
- `make -j8` is safe; the makefile dependencies are complete. One trap: the
  **first `-j` run right after a compile error** can fail test executables
  with bogus undefined references (symbols from this repo's own lib)
  because they link against a library left half-updated by the failed run.
  Run plain `make` once, then `-j` works again. It is not a missing
  dependency.
- Warnings are clean. A new warning is a defect.
- If a sibling's public headers changed, rebuild the sibling first. A link
  warning like `lib<dep>.so.1.N, needed by ../<x>/.../lib<x>.so, not found`
  means sibling `<x>` was built against an older `<dep>` — rebuild `<x>`;
  at run time that warning is a hard failure (for a one-off test run,
  `ln -s ../<dep>/FC43_x86_64/lib64/lib<dep>.so /tmp/shim/lib<dep>.so.1.N`
  and `LD_LIBRARY_PATH=/tmp/shim` works, but fix the build).
- Ad hoc test program: capture the makefile's own lines once —
  `touch src/<any>.cpp; make -n | grep -- '-c src/<any>.cpp'` and
  `make -n | grep -- '-o FC43_x86_64/bin/' | head -1` — then reuse the
  compile line (minus `-c/-o`) and the link line's `-Wl,-rpath` list +
  `FC43_x86_64/lib64/librvcache.a` + the lib tail. If new code pulls in a
  sibling the existing binaries never needed (`DSO missing from command
  line`), append `-L../<dep>/FC43_x86_64/lib64 -l<dep>`.
- Needs a dictionary for RWF/typed work: `-p ~/injinj/rmds-config` (or
  the RaiCore `rmds-config`).
- Map modes: `-m sysv:raikv.shm` attaches to the kv_server map (keep a
  `kv_server -a` running beside it); no `-m` / `-m none` = heap mode.
  Never run a bare `kv_server` against a map with data — it recreates it.

## Versioning and release

- Version is `major_num.minor_num.patch_num-build_num` in `.copr/Makefile`
  (included by the GNUmakefile). `make spec_version` prints the
  `rvcache_dep := MAJOR.MINOR` line dependents pin in `build_depends.mak`.
- `rpm/rvcache.spec` carries placeholders (`Version: 999.999`, release
  `99999`, `BuildRequires: <dep> _<dep>_dep`); `make srpm` substitutes them
  from `build_depends.mak`. Keep the `_ensure_git` line in
  `.copr/Makefile`: COPR's `make_srpm` chroot has no git and every
  `$(shell git …)` would silently expand empty.
- COPR: `injinj/dev` builds the `dev` branch of github.com/injinj/rvcache
  on push (webhook); `injinj/rel` builds github.com/raitechnology/rvcache.
  Remotes: `origin` = raitechnology (release), `ii` = injinj (dev).
  Chroots: epel-7/8/9/10, fedora-43/44/45/rawhide, x86_64.
- ABI convention: a dependent's `Requires` is `MAJOR.MINOR`, its
  `BuildRequires` the full version, and MINOR is bumped with any change to
  the C++ ABI of a public header. Bump `minor_num` here when public
  headers change, then re-pin and rebuild dependents.
- Link only against installable sibling libraries (`$(lnk_lib)`), never a
  sibling's object files.
- Signed rpms are published to https://www.raitechnology.com/repo/
  (COPR rel → download → sign → createrepo → rsync; tooling in `~/injinj/rel`).

## Code conventions

- C++11, two-space indent, `/* … */` block comments, aligned member
  comments; snake_case functions/variables, CamelCase types. System headers
  first, then project headers.
- Errors are return codes / status enums; no exceptions, no RTTI in library
  code (`NO_STL=1` must still build). Raw pointers and placement-new over
  the repo's allocators are the norm; copy how neighbouring code allocates.
- Keep the generated `CMakeLists.txt` out of your edits — it is derived
  from the GNUmakefile (and slated for removal).

## Layout

- `include/rvcache/rv_cache.h`, `cache.h` — the service and the cache
  table types.
- `src/rv_cache.cpp` (service, nets, forwarding), `cache_tab.cpp`
  (`CacheEntry`, image store, shm merge), `config.cpp` (CLI + yaml),
  `map_merge.cpp`, `sass2.cpp`/`sass3.cpp` (feed/sub protocol legs),
  `omm.cpp` (OMM feed and provider nets).
- `cache.yaml` — reference config (nets: 7500 feed / 7600, 7700, 7800
  subs). `test/basic.sh`, `test/shm.sh`, `test/omm.sh`. `map_bench`.
- CLI: `rv_cache -h`; nets via `-N role proto [daemon [network [service
  [wildcard]]]]` (argv-separated fields, `''` skips one), everything also
  settable in the `-c` yaml. See README.

## Contracts and gotchas

- `CacheEntry` trailing `value[]/len` is the subject (it doubles as the
  RouteVec `RouteSub`).
- MSG_TYPE normalisation: images are stored with MSG_TYPE first as a
  fixed-width int so serves can stamp the type in place
  (`stamp_msg_type`); typeless messages (incl. RWF field lists) are stored
  as-is and never get one injected. Typeless ticks merge by default,
  `-r` replaces.
- Serve paths use `find_for_image()` (which mints a local entry in shm
  mode because another process may have cached the subject) +
  `get_image()`; tick and accounting paths use plain `find()`. Mixing
  them up produces phantom entries or missed images.
- Hash entry type byte = raimd codec `ftype`; `enc_of_type_byte()`
  converts to the 32-bit id sassrv's `make_rv_msg` needs.
- OMM nets: the cached element is the RWF field list at
  `msg + EvPublish.hdr_len` (omm's envelope contract); delivery kind from
  `RwfMsgPeek::get_msg_class()`.
- Ordering invariants (SPEC Milestone 3): forward-now / merge-later split
  means a tick can be forwarded before its image is merged; the sequence
  policy (`-Q obs|strict|stamp`) and the pending-snapshot window (`-P`)
  are how that is kept consistent. Don't "fix" an apparent reorder in the
  merge path without reading that section.
- Periodic stats print every 10 s (`print_stats` early-return); short test
  runs never emit them.

## Tests

- `test/basic.sh` — rvd-free smoke via sassrv's `rv_server`. **Three known
  stale FAILs**: "test4 dropped_no_listener stat", "test6e no forwards
  recorded", "test6f no envelope ticks recorded" grep old stats tokens
  (`drop=`, `fwd=`, `in=`) that now sit in an `#if 0` block; live output
  is tabular. Pre-existing; fixing the greps is fair game.
- `test/shm.sh` — shm image store incl. cross-process serve; needs the
  kv map to exist.
- `test/omm.sh` — OMM endpoint smoke (A: rvcache serves `omm_client` from
  a sass2 feed; B: rvcache consumes `omm_server`'s test source and serves
  `rv_client` the converted initial).
- Manual replay rig: `ms_server -d con.yaml -t rv.listen` (raims) or
  `rv_server`; `rv_cache -c cache.yaml -p <rmds-config>`;
  `replayrv -s 7500 -f ../raimd/rsf4_rv.dat -m 10000 -l 0`;
  `rv_client -x -s 7600 -rate -f ../sassrv/top100.txt`.

## Open items / history

- See `TODO.md` (milestone bullets). <!-- TODO: fill from git log -->
