#ifndef _STUB_COMMAND_H
#define _STUB_COMMAND_H
#include <linux/types.h>
struct cmd_tbl { const char *name; };
#define CMD_RET_SUCCESS 0
#define CMD_RET_FAILURE 1
#define CMD_RET_USAGE  -1
/* U_BOOT_CMD: emit the handler as a used static so -Wunused doesn't fire and
 * the body is type-checked. Signature matches include/command.h's cmd hook. */
#define U_BOOT_CMD(_name, _maxargs, _rep, _cmd, _usage, ...) \
	static int (*const _ubcmd_##_name)(struct cmd_tbl *, int, int, \
		char *const[]) __attribute__((unused)) = _cmd;
int printf(const char *fmt, ...);
#endif
