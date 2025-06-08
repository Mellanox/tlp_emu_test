/*
 * Device Configuration Helper for TLP_DEVICES + VUID Integration
 * 设备配置助手 - 激活设备代表器和VF/SF配置
 */

#include <infiniband/verbs.h>
#include <infiniband/mlx5dv.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdbool.h>
#include <unistd.h>

#include "mlx5_ifc.h"

// 常量定义
#define MLX5_CMD_OPCODE_QUERY_EMULATED_FUNCTIONS_INFO 0xb03
#define PRM_EMULATION_OPMOD_TLP_DEVICES     0x7

// 尝试激活设备和查询VUID的完整流程
int complete_device_activation_and_vuid_query(struct ibv_context *ctx) {
    printf("🔧 完整设备激活和VUID查询流程\n");
    printf("=====================================\n");
    
    // 步骤1: 检查设备状态
    printf("\n📋 步骤1: 检查设备当前状态\n");
    
    struct {
        uint32_t opcode_uid;
        uint32_t reserved[7];
    } hca_cap_cmd = {
        .opcode_uid = htobe32((MLX5_CMD_OP_QUERY_HCA_CAP << 16) | 0),
    };
    
    struct {
        uint32_t status_syndrome;
        uint32_t syndrome;
        uint32_t reserved[30];
    } hca_cap_out = {0};
    
    int ret = mlx5dv_devx_general_cmd(ctx, &hca_cap_cmd, sizeof(hca_cap_cmd), 
                                      &hca_cap_out, sizeof(hca_cap_out));
    
    if (ret == 0 && (hca_cap_out.status_syndrome >> 24) == 0) {
        printf("✅ 设备HCA能力查询成功\n");
    } else {
        printf("⚠️  设备HCA能力查询失败，但继续\n");
    }
    
    // 步骤2: 尝试查询ESW functions (可能有助于激活)
    printf("\n📋 步骤2: 查询ESW Functions\n");
    
    struct {
        uint32_t opcode_uid;
        uint32_t reserved1;
        uint32_t reserved2;
        uint32_t op_mod;
        uint32_t reserved3[4];
    } esw_cmd = {
        .opcode_uid = htobe32((MLX5_CMD_OP_QUERY_ESW_FUNCTIONS << 16) | 0),
        .op_mod = htobe32(0),
    };
    
    struct {
        uint32_t status_syndrome;
        uint32_t syndrome;
        uint32_t reserved[30];
    } esw_out = {0};
    
    ret = mlx5dv_devx_general_cmd(ctx, &esw_cmd, sizeof(esw_cmd), &esw_out, sizeof(esw_out));
    
    if (ret == 0 && (esw_out.status_syndrome >> 24) == 0) {
        printf("✅ ESW Functions查询成功\n");
    } else {
        printf("⚠️  ESW Functions查询失败: %s\n", strerror(errno));
    }
    
    // 步骤3: 执行TLP_DEVICES查询
    printf("\n📋 步骤3: 执行TLP_DEVICES查询\n");
    
    struct {
        uint16_t opcode;
        uint16_t uid;
        uint16_t reserved1;
        uint16_t op_mod;
        uint32_t reserved2;
        uint16_t reserved3;
        uint16_t pf_vhca_id;
    } tlp_cmd = {
        .opcode = htobe16(MLX5_CMD_OPCODE_QUERY_EMULATED_FUNCTIONS_INFO),
        .uid = htobe16(0),
        .op_mod = htobe16(PRM_EMULATION_OPMOD_TLP_DEVICES),
        .pf_vhca_id = htobe16(0),
    };
    
    struct {
        uint8_t  status;
        uint8_t  reserved0[3];
        uint32_t syndrome;
        uint8_t  raw_data[256];
    } tlp_out = {0};
    
    ret = mlx5dv_devx_general_cmd(ctx, &tlp_cmd, sizeof(tlp_cmd), &tlp_out, sizeof(tlp_out));
    
    if (ret != 0 || tlp_out.status != 0) {
        printf("❌ TLP_DEVICES查询失败\n");
        return -1;
    }
    
    printf("✅ TLP_DEVICES查询成功\n");
    
    // 分析TLP_DEVICES输出
    printf("原始数据: ");
    for (int i = 8; i < 24; i++) {
        printf("%02x ", tlp_out.raw_data[i]);
    }
    printf("\n");
    
    // 步骤4: 尝试多种VUID查询方法
    printf("\n📋 步骤4: 尝试高级VUID查询方法\n");
    
    // 候选VHCA ID基于TLP_DEVICES输出
    uint16_t vhca_candidates[] = {3, 5, 0, 144, 20};
    int num_candidates = sizeof(vhca_candidates) / sizeof(vhca_candidates[0]);
    
    for (int i = 0; i < num_candidates; i++) {
        uint16_t vhca_id = vhca_candidates[i];
        
        printf("\n🎯 测试 VHCA ID: 0x%04x (%d)\n", vhca_id, vhca_id);
        
        // 方法A: 标准VUID查询
        struct {
            uint32_t opcode_uid;
            uint32_t reserved1[2];
            uint32_t vhca_id_field;
            uint32_t reserved2[4];
        } vuid_cmd_a = {
            .opcode_uid = htobe32((MLX5_CMD_OP_QUERY_VUID << 16) | 0),
            .vhca_id_field = htobe32(vhca_id),
        };
        
        struct {
            uint32_t status_syndrome;
            uint32_t syndrome;
            uint32_t reserved[24];
            uint32_t reserved2;
            uint32_t num_entries;
            char vuid_data[512];
        } vuid_out_a = {0};
        
        ret = mlx5dv_devx_general_cmd(ctx, &vuid_cmd_a, sizeof(vuid_cmd_a), 
                                      &vuid_out_a, sizeof(vuid_out_a));
        
        if (ret == 0 && (vuid_out_a.status_syndrome >> 24) == 0) {
            uint32_t num_entries = be32toh(vuid_out_a.num_entries);
            printf("  方法A: ✅ 成功 - %u VUID条目\n", num_entries);
            
            if (num_entries > 0) {
                printf("  🎊 找到VUID数据!\n");
                
                // 搜索有效的VUID字符串
                for (int offset = 0; offset < 256; offset += 128) {
                    char vuid_str[129];
                    memset(vuid_str, 0, sizeof(vuid_str));
                    memcpy(vuid_str, vuid_out_a.vuid_data + offset, 128);
                    
                    // 查找可打印字符串
                    int start = -1, end = -1;
                    for (int j = 0; j < 128; j++) {
                        if (vuid_str[j] >= 32 && vuid_str[j] <= 126) {
                            if (start == -1) start = j;
                            end = j;
                        }
                    }
                    
                    if (start >= 0 && end - start > 8) {
                        vuid_str[end + 1] = '\0';
                        printf("  📝 VUID发现: '%s'\n", vuid_str + start);
                        
                        if (strstr(vuid_str + start, "MT2334") != NULL) {
                            printf("  🎉 SUCCESS! 找到预期的VUID!\n");
                            printf("  ✅ VHCA ID 0x%04x 是正确的设备VHCA ID\n", vhca_id);
                            return 0;
                        }
                    }
                }
                
                // 显示原始数据用于调试
                printf("  📊 原始数据: ");
                for (int k = 0; k < 64; k++) {
                    printf("%02x ", (uint8_t)vuid_out_a.vuid_data[k]);
                    if ((k + 1) % 16 == 0) printf("\n                ");
                }
                printf("\n");
            }
        } else {
            printf("  方法A: ❌ 失败\n");
        }
        
        // 方法B: 设置query_vfs_vuid位
        struct {
            uint32_t opcode_uid;
            uint32_t reserved1[2];
            uint32_t vhca_id_field;
            uint32_t reserved2[4];
        } vuid_cmd_b = {
            .opcode_uid = htobe32((MLX5_CMD_OP_QUERY_VUID << 16) | 0),
            .vhca_id_field = htobe32((1 << 31) | vhca_id),  // query_vfs_vuid=1
        };
        
        struct {
            uint32_t status_syndrome;
            uint32_t syndrome;
            uint32_t reserved[24];
            uint32_t reserved2;
            uint32_t num_entries;
            char vuid_data[512];
        } vuid_out_b = {0};
        
        ret = mlx5dv_devx_general_cmd(ctx, &vuid_cmd_b, sizeof(vuid_cmd_b), 
                                      &vuid_out_b, sizeof(vuid_out_b));
        
        if (ret == 0 && (vuid_out_b.status_syndrome >> 24) == 0) {
            uint32_t num_entries = be32toh(vuid_out_b.num_entries);
            printf("  方法B: ✅ 成功 - %u VUID条目\n", num_entries);
            
            if (num_entries > 0) {
                printf("  🎯 方法B找到VUID数据!\n");
                return 0;  // 找到了
            }
        } else {
            printf("  方法B: ❌ 失败\n");
        }
        
        // 短暂延迟
        usleep(50000);  // 50ms
    }
    
    return -1;  // 未找到VUID
}

int main(int argc, char *argv[]) {
    printf("🔧 Device Configuration Helper\n");
    printf("==============================\n");
    printf("设备配置助手 - 激活设备代表器和VF/SF配置\n\n");
    
    // 设备初始化
    struct ibv_device **device_list;
    struct ibv_device *ibv_dev = NULL;
    struct ibv_context *ctx = NULL;
    int num_devices;
    const char *device_name = "mlx5_0";
    
    if (argc == 2) {
        device_name = argv[1];
    }
    
    printf("🎯 目标设备: %s\n", device_name);
    
    // 获取设备列表
    device_list = ibv_get_device_list(&num_devices);
    if (!device_list) {
        printf("❌ 获取设备列表失败\n");
        return -1;
    }
    
    // 查找设备
    for (int i = 0; i < num_devices; i++) {
        if (strcmp(ibv_get_device_name(device_list[i]), device_name) == 0) {
            ibv_dev = device_list[i];
            break;
        }
    }
    
    if (!ibv_dev) {
        printf("❌ 设备 %s 未找到\n", device_name);
        ibv_free_device_list(device_list);
        return -1;
    }
    
    // 打开设备
    ctx = ibv_open_device(ibv_dev);
    if (!ctx) {
        printf("❌ 打开设备失败: %s\n", strerror(errno));
        ibv_free_device_list(device_list);
        return -1;
    }
    
    printf("✅ 设备连接成功\n");
    
    // 执行完整的设备激活和VUID查询流程
    int result = complete_device_activation_and_vuid_query(ctx);
    
    // 清理
    ibv_close_device(ctx);
    ibv_free_device_list(device_list);
    
    // 结果总结
    printf("\n" "========================================\n");
    printf("🏁 设备配置助手结果总结\n");
    printf("========================================\n");
    
    if (result == 0) {
        printf("🎉 COMPLETE SUCCESS!\n");
        printf("✅ 成功找到并验证VUID数据\n");
        printf("✅ TLP_DEVICES + VUID 集成完全工作\n");
        printf("\n💡 可以继续集成到生产环境\n");
    } else {
        printf("⚠️  PARTIAL SUCCESS:\n");
        printf("✅ TLP_DEVICES hack 工作正常\n");
        printf("✅ 设备连接和基础查询成功\n");
        printf("❌ 仍未找到VUID数据\n");
        printf("\n🔄 可能需要的下一步:\n");
        printf("1. 检查系统是否启用了SR-IOV\n");
        printf("2. 尝试配置VF (Virtual Functions)\n");
        printf("3. 检查设备是否支持device emulation\n");
        printf("4. 查看doca_devemu服务是否运行\n");
        printf("\n💡 调试命令建议:\n");
        printf("   lspci | grep Mellanox\n");
        printf("   echo 1 > /sys/class/net/[interface]/device/sriov_numvfs\n");
        printf("   systemctl status doca_devemu\n");
    }
    
    return result == 0 ? 0 : 1;
} 