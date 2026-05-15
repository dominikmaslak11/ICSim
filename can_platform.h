#ifndef CAN_PLATFORM_H
#define CAN_PLATFORM_H

#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
typedef uint32_t canid_t;

#define CAN_EFF_FLAG 0x80000000U
#define CAN_RTR_FLAG 0x40000000U
#define CAN_ERR_FLAG 0x20000000U
#define CAN_SFF_MASK 0x000007FFU
#define CAN_EFF_MASK 0x1FFFFFFFU
#define CAN_ERR_MASK 0x1FFFFFFFU
#define CAN_ERR_TX_TIMEOUT 0x00000001U
#define CAN_ERR_LOSTARB 0x00000002U
#define CAN_ERR_CRTL 0x00000004U
#define CAN_ERR_PROT 0x00000008U
#define CAN_ERR_TRX 0x00000010U
#define CAN_ERR_ACK 0x00000020U
#define CAN_ERR_BUSOFF 0x00000040U
#define CAN_ERR_BUSERROR 0x00000080U
#define CAN_ERR_RESTARTED 0x00000100U
#define CAN_MAX_DLC 8
#define CAN_MAX_DLEN 8
#define CANFD_MAX_DLEN 64

struct canfd_frame {
	canid_t can_id;
	uint8_t len;
	uint8_t flags;
	uint8_t __res0;
	uint8_t __res1;
	uint8_t data[CANFD_MAX_DLEN];
};

#define CAN_MTU ((int)sizeof(struct canfd_frame))
#define CANFD_MTU ((int)sizeof(struct canfd_frame))
#else
#include <linux/can.h>
#include <linux/can/raw.h>
#endif

typedef struct can_bus can_bus_t;

#ifdef __cplusplus
extern "C" {
#endif

int can_bus_open(can_bus_t **bus, const char *name);
int can_bus_send(can_bus_t *bus, const struct canfd_frame *frame, size_t mtu);
int can_bus_recv(can_bus_t *bus, struct canfd_frame *frame, size_t *mtu);
int can_bus_set_nonblocking(can_bus_t *bus, int nonblocking);
int can_bus_error_is_would_block(void);
void can_bus_close(can_bus_t *bus);
const char *can_bus_error(void);
int can_bus_is_virtual(const can_bus_t *bus);

#ifdef __cplusplus
}
#endif

#endif
