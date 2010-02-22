#if LAB >= 4
// IDE disk device driver definitions.
// See COPYRIGHT for copyright information.
#ifndef PIOS_DEV_IDE_H
#define PIOS_DEV_IDE_H
#ifndef PIOS_KERNEL
# error "This is a kernel header; user programs should not #include it"
#endif

#include <inc/types.h>

struct iodisk;

void ide_init(void);
void ide_startio(const struct iodisk *io);
bool ide_checkio(struct iodisk *io);

#endif // !PIOS_DEV_IDE_H
#endif // LAB >= 4
