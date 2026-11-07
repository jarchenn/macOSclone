#include "drivers/pci.h"
#include "arch/x86_64/io.h"
#include "core/log.h"

static struct pci_dev devs[PCI_MAX_DEVICES];
static int ndevs;

static uint32_t cfg_read(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off)
{
    outl_cf8(0x80000000u | (uint32_t)bus << 16 | (uint32_t)dev << 11 | (uint32_t)fn << 8 | (off & 0xFC));
    return inl(0xCFC);
}

uint32_t pci_read32(const struct pci_dev *d, uint8_t off) { return cfg_read(d->bus, d->dev, d->fn, off); }

void pci_write16(const struct pci_dev *d, uint8_t off, uint16_t v)
{
    outl_cf8(0x80000000u | (uint32_t)d->bus << 16 | (uint32_t)d->dev << 11 | (uint32_t)d->fn << 8 | (off & 0xFC));
    outw((uint16_t)(0xCFC + (off & 2)), v);
}

void pci_init(void)
{
    ndevs = 0;
    for (int bus = 0; bus < 4; bus++) {
        for (int dev = 0; dev < 32; dev++) {
            for (int fn = 0; fn < 8; fn++) {
                uint32_t id = cfg_read((uint8_t)bus, (uint8_t)dev, (uint8_t)fn, 0);
                if ((id & 0xFFFF) == 0xFFFF) {
                    if (fn == 0)
                        break; /* no device */
                    continue;
                }
                if (ndevs < PCI_MAX_DEVICES) {
                    struct pci_dev *d = &devs[ndevs++];
                    d->bus = (uint8_t)bus;
                    d->dev = (uint8_t)dev;
                    d->fn = (uint8_t)fn;
                    d->vendor = (uint16_t)id;
                    d->device = (uint16_t)(id >> 16);
                    uint32_t cls = cfg_read(d->bus, d->dev, d->fn, 8);
                    d->class_code = (uint8_t)(cls >> 24);
                    d->subclass = (uint8_t)(cls >> 16);
                    for (int b = 0; b < 6; b++)
                        d->bar[b] = cfg_read(d->bus, d->dev, d->fn, (uint8_t)(0x10 + 4 * b));
                    d->irq_line = (uint8_t)cfg_read(d->bus, d->dev, d->fn, 0x3C);
                }
                if (fn == 0 && !((cfg_read((uint8_t)bus, (uint8_t)dev, 0, 0x0C) >> 16) & 0x80))
                    break; /* single-function device */
            }
        }
    }
    klog("pci: %d device(s)", ndevs);
    for (int i = 0; i < ndevs; i++)
        klog("  pci %02x:%02x.%x %04x:%04x class %02x.%02x", devs[i].bus, devs[i].dev, devs[i].fn,
             devs[i].vendor, devs[i].device, devs[i].class_code, devs[i].subclass);
}

int pci_count(void) { return ndevs; }
const struct pci_dev *pci_at(int i) { return (i >= 0 && i < ndevs) ? &devs[i] : NULL; }

const struct pci_dev *pci_find(uint16_t vendor, uint16_t device)
{
    for (int i = 0; i < ndevs; i++)
        if (devs[i].vendor == vendor && devs[i].device == device)
            return &devs[i];
    return NULL;
}

void pci_enable(const struct pci_dev *d)
{
    uint16_t cmd = (uint16_t)pci_read32(d, 4);
    cmd |= 0x1 | 0x4; /* I/O space, bus master */
    pci_write16(d, 4, cmd);
}

uint16_t pci_io_bar(const struct pci_dev *d, int index)
{
    if (index < 0 || index > 5 || !(d->bar[index] & 1))
        return 0;
    return (uint16_t)(d->bar[index] & 0xFFFC);
}
