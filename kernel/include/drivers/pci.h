#ifndef AURORA_PCI_H
#define AURORA_PCI_H
#include "types.h"

/* PCI configuration access through ports 0xCF8/0xCFC (configuration
 * mechanism #1), which the QEMU `pc` machine provides. */
struct pci_dev {
    uint8_t bus, dev, fn;
    uint16_t vendor, device;
    uint8_t class_code, subclass, irq_line;
    uint32_t bar[6];
};

#define PCI_MAX_DEVICES 32

void pci_init(void);
int pci_count(void);
const struct pci_dev *pci_at(int i);
const struct pci_dev *pci_find(uint16_t vendor, uint16_t device);
uint32_t pci_read32(const struct pci_dev *d, uint8_t off);
void pci_write16(const struct pci_dev *d, uint8_t off, uint16_t v);
/* Enables I/O decoding and bus mastering (DMA). */
void pci_enable(const struct pci_dev *d);
/* Port base of an I/O BAR, or 0 if the BAR is not an I/O BAR. */
uint16_t pci_io_bar(const struct pci_dev *d, int index);

#endif
