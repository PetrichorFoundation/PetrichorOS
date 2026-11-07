/* Network card dispatcher: tries each driver in turn and uses the first card that answers. */
#include "kernel.h"

u8 nic_mac[6];
int nic_irq_line = -1;
const char *nic_name = "none";

static void no_poll(void) {}
static void no_irq_poll(void) {}

static const struct nic_driver drivers[] = {
    {"e1000", e1000_init, e1000_send, e1000_irq, e1000_poll},
    {"virtio-net", virtio_net_init, virtio_net_send, virtio_net_irq, virtio_net_poll},
    {"rtl8168", rtl8168_init, rtl8168_send, rtl8168_irq, rtl8168_poll},
    {"rtl8139", rtl8139_init, rtl8139_send, rtl8139_irq, no_poll},
};
static const struct nic_driver *active;

int nic_init(void)
{
    (void)no_irq_poll;
    for (u32 i = 0; i < sizeof drivers / sizeof drivers[0]; i++) {
        if (drivers[i].init() == 0) {
            active = &drivers[i];
            nic_name = drivers[i].name;
            return 0;
        }
    }
    return -ENODEV;
}

int nic_send(const u8 *frame, u32 len) { return active ? active->send(frame, len) : -ENODEV; }
void nic_irq(void) { if (active) active->irq(); }
void nic_poll(void) { if (active) active->poll(); }
