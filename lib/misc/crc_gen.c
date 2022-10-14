/*
 * Copyright (C) 2010 Red Hat, Inc. All rights reserved.
 *
 * This file is part of LVM2.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU Lesser General Public License v.2.1.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/*
 * Helper program to generate table included in crc.c.
 */
#include "lib/misc/lib.h"

int main(int argc, char **argv)
{
	uint32_t crc, i, j;

	printf("\t/* CRC-32 byte lookup table generated by crc_gen.c */\n");
	printf("\tstatic const uint32_t crctab[] = {");

	for (i = 0; i < 256; i++) {
		crc = i;
		for (j = 0; j < 8; j++) {
			if (crc & 1)
				crc = 0xedb88320L ^ (crc >> 1);
			else
				crc = crc >> 1;
		}

		if (i % 8)
			printf(" ");
		else
			printf("\n\t\t");

		printf("0x%08.8x,", crc);
	}

	printf("\n\t};\n");

	return 0;
}
