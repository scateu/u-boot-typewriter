#ifndef _STUB_CROS_EC_H
#define _STUB_CROS_EC_H
enum ec_reboot_cmd { EC_REBOOT_CANCEL=0, EC_REBOOT_COLD=4, EC_REBOOT_HIBERNATE=6, EC_REBOOT_HIBERNATE_CLEAR_AP_OFF=7 };
#define EC_REBOOT_FLAG_ON_AP_SHUTDOWN (1 << 1)
struct udevice;
int cros_ec_reboot(struct udevice *dev, enum ec_reboot_cmd cmd, unsigned char flags);
#endif
int cros_ec_battery_cutoff(struct udevice *dev, unsigned char flags);
