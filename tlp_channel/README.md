# TLP Channel Test

**Forbidden to opensource this project**  
This project is under LicenseRef-NvidiaProprietary control but not only limited to it.

## Overview

This test validates the newly implemented `TLP_EMU_CHANNEL` object functionality in the NVIDIA firmware. The test covers CREATE, QUERY, and DESTROY operations for TLP_EMU_CHANNEL objects based on the firmware modifications in golan_fw.

## Firmware Modifications Tested

The test validates the following firmware changes:

1. **TLP_EMU_CHANNEL Object Support** (Object Type: 0x59)
   - Create operation with validation
   - Query operation to retrieve channel parameters  
   - Destroy operation with resource cleanup

2. **Syndrome Error Codes Validation**
   - `0xE1E101`: Invalid protocol mode (only mode 0 supported)
   - `0xE1E102`: Invalid queue size (must be 1-64KB)
   - `0xE1E103`: Invalid queue address (cannot be zero)
   - `0xE1E104`: Failed to allocate object resource
   - `0xE1E105`: Invalid object ID for query operation
   - `0xE1E106`: Invalid object ID for destroy operation
   - `0xE1E108`: VA to PA translation failed
   - `0xE1E109`: Invalid mkey (cannot be zero)

3. **Scratchpad Integration**
   - TLP Channel Meta written to scratchpad for PCI FW communication
   - Physical address translation using mkey

## Prerequisites

1. **Flash Firmware with TLP_EMU_CHANNEL Support**
   ```bash
   cd /root/code/golan_fw
   sudo mst start
   sudo mlxburn -d /dev/mst/mt41692_pciconf0 -fw fw-BlueField-3.mlx -conf_dir prs/mustang/beta_ini
   ```

2. **Enable FW Trace (Optional for Debugging)**
   ```bash
   cd /root/code/golan_fw
   sudo su -c 'echo 1 > /sys/kernel/debug/tracing/events/mlx5/mlx5_fw/enable'
   sudo mcra /dev/mst/mt41692_pciconf0 0x3ffffffc 0x80000000
   sudo fwtrace -d /dev/mst/mt41692_pciconf0 -s -i all -m CMD_IF -l 1 --tracer_mode MEM -f ./mustang_fw_strings.db --real_ts | tee /tmp/fwtrace_tlp_channel.log
   ```

## Build and Run

1. **Build the Test**
   ```bash
   cd /root/code/doca/samples/doca_devemu/tlp_channel_test
   meson setup build
   meson compile -C build
   ```

2. **Run the Test**
   ```bash
   ./build/tlp_channel_test mlx5_0
   ```

## Test Cases

The test includes the following test cases:

### Test 1: Valid Parameters
- Creates TLP_EMU_CHANNEL with valid parameters (protocol_mode=0, queue_size=4KB)
- Performs QUERY operation to validate stored parameters
- Performs DESTROY operation to clean up resources

### Test 2: Invalid Protocol Mode
- Attempts to create channel with invalid protocol mode (mode=1)
- Should fail with syndrome `0xE1E101`

### Test 3: Maximum Queue Size
- Tests creation with maximum allowed queue size (64KB)
- Validates firmware accepts the maximum limit

### Test 4: Oversized Queue
- Attempts to create channel with oversized queue (>64KB)
- Should fail with syndrome `0xE1E102`

## Expected Output

### Successful Test Run
```
TLP Channel Test for NVIDIA Firmware
Testing device: mlx5_0
Testing TLP_EMU_CHANNEL object (type 0x59)
=====================================

=== Testing TLP_EMU_CHANNEL Operations ===

Test 1: Creating channel with valid parameters
Creating TLP_EMU_CHANNEL with:
  - Protocol Mode: 0
  - Queue Size: 4096 bytes
  - Stride Index: 1
  - Queue Buffer VA: 0x7f8b2c000000
  - Memory Key (mkey): 0x12345678
✓ TLP_EMU_CHANNEL created successfully with object ID: 0x1234

Querying TLP_EMU_CHANNEL object ID: 0x1234
Query Results:
  - Protocol Mode: 0
  - Queue MKey: 0x12345678
  - Queue Size: 4096 bytes
  - Queue Address: 0x7f8b2c000000
  - Stride Index: 1
✓ TLP_EMU_CHANNEL query completed successfully

Destroying TLP_EMU_CHANNEL object ID: 0x1234
✓ TLP_EMU_CHANNEL destroyed successfully

Test 2: Testing invalid protocol mode (should fail)
TLP_EMU_CHANNEL create failed, syndrome 0xe1e101: Remote I/O error
  Error: Invalid protocol mode (only mode 0 is supported)
✓ Test 2 passed (correctly rejected invalid protocol mode)

=== Test Summary ===
✓ All TLP_EMU_CHANNEL tests completed successfully!
  The firmware modifications are working correctly.
```

### Firmware Trace Output
When running with firmware tracing enabled, you should see log entries similar to:
```
[timestamp] I3  create_tlp_emu_channel: gvmi:4, obj_id:0x1234
[timestamp] I3  TLP Channel Meta written to scratchpad: PA=0x123456789abc
```

## Troubleshooting

1. **Syndrome 0x5a82ce**: Firmware feature not enabled or object type not supported
2. **Syndrome 0xe1e101-0xe1e109**: Parameter validation failures (see error codes above)  
3. **Memory allocation failures**: Increase available memory or reduce queue size
4. **Device not found**: Ensure MLX5 device is available and accessible

## Related Files

- `/root/code/golan_fw/src/main/cmdif_tlp_emu.c`: Main firmware implementation
- `/root/code/golan_fw/src/main/reformat_tlp_emu.c`: Format conversion functions
- `/root/code/golan_fw/include/cmdif_tlp_emu.h`: Header definitions
- `/root/code/golan_fw/adabe/*.adb`: ADB structure definitions 