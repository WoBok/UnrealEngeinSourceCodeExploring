# MobileShadingRenderer 中 SceneVisibility.h 的使用清单

> 目标文件：`Engine/Source/Runtime/Renderer/Private/MobileShadingRenderer.cpp`（2358 行）
> 接口头文件：`Engine/Source/Runtime/Renderer/Private/SceneVisibility.h`
> 文档版本：v1.0 ｜ 分析对象：大闹天宫 MR01 主引擎 UE5 移动端分支
> 配套文档：[SceneVisibility2.md](./SceneVisibility2.md) — SceneVisibility.cpp 解析

---

## 0. 总览

`MobileShadingRenderer.cpp` 通过 `SceneVisibility.h` 暴露的 **5 个公开 API** 接入可见性流水线，**调用点共 10 处**（含 1 处 typedef、1 处参数类型、1 处类成员）：

| # | 引用 API | 行号 | 上下文 |
|---|---|---|---|
| 1 | `IVisibilityTaskData` 类型 | 925 | `OnRenderBegin()` 返回值 |
| 2 | `FInitViewTaskDatas` 构造 | 1027 | 把 IVisibilityTaskData 装入 `InitViewTaskDatas` 包裹 |
| 3 | `FInitViewTaskDatas::VisibilityTaskData` 字段 | 489 / 490 / 722 / 726 | `InitViews()` 内多处使用 |
| 4 | `IVisibilityTaskData::ProcessRenderThreadTasks()` | 489 | `InitViews()` 中触发 RT 任务 |
| 5 | `IVisibilityTaskData::FinishGatherDynamicMeshElements()` | 490 | `InitViews()` 中完成 GDME |
| 6 | `IVisibilityTaskData::Finish()` | 722 | `InitViews()` 中等待所有任务 |
| 7 | `IVisibilityTaskData::GetViewCommandsPerView()` | 726 | `InitViews()` 中取出 `FViewCommands` |
| 8 | `FViewCommands` 参数类型 | 377 | `SetupMobileBasePassAfterShadowInit()` 形参 |
| 9 | `FViewCommands` 局部引用 | 386 | `SetupMobileBasePassAfterShadowInit()` 内取用 |
| 10 | `View.PrimitiveVisibilityMap` / `View.StaticMeshVisibilityMap` | 367 / 370 / 373 | `PrepareViewVisibilityLists()` 间接消费（SceneVisibility.cpp 写入） |

> 此外 `IVisibilityTaskData::StartGatherDynamicMeshElements()` 在本文件中**未被显式调用**（`BeginInitViews` 才用），符合移动端走 RenderThread 串行路径的事实。

---

## 1. 头文件依赖

`MobileShadingRenderer.cpp` 第 25 行 `#include "SceneRendering.h"`，**间接** 拉入 `SceneVisibility.h`：

```
MobileShadingRenderer.cpp :25
   #include "SceneRendering.h"
       ↓
       (内部) #include "SceneVisibility.h"   ← SceneRendering.h 引用
       ↓
       获得 IVisibilityTaskData / FViewCommands / LaunchVisibilityTasks
```

> **MR01 注意**：因为是间接依赖，移动端在删除 `SceneVisibility.h` 中的导出符号时需要同步检查 `SceneRendering.h` 的 include 关系。

---

## 2. 全部调用点详解

### 2.1 【#1】 启动可见性任务（925 行）

```cpp
// File: MobileShadingRenderer.cpp:925
IVisibilityTaskData* VisibilityTaskData = OnRenderBegin(GraphBuilder);
```

**所在函数**：`void FMobileSceneRenderer::Render(FRDGBuilder& GraphBuilder)`（910 行起）

**调用路径**：
```
Render()
  └─ OnRenderBegin(GraphBuilder)              ← FSceneRenderer 虚函数
       └─ Scene::Update() 异步回调
            └─ LaunchVisibilityTasks()         ← SceneVisibility.cpp:383
                 └─ new FVisibilityTaskData()
                      └─ LaunchVisibilityTasks() → IVisibilityTaskData*
```

**移动端特性**：
- `OnRenderBegin` 在 `FSceneRenderer::OnRenderBegin`（`SceneRendering.cpp:3560`）里通过 `Scene::Update` 的 `PostStaticMeshUpdate` 回调触发
- 回调触发后 `VisibilityTaskData` 才是非空，所以**必须先于 `InitViews` 调用 OnRenderBegin**
- 移动端 `OnRenderBegin` 不需要特殊处理

**返回类型**：
```cpp
class IVisibilityTaskData  // SceneVisibility.h:32
{
    virtual ~IVisibilityTaskData() {}
    virtual void StartGatherDynamicMeshElements() = 0;
    virtual void ProcessRenderThreadTasks() = 0;
    virtual void FinishGatherDynamicMeshElements(...) = 0;
    virtual void Finish() = 0;
    virtual TArrayView<FViewCommands> GetViewCommandsPerView() = 0;
    virtual UE::Tasks::FTask GetFrustumCullTask() const = 0;
    virtual UE::Tasks::FTask GetComputeRelevanceTask() const = 0;
    virtual UE::Tasks::FTask GetLightVisibilityTask() const = 0;
    virtual bool IsTaskWaitingAllowed() const = 0;
};
```

---

### 2.2 【#2】 包装成 `FInitViewTaskDatas`（1027 行）

```cpp
// File: MobileShadingRenderer.cpp:1027
FInitViewTaskDatas InitViewTaskDatas(VisibilityTaskData);
```

**定义**（`SceneRendering.h:2669-2677`）：

```cpp
struct FInitViewTaskDatas
{
    FInitViewTaskDatas(IVisibilityTaskData* InVisibilityTaskData)
        : VisibilityTaskData(InVisibilityTaskData)
    {}

    IVisibilityTaskData* VisibilityTaskData;
    FDynamicShadowsTaskData* DynamicShadows = nullptr;
};
```

**作用**：把可见性任务数据 + 后续的 `DynamicShadows` 任务数据**统一封装**，作为 `InitViews()` 函数的入参。

**设计动机**：
- 桌面端 `FDeferredShadingSceneRenderer` 与移动端 `FMobileSceneRenderer` 共用同一份 `InitViews` 签名
- 把 `IVisibilityTaskData*` 收纳进结构体，避免函数签名膨胀
- `DynamicShadows` 是后续 `InitDynamicShadows()` 调用后填入的子任务（移动端在 713 行设置）

---

### 2.3 【#3-#7】 `InitViews()` 内的 4 个 API 调用（433-733 行）

**函数签名**：
```cpp
void FMobileSceneRenderer::InitViews(
    FRDGBuilder& GraphBuilder,
    FSceneTexturesConfig& SceneTexturesConfig,
    FInstanceCullingManager& InstanceCullingManager,
    FVirtualTextureUpdater* VirtualTextureUpdater,
    FInitViewTaskDatas& TaskDatas)
```

#### 2.3.1 【#4】 `ProcessRenderThreadTasks()`（489 行）

```cpp
// File: MobileShadingRenderer.cpp:489
TaskDatas.VisibilityTaskData->ProcessRenderThreadTasks();
```

**接口**：基类 `FSceneRenderer` 在 `OnRenderBegin` 中已触发任务图（`Tasks.BeginInitVisibility.Trigger()` 等），但**任务**本身还在工作线程上跑；`ProcessRenderThreadTasks()` 让 **render thread 等待并处理**任务结果。

**内部**（`SceneVisibility.cpp:4472-4579`）：

```cpp
void FVisibilityTaskData::ProcessRenderThreadTasks()
{
    // RenderThread 模式（移动端走这里）：
    for (FVisibilityViewPacket& ViewPacket : ViewPackets) {
        ViewPacket.BeginInitVisibility();
        UpdatePrimitiveFading(...);
    }
    SceneRenderer.WaitOcclusionTests(RHICmdList);
    // ... 处理 occlusion 与 relevance
    GatherDynamicMeshElements(*DynamicMeshElements.PrimitiveViewMasks);

    // Parallel 模式（移动端不会走这里）：
    if (DynamicMeshElements.CommandPipe) {
        Tasks.DynamicMeshElementsPipe->Wait(ENamedThreads::GetRenderThread_Local());
    }
    // ...
}
```

**移动端**：
- 由于 `IsMobilePlatform()` 强制走 `EVisibilityTaskSchedule::RenderThread`（SceneVisibility.cpp:3244），这里走**同步串行路径**
- 整个调用相当于"在 render thread 上同步跑完所有视锥裁剪 + 遮挡剔除 + ComputeRelevance"
- 包含：视锥剔除、遮挡剔除、Relevance 计算、GDME 收集

#### 2.3.2 【#5】 `FinishGatherDynamicMeshElements()`（490 行）

```cpp
// File: MobileShadingRenderer.cpp:490
TaskDatas.VisibilityTaskData->FinishGatherDynamicMeshElements(
    BasePassDepthStencilAccess, InstanceCullingManager, VirtualTextureUpdater);
```

**签名**（`SceneVisibility.h:44`）：
```cpp
virtual void FinishGatherDynamicMeshElements(
    FExclusiveDepthStencil::Type BasePassDepthStencilAccess,
    FInstanceCullingManager& InstanceCullingManager,
    FVirtualTextureUpdater* VirtualTextureUpdater) = 0;
```

**内部**（`SceneVisibility.cpp:4581-4598`）：
```cpp
void FVisibilityTaskData::FinishGatherDynamicMeshElements(
    FExclusiveDepthStencil::Type BasePassDepthStencilAccess,
    FInstanceCullingManager& InstanceCullingManager,
    FVirtualTextureUpdater* VirtualTextureUpdater)
{
    FVirtualTextureSystem::Get().WaitForTasks(VirtualTextureUpdater);
    Tasks.DynamicMeshElements.Wait();
    DynamicMeshElements.ContextContainer.Submit(RHICmdList);

    Tasks.MeshPassSetup = UE::Tasks::Launch(UE_SOURCE_LOCATION, [this, ...]
    {
        SetupMeshPasses(BasePassDepthStencilAccess, InstanceCullingManager);
    }, TaskConfig.TaskPriority);
    // ...
}
```

**移动端作用**：
- 等所有 GDME context 跑完（移动端都在 RT 上所以基本零耗时）
- **关键**：启动一个 **MeshPassSetup 任务**（`Tasks.MeshPassSetup`），这个任务里调用 `SetupMeshPasses()`（SceneVisibility.cpp:4401）
- `SetupMeshPasses` 内部对每个视图调用 `SceneRenderer.SetupMeshPass(View, ..., ViewCommands, InstanceCullingManager)`，但**移动端特意不在这里做 BasePass**，而是延后到 `SetupMobileBasePassAfterShadowInit`（见 2.3.4）

#### 2.3.3 【#6】 `Finish()`（722 行）

```cpp
// File: MobileShadingRenderer.cpp:722
TaskDatas.VisibilityTaskData->Finish();
```

**内部**（`SceneVisibility.cpp:4600-4613`）：
```cpp
void FVisibilityTaskData::Finish()
{
    Tasks.ComputeRelevance.Wait();
    Tasks.FinalizeRelevance.Wait();
    Tasks.DynamicMeshElements.Wait();
    Tasks.MeshPassSetup.Wait();

    ViewPackets.Empty();
    DynamicMeshElements.DynamicPrimitives.Empty();
    Allocator.BulkDelete();
    bFinished = true;
}
```

**调用时机**：在 `InitViews` 末尾，**所有后续 shadow / BasePass 任务启动之前**，确保可见性任务图完全 sync 到 render thread。

**移动端**：
- 由于是串行模式，所有 `Wait()` 几乎瞬间返回
- 释放 `FSceneRenderingBulkObjectAllocator` 上的所有可见性阶段分配的内存

#### 2.3.4 【#7】 `GetViewCommandsPerView()`（726 行）

```cpp
// File: MobileShadingRenderer.cpp:726
SetupMobileBasePassAfterShadowInit(
    BasePassDepthStencilAccess,
    TaskDatas.VisibilityTaskData->GetViewCommandsPerView(),
    InstanceCullingManager);
```

**返回类型**（`SceneVisibility.h:50`）：
```cpp
virtual TArrayView<FViewCommands> GetViewCommandsPerView() = 0;
```

**`FViewCommands` 结构**（`SceneVisibility.h:15-30`）：
```cpp
class FViewCommands
{
public:
    FViewCommands()
    {
        for (int32 PassIndex = 0; PassIndex < EMeshPass::Num; ++PassIndex)
        {
            NumDynamicMeshCommandBuildRequestElements[PassIndex] = 0;
        }
    }

    TStaticArray<FMeshCommandOneFrameArray, EMeshPass::Num> MeshCommands;
    TStaticArray<int32, EMeshPass::Num> NumDynamicMeshCommandBuildRequestElements;
    TStaticArray<TArray<const FStaticMeshBatch*, SceneRenderingAllocator>, EMeshPass::Num> DynamicMeshCommandBuildRequests;
    TStaticArray<TArray<EMeshDrawCommandCullingPayloadFlags, SceneRenderingAllocator>, EMeshPass::Num> DynamicMeshCommandBuildFlags;
};
```

**调用上下文**：
```cpp
if (bRendererOutputFinalSceneColor) {
    SetupMobileBasePassAfterShadowInit(BasePassDepthStencilAccess,
        TaskDatas.VisibilityTaskData->GetViewCommandsPerView(),
        InstanceCullingManager);
    // ...
}
```

**为何在 `InitViews` 末尾才取**：`FViewCommands` 数组在 `SetupMeshPasses`（`SceneVisibility.cpp:4401`）跑完后才填充完毕，必须在 `Finish()` 之后才能安全访问。

---

### 2.4 【#8-#9】 `SetupMobileBasePassAfterShadowInit()` 消费 `FViewCommands`（377-427 行）

```cpp
// File: MobileShadingRenderer.cpp:377
void FMobileSceneRenderer::SetupMobileBasePassAfterShadowInit(
    FExclusiveDepthStencil::Type BasePassDepthStencilAccess,
    TArrayView<FViewCommands> ViewCommandsPerView,        // ★ SceneVisibility.h 类型
    FInstanceCullingManager& InstanceCullingManager)
{
    for (int32 ViewIndex = 0; ViewIndex < AllViews.Num(); ++ViewIndex)
    {
        FViewInfo& View = *AllViews[ViewIndex];
        FViewCommands& ViewCommands = ViewCommandsPerView[ViewIndex];    // ★ #9
        // ...
    }
}
```

**与桌面端的差异（核心）**：

| 步骤 | 桌面端（Deferred） | 移动端（MR01） |
|---|---|---|
| 1. 视锥裁剪 | SceneVisibility → 全部 | 同 |
| 2. ComputeRelevance | SceneVisibility → 全部 | 同 |
| 3. SetupMeshPasses | `SceneRenderer.SetupMeshPass()` 一次出所有 MeshPass | `SetupMeshPasses` **只**做非 BasePass 系列 |
| 4. Shadow | shadow pass 串在 BasePass 之前 | shadow pass 必须在 BasePass 之前完成（CSM 深度） |
| 5. BasePass | SetupMeshPasses 阶段已出 | **延后**到 `SetupMobileBasePassAfterShadowInit`（即此函数） |

**为何这样设计**：
- 移动端 `MobileBasePassCSM` Pass **必须**在 CSM shadow 渲染**之后**才能用 CSM receiver map，否则要重做
- 桌面端没有这个依赖，因为阴影 → BasePass 是分开的
- 因此 `FViewCommands` 的 `MeshCommands[EMeshPass::BasePass]` / `MeshCommands[EMeshPass::MobileBasePassCSM]` **必须延后处理**

**函数体关键代码**（388-425 行）：
```cpp
FMeshPassProcessor* MeshPassProcessor = FPassProcessorManager::CreateMeshPassProcessor(
    EShadingPath::Mobile, EMeshPass::BasePass, ...);

FMeshPassProcessor* BasePassCSMMeshPassProcessor = FPassProcessorManager::CreateMeshPassProcessor(
    EShadingPath::Mobile, EMeshPass::MobileBasePassCSM, ...);

// 一次性产出 BasePass + MobileBasePassCSM 两个 Pass 的 draw command
FParallelMeshDrawCommandPass& Pass = View.ParallelMeshDrawCommandPasses[EMeshPass::BasePass];
Pass.DispatchPassSetup(
    Scene, View,
    FInstanceCullingContext(PassName, ShaderPlatform, &InstanceCullingManager, ViewIds, ...),
    EMeshPass::BasePass,
    BasePassDepthStencilAccess,
    MeshPassProcessor,
    View.DynamicMeshElements,
    &View.DynamicMeshElementsPassRelevance,
    View.NumVisibleDynamicMeshElements[EMeshPass::BasePass],
    ViewCommands.DynamicMeshCommandBuildRequests[EMeshPass::BasePass],     // ★ 消费
    ViewCommands.DynamicMeshCommandBuildFlags[EMeshPass::BasePass],        // ★ 消费
    ViewCommands.NumDynamicMeshCommandBuildRequestElements[EMeshPass::BasePass],
    ViewCommands.MeshCommands[EMeshPass::BasePass],                          // ★ 输出
    BasePassCSMMeshPassProcessor,
    &ViewCommands.MeshCommands[EMeshPass::MobileBasePassCSM]);              // ★ 输出
```

**MR01 性能注意**：
- `DispatchPassSetup` 在 render thread 同步执行 → 单帧可能耗时数毫秒
- 移动端有 `MobileBasePassCSM` 二次 dispatch，与 BasePass 共享同一份 `FParallelMeshDrawCommandPass` 实例（巧妙避免两次 Setup）
- 实例化 (`EInstanceCullingMode::Stereo`) 仅在 ISR（Instanced Stereo Rendering）下开启

---

### 2.5 【#10】 `PrepareViewVisibilityLists()` 消费 `FViewInfo` 可见性位图（359-375 行）

```cpp
// File: MobileShadingRenderer.cpp:359
void FMobileSceneRenderer::PrepareViewVisibilityLists()
{
    for (auto& View : Views)
    {
        FMobileCSMVisibilityInfo& MobileCSMVisibilityInfo = View.MobileCSMVisibilityInfo;
        // Init list of primitives that can receive Dynamic CSM.
        MobileCSMVisibilityInfo.MobilePrimitiveCSMReceiverVisibilityMap.Init(
            false, View.PrimitiveVisibilityMap.Num());

        // Init static mesh visibility info for CSM drawlist
        MobileCSMVisibilityInfo.MobileCSMStaticMeshVisibilityMap.Init(
            false, View.StaticMeshVisibilityMap.Num());

        // Init static mesh visibility info for default drawlist that excludes meshes in CSM only drawlist.
        MobileCSMVisibilityInfo.MobileNonCSMStaticMeshVisibilityMap = View.StaticMeshVisibilityMap;  // ★ 直接拷贝
    }
}
```

**调用点**（718 行）：
```cpp
if (bDynamicShadows) {
    TaskDatas.DynamicShadows = InitDynamicShadows(GraphBuilder, InstanceCullingManager);
} else {
    // TODO: only do this when CSM + static is required.
    PrepareViewVisibilityLists();
}
```

**与 SceneVisibility 的关系**：
- `View.PrimitiveVisibilityMap` / `View.StaticMeshVisibilityMap` 由 SceneVisibility.cpp 的 `BeginInitVisibility`（3506 行）和 `FComputeAndMarkRelevance::Finalize`（2136-2180 行）写入
- 移动端 Shadow 系统在此基础上再分出 **CSM receiver** 和 **non-CSM** 两组静态网格

**`FMobileCSMVisibilityInfo` 字段**（`SceneRendering.h`）：
```cpp
struct FMobileCSMVisibilityInfo
{
    FSceneBitArray MobilePrimitiveCSMReceiverVisibilityMap;     // 接收 CSM 阴影的 primitive
    FSceneBitArray MobileCSMStaticMeshVisibilityMap;            // 在 CSM drawlist 的 static mesh
    FSceneBitArray MobileNonCSMStaticMeshVisibilityMap;         // 不在 CSM drawlist 的 static mesh（直接拷贝）
};
```

**MR01 关键点**：
- `MobileNonCSMStaticMeshVisibilityMap = View.StaticMeshVisibilityMap;` 是**直接拷贝**（不是 Init）
- 假设：BasePass 阶段 non-CSM 静态网格 = 所有可见静态网格（CSM 单独的 mesh 在另一个 Pass 处理）
- 注释 TODO 显示：当前实现没考虑 "CSM + 静态" 的细分场景，可能有精度损失

---

## 3. 时间线：移动端一帧的可见性数据流

```
┌────────────────────────────────────────────────────────────────────┐
│ Frame N 开始                                                        │
│                                                                      │
│ ┌─────────────────────────────────────────────────────────────────┐ │
│ │ MobileShadingRenderer::Render()       [MobileShadingRenderer.cpp:910]│
│ │                                                                  │ │
│ │  ① IVisibilityTaskData* V = OnRenderBegin(GraphBuilder);        │ │
│ │     └─→ SceneRendering.cpp::FSceneRenderer::OnRenderBegin         │ │
│ │         └─→ Scene::Update 异步回调                                │ │
│ │             └─→ LaunchVisibilityTasks (SceneVisibility.cpp:383)   │ │
│ │                 ★ 任务图已发射                                     │ │
│ │                                                                  │ │
│ │  ② FInitViewTaskDatas InitViewTaskDatas(V);    [1027 行]         │ │
│ │                                                                  │ │
│ │  ③ InitViews(...)                                                │ │
│ │     │                                                            │ │
│ │     ├─ PreVisibilityFrameSetup                                  │ │
│ │     ├─ InstanceCullingManager 初始化                             │ │
│ │     ├─ View.RHI 资源初始化                                       │ │
│ │     │                                                            │ │
│ │     ├─ V->ProcessRenderThreadTasks()                [489 行]     │ │
│ │     │    ★ RenderThread 模式：同步完成                            │ │
│ │     │       视锥 → 遮挡 → Relevance                              │ │
│ │     │                                                            │ │
│ │     ├─ V->FinishGatherDynamicMeshElements(...)      [490 行]     │ │
│ │     │    ★ 启动 MeshPassSetup 任务（异步）                        │ │
│ │     │       装配 除 BasePass 外所有 MeshPass                      │ │
│ │     │                                                            │ │
│ │     ├─ InitDynamicShadows(...)                                 │ │
│ │     │                                                            │ │
│ │     ├─ PrepareViewVisibilityLists()                [718 行]     │ │
│ │     │    ★ 从 PrimitiveVisibilityMap 切分 CSM / non-CSM          │ │
│ │     │                                                            │ │
│ │     ├─ V->Finish()                                [722 行]     │ │
│ │     │    ★ 等所有可见性任务结束                                    │ │
│ │     │                                                            │ │
│ │     └─ SetupMobileBasePassAfterShadowInit(                       │ │
│ │          ...,                                                    │ │
│ │          V->GetViewCommandsPerView(),               [726 行]     │ │
│ │          ...)                                                    │ │
│ │        ★ 装配 BasePass + MobileBasePassCSM                       │ │
│ │                                                                  │ │
│ │  ④ 后续 Pass 渲染（MobileDepthPrepass / MobileBasePass / ...）   │ │
│ │                                                                  │ │
│ └─────────────────────────────────────────────────────────────────┘ │
└────────────────────────────────────────────────────────────────────┘
```

---

## 4. 移动端 vs 桌面端 API 使用差异

| API | 桌面端 `FDeferredShadingSceneRenderer` | 移动端 `FMobileSceneRenderer` |
|---|---|---|
| `OnRenderBegin()` | 调用 | 调用（925 行） |
| `FInitViewTaskDatas` 包装 | 调用 | 调用（1027 行） |
| `ProcessRenderThreadTasks()` | **异步**（Parallel 模式） | **同步串行**（RenderThread 模式，489 行） |
| `FinishGatherDynamicMeshElements()` | 调用（5187 行附近） | 调用（490 行） |
| `Finish()` | 调用 | 调用（722 行） |
| `GetViewCommandsPerView()` | 一次（SetupMeshPasses 阶段就消费完） | **延后**到 `SetupMobileBasePassAfterShadowInit`（726 行） |
| `PrepareViewVisibilityLists()` | **没有**（仅移动端私有函数） | 调用（718 行） |
| `StartGatherDynamicMeshElements()` | 调用 | **未调用**（自动随 RT 路径触发） |

---

## 5. MR01 项目级注意事项

### 5.1 移动端未使用的 API（潜在风险）

以下 `IVisibilityTaskData` 接口在 MobileShadingRenderer.cpp **未调用**：

```cpp
virtual UE::Tasks::FTask GetFrustumCullTask() const = 0;        // 未用
virtual UE::Tasks::FTask GetComputeRelevanceTask() const = 0;   // 未用
virtual UE::Tasks::FTask GetLightVisibilityTask() const = 0;     // 未用
virtual void StartGatherDynamicMeshElements() = 0;              // 未用
virtual bool IsTaskWaitingAllowed() const = 0;                   // 未用
```

**原因**：移动端走 RenderThread 串行模式，**不需要**在调用方主动 sync 任务。

**风险**：如果未来切换到 Parallel 模式（MMV、多 GPU 移动端），需要补齐这些调用点。建议在 MR01 中加注释或 TODO 标注。

### 5.2 `MobileNonCSMStaticMeshVisibilityMap` 的潜在语义问题

`MobileShadingRenderer.cpp:373` 的拷贝：
```cpp
MobileCSMVisibilityInfo.MobileNonCSMStaticMeshVisibilityMap = View.StaticMeshVisibilityMap;
```

**问题**：如果某个 static mesh **同时**在 CSM 路径和 non-CSM 路径需要被渲染，移动端当前没有去重机制，会重复画。

**建议**：在 `PrepareViewVisibilityLists` 末尾补充：
```cpp
// 移除已在 CSM drawlist 的 static mesh
MobileCSMVisibilityInfo.MobileNonCSMStaticMeshVisibilityMap &= ~MobileCSMVisibilityInfo.MobileCSMStaticMeshVisibilityMap;
```
（注意：需要等到 shadow pass 完成确定 `MobileCSMStaticMeshVisibilityMap` 后再计算；当前实现是 Init，不存在 AND 关系。）

### 5.3 `SetupMobileBasePassAfterShadowInit` 的同步成本

由于 `DispatchPassSetup` 在 render thread 上同步执行，包含：
- 遍历 `ViewCommands.DynamicMeshCommandBuildRequests[EMeshPass::BasePass]`（可能上千个）
- 状态排序（StateBucket sort）
- 写出 `ParallelMeshDrawCommandPasses[EMeshPass::BasePass]`

**MR01 优化方向**：
- 大场景下 `DispatchPassSetup` 可达 5-10ms
- 可考虑拆分为 `AsyncDispatchPassSetup`（参考桌面端的 `SetupMeshPasses` 在任务里跑）
- 移动端 GPU 一般不卡这里，**但 RT 时间可能成为瓶颈**

### 5.4 头文件间接依赖

`MobileShadingRenderer.cpp` 通过 `SceneRendering.h` 间接 include `SceneVisibility.h`，**修改 SceneVisibility.h 任何导出符号都会触发 MR01 移动端 Renderer 全量重编译**。

建议在 MR01 中：
- 在 `MobileShadingRenderer.cpp` 顶部**显式**写 `#include "SceneVisibility.h"`，避免隐式依赖
- 与 `SceneVisibility.h` 维护者沟通接口稳定性的预期

---

## 6. 移动端特有的"漏出"数据

下表列出 SceneVisibility 给移动端**额外**提供、桌面端用不到的中间数据：

| 字段 | 来源 | 移动端用途 |
|---|---|---|
| `View.StaticMeshVisibilityMap` | `FComputeAndMarkRelevance::Finalize` | 切分 CSM / non-CSM 静态网格（`PrepareViewVisibilityLists`） |
| `View.PrimitiveVisibilityMap` | `FrustumCull` | 切分 CSM receiver primitive（`PrepareViewVisibilityLists`） |
| `View.PrimitiveFadeUniformBuffers` | `UpdatePrimitiveFading` | 移动端 distance fade 效果（基类统一处理） |
| `View.PrimitiveFadeUniformBufferMap` | 同上 | 同上 |
| `View.PotentiallyFadingPrimitiveMap` | 同上 | 同上 |

> **重要**：这些字段不是 `FViewCommands` 的一部分，但**在 `FViewCommands` 填充之前已经可用**。所以移动端的 `PrepareViewVisibilityLists` 在 718 行（即 `InitDynamicShadows` 之后）调用是没问题的。

---

## 7. 总结

`MobileShadingRenderer.cpp` 对 `SceneVisibility.h` 的使用**相当克制**——总共 5 个 API、10 个引用点，主要集中在 `InitViews()` 函数内。相比桌面端：

1. **多了**：`PrepareViewVisibilityLists()` 切分 CSM 路径
2. **少了**：`StartGatherDynamicMeshElements`（自动触发）、任务图同步 API（走串行）
3. **延后了**：`GetViewCommandsPerView` 不在 `SetupMeshPasses` 消费，而是延后到 `SetupMobileBasePassAfterShadowInit`（因为 BasePass 必须等 Shadow 完成）

**MR01 落地建议**：
- 在 `MobileShadingRenderer.cpp` 顶部显式 include `SceneVisibility.h`
- 加 `// TODO: MR01 Mobile Parallel MMV` 注释到 489 行附近，提示未来切并行模式
- 监控 `SetupMobileBasePassAfterShadowInit` 的 `DispatchPassSetup` 耗时，移动端可能成为 RT 瓶颈
- 排查 `MobileNonCSMStaticMeshVisibilityMap` 与 `MobileCSMStaticMeshVisibilityMap` 的潜在重复绘制
