# gpSP ESP32-P4 联机原理与流程

## 概述

ESP32-P4 上的 gpSP 支持三种联机模式，通信均使用 RetroArch 的
**Netplay Core Packet Interface** 协议（`NETPLAY_MODUS_CORE_PACKET_INTERFACE`），
该模式下 RetroArch 不参与帧同步、存档传输、输入同步等逻辑，所有游戏数据交换
完全由 gpSP 核心自己通过 `NETPLAY_CMD_NETPACKET` 命令完成。

### 联机模式

| 模式 | `netplay_ra_mode` | 说明 |
|------|-------------------|------|
| 禁用 | `0` (DISABLED)    | 不启用联机 |
| 客户端 | `1` (CLIENT)    | 连接到 RetroArch 主机（传统模式） |
| 主机 | `2` (HOST)        | ESP32-P4 作为 TCP 服务端，接收最多 3 个客户端 |
| 隧道客户端 | `3` (TUNNEL_CLIENT) | 通过 RetroArch 中继隧道连接到主机 |

## 架构

### 模式 1：CLIENT — 连接到 RetroArch 主机

```
┌──────────────────────────┐     TCP      ┌──────────────────────────────┐
│  ESP32-P4  (gpSP)        │◄────────────►│  RetroArch Host  (gpSP核心) │
│  netpacket_bridge_ra.c   │  port 55435  │  libretro netpacket iface   │
│                          │              │                              │
│  netpacket_send()        │──NETPACKET──►│  server relay ──► 其他客户端 │
│  netpacket_poll_receive()│◄─NETPACKET──│                              │
└──────────────────────────┘              └──────────────────────────────┘
```

### 模式 2：HOST — ESP32-P4 作为服务端

```
┌──────────────────────────┐
│  ESP32-P4  HOST (gpSP)   │ listen on port 55435
│  netpacket_host.c        │
│  client_id = 0           │
│                          │◄─────── TCP ──── Client 1 (RA/ESP32-P4)
│  netpacket_host_poll()   │◄─────── TCP ──── Client 2
│  netpacket_host_send()   │◄─────── TCP ──── Client 3  (max 3)
└──────────────────────────┘
```

HOST 模式下 ESP32-P4 自身为 `client_id=0`，最多接受 `HOST_MAX_CLIENTS=3`
个客户端（host + 3 = 4 玩家，对应 GBA RFU 最大 4 人联机）。

### 模式 3：TUNNEL_CLIENT — 通过中继隧道

```
┌──────────────────────────┐     TCP      ┌─────────────┐     TCP      ┌──────────┐
│  ESP32-P4  (gpSP)        │◄────────────►│  RA Relay    │◄────────────►│  RA Host │
│  netpacket_bridge_ra.c   │              │  (netplay    │              │          │
│  sends RATS + session_id │              │   tunnel)    │              │          │
└──────────────────────────┘              └─────────────┘              └──────────┘
```

隧道模式连接时先发送 `RATS` + 12 字节 `session_id`（从 `tunnel_id` 解析），
然后继续普通 RANP 握手流程。

## 源文件

| 文件 | 作用 |
|------|------|
| `main/netpacket_bridge_ra.c` | 客户端及隧道客户端 TCP 连接、握手、收发；HOST 模式的调度入口 |
| `main/netpacket_host.c` | HOST 模式的 TCP 服务端实现（accept、握手、路由、PING） |
| `main/runtime_config.h` | 模式常量定义、配置变量声明 |
| `main/runtime_config.c` | 配置项解析（`gpsp.cfg`）、保存 |

## TCP 握手流程

CLIENT / TUNNEL_CLIENT 作为客户端连接到主机后，以及 HOST 模式接受新客户端后，
均执行以下握手序列（HOST 端逆向执行对应步骤）：

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
- `header[4]`：服务端选定的协议版本（≥5，≤7）

### 第 2 步：双向交换 NICK（40 字节）

```
字节0-3:  cmd  = 0x00000020 (NETPLAY_CMD_NICK)
字节4-7:  size = 0x00000020 (32)
字节8-39: nick[32] (null-padded ASCII)
```

### 第 3 步：双向交换 INFO（76 字节）

```
字节0-3:   cmd         = 0x00000022 (NETPLAY_CMD_INFO)
字节4-7:   size        = 0x00000044 (68)
字节8-11:  content_crc = ROM的CRC32
字节12-43: core_name[32]    = "gpSP"
字节44-75: core_version[32] = GPSP_NETPACKET_VERSION
```

### 第 4 步：服务端发送 SYNC

SYNC 包含：
- `client_num`：分配给客户端的 client_id（1-based）
- 16 × device config、16 × share mode、16 × device-client mapping
- 32 字节 nick
- SRAM：**在 Core Packet Interface 模式下为 0 字节**

Protocol v6+ 还会附带 `SETTING_ALLOW_PAUSE` 和 `SETTING_INPUT_LATENCY` 命令。

### 第 5 步：客户端发送 PLAY 请求

```
字节0-3: cmd  = 0x00000025 (NETPLAY_CMD_PLAY)
字节4-7: size = 0x00000004
字节8-11: mode = 0x00000000
```

服务端以 `NETPLAY_CMD_MODE` 响应，包含 `YOU(bit31) | PLAYING(bit30) | client_num`。

### 握手后状态

- `netplay_client_id` = 服务端分配的 `client_num`（HOST 模式自身始终为 0）
- `netplay_num_clients` = 通过 MODE 事件或 `host_count_connected()` 实时跟踪

## 数据包交换（NETPLAY_CMD_NETPACKET）

握手后所有 gpSP 游戏数据通过此命令收发：

### 发送格式

```
字节0-3:   cmd       = 0x00000048 (NETPLAY_CMD_NETPACKET)
字节4-7:   size      = payload长度
字节8-11:  client_id = 目标：0=发给主机, N=发给client N, 0xFFFF=广播
字节12+:   payload   = gpSP序列化的游戏数据（最大 2048 字节）
```

### 接收格式

```
字节0-3:   cmd       = 0x00000048
字节4-7:   size      = payload长度
字节8-11:  client_id = 发送者的 client_id
字节12+:   payload   = gpSP游戏数据
```

**注意**：
- 发送时 `client_id` 字段是**目标**，接收时是**来源**
- HOST 模式下发送 `client_id=0`（host 自身），由 `netpacket_host_send()` 路由到指定客户端或广播
- `0xFFFF` 广播时转发给除发送者之外的所有 PLAYING 状态客户端

## 运行时命令处理

| 命令 | 代码 | 处理方式 |
|------|------|----------|
| `NETPLAY_CMD_PING_REQUEST` | 0x1100 | 回复 `PING_RESPONSE`（HOST 每 5 秒向所有客户端主动发 PING） |
| `NETPLAY_CMD_PING_RESPONSE` | 0x1101 | 更新延迟统计 |
| `NETPLAY_CMD_MODE` | 0x0026 | 跟踪自身/他人的连接状态变化 |
| `NETPLAY_CMD_MODE_REFUSED` | 0x0027 | PLAY 请求被拒绝 |
| `NETPLAY_CMD_DISCONNECT` | 0x000A | 对端断开；HOST 模式下客户端断开后如无剩余客户端则重置串口协议 |
| `NETPLAY_CMD_SETTING_*` | 0x2000+ | 忽略 |

## gpSP 内部数据流

```
rfu.c / serial_proto.c
  │
  ├─ netpacket_send(client_id, buf, len)
  │     │
  │     ├─ [CLIENT / TUNNEL_CLIENT]
  │     │     └─ 封装为 CMD_NETPACKET → TCP socket → RA Host / Relay
  │     │
  │     └─ [HOST]
  │           └─ netpacket_host_send(client_id, buf, len)
  │                 └─ 封装为 CMD_NETPACKET → TCP socket(s) → 指定客户端或广播
  │
  └─ netpacket_poll_receive()              ← 每帧调用
        │
        ├─ [CLIENT / TUNNEL_CLIENT]
        │     └─ 从 TCP socket 读取 → 解析 CMD_NETPACKET → dispatch
        │
        └─ [HOST]
              └─ netpacket_host_poll()
                    ├─ accept 新连接 → 握手状态机
                    ├─ recv 已连接客户端数据 → 解析 CMD_NETPACKET → dispatch
                    └─ 周期性 PING + 流量统计
                          │
                          └─ dispatch:
                               rfu_net_receive(buf, len, sender_client_id)
                               serialpoke_net_receive(buf, len, sender_client_id)
                               serialaw_net_receive(buf, len, sender_client_id)
```

## HOST 模式客户端状态机

```
EMPTY → WAIT_HEADER → WAIT_NICK → WAIT_INFO → WAIT_PLAY → CONNECTED
                                                              │
                                                   host_process_commands()
                                                              │
                                                         DISCONNECT
                                                              │
                                                           EMPTY
```

每个客户端由 `host_client_t` 结构管理，包含 fd、状态、协议版本、
`assigned_id`（1-based）、昵称、4KB 接收缓冲区和流量统计计数器。
客户端结构体数组 `host_clients[3]` 分配在 PSRAM（`GPSP_EXTRAM_BSS`）。

## 配置项

在 `gpsp.cfg` 中：

```ini
netplay_ra_mode=0                # 0=禁用, 1=客户端, 2=主机, 3=隧道客户端
netplay_ra_host=192.168.1.243    # RA 主机/隧道服务器 IP（模式1、3使用）
netplay_ra_port=55435            # TCP 端口（模式1、3为目标端口；模式2为监听端口）
netplay_ra_nick=ESP32-P4         # 本机昵称（最长31字符）
netplay_ra_tunnel_id=...         # 24位十六进制隧道会话ID（仅模式3使用）
```

向后兼容：旧配置 `netplay_enable=1` 等同于 `netplay_ra_mode=1`。

以上配置也可通过 Web 管理界面在线修改。

## 操作步骤

### 模式 1：客户端 → RetroArch 主机

**主机端（RetroArch）**：
1. 加载 gpSP 核心和 GBA ROM
2. 进入 **Netplay → Start netplay host**
3. 确认端口为 55435，等待连接

**从机端（ESP32-P4）**：
1. 加载相同的 GBA ROM
2. 确保 WiFi 已通过 ESP32-C6 桥接连接
3. 设置 `netplay_ra_mode=1`，配置 `netplay_ra_host` 和 `netplay_ra_port`
4. ESP32-P4 自动连接，握手成功后开始联机

### 模式 2：ESP32-P4 作为主机

1. 加载 GBA ROM
2. 设置 `netplay_ra_mode=2`，确认 `netplay_ra_port`（默认 55435）
3. ESP32-P4 开始监听 TCP 端口
4. 其他设备（RetroArch 或其他 ESP32-P4）以客户端身份连接到该 IP:端口
5. 最多支持 3 个客户端同时连接（host + 3 = 4 玩家）

### 模式 3：隧道客户端

1. 加载相同的 GBA ROM
2. 设置 `netplay_ra_mode=3`
3. 配置 `netplay_ra_host` 为隧道中继服务器地址（RetroArch 默认中继：`lobby.libretro.com`）
4. 配置 `netplay_ra_tunnel_id` 为 24 位十六进制隧道会话 ID
5. ESP32-P4 连接中继后发送 `RATS` + session_id，由中继路由到目标主机

## FAQ

### 如何获取 tunnel_id？

`tunnel_id` 是 RetroArch 中继服务器为每个 netplay 房间分配的 12 字节会话标识，
以 24 位十六进制字符串表示。获取方式：

1. **RetroArch 大厅页面**：主机启用隧道模式创建房间后，在 RetroArch 的
   **Netplay → Netplay Lobby** 中可以看到房间信息，或通过
   `http://lobby.libretro.com/list/` API 查询房间列表，
   每个房间的 JSON 中包含 `"host_method": 3`（隧道模式）及对应的会话 ID
2. **主机端日志**：RetroArch 创建隧道房间时会在日志中输出 session ID
3. **手动传递**：主机创建房间后将 tunnel_id 通过其他渠道（聊天、网页等）告知客户端

> 注意：只有主机选择了 **Use Relay Server** 模式创建的房间才有 tunnel_id。
> 主机直连（端口转发）模式下应使用 `netplay_ra_mode=1`（CLIENT）直接连主机 IP。

### 如何确定自己是主机还是客户端？

这取决于你的联机场景：

| 场景 | ESP32-P4 的 `netplay_ra_mode` | 说明 |
|------|-------------------------------|------|
| **一台 ESP32-P4 + 一台 RetroArch PC** | `1` (CLIENT) | RetroArch 做主机，ESP32-P4 做客户端连入 |
| **多台 ESP32-P4 对战（无 PC）** | 其中一台 `2` (HOST)，其余 `1` (CLIENT) | 一台 ESP32-P4 当主机，其他连入它 |
| **通过互联网中继连接远程 RA 主机** | `3` (TUNNEL_CLIENT) | 远程主机在 RA 创建隧道房间，ESP32-P4 通过中继连入 |

简单总结：
- **谁开房谁是主机**。如果对方在 RetroArch 上开了 Host，你就用 CLIENT（1）或 TUNNEL_CLIENT（3）
- **多台 ESP32-P4 之间**没有 RetroArch 时，需要选一台设为 HOST（2），其余设为 CLIENT（1）连它的 IP
