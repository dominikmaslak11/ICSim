#ifndef GETOPT_COMPAT_H
#define GETOPT_COMPAT_H

#ifdef _WIN32
extern char *optarg;
extern int optind;
int getopt(int argc, char *const argv[], const char *optstring);
#else
#include <getopt.h>
#endif

#endif
