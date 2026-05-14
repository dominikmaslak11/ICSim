/* ICSim TOML configuration loader
 *
 * Replaces hardcoded #define CAN IDs and byte positions with
 * runtime-loaded TOML files via tomlc99.
 *
 * Defaults match the original built-in values so that -m is
 * always optional.
 */
#ifndef ICSIM_CONFIG_H
#define ICSIM_CONFIG_H

#include "can_platform.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Supported signal types (expand as new signals are added) */
enum {
	ICSIM_SIGNAL_SPEED = 0,
	ICSIM_SIGNAL_DOORS,
	ICSIM_SIGNAL_TURN,
	ICSIM_SIGNAL_COUNT
};

/* Door bit definitions (matches original CAN_DOORx_LOCK values) */
#define ICSIM_DOOR1 0x01
#define ICSIM_DOOR2 0x02
#define ICSIM_DOOR3 0x04
#define ICSIM_DOOR4 0x08

#define ICSIM_TURN_LEFT  0x01
#define ICSIM_TURN_RIGHT 0x02

/* ------------------------------------------------------------------ */

typedef struct {
	canid_t door_id;
	canid_t signal_id;
	canid_t speed_id;
	int door_pos;      /* byte offset in CAN data */
	int signal_pos;
	int speed_pos;
} icsim_can_t;

typedef struct {
	int length;         /* bytes in CAN frame */
	double scaling;     /* raw-to-mph multiplier */
	int divisor;
} icsim_signal_speed_t;

typedef struct {
	int length;
	unsigned char door1_mask;
	unsigned char door2_mask;
	unsigned char door3_mask;
	unsigned char door4_mask;
} icsim_signal_doors_t;

typedef struct {
	int length;
	unsigned char left_mask;
	unsigned char right_mask;
} icsim_signal_turn_t;

typedef struct {
	int width;
	int height;
} icsim_dashboard_t;

typedef struct {
	int x, y, w, h;
	int needle_center_x, needle_center_y;
	double angle_min, angle_max;
	double value_min, value_max;
} icsim_speedometer_t;

/* ------------------------------------------------------------------ */

typedef struct {
	char name[64];
	char description[256];

	icsim_can_t can;

	icsim_signal_speed_t speed;
	icsim_signal_doors_t  doors;
	icsim_signal_turn_t   turn;

	icsim_dashboard_t    dashboard;
	icsim_speedometer_t  speedometer;

	int loaded;       /* 1 if config file was loaded successfully */
} icsim_config_t;

/* ------------------------------------------------------------------ */

/* Load configuration from a TOML file.  Returns 0 on success.
 * If path is NULL the struct is filled with built-in defaults. */
int icsim_config_load(icsim_config_t *cfg, const char *path);

/* Fill config with original hard-coded defaults (no file needed) */
void icsim_config_defaults(icsim_config_t *cfg);

#ifdef __cplusplus
}
#endif

#endif /* ICSIM_CONFIG_H */
