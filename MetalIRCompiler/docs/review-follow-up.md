# Metal IR：实现、review 修复与验收记录

MeloNX 首次遇到 shader 时，MSL 源码编译会阻塞 pipeline 准备。这项工作增加显式选择的 SPIR-V → Mesa/DXIL → Apple MSC 3.1.1 → metallib 路线，绘制继续使用现有 Vulkan/MoltenVK/Metal 后端。默认仍为 MSL；IR 失败明确返回错误，不自动回退。

关联 PR：[MeloNX #285](https://github.com/eezouhong/MeloNX-pending-access-sync-gate/pull/285)、[MoltenVK #24](https://github.com/eezouhong/MoltenVK/pull/24)。两者仍为 Draft。本文区分已实现的功能、已验证的范围和待验收事项。

## 已拆出并合入的三个独立 PR

| PR | 问题与修复 | 已有验证 |
|---|---|---|
| [MeloNX #287](https://github.com/eezouhong/MeloNX-pending-access-sync-gate/pull/287) | flush 从完成时刻开始计下一次 draw 窗口，避免慢 flush 让后续窗口过早到期 | 8 个 CPU 测试、关闭诊断的产品构建通过；合入前未完成主机游戏 off/on 对比，此限制保留 |
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

## 修正和失败记录

- Xcode 源/header membership 曾导致缺 header、重复安装 header 和 wrapper 链接失败。header 改为 Project 可见性，IR 实现只由四个 core static target 编译，dynamic wrapper 使用已有静态库；修复后的完整 CI 通过。
- 第一版 200-stage 夹具有五个 depth-writing shader，但 render pass 没有 depth attachment。按实际输出反射补上 depth attachment 后，整组重跑；未删掉失败 shaders，也未把夹具失败记为产品缺陷。
- 原 benchmark stdout/stderr 混写曾破坏 JSON 解析；成功数据从原日志恢复，随后分流输出。失败记录保留。
- 对缺失局部 autoreleasepool 的初步怀疑撤回：实际 pipeline 创建外层已有 pool，不能用该差异解释额外进程 footprint。
- 两次完整 frame capture 的 controller 分别因存储耗尽、诊断容量上限失败。后续离线检查发现第二次正常退出后的最终文件能完整解析出一帧，并已固定哈希；这修正了“controller 失败即录制不可用”的推断。基线回放先因缺显式 Vulkan loader 失败，配置修复后在 GFXReconstruct 的 graphics-pipeline dispatch table 查找处崩溃，尚无有效画面。原失败、录制及回放崩溃报告保留，未宣称烟雾正确。
- 较早的不同游戏时刻截图不能证明烟雾等价。连续帧显示两条路径都能出现烟雾，但缺少同一实际 smoke draw/input 的逐像素 oracle。

## 合入前仍需完成

1. 将绑定 CPU 回退降至约定门槛；候选优化需要定向回归与最终同指标确认。
2. 找到实际烟雾 draw，固定 shader、资源和 draw inputs，比较 MSL / IR-strict / IR-fast。通用 point/math/system-value 用例不能替代这项验收。
3. 解释并处理额外进程 footprint；已降低的 descriptor pool/Metal residual 与未降低的进程 residual 都需保留报告。
4. 实际 MSC 确定性拒绝与同设备负缓存覆盖已通过；stage-key 复用/失效 GPU 夹具已通过。CPU 层已验证正值 dropped/failed counters、负缓存、瞬时重试与并发拒绝。
5. 可选 ABI9 iOS arm64/macOS compiler framework 已构建并核验四个导出符号、二进制 SHA 和 iOS 17.0 最低部署版本；iOS 对象代码的 ABI 函数返回 9。iOS compiler 为 13.30 MB，Apple MSC 为 34.63 MB，原始厂商 SHA 保持不变。候选 manifest 已准备；默认清单仍须等匹配的 ABI9 native 进入维护 RC 后一起更新。仅构建，没有安装或操作手机；现有功能分支 pin 仍是实验状态。

不再通过六对整局 ABBA 或挑选负载接近的复跑寻找有利结果。数据分析方法已整理为本地 `melonx-graphics-acceptance` skill，并使用历史三组日志验证其计算。手机验收暂不在用户授权范围内；主机数据不能宣称真机动态加载、jetsam 或最终 FPS 已通过。
