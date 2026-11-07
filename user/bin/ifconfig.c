#include <ulib.h>

int main(void)
{
    struct netinfo ni;
    netinfo(&ni);
    if (!ni.up) {
        puts("no network interface");
        return 1;
    }
    printf("eth0: %s%s\n", ni.nic, ni.up ? "" : " (no address yet)");
    printf("      ether %02x:%02x:%02x:%02x:%02x:%02x\n", ni.mac[0], ni.mac[1], ni.mac[2],
           ni.mac[3], ni.mac[4], ni.mac[5]);
    printf("      inet %s", inet_ntoa(ni.ip));
    printf("  netmask %s", inet_ntoa(ni.mask));
    printf("  gateway %s\n", inet_ntoa(ni.gw));
    printf("      dns %s\n", inet_ntoa(ni.dns));
    printf("      RX packets %u  TX packets %u\n", ni.rx_packets, ni.tx_packets);
    return 0;
}
