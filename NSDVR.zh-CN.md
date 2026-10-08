# NSDVR 实验版服务端

[English](NSDVR.md) | **简体中文**

本仓库是 exelix11 的 [SysDVR](https://github.com/exelix11/SysDVR) 的非官方分支。`nsdvr` 分支包含
[NSDVR](https://github.com/onewilk/NSDVR)（第三方鸿蒙版 SysDVR 客户端）所用实验扩展协议的服务端（sysmodule）部分。

这不是 SysDVR 官方版本，与 SysDVR 项目没有关联。使用这个版本遇到的问题请不要反馈给上游 SysDVR。

> [!WARNING]
> 实验阶段。目前只在主机构建、单元/模糊测试和模拟 Switch 的端到端测试（`host/`）中验证过，devkitA64 能正常编译。
> **还没有在真机上测试过。**

## 新增内容

在 TCP Bridge 协议 03 上做的向后兼容扩展。只有客户端在握手里设置 FeatureFlags 第 2 位时才启用；官方握手会忽略未知的
功能位和 Reserved 字节。不带这个标志的客户端（包括官方 SysDVR 客户端）走原来的官方代码路径，行为不变。

- **音频压缩，随时切换**：PCM 48 kHz、PCM 24 kHz（63 阶半带降采样）、IMA ADPCM、Opus（libopus 1.6.1，定点，只用 CELT，
  恒定码率）。客户端通过音频连接上的 8 字节控制消息切换编码、Opus 码率、复杂度和帧长，不需要重连。
- **诊断数据**：视频通道每秒一个诊断包，包含发送阻塞时间、grc 断档、慢发送后的断档、音频编码耗时、3 号 CPU 核空闲计数、
  SysDVR 线程计数和 IP_TOS 设置结果。
- **IP_TOS 0xA0**（AC_VI），按客户端请求设置。
- **不使用堆内存**：Opus 状态和静态伪栈放在 `StaticBuffers` 联合体里，和 RTSP 模式共用内存。libopus 的加固检查失败时
  `longjmp` 回调用方，而不是调用 `abort()`，所以 libopus 出问题只会让音频降级，不会让 sysmodule 崩溃。
- **内存开销**：比官方版本多约 164 KB 进程内存（明细见 [host/README.zh-CN.md](host/README.zh-CN.md)）。

熄屏相关的行为（[SysDVR#402](https://github.com/exelix11/SysDVR/issues/402)）没有改动，留给上游处理。

## 协议

线上格式见 [NSDVR](https://github.com/onewilk/NSDVR) 仓库的 `docs/nsdvr-ext-protocol.zh-CN.md`
（英文版 `docs/nsdvr-ext-protocol.md`）。这份文档是两个仓库之间的约定；共用的测试向量在该仓库的 `tools/ext_vectors/`。

## 目录

| 路径 | 内容 |
|---|---|
| `sysmodule/source/next/` | 扩展代码（可移植 C，sysmodule 和主机构建共用） |
| `sysmodule/source/modes/TCPnext.c` | 接入 TCP Bridge |
| `third_party/opus/` | 未修改的 libopus 1.6.1 正式版（删掉了部分测试、文档和模型目录） |
| `third_party/opus_nx/` | sysmodule 用的 libopus 配置和构建脚本 |
| `host/` | macOS 构建：模拟 Switch（`sysdvr_hostmock`）、测试客户端、单元/模糊测试、性能测试、测试向量生成器 |
| `build_nsdvr_docker.sh` | 用 Docker 编译本分支和官方基线版本 |

分支：`master` 是上游 SysDVR 的 804fd36，没有改动；`nsdvr` 是扩展。

## 编译

用 Docker（不需要在本机装 devkitPro）：

```sh
./build_nsdvr_docker.sh
```

它用 `devkitpro/devkita64` 镜像编译本分支和官方基线（804fd36），并按 `ReleaseSysmodule.sh` 的方式打包：

- `out/nsdvr/atmosphere/contents/00FF0000A53BB665/`：本分支
- `out/baseline/atmosphere/contents/00FF0000A53BB665/`：官方代码，用来对比
- `out/SIZES.txt`：两个版本的段大小

本机装了 devkitPro 的话，也可以像上游一样直接 `make -C sysmodule`。

## 在 Switch 上试用

仅适合愿意测试实验性 sysmodule 的用户：

1. 先备份 SD 卡上的 `atmosphere/contents/00FF0000A53BB665/`。
2. 用 `out/nsdvr/atmosphere` 覆盖，然后重启。
3. 用 NSDVR 通过 TCP Bridge 连接。官方客户端照常可用。

覆盖前先确认剩余内存：扩展比官方版本多用约 164 KB。要恢复的话，把备份放回去再重启即可。

## 没有 Switch 时的测试

见 [host/README.zh-CN.md](host/README.zh-CN.md)：同一份扩展代码在 macOS 上编译，配有模拟 Switch、测试客户端、
单元和模糊测试（ASan/UBSan）以及端到端自测。

## 许可证

GPL-2.0，与 SysDVR 相同（见 [LICENSE](LICENSE)）。libopus 为 BSD-3-Clause（`third_party/opus/COPYING`）。

SysDVR 本身的功劳归 exelix11 和所有 SysDVR 贡献者。
