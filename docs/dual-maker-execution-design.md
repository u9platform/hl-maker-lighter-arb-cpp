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

## 推荐上线顺序

1. 先保留现有 `HL maker -> LT taker`
2. 引入第二条 `LT maker -> HL taker`，但先只做 dry-run telemetry
3. 验证第二条路径的 maker fill、cancel、taker hedge 指标
4. 再开启双边同时真实挂单
5. 最后再考虑更激进的自然配对优化

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

