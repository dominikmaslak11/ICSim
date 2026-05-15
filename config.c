/* ICSim TOML configuration loader — implementation
 *
 * Uses tomlc99 (MIT) to parse vehicle model config files.
 * Falls back to built-in defaults when no config is provided.
 */
#include "config.h"

#include "toml.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */

static canid_t parse_hex(const char *s)
{
	if (!s) return 0;
	return (canid_t)strtoul(s, NULL, 16);
}

static int parse_int(const toml_table_t *tab, const char *key, int def)
{
	toml_datum_t val = toml_int_in(tab, key);
	return val.ok ? (int)val.u.i : def;
}

static double parse_double(const toml_table_t *tab, const char *key, double def)
{
	toml_datum_t val = toml_double_in(tab, key);
	return val.ok ? val.u.d : def;
}

/* Reads a string key, copies it into buf, frees the toml-allocated copy.
 * Returns buf (or def if key missing). buf must be at least bufsz bytes. */
static const char *parse_string_into(const toml_table_t *tab, const char *key,
				     char *buf, size_t bufsz, const char *def)
{
	toml_datum_t val = toml_string_in(tab, key);
	if (val.ok && val.u.s) {
		strncpy(buf, val.u.s, bufsz - 1);
		buf[bufsz - 1] = '\0';
		free(val.u.s);
		return buf;
	}
	strncpy(buf, def, bufsz - 1);
	buf[bufsz - 1] = '\0';
	return buf;
}

/* ------------------------------------------------------------------ */

void icsim_config_defaults(icsim_config_t *cfg)
{
	memset(cfg, 0, sizeof(*cfg));

	strncpy(cfg->name, "Default", sizeof(cfg->name) - 1);
	strncpy(cfg->description,
		"Built-in defaults (no config file loaded)",
		sizeof(cfg->description) - 1);

	/* CAN IDs */
	cfg->can.door_id   = 0x19B;  /* 411 */
	cfg->can.signal_id = 0x188;  /* 392 */
	cfg->can.speed_id  = 0x244;  /* 580 */

	/* Byte offsets */
	cfg->can.door_pos   = 2;
	cfg->can.signal_pos = 0;
	cfg->can.speed_pos  = 3;

	/* Speed signal */
	cfg->speed.length  = 2;
	cfg->speed.scaling = 0.6213751;
	cfg->speed.divisor = 100;

	/* Door signal */
	cfg->doors.length     = 1;
	cfg->doors.door1_mask = ICSIM_DOOR1;
	cfg->doors.door2_mask = ICSIM_DOOR2;
	cfg->doors.door3_mask = ICSIM_DOOR3;
	cfg->doors.door4_mask = ICSIM_DOOR4;

	/* Turn signal */
	cfg->turn.length     = 1;
	cfg->turn.left_mask  = ICSIM_TURN_LEFT;
	cfg->turn.right_mask = ICSIM_TURN_RIGHT;

	/* RPM */
	cfg->rpm.rpm_id  = 0x0AA;
	cfg->rpm.rpm_pos = 4;
	cfg->rpm.length  = 2;
	cfg->rpm.scaling = 1.0;
	cfg->rpm.divisor = 4;

	/* Coolant temperature */
	cfg->temp.temp_id  = 0x1B8;
	cfg->temp.temp_pos = 2;
	cfg->temp.length   = 1;
	cfg->temp.scaling  = 1.0;
	cfg->temp.divisor  = 1;

	/* Fuel level */
	cfg->fuel.fuel_id  = 0x2C8;
	cfg->fuel.fuel_pos = 5;
	cfg->fuel.length   = 1;
	cfg->fuel.scaling  = 1.0;
	cfg->fuel.divisor  = 1;

	/* Dashboard layout */
	cfg->dashboard.width  = 692;
	cfg->dashboard.height = 329;

	cfg->speedometer.x = 200;
	cfg->speedometer.y = 80;
	cfg->speedometer.w = 300;
	cfg->speedometer.h = 130;
	cfg->speedometer.needle_center_x = 135;
	cfg->speedometer.needle_center_y = 20;
	cfg->speedometer.angle_min = 0.0;
	cfg->speedometer.angle_max = 180.0;
	cfg->speedometer.value_min = 0.0;
	cfg->speedometer.value_max = 280.0;
}

/* ------------------------------------------------------------------ */

int icsim_config_load(icsim_config_t *cfg, const char *path)
{
	FILE *fp;
	char errbuf[200];
	toml_table_t *root;
	toml_table_t *tcan, *tspeed, *tdoors, *tturn, *tdash, *tspdo;
	char hexbuf[32];

	if (!cfg)
		return -1;

	/* Always start with defaults — TOML overrides only what it defines */
	icsim_config_defaults(cfg);

	if (!path) {
		/* No config file — stick with defaults */
		cfg->loaded = 0;
		return 0;
	}

	fp = fopen(path, "r");
	if (!fp) {
		fprintf(stderr, "config: cannot open '%s' — using defaults\n", path);
		cfg->loaded = 0;
		return 0;
	}

	root = toml_parse_file(fp, errbuf, sizeof(errbuf));
	fclose(fp);

	if (!root) {
		fprintf(stderr, "config: parse error in '%s': %s — using defaults\n",
			path, errbuf);
		cfg->loaded = 0;
		return 0;
	}

	/* --- [vehicle] --- */
	toml_table_t *veh = toml_table_in(root, "vehicle");
	if (veh) {
		parse_string_into(veh, "name", cfg->name, sizeof(cfg->name), "Default");
		parse_string_into(veh, "description", cfg->description,
				  sizeof(cfg->description), "");
	}

	/* --- [can] --- */
	tcan = toml_table_in(root, "can");
	if (tcan) {
		parse_string_into(tcan, "speed_id", hexbuf, sizeof(hexbuf), "0x244");
		cfg->can.speed_id  = parse_hex(hexbuf);
		parse_string_into(tcan, "door_id", hexbuf, sizeof(hexbuf), "0x19B");
		cfg->can.door_id   = parse_hex(hexbuf);
		parse_string_into(tcan, "signal_id", hexbuf, sizeof(hexbuf), "0x188");
		cfg->can.signal_id = parse_hex(hexbuf);

		cfg->can.speed_pos  = parse_int(tcan, "speed_byte", 3);
		cfg->can.door_pos   = parse_int(tcan, "door_byte", 2);
		cfg->can.signal_pos = parse_int(tcan, "signal_byte", 0);
	}

	/* --- [signals.speed] --- */
	tspeed = toml_table_in(root, "signals.speed");
	if (tspeed) {
		cfg->speed.length  = parse_int(tspeed, "length", 2);
		cfg->speed.scaling = parse_double(tspeed, "scaling", 0.6213751);
		cfg->speed.divisor = parse_int(tspeed, "divisor", 100);
	}

	/* --- [signals.doors] --- */
	tdoors = toml_table_in(root, "signals.doors");
	if (tdoors) {
		cfg->doors.length     = parse_int(tdoors, "length", 1);
		cfg->doors.door1_mask = (unsigned char)parse_int(tdoors, "door1_mask", ICSIM_DOOR1);
		cfg->doors.door2_mask = (unsigned char)parse_int(tdoors, "door2_mask", ICSIM_DOOR2);
		cfg->doors.door3_mask = (unsigned char)parse_int(tdoors, "door3_mask", ICSIM_DOOR3);
		cfg->doors.door4_mask = (unsigned char)parse_int(tdoors, "door4_mask", ICSIM_DOOR4);
	}

	/* --- [signals.turn] --- */
	tturn = toml_table_in(root, "signals.turn");
	if (tturn) {
		cfg->turn.length     = parse_int(tturn, "length", 1);
		cfg->turn.left_mask  = (unsigned char)parse_int(tturn, "left_mask", ICSIM_TURN_LEFT);
		cfg->turn.right_mask = (unsigned char)parse_int(tturn, "right_mask", ICSIM_TURN_RIGHT);
	}

	/* --- [signals.rpm] --- */
	toml_table_t *trpm = toml_table_in(root, "signals.rpm");
	if (trpm) {
		parse_string_into(trpm, "rpm_id", hexbuf, sizeof(hexbuf), "0x0AA");
		cfg->rpm.rpm_id  = parse_hex(hexbuf);
		cfg->rpm.rpm_pos = parse_int(trpm, "rpm_byte", 4);
		cfg->rpm.length  = parse_int(trpm, "length", 2);
		cfg->rpm.scaling = parse_double(trpm, "scaling", 1.0);
		cfg->rpm.divisor = parse_int(trpm, "divisor", 4);
	}

	/* --- [signals.temp] --- */
	toml_table_t *ttemp = toml_table_in(root, "signals.temp");
	if (ttemp) {
		parse_string_into(ttemp, "temp_id", hexbuf, sizeof(hexbuf), "0x1B8");
		cfg->temp.temp_id  = parse_hex(hexbuf);
		cfg->temp.temp_pos = parse_int(ttemp, "temp_byte", 2);
		cfg->temp.length   = parse_int(ttemp, "length", 1);
		cfg->temp.scaling  = parse_double(ttemp, "scaling", 1.0);
		cfg->temp.divisor  = parse_int(ttemp, "divisor", 1);
	}

	/* --- [signals.fuel] --- */
	toml_table_t *tfuel = toml_table_in(root, "signals.fuel");
	if (tfuel) {
		parse_string_into(tfuel, "fuel_id", hexbuf, sizeof(hexbuf), "0x2C8");
		cfg->fuel.fuel_id  = parse_hex(hexbuf);
		cfg->fuel.fuel_pos = parse_int(tfuel, "fuel_byte", 5);
		cfg->fuel.length    = parse_int(tfuel, "length", 1);
		cfg->fuel.scaling   = parse_double(tfuel, "scaling", 1.0);
		cfg->fuel.divisor   = parse_int(tfuel, "divisor", 1);
	}

	/* --- [dashboard] --- */
	tdash = toml_table_in(root, "dashboard");
	if (tdash) {
		cfg->dashboard.width  = parse_int(tdash, "width", 692);
		cfg->dashboard.height = parse_int(tdash, "height", 329);
	}

	/* --- [dashboard.speedometer] --- */
	tspdo = toml_table_in(root, "dashboard.speedometer");
	if (tspdo) {
		cfg->speedometer.x = parse_int(tspdo, "x", 200);
		cfg->speedometer.y = parse_int(tspdo, "y", 80);
		cfg->speedometer.w = parse_int(tspdo, "width", 300);
		cfg->speedometer.h = parse_int(tspdo, "height", 130);
		cfg->speedometer.needle_center_x = parse_int(tspdo, "needle_center_x", 135);
		cfg->speedometer.needle_center_y = parse_int(tspdo, "needle_center_y", 20);
		cfg->speedometer.angle_min = parse_double(tspdo, "angle_min", 0.0);
		cfg->speedometer.angle_max = parse_double(tspdo, "angle_max", 180.0);
		cfg->speedometer.value_min = parse_double(tspdo, "value_min", 0.0);
		cfg->speedometer.value_max = parse_double(tspdo, "value_max", 280.0);
	}

	toml_free(root);
	cfg->loaded = 1;
	return 0;
}
