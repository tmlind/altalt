/*
 * Linux keyboard keycode modifier based on number of alt presses
 *
 * Copyright (C) 2020 Tony Lindgren <tony@atomide.com>
 * License: GPL v2
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include <linux/input.h>
#include <linux/uinput.h>

#define DEFAULT_MODIFIER	KEY_LEFTALT
#define MAX_TAPS		3

#define MOD_KEY(old, new, needs_shift)	\
	{ .keycode = (old), .modkey = (new), .shift = (needs_shift), }
#define ARRAY_SIZE(a)		(sizeof(a) / sizeof(a[0]))

struct mod_key_map {
	unsigned int keycode;
	unsigned int modkey;
	unsigned int shift:1;
};

struct mod_key_table {
	const struct mod_key_map *key_map;
	int nr_keys;
};

enum mod_table {
	MAP_ALTALT,
	MAP_ALTALTALT,
	MAP_LAST,
};

static int armed;
static int taps;

/* Keys with alt + alt */
static const struct mod_key_map keys_altalt[] = {
	/* Top row */
	MOD_KEY(KEY_1, KEY_ESC, 0),
	MOD_KEY(KEY_9, KEY_LEFTBRACE, 0),
	MOD_KEY(KEY_0, KEY_RIGHTBRACE, 0),
	MOD_KEY(KEY_APOSTROPHE, KEY_GRAVE, 0),

	/* Second row */
	MOD_KEY(KEY_E, KEY_EURO, 0),
	MOD_KEY(KEY_BACKSPACE, KEY_DELETE, 0),

	/* Third row */
	MOD_KEY(KEY_TAB, KEY_GRAVE, 1),		/* ~ */
	MOD_KEY(KEY_J, KEY_BACKSLASH, 1),      	/* | */

	/* Fourth row */
	MOD_KEY(KEY_V, KEY_INSERT, 1),		/* shift + insert */
	MOD_KEY(KEY_COMMA, KEY_SEMICOLON, 0),
	MOD_KEY(KEY_DOT, KEY_SEMICOLON, 1),	/* : */

	/* Bottom row */
	MOD_KEY(KEY_SLASH, KEY_BACKSLASH, 0),
	MOD_KEY(KEY_UP, KEY_PAGEUP, 0),
	MOD_KEY(KEY_DOWN, KEY_PAGEDOWN, 0),
};

/* Keys with alt + alt + alt */
static const struct mod_key_map keys_altaltalt[] = {
	/* Top row */
	MOD_KEY(KEY_1, KEY_F1, 0),
	MOD_KEY(KEY_1, KEY_F1, 0),
	MOD_KEY(KEY_2, KEY_F2, 0),
	MOD_KEY(KEY_3, KEY_F3, 0),
	MOD_KEY(KEY_4, KEY_F4, 0),
	MOD_KEY(KEY_5, KEY_F5, 0),
	MOD_KEY(KEY_6, KEY_F6, 0),
	MOD_KEY(KEY_7, KEY_F7, 0),
	MOD_KEY(KEY_8, KEY_F8, 0),
	MOD_KEY(KEY_9, KEY_F9, 0),
	MOD_KEY(KEY_0, KEY_F10, 0),

	/* Second row */
	MOD_KEY(KEY_APOSTROPHE, KEY_GRAVE, 1),	/* ~ */

	/* Third row */
};

static const struct mod_key_table map_altalt = {
	.key_map = keys_altalt,
	.nr_keys = ARRAY_SIZE(keys_altalt),
};

static const struct mod_key_table map_altaltalt = {
	.key_map = keys_altaltalt,
	.nr_keys = ARRAY_SIZE(keys_altaltalt),
};

static const struct mod_key_table *maps[MAP_LAST] = {
	&map_altalt, &map_altaltalt,
};

static struct uinput_setup usetup = {
	.name = "altalt",
	.id.bustype = BUS_USB,
	.id.vendor = 0x1234,
	.id.product = 0x5678,
};

static int register_key_table(int evfd, const struct mod_key_map *keymap,
			     int nr_keys)
{
	const struct mod_key_map *key;
	int i, error;

	for (i = 0; i < nr_keys; i++) {
		key = &keymap[i];
		error = ioctl(evfd, UI_SET_KEYBIT, key->modkey);
		if (error)
			return error;
	}

	return 0;
}

static int register_keys(int evfd)
{
	const struct mod_key_table *map;
	int i, error;

	for (i = 0; i < MAP_LAST; i++) {
		map = maps[i];
		register_key_table(evfd, map->key_map, map->nr_keys);
	}

	error = ioctl(evfd, UI_SET_KEYBIT, KEY_LEFTSHIFT);
	if (error)
		return error;

	return 0;
}

static int find_keycode(const struct mod_key_table *table, int key,
			int *new, int *shift)
{
	const struct mod_key_map *keys, *k;
	int i;

	keys = table->key_map;
	*new = 0;
	*shift = 0;

	for (i = 0; i < table->nr_keys; i++) {
		k = &keys[i];
		if (key == k->keycode) {
			*new = k->modkey;
			*shift = k->shift;
			break;
		}
	}

	return new ? 0 : -ENOENT;
}

/* Value of 1 = down, 2 = repeat, 0 = release */
static void send_key(int evfd, int keycode, int value)
{
	struct input_event ev;
	int error;

	memset(&ev, 0, sizeof(ev));
	ev.type = EV_KEY;
	ev.value = value;
	ev.code = keycode;
	error = write(evfd, &ev, sizeof(ev));
	if (error < 0)
		goto err_out;

	memset(&ev, 0, sizeof(ev));
	ev.type = EV_SYN;
	error = write(evfd, &ev, sizeof(ev));
	if (error < 0)
		goto err_out;

	return;

err_out:
	fprintf(stderr, "Failed to send key: %i\n", error);
}

static int feed_modifier(int fd, int evfd, int keycode, int down)
{
	enum mod_table index = 0;
	int error, new, shift;

	switch (taps) {
	case 2:
		index = MAP_ALTALT;
		break;
	case 3:
		index = MAP_ALTALTALT;
		break;
	default:
		return 0;
	}

	error = find_keycode(maps[index], keycode, &new, &shift);
	if (error)
		return 0;

	if (shift && down)
		send_key(evfd, KEY_LEFTSHIFT, 1);

	send_key(evfd, new, down);

	if (shift && !down)
		send_key(evfd, KEY_LEFTSHIFT, 0);

	return 0;
}

static void arm_handler(int fd)
{
	int error;

	if (armed)
		return;

	error = ioctl(fd, EVIOCGRAB, (void *)1);
	if (!error) {
		armed = 1;
	} else {
		fprintf(stderr, "Could not grab input: %i\n", error);
		armed = 0;
	}
}

static void disarm_handler(int fd)
{
	int error;

	if (!armed)
		return;

	error = ioctl(fd, EVIOCGRAB, (void *)0);
	if (error)
		fprintf(stderr, "Could not release input: %i\n", error);

	armed = 0;
	taps = 0;
}

static int handle_events(int fd, int evfd)
{
	struct input_event ev;
	int n, active = 0;

	while (1) {
		n = read(fd, &ev, sizeof(ev));
		if (n != sizeof(ev))
			return n;
		if (ev.type != EV_KEY)
			continue;

		switch (ev.value) {
		case 1:
			if (ev.code == DEFAULT_MODIFIER)
				taps++;
			if (taps > MAX_TAPS) {
				disarm_handler(fd);
				break;
			}
			if (ev.code != DEFAULT_MODIFIER) {
				feed_modifier(fd, evfd, ev.code, 1);
				active = 1;
			}
			break;
		case 2:
			if (active)
				send_key(evfd, ev.code, 2);
			break;
		case 0:
			if (ev.code == DEFAULT_MODIFIER && taps > 1)
				arm_handler(fd);
			if (active || ev.code != DEFAULT_MODIFIER) {
				feed_modifier(fd, evfd, ev.code, 0);
				disarm_handler(fd);
				active = 0;
			}
			if (ev.code != DEFAULT_MODIFIER)
				taps = 0;
			break;
		default:
			break;
		}

	}

	return 0;
}

int main(int argc, char **argv)
{
	const char *device;
	int fd, evfd, error;

	if (argc < 2) {
		printf("Usage: %s input_device\n\n", argv[0]);

		return -1;
	}

	device = argv[1];

	fd = open(device, O_RDONLY);
	if (fd < 0) {
		fprintf(stderr, "Could not open %s: %i\n", device, fd);

		return fd;
	}

	evfd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
	if (evfd < 0) {
		fprintf(stderr, "Could not open /dev/uinput: %i\n", evfd);
		close(fd);

		return fd;
	}

	error = ioctl(evfd, UI_SET_EVBIT, EV_KEY);
	if (error)
		goto err_close;

	error = register_keys(evfd);
	if (error)
		goto err_close;

	error = ioctl(evfd, UI_DEV_SETUP, &usetup);
	if (error < 0)
		goto err_close;

	error = ioctl(evfd, UI_DEV_CREATE);
	if (error < 0)
		goto err_close;

	error = handle_events(fd, evfd);
	if (error)
		fprintf(stderr, "Got error %i\n", error);

	error = ioctl(evfd, UI_DEV_DESTROY);

err_close:
	close(evfd);
	close(fd);

	return 0;
}
