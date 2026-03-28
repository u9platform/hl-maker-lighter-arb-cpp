# Performance Baseline

这份文档定义了 C++ 版 HL maker / Lighter taker 策略的性能 baseline，目标是回答两个问题：

1. 实盘 roundtrip 的延迟到底花在了哪些阶段？
2. 每次优化之后，具体是哪一段变快了，快了多少？

## 范围

整条 roundtrip 按五类延迟域拆分：

1. 行情接入
2. 信号与下单决策路径
3. HL maker 提交与挂单生命周期
4. HL maker 成交到 Lighter taker 对冲
5. 失败路径与 unwind 处理

## 记录规则

- 所有延迟指标统一使用 `ms`。
- 优先记录 `mean`、`p50`、`p95`、`p99`、`max`。
- 如果拿不到交易所时间戳，需要明确标注该指标为 `local-only`。
- 每次有新的性能优化落地后，在对应指标下追加一行记录。
- `Optimization Summary` 用一句话概括最直接影响该指标的优化动作。

## 指标定义

### `hl_market_exchange_to_local_rx_ms`

Measures the delay between the exchange-provided HL market-data timestamp and the moment the process receives the corresponding message locally.

指标说明：
- 衡量 HL 行情在交易所产生时间与本地进程收到该消息之间的延迟
- 反映网络传输和交易所推送链路的耗时
- 用来判断 HL 行情是否在进入本地处理前就已经偏晚

| Measurement Time | Env / Build | Sample Size | Mean (ms) | P50 (ms) | P95 (ms) | P99 (ms) | Max (ms) | Optimization Summary | Notes |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| TODO | TODO | TODO | TODO | TODO | TODO | TODO | TODO | TODO | TODO |

### `lighter_market_exchange_to_local_rx_ms`

Measures the delay between the exchange-provided Lighter market-data timestamp and the moment the process receives the corresponding message locally.

指标说明：
- 衡量 Lighter 行情在交易所产生时间与本地进程收到该消息之间的延迟
- 反映 Lighter 的网络传输和推送链路耗时
- 用来判断 Lighter 行情到达是否已经构成前置瓶颈

| Measurement Time | Env / Build | Sample Size | Mean (ms) | P50 (ms) | P95 (ms) | P99 (ms) | Max (ms) | Optimization Summary | Notes |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| TODO | TODO | TODO | TODO | TODO | TODO | TODO | TODO | TODO | TODO |

### `hl_market_local_rx_to_bbo_update_ms`

Measures the local processing delay from HL message receipt to the point where the HL BBO cache/store is updated and available to the strategy.

指标说明：
- 衡量 HL 消息到达本地后，到 HL BBO 缓存更新完成之间的本地处理延迟
- 反映 JSON 解析、加锁和缓存写入的成本
- 用来判断本地 feed 热路径是否存在性能瓶颈

| Measurement Time | Env / Build | Sample Size | Mean (ms) | P50 (ms) | P95 (ms) | P99 (ms) | Max (ms) | Optimization Summary | Notes |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| TODO | TODO | TODO | TODO | TODO | TODO | TODO | TODO | TODO | TODO |

### `lighter_market_local_rx_to_bbo_update_ms`

Measures the local processing delay from Lighter message receipt to the point where the Lighter BBO cache/store is updated and available to the strategy.

指标说明：
- 衡量 Lighter 消息到达本地后，到 Lighter BBO 缓存更新完成之间的本地处理延迟
- 反映 order book 消息解析与增量合并的成本
- 用来判断本地逻辑是否引入了可避免的 feed 延迟

| Measurement Time | Env / Build | Sample Size | Mean (ms) | P50 (ms) | P95 (ms) | P99 (ms) | Max (ms) | Optimization Summary | Notes |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| TODO | TODO | TODO | TODO | TODO | TODO | TODO | TODO | TODO | TODO |

### `cross_venue_alignment_ms`

Measures the age gap between the two latest comparable HL and Lighter quotes used to compute a spread signal.

指标说明：
- 衡量用于计算 spread 的 HL 与 Lighter 两边最新可比报价之间的时间差
- 反映 spread 计算是否基于时间上足够对齐的数据
- 用来判断是否长期存在某一侧行情系统性落后

| Measurement Time | Env / Build | Sample Size | Mean (ms) | P50 (ms) | P95 (ms) | P99 (ms) | Max (ms) | Optimization Summary | Notes |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| TODO | TODO | TODO | TODO | TODO | TODO | TODO | TODO | TODO | TODO |

### `signal_detection_ms`

Measures the time from “both books are updated and comparable” to “spread threshold crossing is detected and a signal is emitted.”

指标说明：
- 衡量“两边最新行情都可比较”到“检测到阈值穿越并发出信号”之间的延迟
- 反映策略计算与信号判定成本
- 用来判断正式准备下单前的决策开销

| Measurement Time | Env / Build | Sample Size | Mean (ms) | P50 (ms) | P95 (ms) | P99 (ms) | Max (ms) | Optimization Summary | Notes |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| TODO | TODO | TODO | TODO | TODO | TODO | TODO | TODO | TODO | TODO |

### `signal_to_hl_maker_send_ms`

Measures the time from signal emission to the moment the HL maker order request is sent.

指标说明：
- 衡量信号产生到 HL maker 请求真正发出之间的延迟
- 反映从决策到执行的本地开销
- 包括风控检查、定价逻辑和订单对象构造成本

| Measurement Time | Env / Build | Sample Size | Mean (ms) | P50 (ms) | P95 (ms) | P99 (ms) | Max (ms) | Optimization Summary | Notes |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| TODO | TODO | TODO | TODO | TODO | TODO | TODO | TODO | TODO | TODO |

### `hl_maker_send_to_ack_ms`

Measures the time from sending the HL maker order request to receiving the HL acknowledgement.

指标说明：
- 衡量 HL maker 请求发出到收到 HL 回执之间的延迟
- 反映 HL 原生签名、网络请求和交易所应答的总成本
- 用来判断下单延迟主要来自本地签名还是交易所 ack

| Measurement Time | Env / Build | Sample Size | Mean (ms) | P50 (ms) | P95 (ms) | P99 (ms) | Max (ms) | Optimization Summary | Notes |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| TODO | TODO | TODO | TODO | TODO | TODO | TODO | TODO | TODO | TODO |

### `hl_maker_ack_to_resting_ms`

Measures the delay between HL maker acknowledgement and the point where the order is confirmed resting on-book, if that signal is available.

指标说明：
- 衡量 HL maker 收到 ack 到确认订单真实挂上盘口之间的延迟
- 反映客户端 ack 与真实上簿之间是否存在空档
- 用来判断是否出现“ack 很快但订单真正生效更晚”的情况

| Measurement Time | Env / Build | Sample Size | Mean (ms) | P50 (ms) | P95 (ms) | P99 (ms) | Max (ms) | Optimization Summary | Notes |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| TODO | TODO | TODO | TODO | TODO | TODO | TODO | TODO | TODO | TODO |

### `hl_maker_resting_lifetime_ms`

Measures how long the HL maker order rests before either fill or cancel.

指标说明：
- 衡量 HL maker 单从挂上去到成交或撤销为止的存活时间
- 反映 maker 暴露在市场中的时间长度
- 用来观察优化是否改变了被动成交机会窗口

| Measurement Time | Env / Build | Sample Size | Mean (ms) | P50 (ms) | P95 (ms) | P99 (ms) | Max (ms) | Optimization Summary | Notes |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| TODO | TODO | TODO | TODO | TODO | TODO | TODO | TODO | TODO | TODO |

### `cancel_trigger_to_hl_cancel_send_ms`

Measures the time from cancel condition trigger (`abs(spread) < SPREAD - X`) to sending the HL cancel request.

指标说明：
- 衡量撤单条件触发到 HL cancel 请求真正发出之间的延迟
- 反映策略对价差回落的反应速度
- 用来判断本地撤单逻辑是否增加了可避免的风险暴露

| Measurement Time | Env / Build | Sample Size | Mean (ms) | P50 (ms) | P95 (ms) | P99 (ms) | Max (ms) | Optimization Summary | Notes |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| TODO | TODO | TODO | TODO | TODO | TODO | TODO | TODO | TODO | TODO |

### `hl_cancel_send_to_ack_ms`

Measures the time from sending the HL cancel request to receiving the HL cancel acknowledgement.

指标说明：
- 衡量 HL cancel 请求发出到收到 cancel ack 之间的延迟
- 反映 HL 撤单路径的响应速度
- 用来评估价差回落或竞态场景下的额外暴露风险

| Measurement Time | Env / Build | Sample Size | Mean (ms) | P50 (ms) | P95 (ms) | P99 (ms) | Max (ms) | Optimization Summary | Notes |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| TODO | TODO | TODO | TODO | TODO | TODO | TODO | TODO | TODO | TODO |

### `hl_fill_exchange_to_local_rx_ms`

Measures the delay between the HL fill timestamp at the exchange and the moment the local process receives the fill callback/message.

指标说明：
- 衡量 HL 成交在交易所发生到本地收到 fill 回调之间的延迟
- 反映 fill feed 的传输链路耗时
- 用来判断对冲延迟是不是从 fill 通知阶段就已经开始偏慢

| Measurement Time | Env / Build | Sample Size | Mean (ms) | P50 (ms) | P95 (ms) | P99 (ms) | Max (ms) | Optimization Summary | Notes |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| TODO | TODO | TODO | TODO | TODO | TODO | TODO | TODO | TODO | TODO |

### `hl_fill_local_rx_to_lighter_send_ms`

Measures the local delay from receiving the HL fill to sending the Lighter taker hedge order.

指标说明：
- 衡量本地收到 HL fill 到 Lighter taker 对冲请求发出之间的延迟
- 反映对冲反应速度
- 包括主线程排队、状态切换和 hedge 请求构造的本地成本

| Measurement Time | Env / Build | Sample Size | Mean (ms) | P50 (ms) | P95 (ms) | P99 (ms) | Max (ms) | Optimization Summary | Notes |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| TODO | TODO | TODO | TODO | TODO | TODO | TODO | TODO | TODO | TODO |

### `lighter_taker_send_to_ack_ms`

Measures the time from sending the Lighter taker order to receiving the Lighter acknowledgement.

指标说明：
- 衡量 Lighter taker 请求发出到收到 Lighter 回执之间的延迟
- 反映 Lighter 原生签名与请求链路的总成本
- 用来判断对冲启动延迟主要来自本地签名还是交易所应答

| Measurement Time | Env / Build | Sample Size | Mean (ms) | P50 (ms) | P95 (ms) | P99 (ms) | Max (ms) | Optimization Summary | Notes |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| TODO | TODO | TODO | TODO | TODO | TODO | TODO | TODO | TODO | TODO |

### `lighter_taker_ack_to_fill_ms`

Measures the time from Lighter taker acknowledgement to confirmed fill.

指标说明：
- 衡量 Lighter taker 收到 ack 到确认成交之间的延迟
- 反映订单被接受后，交易所侧完成成交的速度
- 用来判断 taker 是否是“立刻完成”还是“ack 后仍有拖尾”

| Measurement Time | Env / Build | Sample Size | Mean (ms) | P50 (ms) | P95 (ms) | P99 (ms) | Max (ms) | Optimization Summary | Notes |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| TODO | TODO | TODO | TODO | TODO | TODO | TODO | TODO | TODO | TODO |

### `maker_fill_to_taker_fill_total_ms`

Measures the full hedge roundtrip from HL maker fill to confirmed Lighter taker fill.

指标说明：
- 衡量 HL maker 成交到 Lighter taker 确认成交之间的完整对冲 roundtrip
- 反映端到端 hedge 延迟
- 这是成交后暴露风险最关键的单一指标

| Measurement Time | Env / Build | Sample Size | Mean (ms) | P50 (ms) | P95 (ms) | P99 (ms) | Max (ms) | Optimization Summary | Notes |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| TODO | TODO | TODO | TODO | TODO | TODO | TODO | TODO | TODO | TODO |

### `hedge_failure_detect_to_unwind_send_ms`

Measures the time from detecting that the Lighter hedge failed to sending the unwind / fallback action.

指标说明：
- 衡量检测到 Lighter 对冲失败到发出 unwind / fallback 请求之间的延迟
- 反映系统对失败 hedge 的反应速度
- 用来评估失败处理额外引入的裸腿暴露时间

| Measurement Time | Env / Build | Sample Size | Mean (ms) | P50 (ms) | P95 (ms) | P99 (ms) | Max (ms) | Optimization Summary | Notes |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| TODO | TODO | TODO | TODO | TODO | TODO | TODO | TODO | TODO | TODO |

### `unwind_send_to_fill_ms`

Measures the time from sending the unwind order to receiving the unwind fill confirmation.

指标说明：
- 衡量 unwind 请求发出到收到 unwind 成交确认之间的延迟
- 反映失败场景下风险回补所需时间
- 用来观察失败路径的尾部风险表现

| Measurement Time | Env / Build | Sample Size | Mean (ms) | P50 (ms) | P95 (ms) | P99 (ms) | Max (ms) | Optimization Summary | Notes |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| TODO | TODO | TODO | TODO | TODO | TODO | TODO | TODO | TODO | TODO |

## 已有公共行情 Baseline

下面这些是当前已经可用的纯 C++ HTTP polling baseline，来源于 [`native_baseline.cpp`](/Users/yinhl/Documents/Playground/hl-maker-lighter-arb-cpp/src/native_baseline.cpp)。它们目前只覆盖公共 orderbook 拉取，还没有覆盖 websocket 行情接收、maker 下单、fill feed 延迟和 hedge 完成时间。

### `native_hl_orderbook_ms`

指标说明：
- 衡量 `NativeHyperliquidExchange::get_bbo("HYPE")` 的端到端耗时
- 当前测到的是纯 C++ 公共 HTTP 路径，不包含 websocket 推送链路

| Measurement Time | Env / Build | Sample Size | Mean (ms) | P50 (ms) | P95 (ms) | P99 (ms) | Max (ms) | Optimization Summary | Notes |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 2026-03-28 Asia/Tokyo | local binary | 10 | 33.21 | TODO | TODO | TODO | 60.75 | Current native public HTTP path | `./native_baseline 10` |
| 2026-03-28 Asia/Tokyo | local binary | 50 | 49.83 | TODO | TODO | TODO | 296.29 | Current native public HTTP path | first run of `./native_baseline 50` |
| 2026-03-28 Asia/Tokyo | local binary | 50 | 48.43 | TODO | TODO | TODO | 356.43 | Current native public HTTP path | second run of `./native_baseline 50` |

### `native_lighter_orderbook_ms`

指标说明：
- 衡量 `NativeLighterExchange::get_bbo(24)` 的端到端耗时
- 当前测到的是纯 C++ 公共 HTTP 路径，不包含 websocket 推送链路

| Measurement Time | Env / Build | Sample Size | Mean (ms) | P50 (ms) | P95 (ms) | P99 (ms) | Max (ms) | Optimization Summary | Notes |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 2026-03-28 Asia/Tokyo | local binary | 10 | 35.70 | TODO | TODO | TODO | 41.02 | Current native public HTTP path | `./native_baseline 10` |
| 2026-03-28 Asia/Tokyo | local binary | 50 | 37.59 | TODO | TODO | TODO | 143.63 | Current native public HTTP path | first run of `./native_baseline 50` |
| 2026-03-28 Asia/Tokyo | local binary | 50 | 37.77 | TODO | TODO | TODO | 142.48 | Current native public HTTP path | second run of `./native_baseline 50` |
