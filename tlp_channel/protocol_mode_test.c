#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <infiniband/verbs.h>
#include <infiniband/mlx5dv.h>
#include "mlx5_ifc.h"

#define MLX5_OBJ_TYPE_TLP_EMU_CHANNEL 0x59

struct protocol_test_result {
    uint8_t input_mode;
    int create_success;
    uint8_t firmware_mode;
    int should_succeed;
};

/**
 * Test a specific protocol mode value
 */
int test_protocol_mode(struct ibv_context *ctx, struct ibv_pd *pd, uint8_t protocol_mode, int should_succeed)
{
    printf("\n=== Testing Protocol Mode %d ===\n", protocol_mode);
    printf("Expected result: %s\n", should_succeed ? "SUCCESS" : "FAILURE");
    
    // Allocate test buffer
    void *queue_buffer = aligned_alloc(64, 4096);
    if (!queue_buffer) {
        fprintf(stderr, "Failed to allocate queue buffer\n");
        return -1;
    }
    
    struct ibv_mr *mr = ibv_reg_mr(pd, queue_buffer, 4096, IBV_ACCESS_LOCAL_WRITE);
    if (!mr) {
        fprintf(stderr, "Failed to register memory region\n");
        free(queue_buffer);
        return -1;
    }
    
    // Prepare CREATE command
    uint8_t in[DEVX_ST_SZ_BYTES(general_obj_in_cmd_hdr) + DEVX_ST_SZ_BYTES(tlp_emu_channel)] = {0};
    uint8_t out[DEVX_ST_SZ_BYTES(general_obj_out_cmd_hdr)] = {0};
    uint8_t *tlp_channel_in;

    // Setup CREATE command header
    DEVX_SET(general_obj_in_cmd_hdr, in, opcode, MLX5_CMD_OP_CREATE_GENERAL_OBJECT);
    DEVX_SET(general_obj_in_cmd_hdr, in, obj_type, MLX5_OBJ_TYPE_TLP_EMU_CHANNEL);

    // Setup TLP_EMU_CHANNEL parameters - CRITICAL: Test the protocol_mode field
    tlp_channel_in = in + DEVX_ST_SZ_BYTES(general_obj_in_cmd_hdr);
    DEVX_SET(tlp_emu_channel, tlp_channel_in, q_protocol_mode, protocol_mode);  // THIS IS THE KEY TEST
    DEVX_SET(tlp_emu_channel, tlp_channel_in, q_mkey, mr->lkey);
    DEVX_SET(tlp_emu_channel, tlp_channel_in, q_size, 4096);
    DEVX_SET64(tlp_emu_channel, tlp_channel_in, q_addr, (uintptr_t)queue_buffer);
    DEVX_SET(tlp_emu_channel, tlp_channel_in, tlp_channel_stride_index, 1);

    printf("Setting protocol_mode=%d in command structure\n", protocol_mode);
    
    // Print raw bytes to debug data structure issues
    printf("Raw command data (first 32 bytes):\n");
    for (int i = 0; i < 32 && i < sizeof(in); i++) {
        printf("%02x ", in[i]);
        if ((i + 1) % 16 == 0) printf("\n");
    }
    printf("\n");
    
    // Try to create the object
    struct mlx5dv_devx_obj *obj = mlx5dv_devx_obj_create(ctx, in, sizeof(in), out, sizeof(out));
    uint32_t syndrome = DEVX_GET(general_obj_out_cmd_hdr, out, syndrome);
    
    printf("CREATE result: %s (syndrome=0x%x)\n", obj ? "SUCCESS" : "FAILED", syndrome);
    
    if (obj) {
        uint32_t obj_id = DEVX_GET(general_obj_out_cmd_hdr, out, obj_id);
        printf("Object created with ID: 0x%x\n", obj_id);
        
        // Query the object to see what protocol_mode the firmware has
        uint8_t query_in[DEVX_ST_SZ_BYTES(general_obj_in_cmd_hdr)] = {0};
        uint8_t query_out[DEVX_ST_SZ_BYTES(general_obj_out_cmd_hdr) + DEVX_ST_SZ_BYTES(tlp_emu_channel)] = {0};
        
        DEVX_SET(general_obj_in_cmd_hdr, query_in, opcode, MLX5_CMD_OP_QUERY_GENERAL_OBJECT);
        DEVX_SET(general_obj_in_cmd_hdr, query_in, obj_type, MLX5_OBJ_TYPE_TLP_EMU_CHANNEL);
        DEVX_SET(general_obj_in_cmd_hdr, query_in, obj_id, obj_id);
        
        if (mlx5dv_devx_obj_query(obj, query_in, sizeof(query_in), query_out, sizeof(query_out)) == 0) {
            uint8_t *tlp_channel_out = query_out + DEVX_ST_SZ_BYTES(general_obj_out_cmd_hdr);
            uint8_t firmware_mode = DEVX_GET(tlp_emu_channel, tlp_channel_out, q_protocol_mode);
            
            printf("Firmware stored protocol_mode: %d\n", firmware_mode);
            
            if (firmware_mode != protocol_mode) {
                printf("❌ DATA MISMATCH: Sent %d, firmware has %d\n", protocol_mode, firmware_mode);
            } else {
                printf("✅ Data match: Both %d\n", protocol_mode);
            }
        } else {
            printf("⚠️  Query failed\n");
        }
        
        // Clean up
        mlx5dv_devx_obj_destroy(obj);
    } else {
        printf("CREATE failed with syndrome: 0x%x\n", syndrome);
        switch (syndrome) {
            case 0xE1E101:
                printf("  -> Invalid protocol mode (expected for mode != 0)\n");
                break;
            default:
                printf("  -> Other error\n");
                break;
        }
    }
    
    // Validate test result
    int test_passed = 0;
    if (should_succeed && obj) {
        printf("✅ Test PASSED: Expected success, got success\n");
        test_passed = 1;
    } else if (!should_succeed && !obj && syndrome == 0xE1E101) {
        printf("✅ Test PASSED: Expected failure with invalid protocol mode, got syndrome 0xE1E101\n");
        test_passed = 1;
    } else if (should_succeed && !obj) {
        printf("❌ Test FAILED: Expected success, got failure\n");
    } else if (!should_succeed && obj) {
        printf("❌ Test FAILED: Expected failure, got success\n");
    } else {
        printf("❓ Test UNCLEAR: Unexpected result\n");
    }
    
    ibv_dereg_mr(mr);
    free(queue_buffer);
    
    return test_passed ? 0 : -1;
}

/**
 * Find MLX5 device by name
 */
struct ibv_device* get_device(const char *dev_name) {
    struct ibv_device **device_list = ibv_get_device_list(NULL);
    struct ibv_device *device = NULL;
    
    for (int i = 0; device_list[i] != NULL; i++) {
        if (strcmp(ibv_get_device_name(device_list[i]), dev_name) == 0) {
            device = device_list[i];
            break;
        }
    }
    
    ibv_free_device_list(device_list);
    return device;
}

int main(int argc, char *argv[])
{
    const char *dev_name = (argc > 1) ? argv[1] : "mlx5_0";
    
    printf("Protocol Mode Test for TLP_EMU_CHANNEL\n");
    printf("Testing device: %s\n", dev_name);
    printf("=====================================\n");
    
    struct ibv_device *dev = get_device(dev_name);
    if (!dev) {
        fprintf(stderr, "Failed to find device %s\n", dev_name);
        return 1;
    }
    
    struct ibv_context *ctx = ibv_open_device(dev);
    if (!ctx) {
        fprintf(stderr, "Failed to open device\n");
        return 1;
    }
    
    struct ibv_pd *pd = ibv_alloc_pd(ctx);
    if (!pd) {
        fprintf(stderr, "Failed to allocate PD\n");
        ibv_close_device(ctx);
        return 1;
    }
    
    printf("\n🎯 Systematic Protocol Mode Testing\n");
    
    int total_tests = 0;
    int passed_tests = 0;
    
    // Test cases: protocol_mode value, should_succeed
    struct {
        uint8_t mode;
        int should_succeed;
        const char *description;
    } test_cases[] = {
        {0, 1, "Mode 0 (valid according to spec)"},
        {1, 0, "Mode 1 (invalid according to spec)"},
        {2, 0, "Mode 2 (invalid)"},
        {255, 0, "Mode 255 (invalid)"},
    };
    
    for (int i = 0; i < sizeof(test_cases)/sizeof(test_cases[0]); i++) {
        total_tests++;
        printf("\n--- Test Case %d: %s ---\n", i+1, test_cases[i].description);
        
        if (test_protocol_mode(ctx, pd, test_cases[i].mode, test_cases[i].should_succeed) == 0) {
            passed_tests++;
        }
    }
    
    printf("\n=== Test Summary ===\n");
    printf("Total tests: %d\n", total_tests);
    printf("Passed tests: %d\n", passed_tests);
    printf("Failed tests: %d\n", total_tests - passed_tests);
    
    if (passed_tests == total_tests) {
        printf("🎉 All tests PASSED!\n");
    } else {
        printf("❌ Some tests FAILED. Check data structure mapping.\n");
    }
    
    ibv_dealloc_pd(pd);
    ibv_close_device(ctx);
    
    return (passed_tests == total_tests) ? 0 : 1;
} 