# SPDX-License-Identifier: GPL-2.0+
# Build rule for the `typewriter` command. Append this line to cmd/Makefile
# (or keep it here and include it) after copying the sources into cmd/.
obj-$(CONFIG_CMD_TYPEWRITER) += cmd_tw.o cmd_tw_fs.o cmd_tw_video.o \
				ime_table.o wubi_embed.o font_data.o
