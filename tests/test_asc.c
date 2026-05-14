/* Tests for CAN log ASC format (can_log.c) */
#include "runner.h"
#include "../can_platform.h"
#include "../can_log.h"

#include <stdio.h>

int main(void)
{
	struct canfd_frame frame, out;
	double toff;
	size_t mtu;

	printf("=== ASC format tests ===\n\n");

	TEST("record and replay round-trip")
	{
		const char *tmpfile = "test_roundtrip.asc";

		/* Record */
		can_log_t *rec = can_log_open_record(tmpfile);
		ASSERT(rec != NULL);

		memset(&frame, 0, sizeof(frame));
		frame.can_id = 0x244;
		frame.len = 8;
		frame.data[0] = 0x11;
		frame.data[1] = 0x22;
		frame.data[2] = 0x33;
		frame.data[7] = 0x88;
		can_log_record(rec, &frame);

		memset(&frame, 0, sizeof(frame));
		frame.can_id = 0x19B;
		frame.len = 3;
		frame.data[0] = 0xAA;
		frame.data[1] = 0xBB;
		frame.data[2] = 0xCC;
		can_log_record(rec, &frame);

		can_log_close(rec);

		/* Replay */
		can_log_t *rep = can_log_open_replay(tmpfile);
		ASSERT(rep != NULL);

		/* Frame 1 */
		ASSERT(can_log_replay_next(rep, &out, &toff, &mtu) == 1);
		ASSERT_INT_EQ(out.can_id, 0x244);
		ASSERT_INT_EQ(out.len, 8);
		ASSERT_INT_EQ(out.data[0], 0x11);
		ASSERT_INT_EQ(out.data[7], 0x88);
		ASSERT(mtu == CAN_MTU);

		/* Frame 2 */
		ASSERT(can_log_replay_next(rep, &out, &toff, &mtu) == 1);
		ASSERT_INT_EQ(out.can_id, 0x19B);
		ASSERT_INT_EQ(out.len, 3);
		ASSERT_INT_EQ(out.data[0], 0xAA);

		/* EOF */
		ASSERT(can_log_replay_next(rep, &out, &toff, &mtu) == 0);

		can_log_close(rep);
		remove(tmpfile);
	}
	END_TEST;

	TEST("NULL path returns NULL gracefully")
	{
		ASSERT(can_log_open_record(NULL) == NULL);
	}
	END_TEST;

	TEST("record with NULL log is safe")
	{
		can_log_record(NULL, &frame);  /* should not crash */
	}
	END_TEST;

	TEST("replay with NULL log returns 0")
	{
		ASSERT(can_log_replay_next(NULL, &out, &toff, &mtu) == 0);
	}
	END_TEST;

	TEST("close NULL is safe")
	{
		can_log_close(NULL);  /* should not crash */
	}
	END_TEST;

	return test_summary();
}
