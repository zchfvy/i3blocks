/*
 * bar.c - status line handling functions
 * Copyright (C) 2014  Vivien Didelot
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdlib.h>
#include <string.h>

#include "bar.h"
#include "block.h"
#include "config.h"
#include "json.h"
#include "line.h"
#include "log.h"
#include "map.h"
#include "sched.h"
#include "sys.h"

/* See https://i3wm.org/docs/i3bar-protocol.html for details */

static struct {
	const char * const key;
	bool string;
} i3bar_keys[] = {
	{ "", false }, /* unknown key */

	/* Standard keys */
	{ "full_text", true },
	{ "short_text", true },
	{ "color", true },
	{ "background", true },
	{ "border", true },
	{ "min_width", false }, /* can also be a number */
	{ "align", true },
	{ "name", true },
	{ "instance", true },
	{ "urgent", false },
	{ "separator", false },
	{ "separator_block_width", false },
	{ "markup", true },

	/* i3-gaps features */
	{ "border_top", false },
	{ "border_bottom", false },
	{ "border_left", false },
	{ "border_right", false },
};

static unsigned int i3bar_indexof(const char *key)
{
	unsigned int i;

	for (i = 0; i < sizeof(i3bar_keys) / sizeof(i3bar_keys[0]); i++)
		if (strcmp(i3bar_keys[i].key, key) == 0)
			return i;

	return 0;
}

static int i3bar_line_cb(char *line, size_t num, void *data)
{
	unsigned int index = num + 1;
	struct map *map = data;
	const char *key;

	if (index >= sizeof(i3bar_keys) / sizeof(i3bar_keys[0])) {
		debug("ignoring excess line %d: %s", num, line);
		return 0;
	}

	key = i3bar_keys[index].key;

	return map_set(map, key, line);
}

int i3bar_read(int fd, size_t count, struct map *map)
{
	return line_read(fd, count, i3bar_line_cb, map);
}

static int i3bar_dump_key(const char *key, const char *value, void *data)
{
	unsigned int index = i3bar_indexof(key);
	bool string = i3bar_keys[index].string;
	char buf[BUFSIZ];
	bool escape;
	int err;

	/* Skip unknown keys */
	if (!index)
		return 0;

	if (!value)
		value = "null";

	if (string) {
		if (json_is_string(value))
			escape = false; /* Expected string already quoted */
		else
			escape = true; /* Enforce the string type */
	} else {
		if (json_is_valid(value))
			escape = false; /* Already valid JSON */
		else
			escape = true; /* Unquoted string */
	}

	if (escape) {
		err = json_escape(value, buf, sizeof(buf));
		if (err)
			return err;

		value = buf;
	}

	fprintf(stdout, ",\"%s\":%s", key, value);

	return 0;
}

static void i3bar_dump_block(struct block *block)
{
	fprintf(stdout, ",{\"\":\"\"");
	block_for_each(block, i3bar_dump_key, NULL);
	fprintf(stdout, "}");
}

static void i3bar_dump(struct bar *bar)
{
	struct block *block = bar->blocks;

	fprintf(stdout, ",[{\"full_text\":\"\"}");

	while (block) {
		/* full_text is the only mandatory key */
		if (block_get(block, "full_text"))
			i3bar_dump_block(block);
		else
			block_debug(block, "no text to display, skipping");

		block = block->next;
	}

	fprintf(stdout, "]\n");
	fflush(stdout);
}

static void term_save_cursor(void)
{
	fprintf(stdout, "\033[s\033[?25l");
}

static void term_restore_cursor(void)
{
	fprintf(stdout, "\033[u\033[K");
}

static void term_reset_cursor(void)
{
	fprintf(stdout, "\033[?25h");
}

static void term_start(struct bar *bar)
{
	term_save_cursor();
	term_restore_cursor();
}

static void term_stop(struct bar *bar)
{
	term_reset_cursor();
}

static void term_dump(struct bar *bar)
{
	struct block *block = bar->blocks;

	term_restore_cursor();

	while (block) {
		const char *full_text = block_get(block, "full_text");

		if (full_text)
    			fprintf(stdout, "%s ", full_text);

		block = block->next;
	}

	fflush(stdout);
}

static void bar_freeze(struct bar *bar)
{
	bar->frozen = true;
}

static bool bar_unfreeze(struct bar *bar)
{
	if (bar->frozen) {
		bar->frozen = false;
		return true;
	}

	return false;
}

static bool bar_frozen(struct bar *bar)
{
	return bar->frozen;
}

static void i3bar_log(int lvl, const char *fmt, ...)
{
	const char *color, *urgent, *prefix;
	struct bar *bar = log_data;
	char buf[BUFSIZ];
	va_list ap;

	/* Ignore messages above defined log level and errors */
	if (log_level < lvl || lvl > LOG_ERROR)
		return;

	switch (lvl) {
	case LOG_FATAL:
		prefix = "Fatal! ";
		color = "#FF0000";
		urgent = "true";
		break;
	case LOG_ERROR:
		prefix = "Error: ";
		color = "#FF8000";
		urgent = "true";
		break;
	default:
		prefix = "";
		color = "#FFFFFF";
		urgent = "true";
		break;
	}

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);

	/* TODO json escape text */
	fprintf(stdout, ",[{");
	fprintf(stdout, "\"full_text\":\"%s%s. Increase log level and/or check stderr for details.\"", prefix, buf);
	fprintf(stdout, ",");
	fprintf(stdout, "\"short_text\":\"%s%s\"", prefix, buf);
	fprintf(stdout, ",");
	fprintf(stdout, "\"urgent\":\"%s\"", urgent);
	fprintf(stdout, ",");
	fprintf(stdout, "\"color\":\"%s\"", color);
	fprintf(stdout, "}]\n");
	fflush(stdout);

	bar_freeze(bar);
}

static void i3bar_start(struct bar *bar)
{
	fprintf(stdout, "{\"version\":1,\"click_events\":true}\n[[]\n");
	fflush(stdout);

	/* From now on the bar can handle log messages */
	log_data = bar;
	log_handle = i3bar_log;
}

static void i3bar_stop(struct bar *bar)
{
	/* From now on the bar can handle log messages */
	log_handle = NULL;
	log_data = NULL;

	fprintf(stdout, "]\n");
	fflush(stdout);
}

static struct block *bar_find(struct bar *bar, const struct map *map)
{
	const char *block_name, *block_instance;
	const char *map_name, *map_instance;
	struct block *block = bar->blocks;

	/* "name" and "instance" are the only identifiers provided by i3bar */
	map_name = map_get(map, "name") ? : "";
	map_instance = map_get(map, "instance") ? : "";

	while (block) {
		block_name = block_get(block, "name") ? : "";
		block_instance = block_get(block, "instance") ? : "";

		if (strcmp(block_name, map_name) == 0 &&
		    strcmp(block_instance, map_instance) == 0)
			return block;

		block = block->next;
	}

	return NULL;
}

static int bar_click_copy_cb(const char *key, const char *value, void *data)
{
	return block_set(data, key, value);
}

int bar_click(struct bar *bar)
{
	struct block *block;
	struct map *click;
	int err;

	if (bar_unfreeze(bar))
		bar_dump(bar);

	click = map_create();
	if (!click)
		return -ENOMEM;

	for (;;) {
		/* Each click is one JSON object per line */
		err = json_read(STDIN_FILENO, 1, click);
		if (err) {
			if (err == -EAGAIN)
				err = 0;

			break;
		}

		/* Look for the corresponding block */
		block = bar_find(bar, click);
		if (block) {
			err = map_for_each(click, bar_click_copy_cb, block);
			if (err)
				break;

			err = block_click(block);
			if (err)
				break;
		}

		map_clear(click);
	}

	map_destroy(click);

	return err;
}

void bar_dump(struct bar *bar)
{
	if (bar_frozen(bar)) {
		debug("bar frozen, skipping");
		return;
	}

	if (bar->term)
		term_dump(bar);
	else
		i3bar_dump(bar);
}

static struct block *bar_add_block(struct bar *bar, const struct map *map)
{
	struct block *block;
	int err;

	block = block_create();
	if (!block)
		return NULL;

	err = block_setup(block, map);
	if (err) {
		block_destroy(block);
		return NULL;
	}

	return block;
}

static int bar_config_cb(struct map *map, void *data)
{
	struct bar *bar = data;
	struct block *block = bar->blocks;

	while (block->next)
		block = block->next;

	block->next = bar_add_block(bar, map);

	map_destroy(map);

	if (!block->next)
		return -ENOMEM;

	return 0;
}

void bar_load(struct bar *bar, const char *path)
{
	int err;

	err = config_load(path, bar_config_cb, bar);
	if (err)
		fatal("Failed to load bar configuration file");
}

void bar_schedule(struct bar *bar)
{
	int err;

	/* Initial display (for static blocks and loading labels) */
	bar_dump(bar);

	err = sched_init(bar);
	if (err)
		fatal("Failed to initialize scheduler");

	sched_start(bar);
}

void bar_destroy(struct bar *bar)
{
	struct block *block = bar->blocks;
	struct block *next;

	if (bar->term)
		term_stop(bar);
	else
		i3bar_stop(bar);

	while (block) {
		next = block->next;
		block_destroy(block);
		block = next;
	}

	free(bar);
}

struct bar *bar_create(bool term)
{
	struct bar *bar;
	int err;

	bar = calloc(1, sizeof(struct bar));
	if (!bar)
		return NULL;

	bar->blocks = bar_add_block(bar, NULL);
	if (!bar->blocks) {
		free(bar);
		return NULL;
	}

	bar->term = term;
	if (bar->term)
    		term_start(bar);
	else
		i3bar_start(bar);

	return bar;
}
