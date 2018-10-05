/*  test-device-info.c tests to see whether the functionality for converting a
 *  device address to a xrandr output are working properly
 *
 * Copyright 2018 Red Hat, Inc.
 *
 * Red Hat Authors:
 * Jonathon Jongsma <jjongsma@redhat.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

// just include the source file for testing
#include "vdagent/device-info.c"

#define assert_device(dev, domain_, bus_, slot_, function_) \
{ \
    PciDevice* dev_ = (dev); \
    assert(dev_ != NULL); \
    assert(dev_->domain == domain_); \
    assert(dev_->bus == bus_); \
    assert(dev_->slot == slot_); \
    assert(dev_->function == function_); \
}

static void test_compare_addresses()
{
    {
        PciDevice da1 = {1, 0, 3, 0};
        PciDevice da2 = {1, 1, 1, 0};
        PciDevice da3 = {1, 2, 3, 0};
        PciAddress a1 = {1, NULL};
        a1.domain = 0;
        a1.devices = g_list_append(a1.devices, &da1);
        a1.devices = g_list_append(a1.devices, &da2);
        a1.devices = g_list_append(a1.devices, &da3);

        PciDevice db1 = {1, 0, 3, 0};
        PciDevice db2 = {1, 1, 1, 0};
        PciDevice db3 = {1, 2, 3, 0};
        PciAddress a2 = {1, NULL};
        a2.domain = 0;
        a2.devices = g_list_append(a2.devices, &db1);
        a2.devices = g_list_append(a2.devices, &db2);
        a2.devices = g_list_append(a2.devices, &db3);

        assert(compare_addresses(&a1, &a2));
    }
    {
        PciDevice da1 = {1, 0, 3, 0};
        PciDevice da2 = {1, 1, 1, 0};
        PciDevice da3 = {1, 2, 3, 0};
        PciAddress a1 = {1, NULL};
        a1.domain = 0;
        a1.devices = g_list_append(a1.devices, &da1);
        a1.devices = g_list_append(a1.devices, &da2);
        a1.devices = g_list_append(a1.devices, &da3);

        // a 'spice' format PCI address will not provide domain or bus for each
        // device, only slot and function. So first two numbers for each device
        // will always be set to 0
        PciDevice db1 = {0, 0, 3, 0};
        PciDevice db2 = {0, 0, 1, 0};
        PciDevice db3 = {0, 0, 3, 0};
        PciAddress a2 = {1, NULL};
        a2.domain = 0;
        a2.devices = g_list_append(a2.devices, &db1);
        a2.devices = g_list_append(a2.devices, &db2);
        a2.devices = g_list_append(a2.devices, &db3);

        assert(compare_addresses(&a1, &a2));
    }
    // different number of devices
    {
        PciDevice da1 = {0, 0, 3, 0};
        PciDevice da2 = {0, 1, 1, 0};
        PciDevice da3 = {0, 2, 3, 0};
        PciAddress a1 = {0, NULL};
        a1.domain = 0;
        a1.devices = g_list_append(a1.devices, &da1);
        a1.devices = g_list_append(a1.devices, &da2);
        a1.devices = g_list_append(a1.devices, &da3);

        PciDevice db1 = {0, 0, 3, 0};
        PciDevice db2 = {0, 1, 1, 0};
        PciAddress a2 = {0, NULL};
        a2.domain = 0;
        a2.devices = g_list_append(a2.devices, &db1);
        a2.devices = g_list_append(a2.devices, &db2);

        assert(!compare_addresses(&a1, &a2));
    }
    // mismatched function
    {
        PciDevice da1 = {0, 0, 2, 0};
        PciAddress a1 = {0, NULL};
        a1.domain = 0;
        a1.devices = g_list_append(a1.devices, &da1);

        PciDevice db1 = {0, 0, 2, 1};
        PciAddress a2 = {0, NULL};
        a2.domain = 0;
        a2.devices = g_list_append(a2.devices, &db1);

        assert(!compare_addresses(&a1, &a2));
    }
    // mismatched slot
    {
        PciDevice da1 = {0, 0, 2, 0};
        PciAddress a1 = {0, NULL};
        a1.domain = 0;
        a1.devices = g_list_append(a1.devices, &da1);

        PciDevice db1 = {0, 0, 1, 0};
        PciAddress a2 = {0, NULL};
        a2.domain = 0;
        a2.devices = g_list_append(a2.devices, &db1);

        assert(!compare_addresses(&a1, &a2));
    }
    // mismatched domain
    {
        PciDevice da1 = {0, 0, 2, 0};
        PciAddress a1 = {0, NULL};
        a1.domain = 1;
        a1.devices = g_list_append(a1.devices, &da1);

        PciDevice db1 = {0, 0, 2, 0};
        PciAddress a2 = {0, NULL};
        a2.domain = 0;
        a2.devices = g_list_append(a2.devices, &db1);

        assert(!compare_addresses(&a1, &a2));
    }
}

static void test_spice_parsing()
{
    PciAddress *addr = parse_pci_address_from_spice("pci/0000/02.0");
    assert(addr != NULL);
    assert(addr->domain == 0);
    assert(g_list_length(addr->devices) == 1);
    assert_device(addr->devices->data, 0, 0, 2, 0);
    pci_address_free(addr);

    addr = parse_pci_address_from_spice("pci/ffff/ff.f");
    assert(addr != NULL);
    assert(addr->domain == 65535);
    assert(g_list_length(addr->devices) == 1);
    assert_device(addr->devices->data, 0, 0, 255, 15);
    pci_address_free(addr);

    addr = parse_pci_address_from_spice("pci/0000/02.1/03.0");
    assert(addr != NULL);
    assert(addr->domain == 0);
    assert(g_list_length(addr->devices) == 2);
    assert_device(addr->devices->data, 0, 0, 2, 1);
    assert_device(addr->devices->next->data, 0, 0, 3, 0);
    pci_address_free(addr);

    addr = parse_pci_address_from_spice("pci/000a/01.0/02.1/03.0");
    assert(addr != NULL);
    assert(addr->domain == 10);
    assert(g_list_length(addr->devices) == 3);
    assert_device(addr->devices->data, 0, 0, 1, 0);
    assert_device(addr->devices->next->data, 0, 0, 2, 1);
    assert_device(addr->devices->next->next->data, 0, 0, 3, 0);
    pci_address_free(addr);

    addr = parse_pci_address_from_spice("pcx/0000/02.1/03.0");
    assert(addr == NULL);

    addr = parse_pci_address_from_spice("0000/02.0");
    assert(addr == NULL);

    addr = parse_pci_address_from_spice("0000/02.1/03.0");
    assert(addr == NULL);
}

static void test_sysfs_parsing()
{
    PciAddress *addr = parse_pci_address_from_sysfs_path("../../devices/pci0000:00/0000:00:02.0/drm/card0");
    assert(addr != NULL);
    assert(addr->domain == 0);
    assert(g_list_length(addr->devices) == 1);
    assert_device(addr->devices->data, 0, 0, 2, 0);
    pci_address_free(addr);

    addr = parse_pci_address_from_sysfs_path("../../devices/pciffff:ff/ffff:ff:ff.f/drm/card0");
    assert(addr != NULL);
    assert(addr->domain == 65535);
    assert(g_list_length(addr->devices) == 1);
    assert_device(addr->devices->data, 65535, 255, 255, 15);
    pci_address_free(addr);

    addr = parse_pci_address_from_sysfs_path("../../devices/pci0000:00/0000:00:03.0/0000:01:01.0/0000:02:03.0/virtio2/drm/card0");
    assert(addr != NULL);
    assert(addr->domain == 0);
    assert(g_list_length(addr->devices) == 3);
    assert_device(addr->devices->data, 0, 0, 3, 0);
    assert_device(addr->devices->next->data, 0, 1, 1, 0);
    assert_device(addr->devices->next->next->data, 0, 2, 3, 0);
    pci_address_free(addr);
}

// verify that we parse the BDF notation correctly
static bool test_bdf(const char* string, int domain, uint8_t bus, uint8_t slot, uint8_t function)
{
    PciDevice pci_dev;
    return (parse_pci_device(string, NULL, &pci_dev)
            && (pci_dev.domain == domain)
            && (pci_dev.bus == bus)
            && (pci_dev.slot == slot)
            && (pci_dev.function == function));
}

static void test_bdf_parsing()
{
    // valid input
    assert(test_bdf("0000:00:02.1", 0, 0, 2, 1));
    assert(test_bdf("00:00:02.1", 0, 0, 2, 1));
    assert(test_bdf("0000:00:03.0", 0, 0, 3, 0));
    assert(test_bdf("0000:00:1d.1", 0, 0, 29, 1));
    assert(test_bdf("0000:09:02.1", 0, 9, 2, 1));
    assert(test_bdf("0000:1d:02.1", 0, 29, 2, 1));
    assert(test_bdf("0000:00:02.d", 0, 0, 2, 13));
    assert(test_bdf("000f:00:02.d", 15, 0, 2, 13));
    assert(test_bdf("000f:00:02.d", 15, 0, 2, 13));
    assert(test_bdf("000f:00:02.d", 15, 0, 2, 13));
    assert(test_bdf("000f:00:02.d", 15, 0, 2, 13));
    assert(test_bdf("ffff:ff:ff.f", 65535, 255, 255, 15));
    assert(test_bdf("0:0:2.1", 0, 0, 2, 1));

    // invalid input
    assert(!test_bdf("0000:00:02:0", 0, 0, 2, 0));
    assert(!test_bdf("-0001:00:02.1", -1, 0, 2, 1));
    assert(!test_bdf("0000.00.02.0", 0, 0, 2, 0));
    assert(!test_bdf("000f:00:02", 15, 0, 2, 0));
    assert(!test_bdf("000f:00", 15, 0, 0, 0));
    assert(!test_bdf("000f", 15, 0, 0, 0));
    assert(!test_bdf("random string", 0, 0, 0, 0));
    assert(!test_bdf("12345", 12345, 0, 0, 0));
}

int main(int argc, char **argv)
{
    test_bdf_parsing();
    test_sysfs_parsing();
    test_spice_parsing();
    test_compare_addresses();
}

