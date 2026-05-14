#ifdef _WIN32
#include "getopt_compat.h"

#include <stdio.h>
#include <string.h>

char *optarg = NULL;
int optind = 1;

int getopt(int argc, char *const argv[], const char *optstring)
{
	static int optpos = 1;
	const char *option;
	char opt;

	optarg = NULL;

	if (optind >= argc || argv[optind][0] != '-' || argv[optind][1] == '\0')
		return -1;

	if (strcmp(argv[optind], "--") == 0) {
		optind++;
		return -1;
	}

	opt = argv[optind][optpos];
	option = strchr(optstring, opt);
	if (!option || opt == ':') {
		if (argv[optind][++optpos] == '\0') {
			optind++;
			optpos = 1;
		}
		return '?';
	}

	if (option[1] == ':') {
		if (argv[optind][optpos + 1] != '\0') {
			optarg = &argv[optind][optpos + 1];
			optind++;
		} else if (optind + 1 < argc) {
			optarg = argv[++optind];
			optind++;
		} else {
			optind++;
			optpos = 1;
			return '?';
		}
		optpos = 1;
	} else {
		if (argv[optind][++optpos] == '\0') {
			optind++;
			optpos = 1;
		}
	}

	return opt;
}
#endif
