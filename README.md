# evkit：Linux 可复用事件循环 + 队列 FSM

面向多产品复用的**稳定基础库**（Linux）。不覆盖 MCU（无 epoll/eventfd/timerfd）。

版本定位：**v1.0 基线** — 函数契约与线程模型冻结；结构体内存布局**不**保证 ABI 稳定（便于嵌入，改字段不承诺链接兼容）。

## 目录分层（单包）

```text
xmake.lua          主构建（核心库 / 插件库 / 测试 / 示例）
include/evkit/     对外头文件（#include "evkit/….h"）
src/core/          核心：ev_loop + ev_fsm  → libevkit_core.a
src/plugins/       插件：bus / pool / work / task → 对应静态库
tests/             单元测试（含 ownership / pool / work / task）
examples/          示例；examples/demo 为官方写法模板
```

| 层级 | 路径 | 模块 | 职责 |
|------|------|------|------|
| **核心** | `src/core/` | `ev_loop` | epoll + eventfd post + timerfd；`register_fsm` 回合末 poll |
| **核心** | `src/core/` | `ev_fsm` | 边表 + 事件队列；`free_arg`；强制 `EV_FSM_FAULT` |
| **插件** | `src/plugins/` | `ev_bus` | 观察者扇出：`match`+`match_arg`；`SYNC`/`POST`；建在 `ev_loop_post` 之上 |
| **插件** | `src/plugins/` | `ev_pool` | 有界线程池；`submit` 后 `done` 经 `ev_loop_post` 回 `target_loop` |
| **插件** | `src/plugins/` | `ev_work` | 轻量 offload；`submit` 起临时线程，`done` 经 `post` 回 loop；`drain` 等在途 |
| **插件** | `src/plugins/` | `ev_task` | 任务门面：统一 `return`/done；后端可选 work 或 pool |

产品可只链核心；需要总线/offload/任务门面时再链对应插件。

```text
业务对象嵌入 ev_io / ev_timer / ev_fsm
        ↓
ev_loop_register_fsm(loop, &fsm)
        ↓
ev_loop_run（单线程）
        ↓ io / timer / post 回调：只 ev_fsm_post
        ↓ 每轮派发结束：对已注册的 FSM 统一 ev_fsm_poll
```

官方写法模板：`examples/demo/`（目标 `evkit_demo`）。

## 取舍

| 能力 | 结论 |
|------|------|
| on_enter / on_exit | 可选；仅状态变化时调用；`EV_FSM_STAY` 不触发 |
| guard | 不进核心；边界收成独立事件 |
| EV_FSM_ANY | 精确 > 半通配 > 双通配；同级按表序 |
| 跨线程 API | **`ev_loop_post` / `ev_loop_stop` 官方支持**；`ev_fsm_post` 仍仅 loop 线程 |
| FAULT | 边表必须含 `from_state==EV_FSM_ANY && event==EV_FSM_FAULT` 的全局收敛边，否则 `ev_fsm_init` 失败；处理 FAULT 时先清积压再 apply，FAULT 边 post 的事件留到下一次 poll |

## 线程契约（必须遵守）

| API | 线程 |
|-----|------|
| **`ev_loop_post` / `ev_loop_stop`** | **任意线程**（内部锁 + eventfd） |
| `ev_loop_run` / `ev_io_*` / `ev_timer_*` / `register_fsm` / `deinit` | **仅 loop 线程** |
| `ev_fsm_*` | **仅所属 loop 线程** |
| `ev_bus_*`（init/subscribe/publish/…） | **仅 bus 的 owner_loop 线程**；跨线程先 `ev_loop_post` 到 owner 再 publish |

跨线程碰 FSM：

```text
线程 B：ev_loop_post(loop_a, on_msg, payload)
            ↓ eventfd 唤醒
线程 A（跑 loop_a）：on_msg() 里 ev_fsm_post(&fsm_a, …)
            ↓ 本轮派发结束
         已注册的 FSM：ev_fsm_poll(&fsm_a)
```

`ev_loop_deinit` 前须保证：无其它线程再 `post`，且 `run` 已结束。

## 资源所有权（两套模型，勿混用）

库里并存两种「入队后谁负责释放」——用错会导致泄漏或 UAF：

| 通道 | 入队成功后 | 队列满 / 失败 | deinit 或静默丢弃 |
|------|------------|---------------|-------------------|
| **`ev_fsm_post(arg)`** | **库接管**；经 `free_arg` 释放 | 库立刻 `free_arg(arg)` | `ev_fsm_deinit` / FAULT 清队：库 `free_arg` |
| **`ev_loop_post(user_data)`** | **始终调用者**；库不 free | 返回 `-1`，**调用者** free | `ev_loop_deinit`：**不回调、不释放**；调用者须已可丢或已 drain |
| **`ev_loop_post_full(..., free_fn)`** | 见右 | 满时不调 `free_fn`，调用者 free | 正常：callback 后调 `free_fn` 一次；deinit 丢弃：不跑 callback，调 `free_fn` 一次 |
| **`ev_bus` 的 topic / payload** | **始终调用者**（同 loop_post） | — | 须活到各 POST 回调结束（多 POST 见下） |
| **`ev_bus` POST 的 delivery 包装** | 库 `calloc`，经 `post_full`+`free_fn` | post 失败时 bus 立刻 free | 回调后或目标 loop deinit 丢弃时由 `free_fn` 释放包装 |
| **`ev_bus` 的 match_arg** | **始终调用者**；订期内指针有效 | — | `unsubscribe` 前勿释放 |
| **`ev_io` / `ev_timer` / fd** | 调用者 | — | `remove` 不 `free`、不 `close(fd)` |

补充：

- FSM：`free_arg == NULL` 时库不会释放 arg——**堆 arg 必须提供 `free_arg`**。action / enter / exit **不可接管** arg（poll 后库仍会 `free_arg`）。
- FSM：`ev_fsm_deinit` 若在 `poll` / action / enter / exit 中调用会**直接返回且不清队**——视为误用；须先 `unregister`，再在非 poll 路径 `deinit`。
- 普通 `ev_loop_post` 的堆 `user_data`：deinit 前仍应 drain；`post_full` 用于「库分配的包装块」（如 bus delivery）。

### `ev_bus` POST 保活（硬规则）

- **SYNC**：`topic` / `payload` 仅在回调栈内有效；可传栈上对象。
- **POST**：须活到**该订阅者**回调执行完（语义同 `ev_loop_post`）。
- **一次 publish 命中多个 POST**：同一 `topic` / `payload` 指针会入多条队列——必须活到**全部**相关回调结束；**禁止**在其中一个回调里 `free` 共享堆块，也禁止用即将返回的栈缓冲。
- **推荐做法（择一）**：
  1. **环缓 / 槽位 id**：payload 只传下标，帧存在接收环里，回调按 id 读（适合 CAN）。
  2. **堆块 + 明确唯一释放者**：仅一个 POST 订阅者，或发布前按订阅者数 clone；谁 free 写进产品约定。
  3. **能 SYNC 就 SYNC**：同线程拦截原始数据用 `EV_BUS_SYNC`，避免跨线程保活。
- `match_arg`：用静态/堆/长期对象，不要订完就销毁的栈变量。

### 何时可 free 对象

先 `ev_io_remove` / `ev_timer_remove` / `ev_loop_unregister_fsm`；本对象可在当前回调返回前 free。同批 epoll 里其它 `io` 指针勿 free。勿在 `poll` / `action` / `enter` / `exit` 内 free 正在 poll 的本 fsm。

### 关机清单（推荐顺序）

1. 停止产生新事件（业务 stop；无其它线程再 `ev_loop_post` / `publish`）。
2. `ev_loop_stop` → `ev_loop_run` 返回（必要时再短暂 `run`/`post` 空操作，确保 post 队列已空）。
3. 所有逻辑 `ev_timer_remove`；用户 `ev_io_remove`。
4. `ev_bus_unsubscribe` 全部 → `ev_bus_deinit`（bus 不取消已入队 POST；故步骤 2 须先完成）。
5. `ev_loop_unregister_fsm` → `ev_fsm_deinit`（清队走 `free_arg`）。
6. `ev_loop_deinit`（此时 post 队列应已空；否则残留 `user_data` / bus delivery **泄漏且不再回调**）。

## API 要点

- **定时器**（单调时钟；`ev_loop_now_ms_u64()` 与 `deadline_ms` 同钟）  
  - `ev_timer_start(loop, t, after_ms, repeat_ms, …)`：首次=`now+after`；周期下次=`完成时 now+周期`（**会漂**）  
  - `ev_timer_start_at(loop, t, deadline_ms, repeat_ms, …)`：首次=绝对截止；周期下次=`原 deadline+周期`（**不漂**，落后追赶）  
  - 相对固定起点：`start_at(..., t0 + offset, …)`；整点/凌晨：业务算「距目标剩余 ms」再 `start_at(now_ms_u64() + remain, period)`  
  - `after_ms==0` → 尽快（内部 1ms）；`repeat_ms==0` → 单次  
  - **`ev_timer_stop` / `ev_timer_remove`** 仅用 `timer->loop`（未 start 过则 remove 返回 -1）  
  - 先 `remove` 再 free（回调里可 `remove` 兄弟）
- **io**：调用者设 `O_NONBLOCK`；LT 模式读到 `EAGAIN`  
  - 生命周期结束优先 **`ev_io_remove`**；同批其它 io 勿 free
- **FSM**：`ev_fsm_init(..., user_data, free_arg)`；收尾 **`ev_fsm_deinit`**（清队走 `free_arg`）  
  - 回调只 `post`；用 **`ev_loop_register_fsm`** 注册后由 loop 回合末 poll（`run` 入口也会先 poll 一次）  
  - `ev_fsm_queue_len` 查队列深度（也可用 `fsm.queue.count`）
- **stop**：`ev_loop_stop` 可在 `run` 之前调用
- **post_full**：`ev_loop_post_full(cb, ud, free_fn)`；`free_fn` 在 callback 之后或 deinit 丢弃时恰好一次；有 `free_fn` 时 callback 内勿再 free 同一块
- **bus**：`subscribe(mode, target_loop, cb, ud, match, match_arg)`；`publish(topic, payload)` 对命中项扇出；回调类型 `ev_bus_cb`；简单场景用 `ev_bus_match_u32`；范围/掩码等由应用写 `match`；`publish` 返回成功分发次数；POST 满则 `post_dropped++`；退订不撤销已入队 POST；**POST 保活见上文硬规则**；delivery 走 `post_full`
- **pool**：`submit(pool, target_loop, work, done, user_data, free_fn)`；`free_fn` 与 `ev_loop_post_full` 同义；`shutdown(DRAIN|DISCARD)`；计数 `submit_dropped` / `jobs_discarded` / `done_dropped`；`done` 的 `status==0` 在 target_loop，`status!=0` 表示投递失败（在 worker 上调 done，仍禁止碰 fsm/io/timer/bus）
- **work**：轻量 pool（无队列、临时线程）；`submit(work, target_loop, fn, done, user_data, free_fn)` + `drain`；完成语义同 pool；偶发阻塞用 work，持续多任务限并发用 pool
- **task**：`new` → `run_work`/`run_pool`（或 `use_*` + `run`）；线程里 `return_ptr`；loop 上 `done` 里 `get_ptr`/`get_status`；成功 run 后由库释放 task；`free_fn` 释放 user_data
- **post/FSM 队列满**：均返回 `-1` 并累计计数；**FSM** 满时库 `free_arg(arg)`；**loop post** 满时调用方 free `user_data`（两套模型见「资源所有权」）
- **步数上限**：嵌套 `run` → `-1`；post drain 触达上限则 `post_drain_limit_hits++`；FSM 单次 poll 触达 `EV_FSM_POLL_MAX_STEPS` 则清空队列并投入 `EV_FSM_FAULT`，同一次 while 继续消化（`poll_limit_hits++`）；处理 FAULT 时先清积压再 apply，FAULT 边新 post 留到下一次 poll
- **迁移顺序**：`on_exit` → `action` → 改 state → `on_enter`；action 内 `get_state()` 仍是旧状态
- **多超时约定**（不进核心；写法见 `examples/nested_timeout/`、`examples/cmd_retry/`）
  1. 超时有身份：`EV_DEADLINE` / `EV_STEP_TO` / `EV_Wi_TO` …，禁止一个匿名 `EV_TIMEOUT` 打所有边
  2. 生命周期跟状态走：`on_enter` arm、`on_exit` disarm；action 只做业务
  3. 迟到事件靠边表自然丢弃；同 timer 复用跨步时可用 generation
  - 整单截止用 `ev_timer_start_at`；每步用独立或复用的 step timer
  - 步进失败重试：`STEP → RETRY → STEP`，整单 deadline 贯穿不刷新；无 guard 时用 STAY + action 内 post 细分事件（`cmd_retry`）
  - 并发等待：协调态 + 固定 wait 槽（各槽自有 timer），退出态统一 disarm
  - 父子：同 loop 两台 FSM；子超时收敛后再 `post` 父事件冒泡；若子向「本轮已 poll」的父投递，需再 `ev_loop_post` 空回调唤醒下一轮；勿在对方 poll 路径 `deinit`

## 生命周期

1. `ev_loop_init` → `ev_fsm_init`（含 FAULT 边）→ `ev_loop_register_fsm` → 业务 `ev_io_add` / `ev_timer_start`（可选 `ev_bus_init`）
2. `ev_loop_run` 直到 `ev_loop_stop`
3. 关机见上文「关机清单」（先排空 post，再 bus / timer / io / fsm，最后 `ev_loop_deinit`）

## 构建（xmake）

依赖：[xmake](https://xmake.io) ≥ 2.8。

```bash
xmake                  # 构建 libevkit_core / libevkit_bus + evkit_demo
xmake test             # 单元测试（含 test_ownership）
xmake run evkit_demo   # 官方 API 模板
xmake build -g examples
xmake run worker_ping
xmake run pool_ping       # 线程池插件：submit → done@loop → fsm
xmake run work_ping       # 轻量 offload：临时线程 → done@loop → fsm
xmake run task_ping       # 任务门面（默认 work）；`task_ping -- pool` 换池
xmake run topic_hub
xmake run upgrade_link [-- rs485]
xmake run orch_demo
xmake run nested_timeout  # 整单+步进 / 并发 wait 槽 / 父子超时冒泡
xmake run cmd_retry       # 整单 deadline + STEP/RETRY 重试
xmake run bus_demo
xmake build tcp_reconnect
xmake run tcp_reconnect -- 127.0.0.1 9100
xmake build tcp_cmd_client
xmake f -m release     # 可选：发布优化
xmake f --examples=n   # 可选：不编示例
```

产物在 `build/<plat>/<arch>/<mode>/`；`compile_commands.json` 由 xmake 自动更新。

| 目标 | 说明 |
|------|------|
| `evkit_core` | 静态库：loop + fsm |
| `evkit_bus` / `evkit_pool` / `evkit_work` / `evkit_task` | 插件静态库（task 依赖 work+pool） |
| `evkit` | phony：core + 全部插件 |
| `evkit_demo` / `evkit_test` | 模板 / 回归 |
| `worker_ping` / `pool_ping` / `work_ping` 等 | `examples` 组 |

### 推荐阅读顺序（产品切片）

1. **`examples/tcp_reconnect/`** — 连网会话：FSM + `ev_io` + timer，回调只 `post`  
   ```bash
   python3 examples/tcp_reconnect/echo_server.py 9100   # 终端 1
   xmake run tcp_reconnect -- 127.0.0.1 9100            # 终端 2
   ```
2. **`examples/worker_ping/`** — 跨线程：`xmake run worker_ping`  
3. **`examples/topic_hub/`** — 总线扇出与 POST 保活：`xmake run topic_hub`  
4. **`examples/upgrade_link/`** — CAN/485 式双口：日常拼帧+失联不用 FSM，升级用 FSM：`xmake run upgrade_link [-- rs485]`

其余：`bus_demo` / `orchestrator` 偏 API 演示；`nested_timeout` / `cmd_retry` 专讲多超时与重试；`tcp_cmd_client` 为进阶全栈（读代码：`proto.h` → `session.c` → `cmd_engine.c` → `worker.c` → `cmds.c` → `main.c`）。

| 示例 | 依赖 |
|------|------|
| `evkit_demo` / `tcp_reconnect` / `worker_ping` / `upgrade_link` / `orch` / `nested_timeout` / `cmd_retry` | **仅核心** |
| `pool_ping` | 核心 + `ev_pool` |
| `work_ping` | 核心 + `ev_work` |
| `task_ping` | 核心 + `ev_task`（及 work/pool） |
| `bus_demo` / `topic_hub` / `tcp_cmd_client` | 核心 + `ev_bus` |
| `tests/` | 核心 + bus + pool |

## 已知边界

- 绑定 Linux epoll 族；非可移植 OS 抽象层
- 结构体对外可见，**ABI 未冻**；以头文件函数契约为准
- `ev_loop_now_ms()` 为 `uint32`（约 49 天回绕）；内部 deadline 用 `uint64`
- 单 loop 最多 `EV_LOOP_FSM_MAX`（默认 16）台已注册 FSM；bus 最多 `EV_BUS_SUB_MAX`（默认 32）条订阅；`sub_id` 自增耗尽后 subscribe 失败
- `ev_loop_post` 单次 drain 有步数上限（触达则 `post_drain_limit_hits++`，剩余留待下次唤醒）
- `ev_loop_deinit` 丢弃未执行 post：不跑 callback；`free_fn==NULL` 时不释放 `user_data`；`post_full` 会调 `free_fn`（bus delivery 走此路径）
- 无协议编解码、无自动重连；业务自建
