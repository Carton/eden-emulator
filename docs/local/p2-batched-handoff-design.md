建议采用 **(a) 的收紧版本：resolver 私有命令批次 + GPU 线程唯一合并点 + 显式同步请求协议**。保留一个 Scheduler、一个 VulkanWorker 和现有提交序列。第一阶段只验证捕获与合并，第二阶段再开启 GPU 线程与 resolver 的重叠。

关键边界是：**私有捕获不能只解决 `Record()` 的 arena 竞争，还必须覆盖 resolve 触达的调度状态、`Finish()`、guest 读集和资源生命周期。** 未满足这些条件时，`EDEN_DRAW_TOKEN=async` 不应启动 worker 路径。

以下基于简报设计，不需要探索仓库；接口为建议签名。

## 1. 架构决策

### 1.1 单 job 在飞，私有批次作为主命令流的一段

先保持最多一个在飞 job，避免同时引入多 job 的缓存版本、资源所有权和提交依赖问题。

逻辑命令顺序固定为：

```text
主流此前命令
    ↓
resolve(N) 捕获的命令段
    ↓
tail(N)：ConfigureTail → UpdateDynamicStates → RecordDraw
    ↓
后续主流命令
```

普通路径在 `CommitPendingDraw()` 合并：

```text
GPU 线程                         resolver worker

SnapshotAndEnqueue(N)
发布 job 控制状态 ──────────────→ ConfigureResolve
继续允许重叠的工作                 Record → 私有 batch
                                 guest capture → job 私有字节
CommitPendingDraw(N)              seal batch / publish Resolved
  等待并服务同步请求 ←─────────────┘
  拼接 batch
  FinishDrawLocked(N)
```

这里的“继续工作”不能理解为 GPU 线程可以任意执行下一个命令：

- 不产生调度器命令、不改变 job 依赖状态的解码、快照准备可以重叠。
- 与 guest 读集不相交的写入，在第二阶段可放行。
- 后续主流 `Record`、提交、通道切换，以及影响该 job 的缓存操作，必须先经过顺序屏障。
- 保持 `tail(N)` 完成后才启动下一个 job，先不扩展流水深度。

因此，**0.5µs/draw 只是可尝试隐藏的工作量，不能预先承诺收益**。如果两个 draw 之间没有足够的独立工作，线程交接成本可能超过收益。

### 1.2 捕获上下文属于线程，不是 Scheduler 全局开关

采用显式 `ResolverCaptureContext`，底层通过 TLS 将已有 `scheduler.Record()` 路由到对应批次：

```cpp
struct ResolverCaptureContext {
    Scheduler* scheduler;
    DrawJobId job;
    CapturedBatch* batch;
    ResolveSyncBridge* sync;
};
```

约束：

- TLS 必须同时校验 Scheduler 实例和 job ID。
- 使用 RAII 安装、恢复；禁止不受控嵌套。
- Scheduler 不能用一个共享的 `capture_redirect=true`，否则会把 GPU 线程的命令误导入 resolver batch。
- 捕获路径禁止接触主 `current_chunk`、主 `DispatchWork()` 或主 chunk reserve。
- 命令 lambda 必须拥有所需参数，不能保存短寿命局部变量或 job 可变存储的裸引用。

### 1.3 捕获必须覆盖调度状态

`Record()` 重定向只是最低层机制。必须审计 resolve 触达的全部 Scheduler 操作，分类处理：

| 操作 | 捕获期间语义 |
|---|---|
| 纯命令追加 | 写入私有 batch |
| render pass 等录制状态变更 | 捕获为有序操作，或使用局部状态模型 |
| 依赖主流状态的查询 | 使用明确定义的入口状态；无法保证则先同步 |
| 实际提交、tick 分配、完成等待 | 发同步请求，由 GPU 线程执行 |
| 未覆盖的调度器入口 | 开发期立即断言，禁止偷偷落回主流 |

尤其不能直接在 worker 修改“当前 render pass”“待提交状态”或提前读取一个随后失效的提交 tick。

如果无法安全复制调度状态，第一阶段应使用**保守的 render pass 边界**：进入捕获段前结束此前 pass，捕获段也以已结束 pass 的状态交接。正确性稳定后再减少额外边界。

### 1.4 被否方向

**(b) 第二个 Scheduler：不采用。**

独立 worker 并不能自动建立两个命令流的顺序。还会引入：

- 同一 `VkQueue` 的外部同步；
- 提交/timeline 顺序；
- image layout、barrier、资源回收 tick 的跨调度器一致性；
- `Finish()` 究竟排空哪个依赖前缀。

最终仍需要统一排序器，复杂度明显超过私有批次。

**(c) 纹理操作意图：作为局部补充，不作为整体方案。**

对于依赖主调度状态的少数操作，可以捕获稳定的、拥有资源引用的操作描述，在 VulkanWorker 执行时展开。但如果把绑定查找、缓存决策和 guest 读取全搬到 tail，主要成本仍留在 GPU 线程，达不到目标。

## 2. 私有批次与合并协议

### 2.1 chunk 的所有权状态

```text
resolver 独占构建
    → sealed，不可修改
    → GPU 线程接收
    → Scheduler 队列拥有
    → VulkanWorker 执行并销毁命令对象
    → 回收
```

建议接口：

```cpp
class CapturedBatch {
public:
    CapturedBatch(CapturedBatch&&) noexcept;
    CapturedBatch& operator=(CapturedBatch&&) noexcept;

    template <class F>
    void Record(F&& command);

    SealedBatch Seal();
};

class Scheduler {
public:
    CaptureScope BeginResolverCapture(ResolverCaptureContext&);

    // 仅 GPU 线程调用；消费所有权。
    void SpliceCaptured(SealedBatch&&);

    // 捕获状态下的 Finish 转换为同步请求。
    void Finish();
};
```

`SpliceCaptured()` 的顺序要求：

1. 先将 GPU 当前 chunk 中的前序命令发布。
2. 再依次发布捕获 chunk。
3. GPU 后续命令写入新的或确定为空的主 chunk。
4. 捕获批次始终排在 tail 的命令之前。

不需要等待 VulkanWorker 执行完才能 record tail；单 worker 的 FIFO 顺序已经足够。

同时修复主 `DispatchWork()` 的所有权交接：

> 先让 producer 放弃旧 chunk、安装替代 chunk，再把旧 chunk 发布给 consumer。

不得先发布、再继续操作旧 chunk。空 chunk 不发布。

私有 chunk 第一阶段可以独立分配；确认交接成本后再增加受保护的池。不要为了省分配重新共享不安全的 reserve。

### 2.2 job 数据分离

```cpp
struct ResolveJob {
    DrawJobId id;
    ChannelGeneration channel;
    CapturedEngine engine;
    PipelineRef pipeline;
    EpochStorage epoch;       // 每 job 私有、地址稳定
    ResolveReadSet read_set;
    ResolveCompletion state;
};
```

区分两个发布：

- **控制状态发布**：worker 启动前发布 job ID、所属通道、完成事件，供屏障识别。
- **结果发布**：resolve 完成后发布 batch、epoch 和绑定结果，供 tail 使用。

不能把原来的 `pending_commit` 提前设为 true 来兼任两者。否则 resolve 内回调会把一个尚在执行的 job 当作可提交结果，造成自等待。

完成通知采用 release/acquire，状态机建议为：

```text
Queued → Resolving ↔ NeedsGpuSync → Resolved → Committing → Retired
```

失败另设终态；不能将半完成结果伪装成 `Resolved`。

## 3. `Finish()`：提交捕获前缀，由 GPU 线程代办

不能把 `Finish()` 当作普通 lambda 延迟到 tail：调用方可能依赖完成结果继续执行 resolve。

建议桥接接口：

```cpp
struct ResolveSyncRequest {
    DrawJobId job;
    uint64_t sequence;
    SealedBatch prefix;
    SyncRequirement requirement;
};

class ResolveSyncBridge {
public:
    // resolver：发布此前捕获前缀，等待 GPU 服务完成。
    SyncResult RequestAndWait(ResolveSyncRequest&&);

    // GPU：取请求；持锁取出后立即解锁，再执行调度器操作。
    std::optional<ResolveSyncRequest> TryTake();
};

void RasterizerVulkan::CommitPendingDraw();
void RasterizerVulkan::ServiceResolveSyncRequests();
```

worker 捕获期间调用 `Finish()`：

1. 封口当前私有前缀。
2. 发布同步请求。
3. GPU 线程发布自己的前序 chunk。
4. GPU 合并该私有前缀，执行真实的提交及完成等待。
5. GPU 发布完成结果。
6. resolver 换一个空 batch，继续 resolve。

最终合并只接收剩余后缀，不能重复拼接已经提交的前缀。

因此，“唯一合并点”的准确含义是：

> **唯一合并执行者是 GPU 线程；正常入口是 CommitPendingDraw，同步前缀也通过同一交接协议处理。**

所有等待 resolve 的入口都必须使用“等待 + 服务同步请求”循环，禁止直接阻塞在一个完成 future 上。

第一阶段可允许 resolver 在持 cache 锁时等待这个同步请求，但必须满足严格条件：

- GPU 服务路径完全不获取 cache 锁；
- 不进入 tail；
- 不调用可能重新进入 rasterizer/cache 的普通辅助路径；
- VulkanWorker 执行该前缀不依赖这些 cache 锁。

若任何条件不能证明，就必须把对应运行时操作改成**可暂停的两段式操作**：保存拥有引用的 continuation，退出 cache 临界区后等待，再重新获取锁并校验。不能在任意 C++ 调用栈位置随意解锁。

## 4. guest 读集、外部失效与 epoch

### 4.1 读集必须“读之前注册”

只在 resolve 完成后拿到 `(addr,size)` 列表，无法保护正在发生的读取。

接口示意：

```cpp
ReadLease AcquireResolveRead(
    DrawJobId job, GuestRange range, MappingGeneration generation);

MutationGuard BeforeGuestMutation(
    GuestRange range, MutationKind kind, MutationOrigin origin);
```

核心协议：

- resolver 在实际读取前登记范围。
- 写入方在实际修改前登记写入意图并检查读依赖。
- 检查、登记通过同一个独立同步域完成。
- 等待必须在释放该同步域锁后发生。
- map/unmap 也进入此协议，不能只保护字节写入。

还需要处理**尚未发现的读范围**：写入方检查时，resolver 可能还没走到相关描述符。

因此第二阶段应分两步：

1. 对无法提前界定的读取保留保守屏障。
2. 在 enqueue 时注册可覆盖后续读取的读范围上界；动态缩小或扩展前必须有相应保护。

不能宣称“当前 read_set 没撞，所以写入安全”。登记时序正确，也不等于满足 draw 应当观察到的 guest 版本。

不同依赖的 lease 可以有不同释放点：

- 已完整复制到 job 私有 epoch 的字节，可在复制完成且结果稳定后释放。
- tail 仍依赖的缓存内容或映射，保持到 tail 完成或由独立版本/pin 接管。

### 4.2 resolve 内部回调

使用显式来源：

```cpp
enum class MutationOriginKind {
    External,
    GpuCommand,
    ResolveInternal,
};
```

`ResolveInternal` 携带当前 job ID。它不能等待自己，但也不能无条件绕过全部保护：

- 属于当前 resolve 协议内的同步操作：走专门内部接口。
- 会改变已捕获依赖的操作：使相应结果失效并重新校验，或进入受支持的同步分支。
- 禁止递归调用当前 job 的 `CommitPendingDraw()`。

### 4.3 外部 cache 失效

外部线程只访问 cache 自身的锁、generation、pin/失效元数据；不读取 resolver 的 GPU 私有状态。

若外部回调原本在持 cache 锁时等待 resolve，必须改造：**先退出 cache 临界区再等待**，或只记录失效状态，让安全点处理。否则 resolver 正等待同一把锁时必然死锁。

### 4.4 epoch 与资源寿命

直接采用每 job 私有、地址稳定的 epoch 字节存储。第一版不建议复用现有固定槽的跨 draw 存储。

每 job 私有字节只保护 CPU 数据，不自动保护 Vulkan 资源：

- tail 消费完 CPU epoch 后，才能回收其存储。
- 命令对象引用的数据至少活到命令执行完成。
- GPU 使用的资源按实际提交完成点回收。
- 捕获阶段不得把“尚未确定的提交 tick”当作安全回收依据。

通道切换必须先 drain 当前 job、服务全部同步请求，再销毁 resolver/MemoryManager 关联。generation 仅用于检测陈旧引用，不能替代 drain。

## 5. 锁序与 hazard

以下是目标锁序；现有代码若不满足，应在阶段一识别并修正。

| 锁/同步域 | 获取与释放规则 |
|---|---|
| resolver 控制锁 `J` | 仅操作状态与同步请求；持有时不获取其他锁、不执行命令、不等待 |
| guest 依赖锁 `R` | 仅登记读写范围；实际读取/写入/等待前释放；写入方持有它时不得获取 cache 锁 |
| buffer cache `B` | 需要双锁时先获取 |
| texture cache `T` | 双锁顺序固定为 `B → T`；禁止 `T → B` |
| scheduler queue `Q` | 仅发布/取队列元素；调用前释放 `J/R`；GPU 合并路径不持 `B/T` |
| worker execution `E` | consumer 取出工作后释放 `Q`，再获取 `E`；不在持 `Q` 时等待 `E` |
| 可选 chunk pool `P` | 独立短临界区，不能带锁执行命令或获取其他锁 |

如果 guest 读取注册发生于 cache 临界区内，可允许 `B → T → R`，但 `R` 必须短暂持有且不能等待；发生冲突时退出 cache 临界区后处理。整个系统不得出现 `R → B/T` 的反向路径。

GPU 的正常提交流程拆成：

```text
等待 resolve，期间服务同步请求，零 cache 锁
    → Q：拼接 batch，然后释放
    → B → T：执行需要 cache 锁的 tail
    → 释放
```

必须显式排除以下环：

| Hazard | 必需措施 |
|---|---|
| GPU 等 resolve，resolver 等 GPU Finish | 所有 resolve 等待均服务同步请求 |
| GPU 持 cache 锁等 resolver | 等待发生在获取 cache 锁之前 |
| resolver 持 cache 锁等 Finish，GPU 又获取 cache 锁 | 同步服务路径禁止触达 cache；否则 continuation |
| VulkanWorker 等自己的完成 | VulkanWorker 不执行同步桥接等待 |
| 私有 batch 排到 tail 后面 | 合并先发布主前缀，再私有批次，再 tail |
| 捕获后缓存失效释放资源 | pin/version 与实际提交完成点共同管理寿命 |
| resolve 回调递归提交自己 | 控制状态与结果状态分离；携带 job 来源 |
| 通道重建时旧 worker 访问 MemoryManager | 切换前完整 drain |
| 捕获失败后重复副作用 | 已提交前缀不得 replay；禁止半途直接重跑 INLINE |

## 6. 文件级改动计划

| 文件 | 主要改动 |
|---|---|
| `vk_scheduler.h/.cpp` | 私有 batch/sealed batch、TLS capture scope、GPU splice；修复 chunk 发布所有权顺序；捕获入口审计；同步前缀协议；提交与回收信息传递 |
| `vk_draw_resolver.h/.cpp` | job 状态机、私有 epoch、资源 pin、worker 捕获、同步请求、结果发布、drain |
| `vk_rasterizer.h/.cpp` | `CommitPendingDraw()` 等待/服务循环；GPU 有序操作屏障；区分控制发布与 pending commit；通道切换 drain；async 门控 |
| `engine_override.h` | 核实并保证 override 为线程局部、RAII 恢复；禁止保存其他线程的活跃 override |
| `memory_manager.cpp` | 在写入和 map/unmap 真正发生前取得 mutation guard；处理来源及自回调；未知读范围保留屏障 |
| `buffer_cache/buffer_cache.h` | resolve 读取范围登记、资源 pin/version、失效协议；移除持锁等待 resolver 的路径 |
| `texture_cache` 相关 | 审计 descriptors、CopyImage 和全部 runtime 调度调用；处理 render pass 状态、Finish continuation、资源 lifetime；封堵捕获期间主流访问 |

这些修改不要求在第一阶段一次实现精细读集；但第一阶段必须把所有不安全入口变成明确屏障或断言。

## 7. 分阶段里程碑与验收

### 阶段 1A：GPU 线程上捕获，立即合并

继续 INLINE 执行 resolve，只改变命令存储和合并方式。

验收重点：

- `EDEN_TOKEN_CHECK=1`：0 mismatch。
- DrawToken diag 的 resolve/tail/draw 计数闭合。
- chunk 无重复执行、遗漏、泄漏或越界。
- 图像 QA 通过，参照局与测试局背靠背，并先校准 golden 自身噪声。
- 确认 render pass 和提交边界行为等价。

### 阶段 1B：worker 执行，但 GPU 立即等待

不追求重叠，专门验证跨线程所有权和同步协议。

额外覆盖：

- resolve 内 `Finish()`，包括一个 job 多次 Finish；
- 立即提交、关停、通道切换；
- guest 同步回调与外部 cache 失效；
- 满 chunk、多 chunk 和空 batch。

验收仍为 checker 0 mismatch、diag 计数闭合、图像 QA；另要求等待有诊断信息，能定位 job、同步请求序号和等待原因。

### 阶段 2A：有限真重叠

开启 `EDEN_DRAW_TOKEN=async`，保持单 job；未知 guest 依赖继续保守等待。

新增诊断：

- GPU 等待 resolve 时间；
- worker 排队、唤醒、实际 resolve 时间；
- batch 数量/字节数；
- Finish 请求次数；
- 因顺序、guest 写入、cache 操作而等待的次数。

先证明实际存在重叠窗口，再讨论吞吐收益。

### 阶段 2B：读集精化

先覆盖可预登记范围，再处理动态描述符读取；按范围放行无冲突写入和 map/unmap。

除既有三项验收，还应验证确定性的并发时序：

- 写入早于读范围发现；
- 读取进行中发生写入；
- 地址范围重叠的 map/unmap；
- resolve 内部同步回调；
- 外部失效与 Finish 同时发生。

宏观性能只使用现有交错 A/B 规程，检查场景档位、luma 和机器空闲条件，报告逐对比值中位数。若收益仍落在 ±2% 噪声带，不能宣称优化成功。

## 8. 风险与回退

最大的风险依次是：

1. **只捕获命令，没有捕获调度状态和回收依赖。**
2. **读集发现太晚，放行了本应排在 draw 后的写入。**
3. **Finish 桥接存在隐藏的 cache 锁回边。**
4. **0.5µs 的任务不足以覆盖线程交接成本。**

门控建议：

- 默认保持现有 deferred-INLINE。
- `EDEN_DRAW_TOKEN=async` 仅启用完整、通过验收的捕获路径。
- 开发期可另设 capture-inline/capture-worker 验证模式。
- 在 job 开始前发现不支持的条件，可以选择 INLINE。
- 已经修改 cache 或提交捕获前缀后，禁止丢弃结果并重新执行；必须按协议完成当前 job，再在安全边界回退。

实现优先级应是：**先证明私有批次与主流等价，再证明同步协议无环，最后减少等待。** 对这个量级的 resolve，正确性协议完成后测不到收益，也应保留 INLINE 为默认路径。