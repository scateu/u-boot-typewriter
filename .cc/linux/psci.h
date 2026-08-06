#ifndef _STUB_PSCI_H
#define _STUB_PSCI_H
#define PSCI_0_2_FN_SYSTEM_OFF     0x84000008UL
#define PSCI_0_2_FN_PSCI_VERSION   0x84000000UL
#define PSCI_1_0_FN_PSCI_FEATURES  0x8400000AUL
unsigned long invoke_psci_fn(unsigned long a0, unsigned long a1,
                             unsigned long a2, unsigned long a3);
#endif
