/* help.c - Alpine Package Keeper (APK)
 *
 * Copyright (C) 2020 Timo Teräs <timo.teras@iki.fi>
 * All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <zlib.h>
#include "apk_applet.h"
#include "apk_print.h"
#include "help.h"

static LIST_HEAD(apk_applet_list);

#define apk_applet_foreach(iter) list_for_each_entry(iter, &apk_applet_list, node)

void apk_applet_register(struct apk_applet *applet)
{
	list_init(&applet->node);
	list_add_tail(&applet->node, &apk_applet_list);
}

struct apk_applet *apk_applet_find(const char *name)
{
	struct apk_applet *a;

	apk_applet_foreach(a) {
		if (strcmp(name, a->name) == 0)
			return a;
	}
	return NULL;
}

#ifndef NO_HELP
static inline int is_group(struct apk_applet *applet, const char *topic)
{
	if (!applet) return strcasecmp(topic, "apk") == 0;
	if (strcasecmp(topic, applet->name) == 0) return 1;
	for (int i = 0; applet->optgroups[i] && i < ARRAY_SIZE(applet->optgroups); i++)
		if (strcasecmp(applet->optgroups[i]->desc, topic) == 0) return 1;
	return 0;
}
#endif

void apk_applet_help(struct apk_applet *applet)
{
#ifndef NO_HELP
	char buf[uncompressed_help_size], *ptr, *msg;
	unsigned long len = sizeof buf;
	int num = 0;

	uncompress((unsigned char*) buf, &len, compressed_help, sizeof compressed_help);
	for (ptr = buf; *ptr && ptr < &buf[len]; ptr = msg + strlen(msg) + 1) {
		msg = ptr + strlen(ptr) + 1;

		if (is_group(applet, ptr)) {
			fputc('\n', stdout);
			fwrite(msg, strlen(msg), 1, stdout);
			num++;
		}
	}
	if (num == 0) apk_error("Help not found");
#else
	fputc('\n', stdout);
	apk_error("This apk-tools has been built without help");
#endif
}
