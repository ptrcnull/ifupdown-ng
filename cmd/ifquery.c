/*
 * cmd/ifquery.c
 * Purpose: look up information in /etc/network/interfaces
 *
 * Copyright (c) 2020 Ariadne Conill <ariadne@dereferenced.org>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * This software is provided 'as is' and without any warranty, express or
 * implied.  In no event shall the authors be liable for any damages arising
 * from the use of this software.
 */

#define _GNU_SOURCE
#include <fnmatch.h>
#include <stdio.h>
#include <string.h>
#include <getopt.h>
#include "libifupdown/libifupdown.h"
#include "cmd/multicall.h"

void
print_interface(struct lif_interface *iface)
{
	if (iface->is_auto)
		printf("auto %s\n", iface->ifname);

	printf("iface %s\n", iface->ifname);

	struct lif_node *iter;
	LIF_DICT_FOREACH(iter, &iface->vars)
	{
		struct lif_dict_entry *entry = iter->data;

		if (!strcmp(entry->key, "address"))
		{
			struct lif_address *addr = entry->data;
			char addr_buf[512];

			if (!lif_address_unparse(addr, addr_buf, sizeof addr_buf, true))
			{
				printf("  # warning: failed to unparse address\n");
				continue;
			}

			printf("  %s %s\n", entry->key, addr_buf);
		}
		else
			printf("  %s %s\n", entry->key, (const char *) entry->data);
	}

	printf("\n");
}

void
print_interface_dot(struct lif_dict *collection, struct lif_interface *iface, struct lif_interface *parent)
{
	if (iface->is_up)
		return;

	if (parent != NULL)
		printf("\"%s\" -> ", parent->ifname);

	printf("\"%s\"", iface->ifname);

	printf("\n");

	struct lif_dict_entry *entry = lif_dict_find(&iface->vars, "requires");

	if (entry == NULL)
		return;

	char require_ifs[4096] = {};
	strlcpy(require_ifs, entry->data, sizeof require_ifs);
	char *reqp = require_ifs;

	for (char *tokenp = lif_next_token(&reqp); *tokenp; tokenp = lif_next_token(&reqp))
	{
		struct lif_interface *child_if = lif_interface_collection_find(collection, tokenp);

		print_interface_dot(collection, child_if, iface);
		child_if->is_up = true;
	}
}

void
ifquery_usage(void)
{
	fprintf(stderr, "usage: ifquery [options] <interfaces>\n");
	fprintf(stderr, "       ifquery [options] --list\n");

	fprintf(stderr, "\nOptions:\n");
	fprintf(stderr, "  -h, --help                   this help\n");
	fprintf(stderr, "  -V, --version                show this program's version\n");
	fprintf(stderr, "  -i, --interfaces FILE        use FILE for interface definitions\n");
	fprintf(stderr, "  -L, --list                   list matching interfaces\n");
	fprintf(stderr, "  -a, --auto                   only match against interfaces hinted as 'auto'\n");
	fprintf(stderr, "  -I, --include PATTERN        only match against interfaces matching PATTERN\n");
	fprintf(stderr, "  -X, --exclude PATTERN        never match against interfaces matching PATTERN\n");
	fprintf(stderr, "  -P, --pretty-print           pretty print the interfaces instead of just listing\n");
	fprintf(stderr, "  -S, --state-file FILE        use FILE for state\n");
	fprintf(stderr, "  -s, --state                  show configured state\n");
	fprintf(stderr, "  -D, --dot                    generate a dependency graph\n");

	exit(1);
}

struct match_options {
	bool is_auto;
	char *exclude_pattern;
	char *include_pattern;
	bool pretty_print;
	bool dot;
};

void
list_interfaces(struct lif_dict *collection, struct match_options *opts)
{
	struct lif_node *iter;

	if (opts->dot)
	{
		printf("digraph interfaces {\n");
		printf("edge [color=blue fontname=Sans fontsize=10]\n");
		printf("node [fontname=Sans fontsize=10]\n");
	}

	LIF_DICT_FOREACH(iter, collection)
	{
		struct lif_dict_entry *entry = iter->data;
		struct lif_interface *iface = entry->data;

		if (opts->is_auto && !iface->is_auto)
			continue;

		if (opts->exclude_pattern != NULL &&
		    !fnmatch(opts->exclude_pattern, iface->ifname, 0))
			continue;

		if (opts->include_pattern != NULL &&
		    fnmatch(opts->include_pattern, iface->ifname, 0))
			continue;

		if (opts->pretty_print)
			print_interface(iface);
		else if (opts->dot)
			print_interface_dot(collection, iface, NULL);
		else
			printf("%s\n", iface->ifname);
	}

	if (opts->dot)
		printf("}\n");
}

void
list_state(struct lif_dict *state, struct match_options *opts)
{
	struct lif_node *iter;

	LIF_DICT_FOREACH(iter, state)
	{
		struct lif_dict_entry *entry = iter->data;

		if (opts->exclude_pattern != NULL &&
		    !fnmatch(opts->exclude_pattern, entry->key, 0))
			continue;

		if (opts->include_pattern != NULL &&
		    fnmatch(opts->include_pattern, entry->key, 0))
			continue;

		printf("%s=%s\n", entry->key, (const char *) entry->data);
	}
}

int
ifquery_main(int argc, char *argv[])
{
	struct lif_dict state = {};
	struct lif_dict collection = {};
	struct option long_options[] = {
		{"help", no_argument, 0, 'h'},
		{"version", no_argument, 0, 'V'},
		{"interfaces", required_argument, 0, 'i'},
		{"list", no_argument, 0, 'L'},
		{"auto", no_argument, 0, 'a'},
		{"include", required_argument, 0, 'I'},
		{"exclude", required_argument, 0, 'X'},
		{"pretty-print", no_argument, 0, 'P'},
		{"state-file", required_argument, 0, 'S'},
		{"state", no_argument, 0, 's'},
		{"dot", no_argument, 0, 'D'},
		{NULL, 0, 0, 0}
	};
	struct match_options match_opts = {};
	bool listing = false, listing_stat = false;
	char *interfaces_file = INTERFACES_FILE;
	char *state_file = STATE_FILE;

	for (;;)
	{
		int c = getopt_long(argc, argv, "hVi:LaI:X:PS:sD", long_options, NULL);
		if (c == -1)
			break;

		switch (c) {
		case 'h':
			ifquery_usage();
			break;
		case 'V':
			lif_common_version();
			break;
		case 'i':
			interfaces_file = optarg;
			break;
		case 'L':
			listing = true;
			break;
		case 'a':
			match_opts.is_auto = true;
			break;
		case 'I':
			match_opts.include_pattern = optarg;
			break;
		case 'X':
			match_opts.exclude_pattern = optarg;
			break;
		case 'P':
			match_opts.pretty_print = true;
			break;
		case 'S':
			state_file = optarg;
			break;
		case 's':
			listing_stat = true;
			break;
		case 'D':
			match_opts.dot = true;
			break;
		}
	}

	if (!lif_state_read_path(&state, state_file))
	{
		fprintf(stderr, "%s: could not parse %s\n", argv0, state_file);
		return EXIT_FAILURE;
	}

	if (!lif_interface_file_parse(&collection, interfaces_file))
	{
		fprintf(stderr, "%s: could not parse %s\n", argv0, interfaces_file);
		return EXIT_FAILURE;
	}

	/* --list --state is not allowed */
	if (listing && listing_stat)
		ifquery_usage();

	if (listing)
	{
		list_interfaces(&collection, &match_opts);
		return EXIT_SUCCESS;
	}
	else if (listing_stat)
	{
		list_state(&state, &match_opts);
		return EXIT_SUCCESS;
	}

	if (optind >= argc)
		ifquery_usage();

	int idx = optind;
	for (; idx < argc; idx++)
	{
		struct lif_interface *iface = lif_state_lookup(&state, &collection, argv[idx]);

		if (iface == NULL)
		{
			struct lif_dict_entry *entry = lif_dict_find(&collection, argv[idx]);

			if (entry != NULL)
				iface = entry->data;
		}

		if (iface == NULL)
		{
			fprintf(stderr, "%s: unknown interface %s\n", argv0, argv[idx]);
			return EXIT_FAILURE;
		}

		print_interface(iface);
	}

	return EXIT_SUCCESS;
}

struct if_applet ifquery_applet = {
	.name = "ifquery",
	.main = ifquery_main,
	.usage = ifquery_usage
};
