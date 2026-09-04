# OMS 33a7:2388 Linux fingerprint driver

本项目为机械革命无界 15 Pro 内置的江西欧迈斯（OMS/O-Film）
`33a7:2388` Match-on-Chip 指纹模块实现 Linux 支持。

当前已经有可编译的 libfprint 驱动和独立 libusb 诊断工具。2026-08-22
在真实硬件上验证了：打开/关闭、模板列表、6 阶段录入、识别、取消、指定删除、
清空全部模板、fprintd 式“先识别再录入”的重复指纹拒绝，以及操作后重新打开
设备。测试在默认 USB 自动
电源管理下通过。清空测试删除了原 Windows 模板，随后重新录入了 Linux 测试
模板；当前设备只保留槽位 0，共 1 枚模板。

固件 `000059` 不会可靠地在重复录入时返回 Windows 驱动处理的 `0x22`：纯协议
测试先确认同一手指匹配槽位 0，随后仍能把它录入槽位 1。驱动保留 `0x22` 的
兼容处理，但不宣称硬件具备原生重复检测；fprintd 会利用已支持的 identify 能力
在启动 enroll 前完成可靠拦截。直接调用 libfprint 的程序也应遵循这一顺序。

`libfprint-oms2388 1.94.100-3` 已作为 Arch 包安装到测试机，包含经过真机验证的
活动识别挂起/恢复支持，以及让 fprintd 常驻并提前持有睡眠延迟锁的 systemd
drop-in。官方
`fprintd 1.94.5-2` 能通过系统 D-Bus 枚举该设备，用户 `zhe` 已通过 Omarchy
官方设置流程录入右手食指，独立验证返回 `verify-match`。sudo、Polkit 和 Omarchy
锁屏 PAM 均已接入，并保留密码回退及合盖时跳过指纹的保护逻辑。真实 s2idle
测试中，按下一体式电源/指纹键会唤醒机器，并在恢复后约 0.16 秒完成指纹解锁；
仅轻触传感器没有在 10 秒观察窗口内唤醒。当前设备只有槽位 0，共 1 枚由
fprintd 管理的模板。

## 硬件与协议

- USB ID：`33a7:2388`
- 接口：vendor diagnostic `dc/a0/b0`
- 端点：bulk OUT `0x01`、bulk IN `0x81`，最大包长 64 字节
- 固件：`acm32_btl_ofilm_pc_000059`
- 存储：设备内模板；协议索引可表达 256 个 ID，Windows 实际限制为 30 个槽位
- 官方 Windows 驱动：OMS 1.0.0.29，UMDF NativeUSB

协议记录见 [docs/protocol.md](docs/protocol.md)，公开的相关逆向项目见
[docs/prior-art.md](docs/prior-art.md)，Windows 重复检测的静态逆向记录见
[docs/windows-duplicate-analysis.md](docs/windows-duplicate-analysis.md)，完整验证矩阵见
[docs/validation.md](docs/validation.md)。

## 构建独立工具

需要 C 编译器、`pkg-config` 和 libusb 开发文件：

```sh
make
make check
./build/omsctl info
```

常用的非破坏性检查：

```sh
./build/omsctl probe-readonly
./build/omsctl identify
```

直接访问 USB 设备需要合适的 udev 权限，或仅在开发阶段以管理员权限运行。
`delete-template N --yes` 是破坏性命令；不要删除仍需保留的模板。项目完全没有
实现固件擦除、下载或更新命令。

## 构建 libfprint 驱动

libfprint 没有稳定的外部驱动插件 ABI，因此目前的扩展方式是：检出固定版本的
libfprint 源码、应用很小的集成补丁，把 `oms2388` 驱动编译进自定义 libfprint。
脚本会固定上游提交，避免上游变化造成不可复现结果：

```sh
./scripts/build-libfprint.sh
make LIBFPRINT_BUILD=build/libfprint-oms-repro build/libfprint-smoke
```

构建会运行 libfprint 的 `fpi-device`、`fpi-ssm`、`fpi-assembling` 测试，并启用
`-Werror`。驱动源码在 [libfprint/oms2388](libfprint/oms2388)，上游接入改动在
[libfprint/integration.patch](libfprint/integration.patch)。长期目标是提交上游；
上游合并后就不再需要自行维护整份构建。

## 构建 Arch/Omarchy 安装包

项目提供 pacman 管理的替代包 `libfprint-oms2388`。它包含全部默认上游驱动和
本项目驱动，提供与 Arch `libfprint 1.94.100` 相同的公共 ABI，并保留官方
`fprintd` 包：

```sh
make arch-package
```

成品写入 `packaging/arch/out/`。构建会启用 udev rules/hwdb、运行四项核心测试，
并检查包名、依赖提供、SONAME、关键文件和 USB ID。安装与回滚说明见
[packaging/arch/README.md](packaging/arch/README.md)。构建包不会自动安装或修改
系统软件。

真机开发测试程序：

```sh
./build/libfprint-smoke
./build/libfprint-smoke --identify
./build/libfprint-smoke --enroll
./build/libfprint-smoke --duplicate-test
./build/libfprint-smoke --cancel-test
./build/libfprint-smoke --suspend-resume-test
./build/libfprint-smoke --enroll-clear-test  # 破坏性测试
```

`--duplicate-test` 模拟 fprintd 的协调逻辑：先 identify，匹配已有模板后不调用
enroll。`omsctl native-duplicate-test` 是会临时创建重复模板再自动清理的底层诊断，
仅用于协议研究，不应作为日常测试。

需要人工触摸时，可用持续提示音包装命令；命令退出后声音自动停止：

```sh
./scripts/with-attention-sound.sh ./build/libfprint-smoke --identify
```

## 安全警告

不要直接在真实硬件上安装附带的官方 Windows 驱动。它默认设置
`EnableUpdate=1`；服务发现设备版本不包含驱动包的
`acm32_btl_ofilm_pc_000057` 时，会自动进入固件更新路径，而本机实际固件是
`000059`。

官方包仅作为静态分析输入，并在 `.gitignore` 中排除。若以后必须进行 Windows
抓包，应先禁用更新路径；当前驱动的核心功能已经不依赖 Windows 虚拟机。

## 尚未覆盖

- 仅轻触、不按下一体式电源键的 USB remote-wakeup；本机实测没有发生
- 完全关机/冷启动时的 OneKey 预匹配结果交接
- 多台设备及其他固件版本的兼容性验证
