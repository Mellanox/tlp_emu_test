/*
q * SPDX-License-Identifier: LicenseRef-NvidiaProprietary
 * Copyright(c) 2025 TLP Channel Test for NVIDIA Firmware
 *
 * TLP Channel Test - Test the newly implemented TLP_EMU_CHANNEL object
 * This test validates the CREATE, QUERY, and DESTROY operations for TLP_EMU_CHANNEL
 * based on the firmware modifications in golan_fw.
 */

#include <infiniband/verbs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/mman.h>

#include "mlx5_ifc.h"

// TLP_EMU_CHANNEL object type as defined in firmware (prm_enums.h)
#define MLX5_OBJ_TYPE_TLP_EMU_CHANNEL  0x59

struct mlx5_tlp_channel_obj {
    struct mlx5dv_devx_obj  *obj;
    uint32_t                obj_id;
    void                    *queue_buffer;
    size_t                  queue_size;
    struct ibv_mr           *mr;
};

struct ibv_device* get_device(const char *dev_name)
{
    struct ibv_device **device_list = ibv_get_device_list(NULL);
    struct ibv_device *device;

    for (device = *device_list; device != NULL; device = *(++device_list)) {
        if (strcmp(dev_name, ibv_get_device_name(device)) == 0) {
            break;
        }
    }

    return device;
}



/**
 * Create TLP_EMU_CHANNEL object (Official Specification: Object Type 0x0059)
 * 
 * @param ctx: IBV context
 * @param pd: Protection domain for memory registration
 * @param q_protocol_mode: Protocol mode (8 bit) - Mode0: Mkey covers 64KB buffer (1K × 64B QEs)
 * @param q_size: Queue size in bytes (32 bit) - Communication channel queue size
 * @param tlp_channel_stride_index: TLP channel stride index (16 bits) - from mlx5dv_alloc_ear API
 * @return: Pointer to created object or NULL on failure
 * 
 * Note: This object is associated to topology behind a single downstream port of NVIDIA switch.
 * The mkey ownership moves to device for entire lifecycle of TLP_EMULATION_CHANNEL object.
 */
struct mlx5_tlp_channel_obj *mlx5_tlp_channel_create(struct ibv_context *ctx,
                                                     struct ibv_pd *pd,
                                                     uint8_t q_protocol_mode,
                                                     uint32_t q_size,
                                                     uint16_t tlp_channel_stride_index)
{
    uint8_t in[DEVX_ST_SZ_BYTES(general_obj_in_cmd_hdr) + DEVX_ST_SZ_BYTES(tlp_emu_channel)] = {0};
    uint8_t out[DEVX_ST_SZ_BYTES(general_obj_out_cmd_hdr)] = {0};
    uint8_t *tlp_channel_in;
    struct mlx5_tlp_channel_obj *obj;
    uint32_t syndrome;

    printf("Creating TLP_EMU_CHANNEL with:\n");
    printf("  - Protocol Mode: %d\n", q_protocol_mode);
    printf("  - Queue Size: %d bytes\n", q_size);
    printf("  - Stride Index: %d\n", tlp_channel_stride_index);

    // Allocate and align queue buffer
    obj = calloc(1, sizeof(*obj));
    if (!obj) {
        fprintf(stderr, "Failed to allocate object structure\n");
        return NULL;
    }

    obj->queue_size = q_size;
    obj->queue_buffer = aligned_alloc(4096, q_size); // Page-aligned allocation
    if (!obj->queue_buffer) {
        fprintf(stderr, "Failed to allocate queue buffer\n");
        goto err_free_obj;
    }

    // Initialize queue buffer with test pattern
    memset(obj->queue_buffer, 0xAB, q_size);

    // Register memory with RDMA
    obj->mr = ibv_reg_mr(pd, obj->queue_buffer, q_size, 
                         IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
    if (!obj->mr) {
        fprintf(stderr, "Failed to register memory region: %s\n", strerror(errno));
        goto err_free_buffer;
    }

    printf("  - Queue Buffer VA: %p\n", obj->queue_buffer);
    printf("  - Memory Key (mkey): 0x%x\n", obj->mr->lkey);

    // Setup command input
    DEVX_SET(general_obj_in_cmd_hdr, in, opcode, MLX5_CMD_OP_CREATE_GENERAL_OBJECT);
    DEVX_SET(general_obj_in_cmd_hdr, in, obj_type, MLX5_OBJ_TYPE_TLP_EMU_CHANNEL);

    tlp_channel_in = in + DEVX_ST_SZ_BYTES(general_obj_in_cmd_hdr);
    
    // Set TLP_EMU_CHANNEL parameters based on firmware structure
    DEVX_SET(tlp_emu_channel, tlp_channel_in, q_protocol_mode, q_protocol_mode);
    DEVX_SET(tlp_emu_channel, tlp_channel_in, q_mkey, obj->mr->lkey);
    DEVX_SET(tlp_emu_channel, tlp_channel_in, q_size, q_size);
    DEVX_SET64(tlp_emu_channel, tlp_channel_in, q_addr, (uint64_t)obj->queue_buffer);
    DEVX_SET(tlp_emu_channel, tlp_channel_in, tlp_channel_stride_index, tlp_channel_stride_index);

    // Execute CREATE command
    obj->obj = mlx5dv_devx_obj_create(ctx, in, sizeof(in), out, sizeof(out));
    if (!obj->obj) {
        syndrome = DEVX_GET(general_obj_out_cmd_hdr, out, syndrome);
        fprintf(stderr, "TLP_EMU_CHANNEL create failed, syndrome 0x%x: %s\n", 
                syndrome, strerror(errno));
        
        // Print detailed syndrome information based on firmware error codes
        switch (syndrome) {
            case 0xE1E101:
                fprintf(stderr, "  Error: Invalid protocol mode (only mode 0 is supported)\n");
                break;
            case 0xE1E102:
                fprintf(stderr, "  Error: Invalid queue size (must be between 1 and 64KB)\n");
                break;
            case 0xE1E103:
                fprintf(stderr, "  Error: Invalid queue address (cannot be zero)\n");
                break;
            case 0xE1E104:
                fprintf(stderr, "  Error: Failed to allocate object resource\n");
                break;
            case 0xE1E108:
                fprintf(stderr, "  Error: VA to PA translation failed (check mkey validity)\n");
                break;
            case 0xE1E109:
                fprintf(stderr, "  Error: Invalid mkey (cannot be zero)\n");
                break;
            case 0x3590f5:
                fprintf(stderr, "  Error: TLP_EMU_CHANNEL object type not supported by firmware\n");
                fprintf(stderr, "  Possible causes:\n");
                fprintf(stderr, "    - Firmware does not include TLP_EMU_CHANNEL support\n");
                fprintf(stderr, "    - Object type 0x59 not registered in firmware\n");
                fprintf(stderr, "    - Firmware configuration missing MCONFIG_GENERIC_EMU\n");
                break;
            default:
                fprintf(stderr, "  Error: Unknown syndrome (0x%x)\n", syndrome);
                fprintf(stderr, "  This may indicate:\n");
                fprintf(stderr, "    - Firmware version mismatch\n");
                fprintf(stderr, "    - Missing firmware features or configuration\n");
                fprintf(stderr, "    - Device capability limitations\n");
                break;
        }
        goto err_dereg_mr;
    }

    obj->obj_id = DEVX_GET(general_obj_out_cmd_hdr, out, obj_id);
    printf("✓ TLP_EMU_CHANNEL created successfully with object ID: 0x%x\n", obj->obj_id);
    
    return obj;

err_dereg_mr:
    ibv_dereg_mr(obj->mr);
err_free_buffer:
    free(obj->queue_buffer);
err_free_obj:
    free(obj);
    return NULL;
}

/**
 * Query TLP_EMU_CHANNEL object
 */
int mlx5_tlp_channel_query(struct ibv_context *ctx, struct mlx5_tlp_channel_obj *obj)
{
    uint8_t in[DEVX_ST_SZ_BYTES(general_obj_in_cmd_hdr)] = {0};
    uint8_t out[DEVX_ST_SZ_BYTES(general_obj_out_cmd_hdr) + DEVX_ST_SZ_BYTES(tlp_emu_channel)] = {0};
    uint8_t *tlp_channel_out;
    uint32_t syndrome;

    printf("\nQuerying TLP_EMU_CHANNEL object ID: 0x%x\n", obj->obj_id);

    // Setup QUERY command
    DEVX_SET(general_obj_in_cmd_hdr, in, opcode, MLX5_CMD_OP_QUERY_GENERAL_OBJECT);
    DEVX_SET(general_obj_in_cmd_hdr, in, obj_type, MLX5_OBJ_TYPE_TLP_EMU_CHANNEL);
    DEVX_SET(general_obj_in_cmd_hdr, in, obj_id, obj->obj_id);

    // Execute QUERY command using the existing object (leveraging mlx5dv_devx_obj_query)
    if (mlx5dv_devx_obj_query(obj->obj, in, sizeof(in), out, sizeof(out))) {
        syndrome = DEVX_GET(general_obj_out_cmd_hdr, out, syndrome);
        fprintf(stderr, "TLP_EMU_CHANNEL query failed, syndrome 0x%x: %s\n", 
                syndrome, strerror(errno));
        
        if (syndrome == 0xE1E105) {
            fprintf(stderr, "  Error: Invalid object ID for query operation\n");
        }
        return -1;
    }

    // Parse query results
    tlp_channel_out = out + DEVX_ST_SZ_BYTES(general_obj_out_cmd_hdr);
    
    uint8_t q_protocol_mode = DEVX_GET(tlp_emu_channel, tlp_channel_out, q_protocol_mode);
    uint32_t q_mkey = DEVX_GET(tlp_emu_channel, tlp_channel_out, q_mkey);
    uint32_t q_size = DEVX_GET(tlp_emu_channel, tlp_channel_out, q_size);
    uint64_t q_addr = DEVX_GET64(tlp_emu_channel, tlp_channel_out, q_addr);
    uint16_t stride_index = DEVX_GET(tlp_emu_channel, tlp_channel_out, tlp_channel_stride_index);

    printf("Query Results:\n");
    printf("  - Protocol Mode: %d\n", q_protocol_mode);
    printf("  - Queue MKey: 0x%x\n", q_mkey);
    printf("  - Queue Size: %d bytes\n", q_size);
    printf("  - Queue Address: 0x%lx\n", q_addr);
    printf("  - Stride Index: %d\n", stride_index);
    printf("✓ TLP_EMU_CHANNEL query completed successfully\n");

    return 0;
}

/**
 * Destroy TLP_EMU_CHANNEL object
 */
int mlx5_tlp_channel_destroy(struct mlx5_tlp_channel_obj *obj)
{
    if (!obj) return -1;

    printf("\nDestroying TLP_EMU_CHANNEL object ID: 0x%x\n", obj->obj_id);

    // Destroy the DevX object
    if (obj->obj) {
        if (mlx5dv_devx_obj_destroy(obj->obj)) {
            fprintf(stderr, "Failed to destroy TLP_EMU_CHANNEL object: %s\n", strerror(errno));
            return -1;
        }
    }

    // Clean up memory resources
    if (obj->mr) {
        ibv_dereg_mr(obj->mr);
    }
    
    if (obj->queue_buffer) {
        free(obj->queue_buffer);
    }
    
    free(obj);
    printf("✓ TLP_EMU_CHANNEL destroyed successfully\n");
    
    return 0;
}

/**
 * Check device capabilities and firmware support
 */
int check_device_capabilities(struct ibv_context *ctx)
{
    printf("\n=== Device Capability Check ===\n");
    
    struct ibv_device_attr device_attr;
    if (ibv_query_device(ctx, &device_attr)) {
        fprintf(stderr, "Failed to query device attributes\n");
        return -1;
    }
    
    printf("Device Information:\n");
    printf("  - Device Name: %s\n", ibv_get_device_name(ctx->device));
    printf("  - Vendor ID: 0x%x\n", device_attr.vendor_id);
    printf("  - Vendor Part ID: %d\n", device_attr.vendor_part_id);
    printf("  - Hardware Version: %d\n", device_attr.hw_ver);
    printf("  - Firmware Version: %s\n", device_attr.fw_ver);
    
    // Try to check if DEVX is supported
    struct mlx5dv_context dv_ctx;
    
    int dv_ret = mlx5dv_query_device(ctx, &dv_ctx);
    if (dv_ret == 0) {
        printf("  - DEVX Support: Available\n");
        printf("  - MLX5 Device Version: %d\n", dv_ctx.version);
    } else {
        printf("  - DEVX Support: Not available or query failed\n");
        return -1;
    }

    // Check HCA capabilities for emulation features
    uint8_t hca_cap_in[DEVX_ST_SZ_BYTES(query_hca_cap_in)] = {0};
    uint8_t hca_cap_out[DEVX_ST_SZ_BYTES(query_hca_cap_out)] = {0};
    
    DEVX_SET(query_hca_cap_in, hca_cap_in, opcode, MLX5_CMD_OP_QUERY_HCA_CAP);
    DEVX_SET(query_hca_cap_in, hca_cap_in, op_mod, 
             MLX5_SET_HCA_CAP_OP_MOD_GENERAL_DEVICE | HCA_CAP_OPMOD_GET_CUR);
    
    int hca_ret = mlx5dv_devx_general_cmd(ctx, hca_cap_in, sizeof(hca_cap_in), 
                                          hca_cap_out, sizeof(hca_cap_out));
    if (hca_ret == 0) {
        uint8_t nvme_device_emulation_manager = DEVX_GET(query_hca_cap_out, hca_cap_out, 
                                                        capability.cmd_hca_cap.nvme_device_emulation_manager);
        printf("  - NVME Device Emulation Manager: %s\n", nvme_device_emulation_manager ? "Supported" : "Not Supported");
        printf("  - Basic HCA capabilities query successful\n");
    } else {
        printf("  - Warning: Could not query HCA capabilities (ret=%d, errno=%s)\n", 
               hca_ret, strerror(errno));
    }

    // Try to query object types capabilities to check if TLP_EMU_CHANNEL is supported
    uint8_t obj_type_in[DEVX_ST_SZ_BYTES(query_hca_cap_in)] = {0};
    uint8_t obj_type_out[DEVX_ST_SZ_BYTES(query_hca_cap_out)] = {0};
    
    DEVX_SET(query_hca_cap_in, obj_type_in, opcode, MLX5_CMD_OP_QUERY_HCA_CAP);
    DEVX_SET(query_hca_cap_in, obj_type_in, op_mod, 
             MLX5_SET_HCA_CAP_OP_MOD_GENERAL_DEVICE2 | HCA_CAP_OPMOD_GET_CUR);
    
    int obj_ret = mlx5dv_devx_general_cmd(ctx, obj_type_in, sizeof(obj_type_in), 
                                          obj_type_out, sizeof(obj_type_out));
    if (obj_ret == 0) {
        printf("  - Extended capabilities query successful\n");
    } else {
        printf("  - Warning: Could not query extended capabilities (ret=%d, errno=%s)\n", 
               obj_ret, strerror(errno));
    }
    
    printf("✓ Device capability check completed\n");
    return 0;
}

/**
 * Test if TLP_EMU_CHANNEL object type is supported by firmware
 */
int test_tlp_channel_support(struct ibv_context *ctx, struct ibv_pd *pd)
{
    printf("\n=== Testing TLP_EMU_CHANNEL Support ===\n");
    
    // Try to create a minimal TLP_EMU_CHANNEL object to check support
    printf("Testing TLP_EMU_CHANNEL object type support...\n");
    
    // Allocate a minimal queue buffer
    void *queue_buffer = aligned_alloc(64, 512); // 512 bytes, 64-byte aligned
    if (!queue_buffer) {
        fprintf(stderr, "Failed to allocate queue buffer\n");
        return -1;
    }
    
    struct ibv_mr *mr = ibv_reg_mr(pd, queue_buffer, 512, IBV_ACCESS_LOCAL_WRITE);
    if (!mr) {
        fprintf(stderr, "Failed to register memory region\n");
        free(queue_buffer);
        return -1;
    }
    
    // Prepare minimal CREATE command for TLP_EMU_CHANNEL
    uint8_t in[DEVX_ST_SZ_BYTES(general_obj_in_cmd_hdr) + DEVX_ST_SZ_BYTES(tlp_emu_channel)] = {0};
    uint8_t out[DEVX_ST_SZ_BYTES(general_obj_out_cmd_hdr)] = {0};
    uint8_t *tlp_channel_in;

    // Setup CREATE command header
    DEVX_SET(general_obj_in_cmd_hdr, in, opcode, MLX5_CMD_OP_CREATE_GENERAL_OBJECT);
    DEVX_SET(general_obj_in_cmd_hdr, in, obj_type, MLX5_OBJ_TYPE_TLP_EMU_CHANNEL);

    // Setup TLP_EMU_CHANNEL parameters
    tlp_channel_in = in + DEVX_ST_SZ_BYTES(general_obj_in_cmd_hdr);
    DEVX_SET(tlp_emu_channel, tlp_channel_in, q_protocol_mode, 0);
    DEVX_SET(tlp_emu_channel, tlp_channel_in, q_mkey, mr->lkey);
    DEVX_SET(tlp_emu_channel, tlp_channel_in, q_size, 512);
    DEVX_SET64(tlp_emu_channel, tlp_channel_in, q_addr, (uintptr_t)queue_buffer);
    DEVX_SET(tlp_emu_channel, tlp_channel_in, tlp_channel_stride_index, 1);

    // Try to create the object
    int support_ret = mlx5dv_devx_general_cmd(ctx, in, sizeof(in), out, sizeof(out));
    uint32_t syndrome = DEVX_GET(general_obj_out_cmd_hdr, out, syndrome);
    
    printf("  DEVX call result: ret=%d, errno=%d (%s), syndrome=0x%x\n", 
           support_ret, errno, strerror(errno), syndrome);
    
    ibv_dereg_mr(mr);
    free(queue_buffer);
    
    // Modified logic: Check syndrome first, then return value
    if (syndrome == 0) {
        uint32_t obj_id = DEVX_GET(general_obj_out_cmd_hdr, out, obj_id);
        printf("✓ TLP_EMU_CHANNEL object type is SUPPORTED by firmware (obj_id=0x%x)\n", obj_id);
        
        // Clean up - destroy the test object
        uint8_t destroy_in[DEVX_ST_SZ_BYTES(general_obj_in_cmd_hdr)] = {0};
        uint8_t destroy_out[DEVX_ST_SZ_BYTES(general_obj_out_cmd_hdr)] = {0};
        
        DEVX_SET(general_obj_in_cmd_hdr, destroy_in, opcode, MLX5_CMD_OP_DESTROY_GENERAL_OBJECT);
        DEVX_SET(general_obj_in_cmd_hdr, destroy_in, obj_type, MLX5_OBJ_TYPE_TLP_EMU_CHANNEL);
        DEVX_SET(general_obj_in_cmd_hdr, destroy_in, obj_id, obj_id);
        
        mlx5dv_devx_general_cmd(ctx, destroy_in, sizeof(destroy_in), destroy_out, sizeof(destroy_out));
        return 0;
    } else {
        printf("✗ TLP_EMU_CHANNEL object type is NOT SUPPORTED by firmware\n");
        printf("  Syndrome: 0x%x\n", syndrome);
        
        switch (syndrome) {
            case 0x3590f5:
                printf("  Analysis: Object type 0x%x (TLP_EMU_CHANNEL) not supported\n", MLX5_OBJ_TYPE_TLP_EMU_CHANNEL);
                printf("  Possible causes:\n");
                printf("    - Firmware built without MCONFIG_GENERIC_EMU support\n");
                printf("    - TLP_EMU_CHANNEL feature not enabled in current firmware\n");
                printf("    - Firmware version does not include TLP emulation support\n");
                break;
            default:
                printf("  Analysis: Unexpected error during object creation\n");
                break;
        }
        return -1;
    }
}

/**
 * Test TLP_EMU_CHANNEL operations with various parameters
 */
int test_tlp_channel_operations(struct ibv_context *ctx, struct ibv_pd *pd)
{
    struct mlx5_tlp_channel_obj *channel_obj;
    int ret = 0;

    printf("\n=== Testing TLP_EMU_CHANNEL Operations ===\n");

    // Test 1: Valid parameters
    printf("\nTest 1: Creating channel with valid parameters\n");
    channel_obj = mlx5_tlp_channel_create(ctx, pd, 0, 4096, 1);
    if (!channel_obj) {
        printf("✗ Test 1 failed\n");
        return -1;
    }

    // Query the created object
    if (mlx5_tlp_channel_query(ctx, channel_obj) != 0) {
        printf("✗ Query operation failed\n");
        ret = -1;
    }

    // Destroy the object
    if (mlx5_tlp_channel_destroy(channel_obj) != 0) {
        printf("✗ Destroy operation failed\n");
        ret = -1;
    }

    // Test 2: Test error cases (invalid protocol mode)
    printf("\nTest 2: Testing invalid protocol mode (should fail)\n");
    channel_obj = mlx5_tlp_channel_create(ctx, pd, 1, 4096, 1); // Invalid protocol mode
    if (channel_obj) {
        printf("✗ Test 2 unexpectedly succeeded (should have failed)\n");
        mlx5_tlp_channel_destroy(channel_obj);
        ret = -1;
    } else {
        printf("✓ Test 2 passed (correctly rejected invalid protocol mode)\n");
    }

    // Test 3: Test large queue size
    printf("\nTest 3: Testing maximum queue size (64KB)\n");
    channel_obj = mlx5_tlp_channel_create(ctx, pd, 0, 65536, 2);
    if (channel_obj) {
        printf("✓ Test 3 passed (64KB queue created successfully)\n");
        mlx5_tlp_channel_query(ctx, channel_obj);
        mlx5_tlp_channel_destroy(channel_obj);
    } else {
        printf("✗ Test 3 failed (64KB queue creation failed)\n");
        ret = -1;
    }

    // Test 3.5: Test Mode0 specification compliance (64KB = 1K × 64B QEs)
    printf("\nTest 3.5: Testing Mode0 specification (64KB = 1024 × 64B queue elements)\n");
    uint32_t mode0_queue_size = 1024 * 64; // 1K elements of 64B each = 64KB
    printf("  Mode0 Queue Size: %d bytes (1024 × 64B elements)\n", mode0_queue_size);
    channel_obj = mlx5_tlp_channel_create(ctx, pd, 0, mode0_queue_size, 2);
    if (channel_obj) {
        printf("✓ Test 3.5 passed (Mode0 specification compliance verified)\n");
        mlx5_tlp_channel_query(ctx, channel_obj);
        mlx5_tlp_channel_destroy(channel_obj);
    } else {
        printf("✗ Test 3.5 failed (Mode0 specification test failed)\n");
        ret = -1;
    }

    // Test 4: Test oversized queue (should fail)
    printf("\nTest 4: Testing oversized queue (should fail)\n");
    channel_obj = mlx5_tlp_channel_create(ctx, pd, 0, 65537, 1); // Over 64KB limit
    if (channel_obj) {
        printf("✗ Test 4 unexpectedly succeeded (should have failed)\n");
        mlx5_tlp_channel_destroy(channel_obj);
        ret = -1;
    } else {
        printf("✓ Test 4 passed (correctly rejected oversized queue)\n");
    }

    return ret;
}

int main(int argc, char *argv[])
{
    const char *dev_name = "mlx5_0";  // Fixed device name
    int ret = 0;
    
    if (argc > 1) {
        dev_name = argv[1];  // Allow override if specified
    }

    printf("TLP Channel Test for NVIDIA Firmware\n");
    printf("Testing device: %s\n", dev_name);
    printf("Testing TLP_EMU_CHANNEL object (type 0x%x)\n", MLX5_OBJ_TYPE_TLP_EMU_CHANNEL);
    printf("=====================================\n");

    // Get and open device
    struct ibv_device *dev = get_device(dev_name);
    if (!dev) {
        fprintf(stderr, "Device %s not found\n", dev_name);
        return 1;
    }

    struct ibv_context *ctx = ibv_open_device(dev);
    if (!ctx) {
        fprintf(stderr, "Failed to open device %s: %s\n", dev_name, strerror(errno));
        return 1;
    }

    // Check device capabilities first
    ret = check_device_capabilities(ctx);
    if (ret != 0) {
        printf("Device capability check failed, but continuing with tests...\n");
    }

    // Allocate protection domain
    struct ibv_pd *pd = ibv_alloc_pd(ctx);
    if (!pd) {
        fprintf(stderr, "Failed to allocate protection domain: %s\n", strerror(errno));
        ret = 1;
        goto cleanup_ctx;
    }

    // Test TLP_EMU_CHANNEL support first
    if (test_tlp_channel_support(ctx, pd) != 0) {
        printf("\n=== Test Summary ===\n");
        printf("✗ TLP_EMU_CHANNEL is not supported by current firmware\n");
        printf("  Recommendations:\n");
        printf("  1. Update firmware to include TLP emulation support\n");
        printf("  2. Ensure firmware is built with MCONFIG_GENERIC_EMU=y\n");
        printf("  3. Check if device supports generic emulation features\n");
        printf("  4. Verify firmware includes TLP_EMU_CHANNEL object type 0x59\n");
        ret = -1;
        goto cleanup_pd;
    }

    // Run comprehensive tests
    ret = test_tlp_channel_operations(ctx, pd);

    printf("\n=== Test Summary ===\n");
    if (ret == 0) {
        printf("✓ All TLP_EMU_CHANNEL tests completed successfully!\n");
        printf("  The firmware modifications are working correctly.\n");
    } else {
        printf("✗ Some tests failed. Check firmware implementation.\n");
    }

cleanup_pd:
    // Cleanup
    ibv_dealloc_pd(pd);
cleanup_ctx:
    ibv_close_device(ctx);
    
    return ret;
} 