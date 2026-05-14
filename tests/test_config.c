/* Tests for TOML config loader (config.c) */
#include "runner.h"
#include "../can_platform.h"
#include "../config.h"

int main(void)
{
	icsim_config_t cfg;

	printf("=== TOML config loader tests ===\n\n");

	TEST("default config matches hardcoded values")
	{
		icsim_config_defaults(&cfg);
		ASSERT_INT_EQ(cfg.can.door_id, 0x19B);
		ASSERT_INT_EQ(cfg.can.signal_id, 0x188);
		ASSERT_INT_EQ(cfg.can.speed_id, 0x244);
		ASSERT_INT_EQ(cfg.can.door_pos, 2);
		ASSERT_INT_EQ(cfg.can.signal_pos, 0);
		ASSERT_INT_EQ(cfg.can.speed_pos, 3);
		ASSERT_INT_EQ(cfg.speed.length, 2);
		ASSERT(cfg.speed.divisor == 100);
		ASSERT(cfg.speed.scaling > 0.6);
		ASSERT_INT_EQ(cfg.doors.door1_mask, ICSIM_DOOR1);
		ASSERT_INT_EQ(cfg.turn.left_mask, ICSIM_TURN_LEFT);
		ASSERT_INT_EQ(cfg.turn.right_mask, ICSIM_TURN_RIGHT);
		ASSERT(cfg.loaded == 0);
	}
	END_TEST;

	TEST("default RPM config")
	{
		icsim_config_defaults(&cfg);
		ASSERT_INT_EQ(cfg.rpm.rpm_id, 0x0AA);
		ASSERT_INT_EQ(cfg.rpm.rpm_pos, 4);
		ASSERT_INT_EQ(cfg.rpm.length, 2);
	}
	END_TEST;

	TEST("default temp config")
	{
		icsim_config_defaults(&cfg);
		ASSERT_INT_EQ(cfg.temp.temp_id, 0x1B8);
		ASSERT_INT_EQ(cfg.temp.temp_pos, 2);
	}
	END_TEST;

	TEST("default fuel config")
	{
		icsim_config_defaults(&cfg);
		ASSERT_INT_EQ(cfg.fuel.fuel_id, 0x2C8);
		ASSERT_INT_EQ(cfg.fuel.fuel_pos, 5);
	}
	END_TEST;

	TEST("load default.toml from disk")
	{
		int rc = icsim_config_load(&cfg, "../models/default.toml");
		ASSERT_INT_EQ(rc, 0);
		ASSERT(cfg.loaded == 1);
		ASSERT_INT_EQ(cfg.can.speed_id, 0x244);
	}
	END_TEST;

	TEST("load bmw_x1.toml from disk")
	{
		int rc = icsim_config_load(&cfg, "../models/bmw_x1.toml");
		ASSERT_INT_EQ(rc, 0);
		ASSERT(cfg.loaded == 1);
		ASSERT_INT_EQ(cfg.can.speed_id, 0x1B4);
		ASSERT_INT_EQ(cfg.can.speed_pos, 0);
	}
	END_TEST;

	TEST("NULL path gives defaults (no crash)")
	{
		int rc = icsim_config_load(&cfg, NULL);
		ASSERT_INT_EQ(rc, 0);
		ASSERT(cfg.loaded == 0);
		ASSERT_INT_EQ(cfg.can.speed_id, 0x244);
	}
	END_TEST;

	TEST("missing file gives defaults (graceful)")
	{
		int rc = icsim_config_load(&cfg, "../models/nonexistent.toml");
		ASSERT_INT_EQ(rc, 0);
		ASSERT(cfg.loaded == 0);
	}
	END_TEST;

	return test_summary();
}
