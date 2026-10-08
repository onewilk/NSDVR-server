# NSDVR 扩展：Mac 主机构建

[English](README.md) | **简体中文**

这里用 clang 在 macOS 上编译 **和 sysmodule 完全相同** 的扩展代码（`../sysmodule/source/next/*.c`）和同一份
libopus（`../third_party/opus`，1.6.1 官方 tarball，配置在 `../third_party/opus_nx`），产出：

| 产物 | 用途 |
|---|---|
| `build/sysdvr_hostmock` | 模拟 Switch 上的 SysDVR（TCP Bridge 协议 03 + NSDVR 扩展），给客户端做端到端测试 |
| `build/sysdvr_hostclient` | 测试客户端：握手、校验每个包、解码所有音频编码、按计划发控制消息、汇总诊断包 |
| `build/next_tests` | 单元测试 + 模糊测试（ASan + UBSan） |
| `build/next_bench` | 编码耗时、libopus 状态/伪栈大小、线程栈用量 |
| `build/next_vectors` | 生成共享测试向量（写到客户端仓库的 `tools/ext_vectors/`） |

协议文档：客户端仓库 [NSDVR](https://github.com/onewilk/NSDVR) 的 `docs/nsdvr-ext-protocol.zh-CN.md`。

下面的默认路径假设客户端仓库克隆在本仓库旁边（`../NSDVR`），可以用 `CLIENT_REPO=...` 覆盖。

## 编译与测试

```sh
cd NSDVR-server/host
make -j10            # 全部
make test            # 单元/模糊测试（需要一个 48 kHz WAV，默认路径见 Makefile 的 TEST_WAV）
make bench           # 耗时与内存测量
make demo-h264       # 用 tools/make_demo_h264.swift 生成 build/demo.h264（10 s 1280x720@30）
make selftest        # 端到端：9 个场景（端口 29911/29922，不影响默认端口上的 hostmock）
make vectors         # 重新生成测试向量
python3 tools/verify_vectors.py <ext_vectors 目录> <audio_codec_eval.py>   # 需要 numpy：用参考实现复核向量
```

### 测试音频

`TEST_WAV` 可以是任意 48 kHz 16 位 WAV。测试向量和下面的测量用的是 Wikimedia Commons 上的
[Raspberrymusic – Aliens](https://commons.wikimedia.org/wiki/File:Raspberrymusic_-_Aliens_(trailer_music;_cinematic_epic_electronic_classical_music).flac)
（CC BY 3.0），转成 48 kHz 16 位立体声，放在客户端仓库的 `audio_eval/src/aliens_48k.wav`（该目录不入库）。

## sysdvr_hostmock 用法

```sh
./build/sysdvr_hostmock --h264 build/demo.h264 \
    --wav ../../NSDVR/audio_eval/src/aliens_48k.wav
```

默认监听 `0.0.0.0:9911`（视频）和 `0.0.0.0:9922`（音频），每 2 s 往 `255.255.255.255,127.0.0.1` 的 UDP 19999
发 `SysDVR|6.3|03|XAW00000000000` 信标。常用参数：

| 参数 | 说明 |
|---|---|
| `--h264 FILE` | Annex-B H.264，按 `--fps`（默认 30）循环发送；每个 VCL NAL 一个包，前面的 SPS/PPS/SEI 并进去（和 grc 一样一包一帧）。不给则发不可解码的合成帧 |
| `--wav FILE` | 48 kHz 16 位 WAV（单声道会复制成立体声），按实时节奏每块 1024 采样读取、循环 |
| `--listen ADDR` | 监听地址（例如 `--listen 192.168.1.10` 只给局域网网卡） |
| `--video-port N` / `--audio-port N` | 端口（默认 9911 / 9922） |
| `--stall-every SEC --stall-ms MS` | 每 SEC 秒让一次**视频发送**卡住 MS 毫秒（默认 300 ms）。卡顿计入 `videoSendBlock*`；虚拟 grc 照常出帧，读取落后超过 `--grc-queue`（默认 3 帧）时丢掉旧帧，于是出现 grc 断档，`gapsAfterSlowSend` 随之增加 |
| `--stall-audio-every SEC --stall-audio-ms MS` | 同上，作用于音频发送 |
| `--drop-every N` | 在“源头”每 N 帧丢一个非 IDR 帧：有断档但前一次发送不慢（`gapsAfterSlowSend` 不增加），用来区分两种丢帧原因 |
| `--grc-queue N` | 读取落后时虚拟 grc 保留的帧数 |
| `--sndbuf BYTES` | 客户端 socket 的 SO_SNDBUF（模拟 Switch 较小的发送缓冲） |
| `--no-opus` | 模拟 Opus 内存不可用：请求 Opus 时回落到 PCM48（标记 0xE0） |
| `--no-beacon` / `--beacon-targets LIST` / `--serial S` | 信标控制 |
| `--once` | 每个端口服务一个连接后退出 |
| `--duration SEC` | SEC 秒后退出 |
| `--quiet-diag` | 不打印每个诊断包 |

hostmock 会打印握手内容（是否带扩展、初始编码参数）、每次音频编码切换（从实际发出的包里读出）、
每个诊断包的内容，以及断开时虚拟 grc 丢了多少帧。

### 行为要点（和 sysmodule 一致）

- 握手：连上后先发 10 字节 `SysDVR|03\0`，收 16 字节请求，按官方规则校验（magic、版本、通道、AudioBatching ≤ 5），
  回 72 字节响应（请求了 MemoryDiag 时填一组假的内存池数据），然后等 500 ms 开始推流。
- 没有扩展标志（FeatureFlags bit 2）的客户端：走官方循环，音频是原始 PCM、`ReplaySlot = 0xFF`，没有诊断包。
- 有扩展标志：运行 `next_session.c`（sysmodule 用的同一份代码）：
  - 音频包 `ReplaySlot = 0xE0 | codec`、`MetaData = 0x06`，压缩编码带 12 字节 `ExtAudioHeader`（`encodeUs` 是实测值）。
  - 音频 socket 上的控制消息每个包之前非阻塞读一次，下一个包生效；编码切换会丢掉 Opus 未满一帧的尾巴（< 20 ms）。
  - 视频路请求了诊断时，每秒一个诊断包：**`MetaData = 0x07`**（类型位 3 + Data 位 0x04）、`ReplaySlot = 0xFF`、
    `Timestamp` = 最近一帧视频的时间戳、负载 56 字节 ExtDiag v1。客户端分发器请先判断 `(MetaData & 3) == 3`。
  - grc 采集失败的错误包保持官方格式（没有 ExtAudioHeader，`ReplaySlot = 0xFF`）。
  - 诊断里的 `core3IdlePermille` 在 Mac 上是本机 3 号 CPU 的空闲千分比，`sysdvrCpuPermille` 是整个 hostmock 进程的 CPU 占用。
- Opus：libopus 1.6.1 定点、`OPUS_APPLICATION_RESTRICTED_CELT`（只用 CELT）、恒定码率、立体声 48 kHz，
  每帧长度恰好 `kbps × 帧采样数 / 384` 字节；编码器延迟 120 采样（2.5 ms）。

## sysdvr_hostclient 用法

```sh
./build/sysdvr_hostclient --seconds 10 --codec opus --kbps 96 --complexity 5 \
    --ctrl "3:adpcm,5:pcm24,7:opus:64:10:10"      # 时间:编码[:kbps[:复杂度[:帧长ms]]]
./build/sysdvr_hostclient --legacy                # 官方协议（不带扩展标志）
./build/sysdvr_hostclient --fuzz-ctrl             # 在音频 socket 上持续发垃圾字节/截断/非法控制消息
./build/sysdvr_hostclient --dump-wav out.wav      # 解码后的音频（PCM24 简单重复采样到 48 kHz，只供试听）
```

检查项：包头 magic/大小、类型位、音频标记与 ExtAudioHeader 一致性、Opus CBR 帧长与帧数、ADPCM 每包状态头与
上一包解码结束状态一致、音频时间戳连续（同一编码内偏差 > 25 ms 算违规）、诊断包间隔、音频不静音。
`--expect-*` 参数用于自动化断言（见 `tests/selftest.sh`）。退出码 0 = 通过。

## 测量结果（本机 Apple M4，`make bench`）

见 `build/bench.txt`。要点（2026-09-30）：

- libopus 编码器状态（RESTRICTED_CELT、立体声、定点）：10380 字节（静态缓冲 10496）。
- 伪栈（NONTHREADSAFE_PSEUDOSTACK）最坏情况 41532 字节（20 ms、复杂度 ≥ 8；10 ms 帧最多 23660），
  配置 43008 字节。测法：在 libopus 自带的伪栈越界检查（ENABLE_HARDENING）下对 2 种帧长 × 11 档复杂度 × 4 种码率
  × 4 种信号二分查找最小可用上限。
- 编码耗时（每秒音频的 CPU 毫秒，AudioBatching 3）：PCM24 0.30，ADPCM 0.40，
  Opus 96k/20ms：c0 1.5、c5 2.7、c10 3.9；256k/20ms/c10 4.9。

## sysmodule 侧的内存/栈（devkitA64 GCC 15.2，`../build_nsdvr_docker.sh`）

- 与官方 804fd36 相比（`out/SIZES.txt`）：`.text` 67008 → 181760，`.rodata` 1448 → 20464，`.eh_frame` 12424 → 13920，
  `.data` 784 → 792，`.bss` 909288 → 938888。按页对齐的三个段合计 1003520 → 1171456 字节，**进程内存 +164 KB**。
- `.bss` 增量来自两处：`StaticBuffers` 联合体里新增的 `TcpMode`（Opus 状态 10496 + 伪栈 43008 + 帧缓冲 640 字节，
  和 RTSP 模式共用内存，联合体 28 KB → 56 KB）以及 `APkt` 前面的 3840 字节余量（就地编码用）。
- 音频线程栈仍是 0x2000：用 `-fcallgraph-info=su` 做的静态分析（`tools/stackcheck.sh` + `tools/maxstack.py`）
  最坏调用链 3824 字节（NextSession_Audio → … → celt_encode_with_ec → quant_all_bands → quant_partition → alg_quant），
  再加 quant_partition 最多 4 层递归（每层 240 字节）约 4.8 KB；没有 VLA/alloca（伪栈替代了它们）。
