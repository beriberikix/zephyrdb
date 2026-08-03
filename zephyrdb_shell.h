/*
 * Copyright (c) 2026 ZephyrDB contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Internal interface shared by the zdb shell sources. Not a public header:
 * applications use zdb_shell_register() from zephyrdb.h and the commands
 * documented in docs/shell.md.
 */

#ifndef ZEPHYRDB_SHELL_H_
#define ZEPHYRDB_SHELL_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/util.h>

#include "zephyrdb.h"

/*
 * Subcommand sets are grouped by linker section, and a set's tag is built from
 * its depth plus the full parent path. Every parent below must therefore be
 * spelled as the whole path — (zdb, ts, flush), not (zdb_ts_flush) — or the
 * child set's tag collides with the parent's entry for the same word and
 * truncates the parent's list at that point. The leading "zdb" also keeps
 * these sections from merging with another module's commands.
 */
#define ZDB_SHELL_CMD_ADD(_syntax, _subcmd, _help, _handler, _mand, _opt)                          \
	SHELL_SUBCMD_ADD((zdb), _syntax, _subcmd, _help, _handler, _mand, _opt)

/*
 * A KV record is read through one KV I/O slab block (see zephyrdb_kv.c), so no
 * storable value can be larger than one. Sizing the shell's buffer to match is
 * what lets "zdb kv get" promise it never truncates.
 */
#if defined(CONFIG_ZDB_KV_IO_SLAB_BLOCK_SIZE)
#define ZDB_SHELL_KV_VALUE_MAX CONFIG_ZDB_KV_IO_SLAB_BLOCK_SIZE
#else
#define ZDB_SHELL_KV_VALUE_MAX 1
#endif

/*
 * A hex payload of N bytes takes 2N typed characters and the whole command line
 * is capped at CONFIG_SHELL_CMD_BUFF_SIZE, so half the line length is a hard
 * upper bound. Sizing to CONFIG_ZDB_DOC_MAX_BYTES_LEN instead would reserve
 * kilobytes that no operator could ever type.
 */
#define ZDB_SHELL_HEX_MAX (CONFIG_SHELL_CMD_BUFF_SIZE / 2)

/** Shared decode/read buffer; only valid while zdb_shell_lock() is held. */
#define ZDB_SHELL_SCRATCH_SIZE MAX(ZDB_SHELL_KV_VALUE_MAX, ZDB_SHELL_HEX_MAX)

extern uint8_t zdb_shell_scratch[ZDB_SHELL_SCRATCH_SIZE];

/** Rows a listing prints before reporting itself truncated. */
#define ZDB_SHELL_LIST_DEFAULT 20U

/** Bytes of a value previewed per row by "zdb kv list". */
#define ZDB_SHELL_PREVIEW_BYTES 16U

/**
 * Take the shell mutex and resolve the registered instance.
 *
 * On success the caller owns the mutex and must release it with
 * zdb_shell_unlock() on every exit path.
 *
 * @return 0, or -ENODEV when no instance is registered (message printed).
 */
int zdb_shell_lock(const struct shell *sh, zdb_t **out_db);

/** Release the shell mutex taken by zdb_shell_lock(). */
void zdb_shell_unlock(void);

/** Map a status onto the negative errno a command handler returns. */
int zdb_shell_errno(zdb_status_t status);

/**
 * Report a failed operation and produce the handler's return value.
 *
 * Doing both in one call is what keeps a call site from printing one status
 * and returning an errno that disagrees with it.
 */
int zdb_shell_fail(const struct shell *sh, const char *op, zdb_status_t status);

/** Report an unusable argument; always returns -EINVAL. */
int zdb_shell_bad_arg(const struct shell *sh, const char *what, const char *got);

int zdb_shell_parse_u64(const char *s, uint64_t *out);
int zdb_shell_parse_i64(const char *s, int64_t *out);
int zdb_shell_parse_f64(const char *s, double *out);
int zdb_shell_parse_bool(const char *s, bool *out);
int zdb_shell_parse_hex(const char *s, uint8_t *out, size_t capacity, size_t *out_len);
int zdb_shell_parse_timeout(const char *s, k_timeout_t *out);

/** True when every byte is printable ASCII, tolerating one trailing NUL. */
bool zdb_shell_printable(const uint8_t *data, size_t len);

/** Print "<label>: <hex>", wrapping long values across repeated labels. */
void zdb_shell_print_hex(const struct shell *sh, const char *label, const uint8_t *data,
			 size_t len);

/**
 * Parse the optional trailing "[limit] [from_ms to_ms]" arguments shared by the
 * listing commands, starting at @p first.
 *
 * @return 0, or a negative errno after printing the reason.
 */
int zdb_shell_parse_listing_args(const struct shell *sh, size_t argc, char **argv, size_t first,
				 uint64_t *limit, uint64_t *from_ms, uint64_t *to_ms,
				 bool *windowed);

#endif /* ZEPHYRDB_SHELL_H_ */
