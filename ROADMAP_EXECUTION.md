# Roadmap execution state

Temporary tracker for the Now + Next roadmap tiers. **Delete this file in the
final iteration** — it is deliberately outside `docs/` because everything there
is a Doxygen input.

Full per-item specs live in the plan file at
`~/.claude/plans/swift-yawning-goose.md` (section §5.N per item).

## Environment

- Build VM: multipass **`zephyrdb-build`** (Ubuntu 24.04 arm64, 4 CPU / 4G / 20G).
  The `zephyr-ref` VM belongs to another project — never exec, mount, or delete it.
- Host source mounted at `/home/ubuntu/zdb-src` (this repo is
  `/home/ubuntu/zdb-src/zephyrdb`). Edits happen on the Mac; the VM only builds.
- West workspace on VM-local disk at `~/zdb-ws`; twister output at
  `~/zdb-twister-out`. Never build onto the sshfs mount.
- arm64 note: plain 32-bit `native_sim` does not build here. Use
  `-p native_sim/native/64 -K` (`--force-platform`, needed because the suites
  declare `platform_allow: [native_sim]`). CI stays on x86-64 plain `native_sim`
  — do not edit any `testcase.yaml`/`sample.yaml` to suit the local VM.

Verify command:

```bash
multipass exec zephyrdb-build -- bash -lc '
  export ZEPHYR_BASE=~/zdb-ws/tools/zephyr &&
  ~/venv/bin/python3 $ZEPHYR_BASE/scripts/twister \
    -T /home/ubuntu/zdb-src/zephyrdb/tests -p native_sim/native/64 -K \
    -O ~/zdb-twister-out -v --inline-logs [-s <testcase-id>]'
```

## Protocol (per iteration)

1. Implement the next unchecked item per its §5.N spec.
2. Run the item's twister suites in `zephyrdb-build`; full `-T tests` for the
   structural items (5, 10, 11) and the final item.
3. Run `doxygen` in the VM after any header change (WARN_AS_ERROR is on in CI).
4. Update the docs the spec lists (api.md, configuration.md, samples.md,
   testing.md, tests/README.md, architecture.md).
5. Remove the item's bullet from `docs/roadmap.md`.
6. Update this file: check the item off with its commit hash.
7. Commit — one commit per roadmap item, style `area: imperative summary`.
   Folded-in pre-existing bug fixes go in separate preceding commits.
   Topic branch + PR for items 5, 10, 11; direct to `main` otherwise.

All tests must pass before committing. A genuinely blocked item is recorded
under Notes with its reason, keeps its roadmap bullet, and the loop moves on.

## Items

- [x] 0. Setup: roadmap commit, doc debt, tracker, VM, baseline — `6756b5d`, `a1aa968`
- [x] 1. Atomic document saves + full-payload CRC (§5.1, M) — `bd5fabf`
- [x] 2. TS COUNT correctness & cost (§5.2, M) — `8889103`; folded fixes `a063025` (slab asserts), `bbcb5c4` (work_q)
- [ ] 3. KV string convenience API (§5.3, S) — folds: zero-length value fix
- [ ] 4. KV defaults with auto-init (§5.4, M) — folds: ZDB_EVENTING dependency fix
- [ ] 5. Persistent KV iteration (§5.5, L) — topic branch
- [ ] 6. KV reset to defaults (§5.6, S/M)
- [ ] 7. Sample: doc_kv_blob (§5.7, S)
- [ ] 8. TS reverse cursor traversal (§5.8, M) — folds: cursor_reset rewind fix
- [ ] 9. TS consumed watermark (§5.9, S/M)
- [ ] 10. TS multiple concurrent streams (§5.10, L) — topic branch
- [ ] 11. TS rollover / circular buffer mode (§5.11, L) — topic branch
- [ ] 12. FlatBuffers document export (§5.12, M)
- [ ] 13. Sample: doc_cbor_nested + final cleanup (§5.13, S)

## Notes / deviations

- **Local runner**: `~/zdb-test.sh [lfs|kv|all] [twister args]` in the VM. Two
  invocations are needed because Zephyr does not apply
  `boards/native_sim.overlay` for the `native_sim/native/64` variant, so the
  LittleFS suites must be built with
  `--extra-args=EXTRA_DTC_OVERLAY_FILE=boards/native_sim.overlay`, which the
  overlay-less KV suites cannot tolerate. Also requires
  `ZEPHYR_TOOLCHAIN_VARIANT=host`.
- The VM builds the **live mounted tree**, so never start a test run while an
  edit is half-applied.
- The west workspace's own `zephyrdb` clone was replaced with a symlink to
  `/home/ubuntu/zdb-src/zephyrdb`; otherwise Zephyr sees two modules named
  `zephyrdb` (the west project and the test's `EXTRA_ZEPHYR_MODULES`).
- **RESOLVED in item 2**: the pre-existing `zephyrdb.integration.workflows`
  mem-slab assertion on `native_sim/native/64` was the *cursor* slab, not the
  core slab: `zdb_ts_cursor_ctx` is 120 bytes on 64-bit against a 96-byte
  `CONFIG_ZDB_CURSOR_SLAB_BLOCK_SIZE`. (`zdb_ts_core_ctx` is 112 and fit.)
  Fixed by `a063025`; the whole suite passes now.
- Baseline (iteration 0, pristine tree): KV group 3/3 suites, 36/36 cases pass;
  `ts_basic`, `doc_basic`, `samples/verify` pass; `integration/workflows` fails
  as above.
