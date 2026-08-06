#ifndef _STUB_DM_H
#define _STUB_DM_H
struct udevice;
enum uclass_id { UCLASS_VIDEO = 42, UCLASS_CROS_EC = 43, UCLASS_FIRMWARE = 44 };
int uclass_first_device_err(enum uclass_id id, struct udevice **devp);
void *dev_get_uclass_priv(const struct udevice *dev);
int uclass_get_device_by_name(enum uclass_id id, const char *name, struct udevice **devp);
#endif
