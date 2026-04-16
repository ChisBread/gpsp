# gpSP ESP32-P4 联机原理与流程

## 概述

ESP32-P4 上的 gpSP 作为 **TCP 客户端**连接到运行 gpSP 核心的 RetroArch 主机。
通信使用 RetroArch 的 **Netplay Core Packet Interface** 协议（`NETPLAY_MODUS_CORE_PACKET_INTERFACE`），
该模式下 RetroArch 不参与帧同步、存档传输、输入同步等逻辑，所有游戏数据交换
完全由 gpSP 核心自己通过 `NETPLAY_CMD_NETPACKET` 命令完成。

## 架构

```
┌─────────────────────┐        TCP         ┌──────────────────────────────┐
│  ESP32-P4  (gpSP)   │◄──────────────────►│  RetroArch Host  (gpSP核心) │
│  netpacket_bridge.c │    port 55435      │  libretro netpacket iface   │
│                     │                     │                              │
│  netpacket_send()   │─── CMD_NETPACKET──►│  server relay ──► 其他客户端 │
│  netpacket_poll_receive()◄─CMD_NETPACKET─│                              │
└─────────────────────┘                     └──────────────────────────────┘
```

- **主机（Host）**：运行 RetroArch + gpSP 核心的设备，启动 Netplay Host 模式
- **从机（Client）**：ESP32-P4 设备，通过 TCP 连接到主机
- **client_id**：由 RetroArch 服务端分配（1-based），主机自身为 0

## TCP 握手流程

ESP32-P4 作为客户端连接到 RetroArch 主机后执行以下握手序列：

### 第 1 步：双向交换 Header（24 字节，6 × uint32_t BE）

```
偏移  字段              客户端发送值                    说明
─────────────────────────────────────────────────────────────────
 0    magic             0x52414E50 ("RANP")            协议标识
 4    platform_magic    按平台计算                      字节序+类型大小
 8    compression       0x00000000                     不支持压缩
12    hi_protocol       0x00000007 (HIGH_PROTOCOL=7)   客户端支持的最高版本
16    protocol          0x00000000                     请求协商
20    impl_magic        按版本字符串计算                 实现版本哈希
```

客户端发送后，等待接收服务端的 Header：
- `header[3]`：如果非零，说明需要密码（salt）
- `header[4]`：服务端选定的协议版本

### 第 2 步：双向交换 NICK（40 字节）

```
字节0-3:  cmd  = 0x00000020 (NETPLAY_CMD_NICK)
字节4-7:  size = 0x00000020 (32)
字节8-39: nick[32] (null-padded ASCII)
```

客户端发送自己的昵称，接收服务端的昵称。

### 第 3 步：双向交换 INFO（76 字节）

```
字节0-3:   cmd         = 0x00000022 (NETPLAY_CMD_INFO)
字节4-7:   size        = 0x00000044 (68)
字节8-11:  content_crc = ROM的CRC32
字节12-43: core_name[32]    = "gpSP"
字节44-75: core_version[32] = "gpSP v1.0" (GPSP_NETPACKET_VERSION)
```

服务端验证 `core_name` 和 `core_version` 是否匹配。

### 第 4 步：接收 SYNC

服务端发送 SYNC 命令，关键数据：
- `client_num`：分配给我们的 client_id（1-based）
- 16 × device config（uint32_t）
- 16 × share mode（uint8_t）
- 16 × device-client mapping（uint32_t）
- 32 字节 nick
- SRAM：**在 Core Packet Interface 模式下为 0 字节**

SYNC 之后可能还有 protocol v6+ 的 SETTING 命令，需要读取并忽略。

### 第 5 步：发送 PLAY 请求

```
字节0-3: cmd  = 0x00000025 (NETPLAY_CMD_PLAY)
字节4-7: size = 0x00000004
字节8-11: mode = 0x00000000 (无设备请求，无slave)
```

服务端以 `NETPLAY_CMD_MODE` 响应。

### 握手后状态

握手完成后进入数据交换阶段：
- `netplay_client_id` = 服务端分配的 client_num
- `netplay_num_clients` = 通过 connected/disconnected 事件跟踪

## 数据包交换（NETPLAY_CMD_NETPACKET）

握手后所有 gpSP 游戏数据通过此命令收发：

### 发送格式（客户端 → 服务端）

```
字节0-3:   cmd       = 0x00000048 (NETPLAY_CMD_NETPACKET)
字节4-7:   size      = payload长度（不含此12字节头）
字节8-11:  client_id = 目标：0=发给主机, N=发给client N, 0xFFFF=广播
字节12+:   payload   = gpSP序列化的游戏数据
```

### 接收格式（服务端 → 客户端）

```
字节0-3:   cmd       = 0x00000048
字节4-7:   size      = payload长度
字节8-11:  client_id = 发送者的 client_id
字节12+:   payload   = gpSP游戏数据
```

**注意**：
- 客户端发送时 `client_id` 字段是**目标**
- 客户端接收时 `client_id` 字段是**来源**
- 服务端负责路由和转发，客户端不需要知道其他客户端的网络地址
- `0xFFFF` 广播时，服务端会转发给除发送者之外的所有 PLAYING 状态客户端

## 运行时命令处理

除了 NETPACKET，客户端还需处理（或忽略）以下运行时命令：

| 命令 | 代码 | 处理方式 |
|------|------|----------|
| `NETPLAY_CMD_PING_REQUEST` | 0x1100 | 回复 `PING_RESPONSE` |
| `NETPLAY_CMD_PING_RESPONSE` | 0x1101 | 更新延迟统计 |
| `NETPLAY_CMD_MODE` | 0x0026 | 跟踪自身/他人的连接状态变化 |
| `NETPLAY_CMD_MODE_REFUSED` | 0x0027 | PLAY请求被拒绝 |
| `NETPLAY_CMD_DISCONNECT` | 0x000A | 对端断开 |
| `NETPLAY_CMD_SETTING_*` | 0x2000+ | 忽略 |

## gpSP 内部数据流

```
rfu.c / serial_proto.c
  │
  ├─ netpacket_send(client_id, buf, len)   ← 发送游戏数据
  │     └─ 封装为 CMD_NETPACKET → TCP socket → RetroArch Host
  │
  └─ netpacket_poll_receive()              ← 每帧调用
        └─ 从 TCP socket 读取 → 解析 CMD_NETPACKET
             └─ rfu_net_receive(buf, len, sender_client_id)
                serial{poke,aw}_net_receive(buf, len, sender_client_id)
```

## 配置项

在 `gpsp.cfg` 中：

```
netplay_ra_host=192.168.1.243    # RetroArch 主机 IP
netplay_ra_port=55435            # RetroArch 主机端口
netplay_ra_nick=ESP32-P4         # 本机昵称
```

## 操作步骤

### 主机端（RetroArch）
1. 加载 gpSP 核心和 GBA ROM
2. 进入 **Netplay → Start netplay host**
3. 确认端口为 55435，等待连接

### 从机端（ESP32-P4）
1. 加载相同的 GBA ROM
2. 确保 WiFi 已连接（通过 ESP32-C6 桥接）
3. 在 `gpsp.cfg` 中配置 `netplay_ra_host` 和 `netplay_ra_port`
4. ESP32-P4 自动尝试连接，握手成功后开始联机游戏
