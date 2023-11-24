#ifndef __BOOT_H__
#define __BOOT_H__

struct boot_ops {
	void (*opened)(void);
	void (*disconnect)(void);
	void (*info)(const void *, size_t);
};

#endif
