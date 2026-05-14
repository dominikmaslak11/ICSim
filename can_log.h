/* CAN frame logging in Vector ASC format
 *
 * ASC is the ASCII logging format used by Vector CANalyzer / CANoe.
 * Files are readable by Wireshark, SavvyCAN, and most CAN tools.
 */
#ifndef ICSIM_CAN_LOG_H
#define ICSIM_CAN_LOG_H

#include "can_platform.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque log handle */
typedef struct can_log can_log_t;

/* Open a log file for recording.
 * Returns NULL on failure (prints to stderr). */
can_log_t *can_log_open_record(const char *path);

/* Open a log file for replay.
 * Returns NULL on failure (prints to stderr). */
can_log_t *can_log_open_replay(const char *path);

/* Record a CAN frame to the log (timestamped automatically).
 * Safe to call from any thread. */
void can_log_record(can_log_t *log, const struct canfd_frame *frame);

/* Read the next CAN frame from a replay log.
 * Returns timestamp offset in seconds via *t_offset.
 * Returns 0 on EOF or error.
 * `frame` is filled with the parsed CAN frame.
 * `mtu_out` receives CAN_MTU. */
int can_log_replay_next(can_log_t *log, struct canfd_frame *frame,
			double *t_offset, size_t *mtu_out);

/* Close and free the log. */
void can_log_close(can_log_t *log);

#ifdef __cplusplus
}
#endif

#endif /* ICSIM_CAN_LOG_H */
