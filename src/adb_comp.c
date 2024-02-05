/* adb_comp.c - Alpine Package Keeper (APK)
 *
 * Copyright (C) 2021 Timo Teräs <timo.teras@iki.fi>
 * All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "apk_defines.h"
#include "adb.h"

struct apk_istream *adb_decompress(struct apk_istream *is, adb_comp_t *compression)
{
	adb_comp_t c = -1;

	if (IS_ERR(is)) return is;

	uint8_t *buf = apk_istream_peek(is, 4);
	if (IS_ERR(buf)) return ERR_PTR(apk_istream_close_error(is, PTR_ERR(buf)));
	if (memcmp(buf, "ADB", 3) != 0) return ERR_PTR(apk_istream_close_error(is, -APKE_ADB_HEADER));
	switch (buf[3]) {
	case '.':
		c = ADB_COMP_NONE;
		break;
	case 'd':
		c = ADB_COMP_DEFLATE;
		apk_istream_get(is, 4);
		is = apk_istream_deflate(is);
		break;
	case 'z':
		c = ADB_COMP_ZSTD;
		apk_istream_get(is, 4);
		is = apk_istream_zstd(is);
		break;
	case 'Z':
		c = ADB_COMP_ZSTD_FAST;
		apk_istream_get(is, 4);
		is = apk_istream_zstd(is);
		break;
	case 's':
		c = ADB_COMP_ZSTD_SLOW;
		apk_istream_get(is, 4);
		is = apk_istream_zstd(is);
		break;
	}
	if (c == -1) return ERR_PTR(apk_istream_close_error(is, -APKE_ADB_COMPRESSION));
	if (compression) *compression = c;
	return is;
}

struct apk_ostream *adb_compress(struct apk_ostream *os, adb_comp_t compression)
{
	if (IS_ERR(os)) return os;
	switch (compression) {
	case ADB_COMP_NONE:
		return os;
	case ADB_COMP_DEFLATE:
		if (apk_ostream_write(os, "ADBd", 4) < 0) goto err;
		return apk_ostream_deflate(os);
	case ADB_COMP_ZSTD:
		if (apk_ostream_write(os, "ADBz", 4) < 0) goto err;
		return apk_ostream_zstd(os, 1);
	case ADB_COMP_ZSTD_FAST:
		if (apk_ostream_write(os, "ADBZ", 4) < 0) goto err;
		return apk_ostream_zstd(os, 0);
	case ADB_COMP_ZSTD_SLOW:
		if (apk_ostream_write(os, "ADBs", 4) < 0) goto err;
		return apk_ostream_zstd(os, 2);
	}
err:
	apk_ostream_cancel(os, -APKE_ADB_COMPRESSION);
	return ERR_PTR(apk_ostream_close(os));
}
