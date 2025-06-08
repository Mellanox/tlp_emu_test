#
# SPDX-License-Identifier: LicenseRef-NvidiaProprietary
# Copyright (c) 2025 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
#

project('tlp_query', 'c')

sample_dependencies = [
    dependency('libibverbs'),
    dependency('libmlx5'),
]

# 原始generic emu查询程序
executable(
    'tlp_query_test',
    'tlp_query_test.c',
    dependencies: sample_dependencies,
    install: true,
)

# 增强版TLP_DEVICES + VUID集成测试程序
executable(
    'tlp_devices_enhanced',
    'tlp_devices_enhanced.c',
    dependencies: sample_dependencies,
    install: true,
)

# 设备配置助手程序
executable(
    'device_config_helper',
    'device_config_helper.c',
    dependencies: sample_dependencies,
    install: true,
)

# 可选：如果还有其他测试程序，也可以添加
if get_option('build_all_tests').enabled()
    # 其他实验性测试程序可以在这里添加
endif 