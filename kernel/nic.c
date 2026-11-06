#include "e1000.h"
#include "nic.h"
#include "vmxnet3.h"

enum nic_driver {
    NIC_NONE,
    NIC_E1000,
    NIC_VMXNET3,
};

static enum nic_driver driver;

bool nic_initialize(void) {
    if (driver != NIC_NONE) {
        return true;
    }
    if (e1000_initialize()) {
        driver = NIC_E1000;
    } else if (vmxnet3_initialize()) {
        driver = NIC_VMXNET3;
    }
    return driver != NIC_NONE;
}

bool nic_ready(void) {
    return driver != NIC_NONE;
}

const char *nic_name(void) {
    switch (driver) {
    case NIC_E1000:
        return "Intel e1000";
    case NIC_VMXNET3:
        return "VMware vmxnet3";
    default:
        return "";
    }
}

uint32_t nic_pci_id(void) {
    switch (driver) {
    case NIC_E1000:
        return e1000_pci_id();
    case NIC_VMXNET3:
        return vmxnet3_pci_id();
    default:
        return 0;
    }
}

void nic_mac_address(uint8_t mac[6]) {
    if (driver == NIC_VMXNET3) {
        vmxnet3_mac_address(mac);
    } else {
        e1000_mac_address(mac);
    }
}

bool nic_link_up(void) {
    switch (driver) {
    case NIC_E1000:
        return e1000_link_up();
    case NIC_VMXNET3:
        return vmxnet3_link_up();
    default:
        return false;
    }
}

bool nic_send(const void *frame, size_t length) {
    switch (driver) {
    case NIC_E1000:
        return e1000_send(frame, length);
    case NIC_VMXNET3:
        return vmxnet3_send(frame, length);
    default:
        return false;
    }
}

size_t nic_receive(void *buffer, size_t capacity) {
    switch (driver) {
    case NIC_E1000:
        return e1000_receive(buffer, capacity);
    case NIC_VMXNET3:
        return vmxnet3_receive(buffer, capacity);
    default:
        return 0;
    }
}

uint64_t nic_sent_count(void) {
    switch (driver) {
    case NIC_E1000:
        return e1000_sent_count();
    case NIC_VMXNET3:
        return vmxnet3_sent_count();
    default:
        return 0;
    }
}

uint64_t nic_received_count(void) {
    switch (driver) {
    case NIC_E1000:
        return e1000_received_count();
    case NIC_VMXNET3:
        return vmxnet3_received_count();
    default:
        return 0;
    }
}
