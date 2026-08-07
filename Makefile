# SPDX-License-Identifier: GPL-2.0+
# Build rules for the `typewriter` command. APPEND these lines to U-Boot's
# existing cmd/Makefile after copying the sources into cmd/ - do NOT overwrite
# cmd/Makefile with this file (that would drop bootm/booti/bootd etc. and break
# the U-Boot link with "undefined reference to do_bootm").
obj-$(CONFIG_CMD_TYPEWRITER) += cmd_tw.o cmd_tw_fs.o cmd_tw_video.o \
				ime_table.o wubi_embed.o font_data.o
# Temporary WFI-idle debug command (twwfi); remove once WFI idle is solved.
obj-$(CONFIG_CMD_TYPEWRITER) += cmd_twwfi.o
