/* Tests for parse_canframe() in lib.c */
#include "runner.h"
#include "../can_platform.h"
#include "../lib.h"

#include <string.h>

int main(void)
{
	struct canfd_frame frame;
	int mtu;

	printf("=== CAN frame parser tests ===\n\n");

	TEST("standard frame, 8 bytes data")
	{
		mtu = parse_canframe("244#1122334455667788", &frame);
		ASSERT(mtu == CAN_MTU);
		ASSERT_INT_EQ(frame.can_id, 0x244);
		ASSERT_INT_EQ(frame.len, 8);
		ASSERT_INT_EQ(frame.data[0], 0x11);
		ASSERT_INT_EQ(frame.data[1], 0x22);
		ASSERT_INT_EQ(frame.data[7], 0x88);
	}
	END_TEST;

	TEST("standard frame, no data (empty)")
	{
		mtu = parse_canframe("7A1#", &frame);
		ASSERT(mtu == CAN_MTU);
		ASSERT_INT_EQ(frame.can_id, 0x7A1);
		ASSERT_INT_EQ(frame.len, 0);
	}
	END_TEST;

	TEST("standard frame, 3-byte data")
	{
		mtu = parse_canframe("19B#000001", &frame);
		ASSERT(mtu == CAN_MTU);
		ASSERT_INT_EQ(frame.can_id, 0x19B);
		ASSERT_INT_EQ(frame.len, 3);
		ASSERT_INT_EQ(frame.data[0], 0x00);
		ASSERT_INT_EQ(frame.data[1], 0x00);
		ASSERT_INT_EQ(frame.data[2], 0x01);
	}
	END_TEST;

	TEST("extended frame")
	{
		mtu = parse_canframe("18DAF100#AABBCCDD", &frame);
		ASSERT(mtu == CAN_MTU);
		ASSERT(frame.can_id & CAN_EFF_FLAG);
		ASSERT_INT_EQ(frame.can_id & CAN_EFF_MASK, 0x18DAF100);
		ASSERT_INT_EQ(frame.len, 4);
	}
	END_TEST;

	TEST("RTR frame")
	{
		mtu = parse_canframe("123#R", &frame);
		ASSERT(mtu == CAN_MTU);
		ASSERT(frame.can_id & CAN_RTR_FLAG);
		ASSERT_INT_EQ(frame.can_id & CAN_SFF_MASK, 0x123);
	}
	END_TEST;

	TEST("RTR frame with DLC")
	{
		mtu = parse_canframe("456#R4", &frame);
		ASSERT(mtu == CAN_MTU);
		ASSERT(frame.can_id & CAN_RTR_FLAG);
		ASSERT_INT_EQ(frame.can_id & CAN_SFF_MASK, 0x456);
		ASSERT_INT_EQ(frame.len, 4);
	}
	END_TEST;

	TEST("data with dot separators")
	{
		mtu = parse_canframe("123#11.22.33.44", &frame);
		ASSERT(mtu == CAN_MTU);
		ASSERT_INT_EQ(frame.len, 4);
		ASSERT_INT_EQ(frame.data[0], 0x11);
		ASSERT_INT_EQ(frame.data[3], 0x44);
	}
	END_TEST;

	TEST("error frame flag")
	{
		mtu = parse_canframe("20000004#01", &frame);
		ASSERT(mtu == CAN_MTU);
		ASSERT(frame.can_id & CAN_ERR_FLAG);
	}
	END_TEST;

	TEST("invalid: too short")
	{
		mtu = parse_canframe("12", &frame);
		ASSERT(mtu == 0);
	}
	END_TEST;

	TEST("invalid: missing hash")
	{
		mtu = parse_canframe("1234567", &frame);
		ASSERT(mtu == 0);
	}
	END_TEST;

	TEST("reject extended frame with error flag already set (8-hex starts with error bit)")
	{
		/* 20000000 is the error flag, this is valid as error frame */
		mtu = parse_canframe("20000000#", &frame);
		ASSERT(mtu == CAN_MTU);
		ASSERT(frame.can_id & CAN_ERR_FLAG);
	}
	END_TEST;

	return test_summary();
}
