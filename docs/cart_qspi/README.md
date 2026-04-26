# GBA Cart QSPI Bridge — Protocol Spec

通用 16-bit 异步并行总线 ↔ QSPI 桥接协议。host 端是 ESP32-P4，桥接器是 FPGA，
被桥接对象是 GBA 卡带总线（含烧录卡的 bank-switch / 控制寄存器）。

## 设计立场

本协议**不假设被桥接的设备是什么**。FPGA 只做总线 strobe 翻译，所有"这是 ROM"
"这是 EEPROM"等语义由 host 决定。这样的好处：

- SuperCard / EZ-Flash / EverDrive 等带魔法寄存器、bank 切换的烧录卡可以无缝支持
- 不可知的新卡带协议，host 可用 `BUS_CYCLES` 命令直接编排原始总线周期
- FPGA gateware 不必随每种卡带迭代

## 文档索引

| 文档 | 内容 |
|---|---|
| [01-overview.md](01-overview.md) | 总体架构、信号、层次、抽象模型 |
| [02-protocol.md](02-protocol.md) | QSPI 物理层、帧格式、命令集 |
| [03-timing.md](03-timing.md) | 时序参数（waitstate）模型、配置寄存器、N/S 周期定义 |
| [04-regions.md](04-regions.md) | Region 属性系统、透明性保证、缓存/预取策略协商 |

## 状态

v0.2 草案，2026-04。
