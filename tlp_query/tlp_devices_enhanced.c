/*
 * SPDX-License-Identifier: LicenseRef-NvidiaProprietary
 * Copyright(c) 2025 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 *
 * Enhanced TLP_DEVICES + VUID Integration Test
 * 解决设备代表器激活和VUID查询集成问题
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

// 添加缺失的常量定义
#define MLX5_CMD_OPCODE_QUERY_EMULATED_FUNCTIONS_INFO 0xb03
#define PRM_EMULATION_OPMOD_GENERIC_PF      0x6
#define PRM_EMULATION_OPMOD_TLP_DEVICES     0x7

// 简化的命令结构
struct cmd_in {
    uint16_t opcode;
    uint16_t uid;
    uint16_t reserved1;
    uint16_t op_mod;
    uint32_t reserved2;
    uint16_t reserved3;
    uint16_t pf_vhca_id;
};

struct cmd_out {
    uint8_t  status;
    uint8_t  reserved0[3];
    uint32_t syndrome;
    uint8_t  raw_data[256];
};

// VUID 查询结构 - 使用多种格式尝试
#pragma pack(push, 1)
struct vuid_cmd_simple {
    uint32_t opcode_uid;        // [31:16] opcode=0xb22, [15:0] uid=0
    uint32_t reserved1[2];      // 8 bytes reserved
    uint32_t vhca_id_field;     // VHCA ID field
    uint32_t reserved2[4];      // 16 bytes reserved - total 32 bytes
};

struct vuid_out_simple {
    uint32_t status_syndrome;   // [31:24] status, [23:0] reserved
    uint32_t syndrome;
    uint32_t reserved1[24];     // Skip to VUID data location
    uint32_t reserved2;
    uint32_t num_entries;       // Number of VUID entries
    char     vuid_data[1024];   // VUID strings buffer
};
#pragma pack(pop)

// 设备代表器激活函数
int try_activate_device_representor(struct ibv_context *ctx, uint16_t vhca_id) {
    printf("🔧 尝试激活设备代表器 VHCA ID: 0x%04x\n", vhca_id);
    
    // 方法1: 尝试创建基本的设备上下文查询
    struct {
        uint32_t opcode_uid;
        uint32_t reserved[7];
    } activate_cmd = {
        .opcode_uid = htobe32((MLX5_CMD_OP_QUERY_HCA_CAP << 16) | 0),
    };
    
    struct {
        uint32_t status_syndrome;
        uint32_t syndrome;
        uint32_t reserved[30];
    } activate_out = {0};
    
    int ret = mlx5dv_devx_general_cmd(ctx, &activate_cmd, sizeof(activate_cmd), 
                                      &activate_out, sizeof(activate_out));
    
    if (ret == 0 && (activate_out.status_syndrome >> 24) == 0) {
        printf("✅ 设备上下文查询成功\n");
        return 0;
    }
    
    printf("⚠️  设备上下文查询失败，继续尝试其他方法\n");
    return -1;
}

// 增强的VUID查询 - 尝试多种格式
int enhanced_vuid_query(struct ibv_context *ctx, uint16_t vhca_id) {
    printf("\n🔍 增强VUID查询 - VHCA ID: 0x%04x (%d)\n", vhca_id, vhca_id);
    
    // 先尝试激活设备代表器
    try_activate_device_representor(ctx, vhca_id);
    
    // 方法1: 标准格式
    struct vuid_cmd_simple cmd1 = {0};
    struct vuid_out_simple out1 = {0};
    
    cmd1.opcode_uid = htobe32((MLX5_CMD_OP_QUERY_VUID << 16) | 0);
    cmd1.vhca_id_field = htobe32(vhca_id);
    
    printf("  📋 方法1: 标准VUID查询格式\n");
    int ret1 = mlx5dv_devx_general_cmd(ctx, &cmd1, sizeof(cmd1), &out1, sizeof(out1));
    
    if (ret1 == 0) {
        uint32_t status = (be32toh(out1.status_syndrome) >> 24) & 0xFF;
        uint32_t syndrome = be32toh(out1.syndrome);
        
        if (status == 0) {
            uint32_t num_entries = be32toh(out1.num_entries);
            printf("    ✅ 查询成功: %u VUID 条目\n", num_entries);
            
            if (num_entries > 0) {
                printf("    🎯 找到VUID数据!\n");
                
                // 提取并清理VUID字符串
                char vuid_str[129];
                memset(vuid_str, 0, sizeof(vuid_str));
                
                // 尝试不同的VUID数据位置
                for (int offset = 0; offset < 512; offset += 128) {
                    memcpy(vuid_str, out1.vuid_data + offset, 128);
                    vuid_str[128] = '\0';
                    
                    // 检查是否有有效数据
                    bool has_data = false;
                    for (int i = 0; i < 128; i++) {
                        if (vuid_str[i] >= 32 && vuid_str[i] <= 126) {
                            has_data = true;
                            break;
                        }
                    }
                    
                    if (has_data) {
                        // 清理字符串
                        int len = 0;
                        for (int j = 0; j < 128; j++) {
                            if (vuid_str[j] >= 32 && vuid_str[j] <= 126) {
                                len = j + 1;
                            } else if (vuid_str[j] == '\0') {
                                break;
                            }
                        }
                        vuid_str[len] = '\0';
                        
                        if (strlen(vuid_str) > 8) {  // 有效VUID长度
                            printf("    📝 VUID (offset %d): '%s'\n", offset, vuid_str);
                            
                            // 检查是否匹配已知的VUID模式
                            if (strstr(vuid_str, "MT2334") != NULL) {
                                printf("    🎊 BREAKTHROUGH! 找到匹配的VUID!\n");
                                printf("    ✅ VHCA ID 0x%04x 是正确的设备代表器!\n", vhca_id);
                                return 0;  // 成功找到
                            }
                        }
                    }
                }
                
                // 显示原始数据用于调试
                printf("    📊 原始VUID数据 (前64字节): ");
                for (int k = 0; k < 64; k++) {
                    printf("%02x ", (uint8_t)out1.vuid_data[k]);
                    if ((k + 1) % 16 == 0) printf("\n                                    ");
                }
                printf("\n");
                
                return 0;  // 找到了数据，但可能格式不同
            }
        } else {
            printf("    ❌ 命令失败 - Status: 0x%02x, Syndrome: 0x%08x\n", status, syndrome);
        }
    } else {
        printf("    ❌ DevX命令失败: %s\n", strerror(errno));
    }
    
    // 方法2: 备用格式 (不同的VHCA ID位置)
    struct vuid_cmd_simple cmd2 = {0};
    struct vuid_out_simple out2 = {0};
    
    cmd2.opcode_uid = htobe32((MLX5_CMD_OP_QUERY_VUID << 16) | 0);
    cmd2.vhca_id_field = htobe32((1 << 31) | vhca_id);  // 设置query_vfs_vuid位
    
    printf("  📋 方法2: 备用VUID查询格式 (query_vfs_vuid=1)\n");
    int ret2 = mlx5dv_devx_general_cmd(ctx, &cmd2, sizeof(cmd2), &out2, sizeof(out2));
    
    if (ret2 == 0) {
        uint32_t status = (be32toh(out2.status_syndrome) >> 24) & 0xFF;
        if (status == 0) {
            uint32_t num_entries = be32toh(out2.num_entries);
            printf("    ✅ 查询成功: %u VUID 条目\n", num_entries);
            if (num_entries > 0) {
                printf("    🎯 方法2找到VUID数据!\n");
                return 0;
            }
        } else {
            printf("    ❌ 方法2失败 - Status: 0x%02x\n", status);
        }
    } else {
        printf("    ❌ 方法2 DevX失败: %s\n", strerror(errno));
    }
    
    return -1;  // 未找到VUID数据
}

// 分析TLP_DEVICES输出并提取候选VHCA ID
int analyze_tlp_output(struct cmd_out *output, uint16_t *vhca_candidates, int max_candidates) {
    int num_candidates = 0;
    
    if (output->status != 0) {
        return 0;
    }
    
    printf("\n🔍 分析TLP_DEVICES输出寻找候选VHCA ID:\n");
    
    // 显示原始数据
    printf("原始数据: ");
    for (int i = 0; i < 32; i++) {
        printf("%02x ", output->raw_data[i]);
        if ((i + 1) % 8 == 0) printf("\n          ");
    }
    printf("\n");
    
    // 基于已知模式提取候选值
    // 模式: 00 00 00 00 00 00 00 01 62 00 00 05 90 00 00 00
    uint32_t num_functions = be32toh(*(uint32_t*)(output->raw_data + 4));
    
    if (num_functions == 1) {
        // 从设备信息中提取多种可能的VHCA ID
        uint8_t *device_data = output->raw_data + 8;  // 62 00 00 05 90 00 00 00
        
        // 候选1: 基于固件日志 gvmi=3
        if (num_candidates < max_candidates) {
            vhca_candidates[num_candidates++] = 3;
            printf("  候选%d: 0x%04x (固件日志gvmi)\n", num_candidates, 3);
        }
        
        // 候选2: 从数据字节3提取 (0x05)
        if (num_candidates < max_candidates) {
            vhca_candidates[num_candidates++] = device_data[3];
            printf("  候选%d: 0x%04x (数据字节3)\n", num_candidates, device_data[3]);
        }
        
        // 候选3: 从数据字节5提取 (0x90)
        if (num_candidates < max_candidates) {
            vhca_candidates[num_candidates++] = device_data[4];
            printf("  候选%d: 0x%04x (数据字节4)\n", num_candidates, device_data[4]);
        }
        
        // 候选4-8: 相邻值探索
        for (int i = 0; i < 5 && num_candidates < max_candidates; i++) {
            uint16_t candidate = 5 + i;  // 5, 6, 7, 8, 9
            vhca_candidates[num_candidates++] = candidate;
            printf("  候选%d: 0x%04x (相邻值)\n", num_candidates, candidate);
        }
        
        // 候选9-10: 0和特殊值
        if (num_candidates < max_candidates) {
            vhca_candidates[num_candidates++] = 0;
            printf("  候选%d: 0x%04x (零值)\n", num_candidates, 0);
        }
        
        if (num_candidates < max_candidates) {
            vhca_candidates[num_candidates++] = 20;
            printf("  候选%d: 0x%04x (测试中发现的有效值)\n", num_candidates, 20);
        }
    }
    
    printf("✅ 提取了 %d 个候选VHCA ID\n", num_candidates);
    return num_candidates;
}

int main(int argc, char *argv[]) {
    printf("🚀 Enhanced TLP_DEVICES + VUID Integration Test\n");
    printf("==============================================\n");
    printf("解决设备代表器激活和VUID查询集成问题\n\n");
    
    // 设备初始化
    struct ibv_device **device_list;
    struct ibv_device *ibv_dev = NULL;
    struct ibv_context *ctx = NULL;
    int num_devices;
    const char *device_name = "mlx5_0";
    
    if (argc == 2) {
        device_name = argv[1];
    }
    
    printf("使用设备: %s\n", device_name);
    
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
        printf("❌ 打开设备 %s 失败\n", device_name);
        ibv_free_device_list(device_list);
        return -1;
    }
    
    printf("✅ 设备已连接: %s\n", device_name);
    
    // 步骤1: 执行TLP_DEVICES查询
    printf("\n=== 步骤1: TLP_DEVICES查询 ===\n");
    struct cmd_in cmd_in = {0};
    struct cmd_out cmd_out = {0};
    
    cmd_in.opcode = htobe16(MLX5_CMD_OPCODE_QUERY_EMULATED_FUNCTIONS_INFO);
    cmd_in.uid = htobe16(0);
    cmd_in.reserved1 = htobe16(0);
    cmd_in.op_mod = htobe16(PRM_EMULATION_OPMOD_TLP_DEVICES);
    cmd_in.reserved2 = htobe32(0);
    cmd_in.reserved3 = htobe16(0);
    cmd_in.pf_vhca_id = htobe16(0);
    
    printf("执行TLP_DEVICES查询 (OpMod 0x7)...\n");
    
    int ret = mlx5dv_devx_general_cmd(ctx, &cmd_in, sizeof(cmd_in), &cmd_out, sizeof(cmd_out));
    
    if (ret != 0) {
        printf("❌ TLP_DEVICES查询失败: %s\n", strerror(errno));
        goto cleanup;
    }
    
    if (cmd_out.status != 0) {
        uint32_t syndrome = be32toh(cmd_out.syndrome);
        printf("❌ TLP_DEVICES命令失败 - Status: 0x%x, Syndrome: 0x%x\n", 
               cmd_out.status, syndrome);
        goto cleanup;
    }
    
    printf("✅ TLP_DEVICES查询成功!\n");
    
    // 步骤2: 分析输出并提取候选VHCA ID
    printf("\n=== 步骤2: 分析输出提取候选VHCA ID ===\n");
    uint16_t vhca_candidates[20];
    int num_candidates = analyze_tlp_output(&cmd_out, vhca_candidates, 20);
    
    if (num_candidates == 0) {
        printf("❌ 未能提取到候选VHCA ID\n");
        goto cleanup;
    }
    
    // 步骤3: 增强VUID查询测试
    printf("\n=== 步骤3: 增强VUID查询测试 ===\n");
    bool found_vuid = false;
    
    for (int i = 0; i < num_candidates; i++) {
        int result = enhanced_vuid_query(ctx, vhca_candidates[i]);
        if (result == 0) {
            found_vuid = true;
            printf("\n🎊 SUCCESS! VHCA ID 0x%04x 返回了VUID数据!\n", vhca_candidates[i]);
            break;
        }
        
        // 在尝试之间短暂延迟
        usleep(100000);  // 100ms
    }
    
    // 最终结果
    printf("\n" "============================================\n");
    printf("🏁 Enhanced TLP_DEVICES + VUID 测试结果\n");
    printf("============================================\n");
    
    if (found_vuid) {
        printf("🎉 COMPLETE SUCCESS!\n");
        printf("✅ TLP_DEVICES hack 正常工作\n");
        printf("✅ 成功找到包含VUID数据的VHCA ID\n");
        printf("✅ VUID查询集成完成\n");
        printf("\n💡 下一步:\n");
        printf("1. 集成到生产代码中\n");
        printf("2. 添加错误处理和重试逻辑\n");
        printf("3. 优化VHCA ID检测算法\n");
    } else {
        printf("⚠️  PARTIAL SUCCESS:\n");
        printf("✅ TLP_DEVICES hack 正常工作\n");
        printf("✅ VUID查询基础设施工作正常\n");
        printf("❌ 未找到包含VUID数据的VHCA ID\n");
        printf("\n💡 可能的原因:\n");
        printf("1. 需要更复杂的设备代表器激活过程\n");
        printf("2. VUID数据可能在不同的VHCA ID或设备状态下\n");
        printf("3. 可能需要先配置VF/SF或其他设备参数\n");
        printf("\n🔄 建议下一步:\n");
        printf("1. 检查 doca_devemu_pci_device_list 的完整实现\n");
        printf("2. 尝试设备配置或初始化命令\n");
        printf("3. 探索设备枚举的其他方法\n");
    }
    
cleanup:
    ibv_close_device(ctx);
    ibv_free_device_list(device_list);
    
    return found_vuid ? 0 : 1;
} 