/* CAN frame logging in Vector ASC format — implementation
 *
 * ASC header:
 *   date <current date>
 *   base hex timestamps absolute
 *   internal events logged
 *   Begin Triggerblock <date>
 *
 * ASC frame line:
 *   <timestamp> 1 <can_id> Rx d <dlc> <hex bytes> <padding>
 *
 * Timestamps are double-precision seconds relative to file start.
 */
#include "can_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#define snprintf _snprintf
#include <windows.h>
#endif

struct can_log {
	FILE  *fp;
	int    mode;       /* 0 = record, 1 = replay */
	double start_time; /* record: program start; replay: first frame offset */
	double base_time;  /* record: absolute wall time (used for header) */
	int    replay_eof;
	char   replay_line[256]; /* buffered next line */
	double replay_next_t;    /* timestamp of buffered line */
};

/* -------- Recording -------- */

static double wall_seconds(void)
{
#ifdef _WIN32
	LARGE_INTEGER freq, cnt;
	QueryPerformanceFrequency(&freq);
	QueryPerformanceCounter(&cnt);
	return (double)cnt.QuadPart / (double)freq.QuadPart;
#else
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
#endif
}

static void write_asc_header(FILE *fp, double base)
{
	time_t t = (time_t)base;
	struct tm *tm = localtime(&t);
	char datebuf[64];
	strftime(datebuf, sizeof(datebuf), "%a %b %d %H:%M:%S %Y", tm);
	fprintf(fp, "date %s\n", datebuf);
	fprintf(fp, "base hex timestamps absolute\n");
	fprintf(fp, "internal events logged\n");
	fprintf(fp, "Begin Triggerblock %s\n", datebuf);
}

can_log_t *can_log_open_record(const char *path)
{
	can_log_t *log = calloc(1, sizeof(*log));
	if (!log) return NULL;

	log->fp = fopen(path, "w");
	if (!log->fp) {
		fprintf(stderr, "can_log: cannot open '%s' for recording\n", path);
		free(log);
		return NULL;
	}

	log->mode = 0;
	log->start_time = wall_seconds();
	log->base_time = log->start_time;
	write_asc_header(log->fp, log->base_time);

	return log;
}

void can_log_record(can_log_t *log, const struct canfd_frame *frame)
{
	if (!log || !log->fp || log->mode != 0) return;

	double elapsed = wall_seconds() - log->start_time;
	unsigned int id = frame->can_id & CAN_SFF_MASK;
	unsigned int len = frame->len;
	if (len > CAN_MAX_DLEN) len = CAN_MAX_DLEN;

	fprintf(log->fp, "%12.6f 1 %03X Rx d %u", elapsed, id, len);
	for (unsigned int i = 0; i < len; i++)
		fprintf(log->fp, " %02X", frame->data[i]);
	/* Pad to 8 bytes with zeros (ASC convention) */
	for (unsigned int i = len; i < 8; i++)
		fprintf(log->fp, " 00");
	fprintf(log->fp, "\n");
	fflush(log->fp);
}

/* -------- Replay -------- */

/* Read one non-empty, non-comment line. Returns NULL on EOF. */
static char *read_line(FILE *fp, char *buf, size_t sz)
{
	while (fgets(buf, (int)sz, fp)) {
		/* Skip comments and headers */
		char *p = buf;
		while (*p == ' ' || *p == '\t') p++;
		if (*p == '\0' || *p == '\n' || *p == '\r' || *p == '/' || *p == ';')
			continue;
		if (strncmp(p, "date ", 5) == 0) continue;
		if (strncmp(p, "base ", 5) == 0) continue;
		if (strncmp(p, "Begin ", 6) == 0) continue;
		if (strncmp(p, "End ", 4) == 0) continue;
		if (strncmp(p, "internal ", 9) == 0) continue;
		return buf;
	}
	return NULL;
}

/* Parse ASC frame line: "  0.123456 1 244 Rx d 8 AA BB ..."
 * Returns 1 on success, 0 on failure. */
static int parse_asc_line(const char *line, struct canfd_frame *frame,
			  double *t_out)
{
	double t;
	unsigned int bus, id, dlc;
	char dir[3], dtype[2];
	int n;

	n = sscanf(line, "%lf %u %X %2s %1s %u", &t, &bus, &id, dir, dtype, &dlc);
	if (n < 6) return 0;

	/* We expect 1 <id> Rx d <dlc> */
	if (bus != 1) return 0;
	if (strcmp(dir, "Rx") != 0 && strcmp(dir, "Tx") != 0) return 0;

	if (dlc > CAN_MAX_DLEN) dlc = CAN_MAX_DLEN;

	memset(frame, 0, sizeof(*frame));
	frame->can_id = (canid_t)id & CAN_SFF_MASK;
	frame->len = (uint8_t)dlc;

	/* Find data bytes after the DLC */
	const char *p = line;
	/* Skip past timestamp */
	while (*p == ' ') p++;
	p = strchr(p, ' '); if (!p) return 0; /* skip timestamp */
	p = strchr(p + 1, ' '); if (!p) return 0; /* skip bus */
	p = strchr(p + 1, ' '); if (!p) return 0; /* skip ID */
	p = strchr(p + 1, ' '); if (!p) return 0; /* skip Rx/Tx */
	p = strchr(p + 1, ' '); if (!p) return 0; /* skip d */
	/* Now p points to after "d" */
	while (*p == ' ') p++;
	/* Skip DLC */
	while (*p >= '0' && *p <= '9') p++;

	for (unsigned int i = 0; i < dlc; i++) {
		while (*p == ' ') p++;
		if (!*p || *p == '\n' || *p == '\r') break;
		char hex[3] = {p[0], p[1], '\0'};
		frame->data[i] = (uint8_t)strtoul(hex, NULL, 16);
		p += 2;
	}

	*t_out = t;
	return 1;
}

can_log_t *can_log_open_replay(const char *path)
{
	can_log_t *log = calloc(1, sizeof(*log));
	if (!log) return NULL;

	log->fp = fopen(path, "r");
	if (!log->fp) {
		fprintf(stderr, "can_log: cannot open '%s' for replay\n", path);
		free(log);
		return NULL;
	}

	log->mode = 1;
	log->replay_eof = 0;
	log->replay_next_t = -1.0;

	/* Read and parse the first frame line (skip headers) */
	char *line = read_line(log->fp, log->replay_line, sizeof(log->replay_line));
	if (!line) {
		log->replay_eof = 1;
	} else {
		struct canfd_frame tmp;
		double t;
		if (parse_asc_line(line, &tmp, &t)) {
			log->replay_next_t = t;
			log->start_time = t;        /* first frame is offset 0 */
		} else {
			log->replay_eof = 1;
		}
	}

	return log;
}

int can_log_replay_next(can_log_t *log, struct canfd_frame *frame,
			double *t_offset, size_t *mtu_out)
{
	if (!log || log->mode != 1 || log->replay_eof)
		return 0;

	double t;
	if (!parse_asc_line(log->replay_line, frame, &t))
		return 0;

	*t_offset = t - log->start_time;
	*mtu_out = CAN_MTU;

	/* Read next line for future calls */
	char *line = read_line(log->fp, log->replay_line, sizeof(log->replay_line));
	if (!line) {
		log->replay_eof = 1;
	} else {
		double next_t;
		struct canfd_frame tmp;
		if (!parse_asc_line(line, &tmp, &next_t))
			log->replay_eof = 1;
		else
			log->replay_next_t = next_t;
	}

	return 1;
}

/* -------- Close -------- */
void can_log_close(can_log_t *log)
{
	if (!log) return;

	if (log->mode == 0 && log->fp) {
		fprintf(log->fp, "End Triggerblock\n");
	}

	if (log->fp) fclose(log->fp);
	free(log);
}
