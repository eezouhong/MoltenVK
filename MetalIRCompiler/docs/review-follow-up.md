# Metal IR：实现、review 修复与验收记录

MeloNX 首次遇到 shader 时，MSL 源码编译会阻塞 pipeline 准备。这项工作增加显式选择的 SPIR-V → Mesa/DXIL → Apple MSC 3.1.1 → metallib 路线，绘制继续使用现有 Vulkan/MoltenVK/Metal 后端。默认仍为 MSL；IR 失败明确返回错误，不自动回退。

关联 PR：[MeloNX #285](https://github.com/eezouhong/MeloNX-pending-access-sync-gate/pull/285)、[MoltenVK #24](https://github.com/eezouhong/MoltenVK/pull/24)。两者仍为 Draft。本文区分已实现的功能、已验证的范围和待验收事项。

## 当前交付状态（2026-10-09）

用户已明确接受最新实测 binding 增量 **+0.2871 ms/帧**，并要求达到不了的门槛留下 PR 评论，由用户统一验收。原 review 的 ≤0.1 ms 条件仍记为未达到；这是显式接受的例外，不改写测量结果。两个功能 PR 保持 Draft，默认维护 RC pin 的更新及手机验收留待该最终决定。

最新保留实现把固定 IR descriptor table 的 136 B 只读 root 放在各 allocation 尾部，只有单 used-set、没有实际 push/runtime 参数的 stage 使用。UNIT_POINT_SIZE 等 raster annotation 不请求 root payload。普通 root bytes 路线保持完整 ABI；GPU→bytes 切换强制刷新地址和 ABI。IR 复用无 auxiliary offsets 的 union slot 保存 allocation 地址，MSL auxiliary pointer 与 64 B descriptor-set stride 保持原状；pool 包含 root/对齐容量并提前拒绝越界。没有采用慢的多 set 快照实验。私有 proof/pool 日志已从产品代码移除。

| 最新测量 | MSL | IR | 范围与结论 |
|---|---:|---:|---|
| encode 线程 CPU | 7.4809 ms | 7.5225 ms | 1787/1784 个 pipeline-free 帧，增量 +0.0416 ms；1/30/60/120-frame bootstrap 上界最大 +1.07%，满足总编码 ≤2% |
| 校准 resource binding 增量 | — | +0.2871 ms | 原 ≤0.1 未达到；用户明确接受已披露开销 |
| GPU interval union | 18.1707 ms | 17.9574 ms | 单个 warm 静态对照，不能外推 FPS |
| 静态窗口末 native pool | 18.456 MiB | 31.300 MiB | 退出均全部释放；历史旧 IR 79.66 MiB |
| Metal allocated−Vulkan requested 中位差 | — | +21.63 MiB | Vulkan requested live >512 MiB 的整段会话、含加载；历史 +58–64 MiB |
| footprint−Vulkan requested 中位差 | — | −60.14 MiB | 同范围的比较代理，不能作物理所有者分解 |

两路使用同产品代码、冻结输入、诊断/校准配置，各正常 F24 与实际 process exit0。native/.NET 时钟分别校准，pipeline ticket 区间 ±100 ms 明确排除。采样校准保留负值、要求实际激活与完整计数匹配；串行 frame bootstrap 不等于独立运行置信区间。CPU 验收来自保留的私有诊断候选；去掉临时日志的正式版本另做 GPU/固定输入与 package 校验，不声称测得未插桩产品 FPS。

### 最终正式版本校验

去掉私有 proof/pool 日志后的 Mac ON/OFF 版本均编译成功。Release OFF 的系统值/缓存/明确拒绝 23 项、点精灵 14 项与 IR 描述符 40-row oracle 通过。需要 replay 导出的独立 root probes 使用 ON 版本：set0/set7、100000 小 UBO 容量/free/reset、提前 capacity-overflow 拒绝、GPU→bytes→GPU 的三个独立输出区域、交替 pool 和大表均通过 Metal API/GPU validation。

最终 Release native、release compiler 对同一捕获的 MSL/IR-strict/IR-fast 均正常 process exit0，加载身份验证通过，无 replayer ERROR/FATAL 或 IR stage rejection。烟雾 ROI 共 53868 pixels，MSL/IR 各29 pixels差>1、4 pixels差>4、最大6；strict/fast最大1。整帧 MSL/IR 最大43，strict/fast最大5，完整分布保留；不能把 ROI 结论当作整帧位级等价。startup 的“automatic MSL fallback disabled”是明确选择提示，不是失败计数。

新增 MSL combined-image/sampler 描述符夹具在 API/GPU validation 下抛出 `MTLDebugSamplerState storageMode` 异常，未产生输出 oracle。未改动的维护 RC6 `9a09a595` 使用同一输入也出现相同异常；这支持“不是本次 root/union 改动新增”的判断，但该额外 MSL 用例仍未通过，保留为基线独立问题，不能计入全部通过。

Xcode 首次重建因默认工具链缺 iOS 平台失败，改用已安装且原先成功的 beta3 工具链。提交后的增量 build 又复用了旧 revision header；该生成 phase 声明 output、没有 Git input。删除仅本任务 derived output 的自动生成 header 后重建，二进制已确认宣告实际代码提交 `fe36cb9d`。所有失败和误配均保留。

最终 `fe36cb9d` native iOS Release 与 ABI9 compiler 已进入隔离 preview manifest，pin/dependency preflight 通过；现有当前 NativeAOT 库搭配它完成真实 iOS app 增量构建，静态包验证和 app 内 native/compiler SHA 对应检查通过。二进制无 replay 导出，compiler 不直接链接进 iOS15 executable，最低动态加载范围仍是 iOS17。当前完整 app 的首次构建与后续匹配 native 的增量构建均保留日志；不是签名/安装/设备运行验证。正式清单仍为既有 `3744ea5f`/ABI8，尚未满足 review 要求的“合入维护 RC 后改为实际合入 revision”。

下文保留各阶段设计、失败和不利结果。早期“继续追 ≤0.1”的阶段结论已由上述用户决定取代。

## 已拆出并合入的三个独立 PR

| PR | 问题与修复 | 已有验证 |
|---|---|---|
| [MeloNX #287](https://github.com/eezouhong/MeloNX-pending-access-sync-gate/pull/287) | flush 从完成时刻开始计下一次 draw 窗口，避免慢 flush 让后续窗口过早到期 | 8 个 CPU 测试、关闭诊断产品构建、实际 MSL off/on 主机对照通过；DrawTimer/fast-mode producer 实际非零，完整范围见下文 |
| [MoltenVK #25](https://github.com/eezouhong/MoltenVK/pull/25) | 原生非索引 triangle fan 使用相对索引和正确 base vertex；间接生成与读取都按容量限制 | MSL、IR 各 7 个 GPU 用例；非零 firstVertex、高偏移、超过容量、系统值 CPU oracle；Metal validation 通过 |
| [MoltenVK #26](https://github.com/eezouhong/MoltenVK/pull/26) | replay 诊断默认编译关闭，提供空 inline stub；真实普通与 mesh 间接调用更新计数器 | 6 个 CPU 测试、OFF/ON 普通与 mesh GPU 输出、导出符号检查、完整平台 CI 通过 |

主 IR PR 已基于实际合入的 RC6/master，不再以三个临时功能分支作为依赖。MeloNX 自己的 primitive-restart 拓扑转换未被这些修改替换。

## IR review 修改

- **描述符表**：由每个 descriptor 固定 96 B 改为按 CBV/SRV/UAV/sampler mask 分配 24 B 表项。原生 pool、更新、跨绑定复制、immutable sampler、compiler descriptor range 使用同一组绝对表偏移。桥接 ABI 升为 9。
- **生命周期**：pipeline 只持有不可变编码元数据；MTLLibrary/MTLFunction 留在构建期及有界 resident LRU。元数据清理只删除过期 weak owner，并限制每次全局锁下的扫描量。
- **编译并发**：默认最多两个 IR cache miss 同时进入编译，可配置 1–8；一批编译结束且安静 500 ms 后尝试 allocator pressure relief。反射 parser 在等待 admission 前释放。
- **缓存**：key 仅使用当前 stage 活跃 binding，同时保留实际 register/表偏移、root ABI、数学模式、MoltenVK revision、插件与实际依赖 hash。完整 metallib/反射校验、损坏重编、原子落盘及 LRU 保留；后台写入使用分开的工作/空闲通知和短索引锁。写入丢弃、写入失败有计数；确定性拒绝负缓存，瞬时失败可重试。
- **减少复制**：metallib 的 malloc 所有权交给 dispatch data，AIR math adapter 原地修改。直接解析 SPIR-V 数学策略，避免为策略选择构造 CompilerMSL。
- **运行时与产品接入**：真实 indirect/runtime/descriptor writer 更新遥测；IR 缓存状态放在共享绑定结构末尾；去掉重复资源驻留；IR 隐藏 push-descriptor 扩展和 Vulkan 1.4 对应 feature。MeloNX 在会话启动时保存编译器选择，由 native ABI 检查决定兼容性，并说明缓存预算配置。
- **绑定候选优化**：切换 shader 时按实际 root ABI、使用的 set 和 runtime 地址判断能否复用。该修改仍需性能验收，不把实现完成当作回退已解决。

最新 root 复用修改已通过 23 个系统值/缓存/拒绝回归（21 个预期成功用例均确认 Metal API/GPU validation，另两个验证明确拒绝）。一个定向夹具使用语义相同、debug name 不同的两个 VS，交替提交 5,000 个 draw；每条路径一个进程、一个 warmup 加八个测量批次，GPU 系统值和 sentinel 输出全部正确。IR 绑定原始采样 CPU 中位数约 252→245 ns，完整编码 CPU 中位数 126.09→125.56 ms/批次。该结果只验证这种切换模式的小幅收益，不能外推为固定场景 0.65 ms/帧回退已解决。

push descriptor 的真实 query 也已验证：MSL 同时宣告扩展和 Vulkan 1.4 feature，IR 两者都不宣告。

stage key 的 GPU 验证通过：增加无关 sampler 或 trailing unused set 时编译次数保持 1；改变活跃 descriptor 数量或真实表偏移时分别增至 2、3。五个输出区域和未写入 sentinel 共 16 rows 全部正确。

现有固定场景日志没有任何 VS stage 请求 `DRAW_PARAMETERS`（index 4），因此不能把该场景的回退归因于 index 4 的 setBytes。大部分 VS 只有 point-size 标记；跳过不需要 draw 数据的准备路径后，23 个普通回归及 14 个点精灵/缓存用例通过。仍需真实场景的性能确认。

## 数据及其范围

所有主机图形测试都通过共享 Debug Tool 队列。测试后释放 runner，再分析日志。游戏 shaders、存档、keys、截图、原始运行日志及测试 IPA 均未提交到公共仓库。Apple 系统 Metal cache 未清，不能把以下时间当作完全无缓存的编译器基准。

### 历史编译与正确性证据

应用冷缓存的历史 pipeline 累计时间：IR 11.6–20.8 s，MSL 29.3–130.5 s。IR 的连续 cold/warm 对照从 11.541 s 降至 2.414 s；warm 恢复 996 个 stage，编译 0 个，Mesa/MSC 工作为 0。累计 pipeline 时间不是前台阻塞时间，不能单独证明 FPS 收益。

已运行的合成矩阵包括系统值/缓存/明确拒绝 23 项、描述符 40-row oracle、scalar math 48 项、graphics math 8 项、point raster 14 项、submission parity 20 项，以及同设备并发编译。各批次有各自的源与二进制身份；不表示这些历史矩阵都在最新提交重跑。公开入口：[合成 replay](../tests/replay/README.md)。

最新公开 native 基线 `fc5afc32` 的[完整 CI](https://github.com/eezouhong/MoltenVK/actions/runs/37871350297)通过。MeloNX `66b14bc68e` 的相关本地测试为 53 passed / 0 failed / 0 skipped。后续修改的验证需要单独记录，不能继承为最新 CI 已通过。

补充复算旧 cold/warm 日志的显式 join：cold 的 `blockingShaderJoinMs` 为 MSL 6.17 / IR 31.04 ms，warm 为 5.28 / 55.67 ms；四条日志的 `blockingPipelineJoinMs` 均为 0。`pipelineCreateMs` 是累计 elapsed wall time，并非渲染线程 CPU。pipeline 诊断事件在持久化前会限流，不能靠事件之和还原精确的前台创建总时长，也不能为遗漏事件建立时长上界。`draw_request_overlap` 标签还需实际执行 lane 才能归为渲染阻塞。这些数据没有证明前台阻塞下降，不能把累计准备时间的收益直接当作卡顿/FPS收益。

同四条日志中的连续 guest fence-ready 间隔 >100 ms 分别为 cold MSL 82/10136、IR 43/8227，warm MSL 23/8967、IR 13/8294。ready sequence 连续、无 trace drop/layer切换；窗口是包含加载的整段会话且长度不同。它们是 guest buffer ready 间隔，不是显示帧或 native GPU 时间，保留为描述性证据，不作为静态场景验收。

### 更新后的固定场景

同产品源、存档和诊断设置，MSL/IR 各一个固定场景会话；两者正常关闭、实际 process exit 0。使用 1,718 个 pipeline-free 帧，比较渲染线程 CPU 时间与 GPU interval union。30 FPS 限帧下不使用小幅 FPS 差异作为验收指标。

| 指标 | MSL | IR | 结论 |
|---|---:|---:|---|
| descriptor pool 实际分配 | 19.08 MiB | 30.98 MiB | 旧 IR 为 79.66 MiB，减少约 48.68 MiB；退出后 pool 全部释放 |
| encode CPU | 7.24 ms/帧 | 8.19 ms/帧 | **约 +13%，未通过** |
| resource binding CPU 增量 | — | +0.65 ms/帧 | **超过 ≤0.1 ms 门槛** |
| GPU interval union | 18.21 ms/帧 | 18.24 ms/帧 | 此单对 GPU 时间接近 |

CPU 的 IID 与 30/60/120 帧 block bootstrap 区间均为正；这些区间描述本次串行帧样本，不代表运行间置信区间。外部负载保留记录，不因负载未差在 1pp 内而丢弃结果。

按 Vulkan requested live >512 MiB 的同一采样点计算，`Metal allocated − Vulkan requested` 的整段中位数 IR 增量为 +23.27 MiB，较历史 +58–64 MiB 降低；`process footprint − Vulkan requested` 的中位数增量仍为 +664.19 MiB。IR 本次编译 1001、恢复 782 个 stage，应用缓存保留不等于新 ABI/key 已热。两个残差均是比较代理，不能当作物理内存所有者分解，也不能据此认定内存验收通过。

### 200 个真实 graphics stage 的编译实验

固定 100 个 VS/FS pair，共 200 个不同 stage body；请求 1/4/8 个 worker，MSL/IR 各跑一遍，全部 600 个 PSO 创建通过 Metal API/GPU validation。只有编译和链接，没有绘制；不是游戏 FPS 或烟雾正确性证据。1 ms 是采样目标，实际间隔 P95 为 1.54–1.64 ms。

| 请求 workers | MSL compile peak − baseline | IR compile peak − baseline | IR 实际最大编译并发 |
|---:|---:|---:|---:|
| 1 | 112.86 MiB | 182.63 MiB | 1 |
| 4 | 118.14 MiB | 192.97 MiB | 2 |
| 8 | 124.63 MiB | 190.92 MiB | 2 |

默认并发上限已验证。pressure relief 返回 0，销毁 PSO 后的进程残留仍接近编译峰值，IR 比 MSL 多约 70 MiB。该范围包含原生/Metal 缓存及 allocator；还不能归因为泄漏，也不能关闭内存待验收项。

一个额外的最小归因实验仅关闭 resident library 缓存：4 workers 的残留从 193.09 MiB 降至 189.52 MiB，约减少 3.58 MiB；实际 retained/code bytes 为 0，200 stage 全部重编且 100 PSO 通过 validation。它不能解释主要的约 70 MiB 差距；没有据此修改默认缓存容量。

补充生命周期实验使用相同 200 stage、默认 resident LRU64 和 4 个请求 worker，记录 allocator 与对象销毁阶段；两条路线各 100 PSO 通过。开启 API/GPU validation 时，销毁 instance 后的 footprint 增量为 MSL 121.23 / IR 186.48 MiB（差 +65.25）。关闭两种 validation 后为 42.69 / 72.44 MiB（差 +29.75），而 malloc 活跃分配增量仅为 11.05 / 11.24 MiB。validation 放大了本夹具的驻留开销；剩余差距不能归于已销毁的 pipeline，也不能把 malloc 保留空间当作物理所有者分解。该实验只有固定 shader 的编译/链接，没有游戏内存或画面验收结论。

缓存 descriptor pool GPU 基地址的候选修改也未保留。5,000 个不同 set/输出偏移的 GPU oracle 通过；关闭 API/GPU validation 后编码 CPU 中位数 1.16875→1.16981 ms/批次，没有明显收益。开启 validation 的成本明显放大，未用该数据宣称产品性能改善。

另两个 root 上传候选均通过输出 oracle 后撤回：GPU buffer/offset 方案的 5,000-set 编码 CPU 中位数增加 3.52%；CPU payload 复用与活跃 set 遍历方案在稀疏 set 2 夹具增加 2.77%。两者均在 validation 关闭的配置测量，失败候选和输入保留。没有用正确性通过替代性能收益。

实际 MSC 负缓存端到端测试已通过：两个不同 shader module 提交相同 FP64 stage，MSC 3.1.1 明确返回 `IRErrorCodeFP64Usage`（19）。两次 Vulkan pipeline 请求都失败，compiler calls 保持 1，正值 MSC 累计时间保持不变；没有 MSL 回退。此用例只验证不支持的 optional feature 的拒绝与缓存，不提交 dispatch。可复用入口为 [run_msc_negative.py](../tests/replay/run_msc_negative.py)。

新增的绝对 ID 路线已通过 23 系统值、14 点精灵和 descriptor oracle；真实 GPU 的额外用例覆盖 60 个直接/索引 draw、变化的 vertex/instance origin（含负 indexed ID），960 rows/sentinels 正确，ID-only VS runtime flags 为 0。旧 header client 调用新 Mesa 的原入口仍保留 raw runtime，新入口不再需要它；普通同名输入仍为普通 attribute，新插件混入旧 sidecar 则明确拒绝加载。

5,000-set 定向夹具中，固定 draw origins 的编码 CPU 没有改善（1.1861→1.1964 ms）；变化 origins 的同输入对照降约 4.01%（1.2544→1.2041 ms）。每条路径一个进程、一批 warmup 加八批测量，所有 320,000 rows/sentinels 均核验，时序测量关闭 validation。这个范围只支持变化参数路径的收益，不能关闭固定场景的 +0.65 ms binding gate。首份 microfixture 的 criteria 文本误沿用了 root-payload 候选描述，原文保留，实际源码/身份/ flags 和修正的范围另行记录。

烟雾已定位到捕获中的全屏合成 draw 631800（pipeline 100918，indexed 3 vertices）。一个隔离回放器在关闭跳过开关时，烟雾 ROI 与普通 MSL 回放逐像素一致；整帧只有 ROI 外四个像素相差 1 色阶。只跳过该 draw（日志确认一次）后，白色地面烟雾消失，ROI 33,330 个像素变化超过 1 色阶。这个干预支持来源归因，不是对省略绘制的正确性验收。

实际 VS/FS bytes、绑定序列与完整资源输入录制已核验并留在私有 handoff。之前同一固定输入的 MSL/IR-strict/IR-fast 都出现烟雾：53,868-pixel ROI 中 MSL/IR 各 29 个像素差 >1、4 个差 >4，最大差 6；strict/fast 最大差 1。没有任意容差宣布等价；最终 public 860615/匹配 compiler 的三路对比正在执行。资源抓取检查点曾改变最终画面，因此已排除其 oracle 用途；原始失败保留。

## 修正和失败记录

- Xcode 源/header membership 曾导致缺 header、重复安装 header 和 wrapper 链接失败。header 改为 Project 可见性，IR 实现只由四个 core static target 编译，dynamic wrapper 使用已有静态库；修复后的完整 CI 通过。
- 第一版 200-stage 夹具有五个 depth-writing shader，但 render pass 没有 depth attachment。按实际输出反射补上 depth attachment 后，整组重跑；未删掉失败 shaders，也未把夹具失败记为产品缺陷。
- 原 benchmark stdout/stderr 混写曾破坏 JSON 解析；成功数据从原日志恢复，随后分流输出。失败记录保留。
- 对缺失局部 autoreleasepool 的初步怀疑撤回：实际 pipeline 创建外层已有 pool，不能用该差异解释额外进程 footprint。
- 两次完整 frame capture 的 controller 分别因存储耗尽、诊断容量上限失败。后续离线检查发现第二次正常退出后的最终文件能完整解析出一帧，并已固定哈希；这修正了“controller 失败即录制不可用”的推断。基线回放先因缺显式 Vulkan loader 失败，配置修复后在 GFXReconstruct 的 graphics-pipeline dispatch table 查找处崩溃，尚无有效画面。原失败、录制及回放崩溃报告保留，未宣称烟雾正确。
- 较早的不同游戏时刻截图不能证明烟雾等价。连续帧显示两条路径都能出现烟雾，但缺少同一实际 smoke draw/input 的逐像素 oracle。

### Public 860615 的最终固定场景对照

同产品运行时和冻结存档，MSL/IR 各正常 F24/实际 process exit 0；实际 native SHA 与 public 860615 对应，IR 使用匹配 compiler。两路各一次静态窗口，分别校准 native clock 6 与 .NET clock 8，再排除 pipeline 时间段（±100 ms）。MSL/IR 有效 pipeline-free 帧为 1746/1786。

| 指标 | MSL | IR | 当前结论 |
|---|---:|---:|---|
| 编码线程 CPU | 7.4928 ms | 7.5654 ms | 增量 +0.0726；1/30/60/120-frame bootstrap 上界最大 +1.63%，通过总编码 ≤2% 条件 |
| 校准 resource binding CPU | 1.3481 ms | 1.6716 ms | **+0.3234 ms，仍超过 ≤0.1 条件** |
| GPU interval union | 18.0412 ms | 17.6446 ms | 这一个静态对照的原始范围；不外推 FPS |
| Metal−Vulkan 整段中位增量 | — | +9.88 MiB | 较历史 +58–64 和上一轮 +23.27 明显下降；为同采样点代理 |
| footprint−Vulkan 整段中位增量 | — | +39.25 MiB | 较上一轮 +664.19 下降；不是物理所有者分解 |

内存窗口是 requested Vulkan live >512 MiB 的整段会话，包含加载，不与静态帧窗口混用。两路采样窗口中仍有 11/2 个 pipeline 创建，均通过 ticket 时间段转换明确排除，而不是因“稳定 warmup”默认为完全无编译。单对串行帧 bootstrap 不能替代多运行置信区间。

最终 public 860615/匹配 compiler 的 MSL/IR-strict/IR-fast 对同一输入录制均正常 process exit 0，无 shader/replayer 错误。已定位合成 draw 的 53,868-pixel 烟雾 ROI，MSL/IR 仍只有 29 pixels 差 >1、4 pixels 差 >4，最大 6；strict/fast 最大 1。没有重现烟雾消失，没有宣称位级完全相同。剩余性能工作仍针对 binding 分项，不用总编码通过替代它。

### 最新构建与后续绑定实验

绝对 ID patch 0007 已进入新的 iOS arm64 Mesa 静态依赖和编译器
framework。iOS 编译器 13,312,384 B，四个导出保持不变；原始 Apple
MSC 为 34,626,720 B，厂商 SHA 未变。匹配的 native iOS Release
构建成功，未导出 replay 接口。实际安装和嵌入脚本使用候选 manifest
通过二进制哈希、可选动态加载边界及依赖路径静态校验。该校验使用旧
app executable 作为可选链接控制，并不是新产品 app 的完整重建或
真机加载验收；默认产品 pin 仍等待维护 RC。

原地维护 CPU 根表的私有候选通过 GPU oracle，但固定微基准的编码
CPU 反而增加 1.09%，已拒绝。补充的 8 秒精确进程调用栈采样正常
退出，指出重复资源驻留登记是下一项可测试的机制；调用树样本不能
直接当作 CPU 毫秒分解。

新候选只复用同一编码器、同一阶段和同一描述符池的只读驻留登记。
普通 23 项、点精灵 14 项、描述符 oracle，以及两个 pool 交替重绑的
九批独立输出/哨兵检查均通过 API/GPU validation。旧微基准遗漏了
空探针校准，原始 binding 采样不利结果继续保留；修复后在计时区域
以外发布最终校准快照，验证 90,000 次 binding 调用完整覆盖。该次
固定微基准编码中位数 1.1657→1.1515 ms，校准 binding 均值
24.74→20.31 ns，后者包含一批 warmup。单进程批次不是多次独立
运行。该驻留候选实际场景 binding 增量 +0.3652 ms，总编码上界
+2.93%，两项未过，已拒绝。

后续小表静态 GPU 根表候选通过系统、点精灵、描述符、独立三阶段
输出、不同 pool、释放后复用和 100,000 个小 UBO 的完整容量验证。
136 B tail 包括一个未使用 push 指针的终止零，按 stage setCount
平移绑定，保留完整 ABI。定向编码中位数 1.1558→0.9775 ms，
校准 binding 23.90→11.78 ns；它仍不能代替实际场景验收。
实际场景该候选 binding 增量 +0.3057 ms、总编码上界 +2.83%，
两项仍未过，已保留并撤下。整段同采样点 Metal/process residual
增量 +26.19/+109.50 MiB；静态窗口末的 IR pool 约 30.76 MiB，
退出池全部释放。native pool 事件在时窗内 MSL/IR 45/14 条，
该探针配置不是无观察器的产品 FPS。host logger 会给 native
stderr 加前缀；最初只匹配行首而遗漏事件的解析结果已保留并纠正。

下一项私有实验覆盖所有固定 IR 表：pool 已为全部 maxSets 预留
root 容量，不再只为 ≤128 B 的小表建立 tail，以扩大单 used-set
路径覆盖。实际使用 push/runtime 或多个 set 的 stage 继续原路线。
候选尚未进入生产代码，同一双条件门槛保持不变。

### 最新场景与完整 app 构建证据

全部固定表的静态根表候选在实际对照中仍未过线：binding 增量
+0.3871 ms，总编码 bootstrap 上界最大 +8.06%。同采样点整段
Metal/process residual 中位增量 +10.59/−159.61 MiB；窗口末
IR pool 请求量约 31.39 MiB，退出全部释放。容量越界负例确认在
分配前返回 OUT_OF_DEVICE_MEMORY 并清空句柄。该候选仍已撤下。

进一步检查发现 runtimeFlags 包含 raster annotation：flags4
为 UNIT_POINT_SIZE，而 root 参数只由 bits1/2/32/64 请求。
此前要求所有 flags 为零错误排除了实际记录中 398 个单 set 顶点
stage。这个数字是 stage inventory，不是运行时 draw 频率。
修正条件后 flags4 的独立输出与 direct-root proof、普通/点精灵/
描述符回归均通过；实际场景仍为 binding +0.5272 ms、编码上界
+3.39%，不能宣布性能接受。原始结果全部保留。当前私有实验只
改变地址读取策略：按 descriptor invalidation 复用编码器本地地址，
避免每次访问大 GPU table 尾部；定向收益尚需原场景双门槛验收。

当前产品代码已经完成真实 NativeAOT 库和 iOS app 构建，而不再
只有旧 executable 的包检查。隔离 preview worktree 使用匹配 ABI9
compiler/framework、实际已核验 native 和 preview manifest；正式
产品清单保持不变。NativeAOT 库约 41.12 MB；Xcode app build 成功，
可选动态加载边界、四个 compiler 导出、依赖路径与 legal notices
静态检查通过。41 条 Xcode warning 和原始 AOT warning 留在证据中。
首次 AOT 构建的缺失路径斜杠导致错误 sysroot 已修正；失败日志
lossless gzip 保留。只构建、未签名验收、未安装或操作手机。

### 地址刷新候选的实际对照

避免每次读取大 GPU table 尾部、按 descriptor invalidation/cache reset
刷新地址后，实际 MSL/IR 编码线程 CPU 为 7.3951/7.2167 ms，
增量 −0.1784 ms；1/30/60/120-frame bootstrap 区间都为负，总编码
条件通过。但校准 binding 增量仍为 **+0.3086 ms**，≤0.1 分项未过。
不能把总编码收益替代分项门槛。该修改以实测总收益保留为私有
继续工作基础，仍未进入生产代码。

两路正常 F24/实际 process exit0，分别 1786/1785 个有效帧；
GPU union 18.2742/17.5927 ms。native pool 事件在计时时窗内为
42/12 条，窗口末 IR pool 约 30.52 MiB，退出全部释放。整段
同采样点 Metal/process residual 中位增量 +24.66/−30.00 MiB，
只是比较代理。binding 的诊断 wall 子段显示剩余差异集中在 root
准备/提交，共约 +0.305 ms；这些 wall 子段不是 CPU 毫秒分区，
下一项干预必须结合实际调用路径，不能仅由这三个数猜具体指令。

### 实际 root 路径计数与被拒绝的快照实验

一次独立短诊断先验证 single/multiple/parameter/empty 的正值 producer，
再记录静态窗口 9.02 s/270 帧，无 pipeline 创建，正常退出。实际绑定
调用每帧约：vertex single GPU 2123、multiple/no-parameters 2579、
parameters 44；fragment multiple/no-parameters 3360、empty 1371、
single GPU 46；compute multiple约26。约64%落在多个 set 且不需要
参数的 root bytes 路线。计数按实际绑定调用，不是编译 stage 数量或
GPU 已执行 draw 次数；插入计数器的这次运行不作 CPU 验收。

因此单独测试了每 allocation/阶段只发布一次的不可变多 set GPU
root；tuple 不同或并发 writer 占用时走原 bytes。8-thread/32-round
发布、同 tuple 复用、不同 tuple 不覆盖、Busy 不等待、终止零的
CPU 夹具通过，普通/点精灵/描述符和独立 GPU 输出均通过。
但固定输入的编码 1.3031→1.5195 ms（+16.6%），校准 binding
23.64→71.20 ns，已拒绝；没有为它再跑游戏。新增容量估算约6.53 MB
也未作为可接受 tradeoff 留入产品。原始代码、二进制与结果保留。

下一项私有测试在 descriptor allocation 时缓存稳定的 IR table
地址，复用 IR 不用的 aux slot；MSL aux pointer 和64 B结构 stride
保持不变。写入/复制路径按 IR/MSL 分派，IR没有 aux offset。
定向 parent 对照编码 1.3041→1.2396 ms，校准 binding 35.08→26.90 ns，
正确性已验证，实际双条件验收仍在进行；不据此提前接受。

### Flush 完成计时的实际 MSL off/on 对照

补齐 #287 的主机要求：同一个完整测试程序、相同 native/冻结存档与诊断配置，只有私有启动控制切换旧的 flush-start 时间戳和产品的 flush-completed 时间戳。通过 Debug Tool 共享队列，每路30秒稳定等待后观察60秒，均正常 F24/实际 exit0。原有每秒累计 snapshot 不足以给出逐帧分布，因此私有诊断在 Window.Present 入口发布累计数组，并核验连续 sequence、时钟与实际生产计数。诊断补丁和测量 DLL 保留，测试后恢复源码并重建，未进入产品。

| 有效 pipeline-free CPU Present 区间 | 旧 start 策略 | 新 completed 策略 |
|---|---:|---:|
| 有效区间数 | 1432 | 1545 |
| 所有原因 flush 平均次数/区间 | 21.1418 | 17.7320 |
| DrawTimer 平均次数/区间 | 6.8848 | 4.2278 |
| AttachmentTimer 平均次数/区间 | 3.4756 | 2.7489 |
| 所有 flush 工作 wall 总和/区间 | 11.1827 ms | 10.6817 ms |
| 单次 DrawTimer flush 的平均 work wall | 0.7111 ms | 0.9892 ms |
| CPU Present 间隔中位数 / P95 | 33.4215 / 54.4950 ms | 33.5670 / 51.0737 ms |

总 flush 次数少16.13%，DrawTimer少38.59%，总 flush work wall少4.48%；单次 flush更长，不将其写成单次成本下降。DrawTimer全部处于fast mode（9859/6532次），每次 DrawTimer 前平均draw数425.09/621.58，验证了真实路径，而不只依赖零计数。其他原因的完整分项、原始不利长帧保留。采样窗口有14/2个pipeline创建，按实际ticket区间±100ms排除33/14个Present区间；分别仍有45/21个>100ms区间，未删除。

Window.Present入口的CPU调用间隔不是物理显示帧或GPU时间；flush work含submit/rent/restore等等待，并非线程CPU。这是一对观察，不是多运行置信区间，也不据此宣称FPS或手机收益。native为与测试清单860615匹配的既有a69诊断版本，IR/replay/private pool日志关闭，两路相同；不是最终clean Release性能验收。首次用clean代码但旧embedded revision的库因严格版本门禁在启动前失败，原记录保留，未绕过门禁。

### Debug Tool 的离线 AIR 接入

Debug Tool [PR24](https://github.com/eezouhong/ryujinx-ios-host-debug/pull/24) 已合入，提供 `air diagnose`。共享工具 checkout 仍有本地未提交改动，保留原样；已用 merge96408dd的隔离快照、task-local资源别名，对之前保留的合成vertex AIR文本跑通source-only诊断（exit0、source_only）。输入/快照、stage、工具及caller request ID进入独立证据；未启动GPU或手机。首次私有alias配置缺少evidenceDirectory而拒绝启动，纠正后成功，失败记录保留。

这能把guest→SPIR-V→DXIL→AIR的离线观察整理成标准证据，帮助查浮点权限、volatile/atomic和资源访问结构；不会自动抓取实际shader，也不证明哈希关联的产物被GPU消费。此次样本是保留合成AIR，未冒充最新真实烟雾compile request；数值/像素/性能验收仍以原独立oracle为准。方法已加入项目graphics-debugging skill。

### 最终 compiler 的 200-stage 多样性与生命周期补验

之前200-stage实测使用较早的compiler。现在补齐最终clean native（实际codefe36、Mac Release OFF）与release17/absoluteID0007 compiler，同一批100个真实VS/FS pairs、200个distinct stages，按1/4/8请求worker各跑MSL/IR一次；共享队列1782，六组全部100/100 PSO创建/链接成功，共600个PSO，API/GPU validation无报错。输入/native/plugin哈希前后核验。此实验不提交draw，不替代数值/烟雾oracle。

| 路线 / workers | 成功PSO | 编译峰值−基线 MiB | pipeline销毁后−基线 MiB | IR实际并发峰值 |
|---|---:|---:|---:|---:|
| 1-msl | 100 | 109.500 | 109.875 | 0 |
| 1-ir | 100 | 179.032 | 179.000 | 1 |
| 4-ir | 100 | 184.688 | 184.688 | 2 |
| 4-msl | 100 | 117.813 | 117.703 | 0 |
| 8-msl | 100 | 122.891 | 123.407 | 0 |
| 8-ir | 100 | 182.204 | 182.422 | 2 |

IR并发峰值1/2/2，默认cap2，结束时active/waiting均0；batch/quiet-relief producer实际非零。relief计数表示调用尝试，allocator返回释放字节0，不能称为实际归还物理内存。采样目标1ms，实际间隔P95为1.534–1.847ms、最长4.798ms。应用disk cache关闭，系统Metal cache未清、顺序重复相同shader；首个IR/1的7.236s与后续IR/4、IR/8的约0.64/0.63s受系统cache priming混杂，不据此声称多worker吞吐收益。

再补一个4-worker validation OFF生命周期对照（共享队列1785），两路各100/100PSO成功，七阶段allocator/销毁记录齐全。MSL/IR编译峰值−基线43.016/75.094MiB；销毁instance后的footprint增量42.641/23.828MiB（IR−MSL−18.813MiB）；malloc活跃增量11.488/11.686MiB（差+0.199MiB）。物理footprint与malloc记账不同，不把差值归于某单一所有者。历史旧compiler的+29.75MiB结果继续保留；本次是新source/system-cache状态下的一对观察，不把变化全归功于某个patch，也不外推实际游戏。

机械执行器在测试结束后误用zsh保留变量`status`，其外层包装返回1；队列1785的权威终态exit0、两实际子进程exit0、结果和输入身份核验均通过。包装错误单独保留，没有作为产品失败或隐藏掉。

### 正式 180 秒 ZL 巡逻补验

此前约 30 FPS 的窗口是静态 binding 诊断，不能作为 Debug Tool 的正式移动路线结果。用户指出范围缺口后，重新读取 canonical TOTK case，使用当前 shipping master `09d98ddd5ccf1e3f980ff012ac6b9ed6ae4494a4` / native pin `9a09a595a54792a517c7d34437ad4857a894a685` 作为 MSL 基线；IR 候选使用合入该 master 的 managed runtime `558b568d50935efbdacbc66e98e0c9cf4420299d`、native `c2ca76af6dcde4355969ddf2a8933eb51e7f7765`（代码仍为 `fe36cb9d`）及 release17 / absolute-ID0007 / ABI9 compiler。合入 master 后的四类定向 CPU 测试共 52 项通过，0 失败、0 跳过。

两路从同一 frozen save 完整恢复到独立 profile，manifest 相同，固件均确认 23.0.0；应用 shader/MSL/IR cache 冷启动，系统 Metal cache 未清。canonical navigation 使用 `--start-clock none`，从第一个可读 HUD 开始，起始时钟分别 17:30 / 17:35。canonical `run_zl_patrol.py` 执行 10 个 18 秒循环，每条输入包含 ZL，前进 5+3 秒、后退 5+5 秒。测量期间没有截图或额外导航输入。

共享队列 1812 两组均完成 40/40 段 matching ACK，计划时序/按住时长全部一致，最大提交延迟 11.879 / 10.074 ms，远低于脚本 1 秒拒绝线。实际时长 180.0155 / 180.0227 秒；F24 ACK、关闭 handler、controller 和实际 process exit0 均确认，无强制停止。实际映射的 native SHA 与候选 compiler SHA 均核验。聚合身份和完整分布见 [路线证据](evidence/zl-route-20261009.json)。

| 正式路线窗口 | MSL master | IR 候选 |
|---|---:|---:|
| FPS 区间样本数 | 177 | 177 |
| FPS 样本均值 | 19.8637 | 25.0378 |
| FPS 样本中位数 | 21.3252 | 25.4929 |
| FPS 样本 P05 | 3.5147 | 16.8834 |
| FPS 样本范围 | 0–31.7319 | 6.4309–31.2727 |
| 近似前进分段 FPS 均值 | 19.2106 | 25.7274 |
| 近似后退分段 FPS 均值 | 20.3783 | 24.4818 |
| pipeline 创建次数 / 累计耗时 | 551 / 39463.3171 ms | 391 / 9011.1525 ms |
| blocking shader join 累计耗时 | 13.6250 ms | 2.7494 ms |
| blocking pipeline join 累计耗时 | 0 ms | 0 ms |

这补齐了正式移动输入协议的主机观测，**不是独立重复实验的性能 PASS，也不证明 IR 编译器单独带来上述 FPS 差值**。MSL 先跑、IR 后跑，系统 Metal cache 与外部负载未受控；比较是 shipping master/pin 与整个 IR 功能候选，实际终点位置和游戏时钟不同，pipeline 工作量也不同。起终点截图中人物均在地面，未见历史拒绝路线的爬墙症状；截图只回答这些可见状态，不证明中途所有帧的正确性。FPS/分位数属于遥测区间样本，不能冒充逐帧 P95；前后分段按 ACK 关联的性能游标近似划分，不能证明原生按键 acceptance/expiry 或绝对持续 ZL。累计 pipeline 耗时与 join 计数也不能相加为前台阻塞时间。

首次 baseline 因旧二进制 revision header 不匹配 pin 被启动门禁拒绝；纠正本任务生成 header 并重建后，下一次 baseline 首帧前又发生 NAS 游戏输入 I/O 错误并 SIGABRT。两次失败都保留；用户暂停 NAS 解压期间未再启动游戏，确认 NAS 正常后 fresh905 两路才完成。没有丢弃不利样本，也没有为寻找有利负载复跑。

当前源码已推送，可以 review。正式路线退出后，最新 master-merged `558b568d` 的 NativeAOT 库和 unsigned iOS preview app 重建成功，静态包验证通过；app 内 `Ryujinx.Library.dylib` 与本次 AOT 产物逐字节相同（41137632 B，SHA256 `6855e244da8552d7a01645799a7e3324980d17e83705b939beb7be5d3fbdb867`）。包使用已核验的 native iOS `fe36cb9d` / ABI9 preview 依赖，compiler 和 MSC SHA 与 manifest 一致。该结果补齐最新 managed 代码的构建与嵌入身份，不证明设备动态加载、签名、jetsam 或手机 FPS；手机未操作。

## 用户统一验收的事项

- 原 binding ≤0.1 ms 条件未达到，最新 +0.2871 ms 已由用户明确接受；总 encode 条件通过。所有不利候选继续保留，不再通过复跑寻找有利负载。
- 同一实际 smoke draw/input 的 MSL/IR-strict/IR-fast 没有重现烟雾消失；微小像素差仍披露，不声称位级等价。
- 源码、主机验证、ABI9 compiler 与最新 master-merged managed runtime 的 NativeAOT/iOS preview package 校验均已准备，身份和范围见上述记录。默认清单应在 native #24 经用户验收合入维护 RC 后，更新到实际 merge revision，并补维护 RC 的 MSL 回归；没有提前把默认清单改成新功能分支。
- 最新完整远端 CI 无法执行：native 仓库 Actions 禁用，产品 hosted CI 有账单限制。旧全平台成功 CI 不代表新代码已通过；本地 Mac/iOS build 与定向 GPU 证据单独记录。
- 主机静态场景没有 indirect draw；真实非零合成夹具验证了 producer、输出和参数路径，游戏 indirect 场景覆盖、真机动态加载、jetsam 与 FPS 仍由用户决定后续范围。
- 历史累计编译时间与 warm cache 命中已改善，现有前台 join/限流事件不足以证明前台卡顿下降；该限制明确保留。

数据分析和 first-divergence 调试方法已提交到项目 `.agents/skills`，本机副本同步。原始资源、失败和身份记录保存在私有 handoff，公共仓库只保留方法、源码与聚合结论。
