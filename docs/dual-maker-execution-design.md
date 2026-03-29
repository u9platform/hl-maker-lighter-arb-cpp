# Dual-Maker Execution Design

## 背景

当前系统已经支持一条主执行路径：

- Hyperliquid maker -> Lighter taker

这条路径的优势是：

- Lighter taker 提交链路已经优化到低毫秒级
- Hyperliquid maker 的被动成交逻辑已经较完整

但当前系统也暴露出明显的单边依赖问题：

- Hyperliquid 下单 ack 仍然偏慢
- Hyperliquid 行情新鲜度存在波动
- 单一路径会把执行质量过度绑定在某一边的 maker 成交质量上

因此需要增加第二条并行执行路径：

- Lighter maker -> Hyperliquid taker

目标不是手工选择某一边做 maker，而是允许两条 maker 路径同时运行，由市场自己决定哪条路径先成交。

## 目标

### 核心目标

- 同时运行两条 entry engine：
  - `HL maker -> LT taker`
  - `LT maker -> HL taker`
- 不设置人工“哪边 maker”的单独开关
- 两条路径共享同一个全局风险预算和仓位协调层
- 任一侧 maker 成交后，都能快速完成另一侧 taker 对冲

### 非目标

- 不追求第一版就支持复杂的双边自然配对优化
- 不追求第一版就支持无限频繁的双边 repricing
- 不把两个执行路径做成互不知情的独立机器人

## 总体架构

系统拆成四层：

1. Signal Layer
2. Entry Engine A: `HL maker -> LT taker`
3. Entry Engine B: `LT maker -> HL taker`
4. Global Risk Coordinator

其中：

- Signal Layer 统一消费两边行情
- 两个 Entry Engine 并行运行
- Global Risk Coordinator 决定某一时刻两边是否允许新增 maker 暴露

## Signal Layer

Signal Layer 继续统一维护：

- HL BBO / trades / quote age
- Lighter BBO / quote age
- cross spread
- open spread threshold
- close spread threshold
- cancel band

Signal Layer 不直接发单，只负责产出两类信息：

1. `HL-maker qualification`
2. `LT-maker qualification`

这两个 qualification 必须独立维护，不能共享一个布尔 flag。

建议最小结构：

```text
MakerSignal {
  active: bool
  direction: long_hl_short_lt | short_hl_long_lt
  trigger_spread_bps: double
  latest_cross_spread_bps: double
  last_update_ms: int64
}
```

其中需要至少区分：

- `hl_maker_signal`
- `lt_maker_signal`

## 两条 Entry Engine

### Engine A

路径：

- HL maker -> LT taker

职责：

- 根据 `hl_maker_signal` 判断是否允许挂 HL maker
- 管理 HL maker 的 place / cancel / fill
- HL fill 后触发 LT taker hedge

### Engine B

路径：

- LT maker -> HL taker

职责：

- 根据 `lt_maker_signal` 判断是否允许挂 LT maker
- 管理 LT maker 的 place / cancel / fill
- LT fill 后触发 HL taker hedge

### 统一约束

两条 engine 的结构尽量对称，但不要求底层实现完全共用：

- maker venue 不同
- fill source 不同
- taker venue 不同
- ack / fill 风险特征不同

## Global Risk Coordinator

这是双 maker 并行设计里最关键的部分。

如果没有这一层，两边同时挂单会导致：

- 总待成交 notional 超预算
- 两边同时成交后出现双倍裸腿
- 两个 engine 在相反方向上互相打架

### Coordinator 需要跟踪的全局量

1. `pending_maker_notional_usd`
- 当前所有已挂出、未成交、未撤销确认的 maker 单总风险名义

2. `unhedged_fill_notional_usd`
- 已成交但尚未完成对冲的裸腿风险

3. `net_hl_position_base`
- HL 当前净仓位

4. `net_lt_position_base`
- Lighter 当前净仓位

5. `active_entry_count`
- 当前活跃 entry engine 数

### Coordinator 的最小事实状态

仅靠聚合 notional 不足以正确处理：

- 对侧挂单撤单中
- fill / cancel race
- 双边同时成交
- 部分成交后的净额对冲

因此协调层必须维护每侧逐笔事实状态。建议最小结构如下：

```text
PerEngineExecutionState {
  engine_id: hl_maker_lt_taker | lt_maker_hl_taker
  active_maker_oid: optional<string>
  cancel_pending_oid: optional<string>
  maker_working: bool
  maker_remaining_size: double
  maker_filled_size: double
  hedge_sent_size: double
  hedge_confirmed_size: double
  remaining_unhedged_size: double
  last_fill_ts_ms: int64
  last_cancel_ts_ms: int64
  last_error: optional<string>
}
```

Coordinator 的 source of truth 应该来自这两个 `PerEngineExecutionState`，再由其派生：

- `pending_maker_notional_usd`
- `unhedged_fill_notional_usd`
- `active_entry_count`

### Coordinator 的硬约束

第一版建议直接上硬规则：

1. 总待成交 maker notional 不能超过 `max_pending_maker_usd`
2. 总裸腿 notional 不能超过 `max_unhedged_usd`
3. 当任一侧已有未对冲 fill 时，禁止另一侧新增 maker
4. 当全局 kill switch 激活时，禁止两边新增 maker
5. 当任一侧存在 cancel-pending 状态时，不允许另一侧增加同方向风险

## 推荐的第一版挂单规则

为了避免系统复杂度失控，第一版建议：

1. 允许双 engine 并行监听机会
2. 允许双边同时存在 maker 挂单
3. 但必须满足总待成交 notional 上限
4. 一旦任一侧 maker 成交，立即冻结另一侧新增挂单
5. 另一侧已有挂单应进入“优先撤单”流程

这样做的目标是：

- 保留双边并行找机会的能力
- 但把“同时双边成交”的概率和后果控制住

## 状态机设计

### 全局状态

建议新增一个全局协调状态：

```text
GlobalExecutionState
- Idle
- MakersWorking
- HedgeInFlight
- UnwindInFlight
- KillSwitch
```

### 单边 Engine 状态

每个 engine 单独维护：

```text
EntryEngineState
- Idle
- PendingMaker
- CancelPending
- FillDetected
- HedgePending
- HedgeDone
- Error
```

### 推荐的全局联动规则

1. 两边都 `Idle`
- 允许同时评估并各自挂 maker

2. 一边 `PendingMaker`，另一边 `Idle`
- 另一边仍可挂，但必须通过全局待成交预算检查

3. 一边 `FillDetected` 或 `HedgePending`
- 全局转入 `HedgeInFlight`
- 禁止另一边新增 maker
- 若另一边已有挂单，优先触发 cancel

4. 任一侧进入 `Error` 或裸腿超限
- 全局进入 `KillSwitch`

## 唯一执行优先级

当任一侧 maker 先成交，而对侧挂单尚未撤净时，第一版必须采用唯一执行优先级，避免不同实现做出不同决策。

### 硬规则

1. 任一侧 fill 一到，当前侧 `taker hedge` 立即发送
2. 对侧已有挂单，立即发送 cancel
3. 不等待对侧 cancel ack 再决定是否 hedge
4. 对侧若在 cancel 期间成交，统一进入 reconciliation
5. reconciliation 基于两侧实际 filled / hedged 结果计算净暴露

### 原因

- 等待对侧 cancel 结果会显著拉长裸腿时间
- 双边同时成交属于低概率但必须由 reconciler 收尾的场景
- 执行优先级必须稳定，不能由各实现自行选择“先 hedge”还是“先等 cancel”

### 推荐执行顺序

```text
1. maker fill detected
2. freeze other side new maker placements
3. send current side taker hedge immediately
4. send cancel for opposite active maker
5. wait for:
   - hedge result
   - opposite side cancel ack
   - opposite side late fill if any
6. run reconciliation
7. if residual exposure != 0, send correction / unwind
```

## 双边同时成交的处理原则

这是该设计里最危险但必须明确的场景。

### 场景 A

- HL maker 成交
- LT maker 也在非常短时间内成交
- 两边方向恰好互相抵消

这种情况下，理论上可能已经形成自然对冲。

### 场景 B

- 双边都成交
- 但数量不相等
- 或方向不完全抵消

这种情况下必须进入剩余量对冲流程。

### 第一版建议

第一版不要做复杂的“自动自然配对判定”。

先采用更保守规则：

- 任一侧 fill 一旦触发 hedge，另一侧即使也成交，仍统一进入 reconciliation
- 由 reconciler 计算最终净暴露
- 对剩余风险做一次净额对冲

这会多一些交易，但更安全，更容易验证。

## Reconciler

新增一个统一的 `ExecutionReconciler` 负责：

1. 汇总双边 fill
2. 计算当前净风险
3. 判断是否已经自然对冲
4. 如果未完全对冲，生成 correction order
5. 如果 correction 失败，进入 unwind

建议它只看事实状态：

- HL filled size
- LT filled size
- HL hedge ack / fill
- LT hedge ack / fill

不要依赖 strategy 推断状态。

## 风险场景

### 1. 双边同时成交

风险：

- 瞬时暴露翻倍

控制：

- 任一 fill 触发后冻结新增 maker
- 立即 cancel 对侧挂单
- 统一 reconciliation

### 2. 对侧挂单取消失败

风险：

- 一边已成交并开始 hedge
- 另一边挂单仍然活着

控制：

- cancel pending 不可视为风控完成
- 直到 cancel ack 或 fill 回报到达之前，都要保留风险占用

### 3. 一边 hedge 成功，另一边又成交

风险：

- 系统误以为已完成交易，实际再次暴露

控制：

- recently placed / recently cancelled oid 追踪必须双边都保留
- fill 事件始终优先于“以为自己已经结束”

### 4. 双边方向打架

风险：

- 两个 engine 在不同 spread 解释下做出相反挂单

控制：

- Signal Layer 必须统一方向定义
- 所有 engine 都使用同一套 spread 语义

## 第一版实现建议

### 必做

1. 将当前执行层抽象为 venue-agnostic engine 接口
2. 将 maker venue 和 taker venue 参数化
3. 新增 Global Risk Coordinator
4. 新增双 engine 状态 telemetry
5. 新增全局 reconciliation 层

### 暂时不做

1. 双边同时成交后的最优净额撮合
2. 双边自适应 repricing 竞争
3. 基于历史表现自动决定哪边更优先挂单

## 需要新增的关键指标

### 每条路径单独统计

- `hl_maker_lt_taker_trade_count`
- `lt_maker_hl_taker_trade_count`
- `hl_maker_lt_taker_net_pnl`
- `lt_maker_hl_taker_net_pnl`
- `hl_maker_lt_taker_hedge_total_ms`
- `lt_maker_hl_taker_hedge_total_ms`

### 全局协调层统计

- `dual_maker_overlap_count`
- `dual_maker_cancel_other_side_count`
- `dual_maker_simultaneous_fill_count`
- `dual_maker_reconcile_correction_count`
- `dual_maker_killswitch_count`

## Performance Instrumentation

双 maker 并行后，性能打点不能只看单笔交易结果，还必须覆盖：

- 单路径执行时延
- 协调层时延
- 双边竞争场景
- 行情质量

建议第一版就把埋点体系写全，避免后续只能看到“整体慢了”，却无法定位慢在哪一层。

### A. 单路径执行埋点

两条路径都要各自保留完整 roundtrip。

#### `HL maker -> LT taker`

- `hl_maker_signal_to_send_ms`
- `hl_maker_send_to_ack_ms`
- `hl_maker_ack_to_fill_ms`
- `hl_fill_to_lt_taker_send_ms`
- `lt_taker_send_to_ack_ms`
- `hl_fill_to_lt_taker_ack_total_ms`

#### `LT maker -> HL taker`

- `lt_maker_signal_to_send_ms`
- `lt_maker_send_to_ack_ms`
- `lt_maker_ack_to_fill_ms`
- `lt_fill_to_hl_taker_send_ms`
- `hl_taker_send_to_ack_ms`
- `lt_fill_to_hl_taker_ack_total_ms`

### B. 协调层埋点

这是双 maker 系统新增的关键性能层。

必须单独记录：

- `fill_detected_to_other_side_freeze_ms`
- `fill_detected_to_other_side_cancel_send_ms`
- `other_side_cancel_send_to_ack_ms`
- `fill_detected_to_reconcile_start_ms`
- `reconcile_start_to_done_ms`
- `residual_detected_to_correction_send_ms`
- `correction_send_to_ack_ms`

这些指标用于回答：

- 双边并行后慢在交易所，还是慢在协调层
- 对侧挂单撤不掉是不是系统内部拖慢
- reconciliation 是否成为新的瓶颈

### C. 竞争场景埋点

双 maker 最危险的场景不是普通成交，而是竞争场景。

建议新增以下计数和时延：

- `dual_maker_overlap_count`
- `dual_maker_simultaneous_fill_count`
- `dual_maker_late_fill_after_cancel_count`
- `dual_maker_cancel_race_count`
- `dual_maker_reconcile_correction_count`
- `dual_maker_residual_unwind_count`

以及每类场景的耗时：

- `simultaneous_fill_to_reconcile_done_ms`
- `late_fill_after_cancel_to_reconcile_done_ms`
- `residual_unwind_total_ms`

### D. 行情质量埋点

双 maker 更依赖两边行情质量，因此行情埋点仍然是一级指标：

- `hl_quote_age_ms`
- `lt_quote_age_ms`
- `cross_venue_alignment_ms`
- `hl_market_local_rx_to_bbo_update_ms`
- `lt_market_local_rx_to_bbo_update_ms`

如果后续上线 HL 自建 node，则必须支持 before / after 对比：

- `hl_public_quote_age_ms`
- `hl_local_node_quote_age_ms`

### E. 分路径聚合要求

所有性能 summary 都应至少按以下维度分组：

- `execution_policy`
  - `hl_maker_lt_taker`
  - `lt_maker_hl_taker`
- `event_type`
  - normal
  - cancel_race
  - simultaneous_fill
  - late_fill_after_cancel
  - correction
  - unwind

至少输出：

- `count`
- `mean`
- `p50`
- `p95`
- `p99`
- `max`

### F. 第一版最低要求

如果第一版不能把全部埋点都接完，最低也必须有：

1. 两条路径完整 roundtrip
2. fill -> other-side cancel send
3. other-side cancel send -> ack
4. reconcile start -> done
5. simultaneous fill / late fill after cancel 的事件计数

否则双 maker 一上线，性能问题将很难定位。

## 推荐上线顺序

1. 先保留现有 `HL maker -> LT taker`
2. 引入第二条 `LT maker -> HL taker`，但先只做 dry-run telemetry
3. 开启中间阶段：
   - 新路径真实 maker/cancel，hedge 仅 shadow
   - 或新路径 shadow maker，但对 hedge/reconcile 做真实模拟
4. 验证第二条路径的 maker fill、cancel、hedge 时序、reconcile 结果
5. 再开启双边同时真实挂单
6. 最后再考虑更激进的自然配对优化

### 为什么需要中间阶段

双 maker 系统的主要风险不只在挂单本身，还在：

- fill 回调顺序
- cancel race
- 双边同时成交
- reconciliation 的净额计算

因此从 dry-run 直接跳到双边同时实盘风险过高。中间阶段可以帮助隔离：

- maker 逻辑是否正确
- cancel 逻辑是否稳定
- reconcile 逻辑是否正确
- shadow hedge 结果是否和预期一致

## 最终建议

“不设置哪边 maker 的开关”是可以成立的，但实现方式不应该是：

- 两边各写一个机器人，各跑各的

而应该是：

- 两个 maker engine 并行运行
- 一个全局风险协调层统一控预算
- 一个统一 reconciler 收拾双边成交后的真实净风险

这套设计的核心原则是：

- 允许机会同时出现
- 但风险不能分裂管理
