/* ===========================================================================
 * PumpkinOS - PCI configuration space access (mechanism #1)
 * ========================================================================= */
#ifndef PUMPKIN_PCI_H
#define PUMPKIN_PCI_H

#include <stdint.h>
#include "io.h"

#define PCI_CONFIG_ADDR 0xCF8
#define PCI_CONFIG_DATA 0xCFC

static inline uint32_t pci_cfg_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off) {
    uint32_t addr = 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)dev << 11) |
                    ((uint32_t)fn << 8) | (off & 0xFC);
    outl(PCI_CONFIG_ADDR, addr);
    return inl(PCI_CONFIG_DATA);
}

static inline void pci_cfg_write32(uint8_t bus, uint8_t dev, uint8_t fn,
                                   uint8_t off, uint32_t val) {
    uint32_t addr = 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)dev << 11) |
                    ((uint32_t)fn << 8) | (off & 0xFC);
    outl(PCI_CONFIG_ADDR, addr);
    outl(PCI_CONFIG_DATA, val);
}

static inline uint16_t pci_cfg_read16(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off) {
    return (uint16_t)(pci_cfg_read32(bus, dev, fn, off) >> ((off & 2) * 8));
}

#endif /* PUMPKIN_PCI_H */
