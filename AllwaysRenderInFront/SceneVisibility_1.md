# MobileShadingRenderer.cpp 对 SceneVisibility 的使用清单

> **目标文件**: `Engine/Source/Runtime/Renderer/Private/MobileShadingRenderer.cpp`(2358 行)
> **范围**: 该文件对 `SceneVisibility.h` / `SceneVisibilityPrivate.h` / `SceneVisibility.cpp` 中导出符号的所有引用
> **文档版本**: v1.0 / 2026-06-18

---

## 0. 核心结论(先看这里)

`MobileShadingRenderer.cpp` **不直接 `#include "SceneVisibility.h"`**。SceneVisibility 提供的接口通过以下头**间接引入**:

| 中转头 | 提供给 MobileShadingRenderer 的能力 |
|---|---|
| `SceneRendering.h` (line 25) | `FViewInfo`、`FInitViewTaskDatas`、`IVisibilityTaskData`、`FViewCommands` |
| `ScenePrivate.h` (line 26) | `FScene::Octree`、`FScene::PrimitiveSceneProxies` 等场景数据 |
| `SceneOcclusion.h` (line 63) | `FSceneOcclusion` / HZB 相关类型 |
| `MobileBasePassRendering.h` (line 58) | `EMeshPass::MobileBasePassCSM` 等 |
| `InstanceCulling/InstanceCullingManager.h` (line 61) | `FInstanceCullingManager` |

直接使用、来自 SceneVisibility 体系的符号按用途可分为 **3 类**:

1. **`IVisibilityTaskData*` 接口调用**(SceneVisibility.h:32)
2. **`FViewInfo` 上的可见性位图字段**(SceneRendering.h,被 SceneVisibility 写)
3. **Mobile 端专属的 CSM 可见性辅助结构**(FMobileCSMVisibilityInfo,SceneRendering.h:109)

---

## 1. 包含关系(没找到直接 include)

```
$ grep -n "^#include" MobileShadingRenderer.cpp | grep -i visibility
(no match)

$ grep -n "SceneVisibility" MobileShadingRenderer.cpp
(no match for the literal string "SceneVisibility")
```

**关键观察**:MobileShadingRenderer.cpp 的 include 列表(行 7-76)**完全没有** `SceneVisibility.h` 字符串。但所有 SceneVisibility 公共 API 都通过 `SceneRendering.h`(行 25)间接可见,因此无需显式 include。

---

## 2. `IVisibilityTaskData` 接口的所有使用

`IVisibilityTaskData` 在 `SceneVisibility.h:32` 定义,是 `LaunchVisibilityTasks` 返回的句柄。MobileShadingRenderer 通过它控制整帧 visibility 任务的推进。

### 2.1 获取句柄:`OnRenderBegin`(行 925)

```cpp
// MobileShadingRenderer.cpp:910-927  (Render 主入口)
void FMobileSceneRenderer::Render(FRDGBuilder& GraphBuilder)
{
    if (!ViewFamily.EngineShowFlags.Rendering) { return; }
    ...
    IVisibilityTaskData* VisibilityTaskData = OnRenderBegin(GraphBuilder);
    ...
}
```

- **`OnRenderBegin`** 在 `SceneRendering.h:2474` 声明,实际定义位于 `SceneVisibility.cpp`(`FSceneRenderer::OnRenderBegin`),内部调用 `LaunchVisibilityTasks` (SceneVisibility.cpp:383),启动 `FVisibilityTaskData::LaunchVisibilityTasks` (SceneVisibility.cpp:4089)。
- 返回的 `IVisibilityTaskData*` 实际指向 `FVisibilityTaskData`,持有整帧的 frustum cull / occlusion / relevance / GDME 任务图。

### 2.2 装入 `FInitViewTaskDatas`(行 1027)

```cpp
// MobileShadingRenderer.cpp:1027
FInitViewTaskDatas InitViewTaskDatas(VisibilityTaskData);
```

- `FInitViewTaskDatas` 在 `SceneRendering.h:2669` 定义,内部持有 `VisibilityTaskData` 与 `DynamicShadows`。
- 这是把 `IVisibilityTaskData*` 透传到 `InitViews()` 的容器。
- 同样模式在 `DeferredShadingRenderer.cpp:1474`、`SceneHitProxyRendering.cpp:571/620`、`SceneVisibility.cpp:5458/5631` 都出现。

### 2.3 推进任务图:`ProcessRenderThreadTasks`(行 489)

```cpp
// MobileShadingRenderer.cpp:489
TaskDatas.VisibilityTaskData->ProcessRenderThreadTasks();
```

- 调用时机:`InitViews()` 中,在 `PreVisibilityFrameSetup(GraphBuilder)` (行 451) 之后,`FinishGatherDynamicMeshElements` (行 490) 之前。
- 对应 `SceneVisibility.cpp:4472` `FVisibilityTaskData::ProcessRenderThreadTasks`,做 RT 端串行模式下的 occlusion cull、relevance finalize、GDME 收尾。

### 2.4 收尾 GDME:`FinishGatherDynamicMeshElements`(行 490)

```cpp
// MobileShadingRenderer.cpp:490
TaskDatas.VisibilityTaskData->FinishGatherDynamicMeshElements(
    BasePassDepthStencilAccess,
    InstanceCullingManager,
    VirtualTextureUpdater);
```

- 对应 `SceneVisibility.h:38` 的 `IVisibilityTaskData::FinishGatherDynamicMeshElements`。
- 触发 dynamic mesh elements 的 finalize,把命令写入 `ViewCommands.MeshCommands[EMeshPass::...]`。
- 在 RDG 调度 BasePass **之前** 必须完成,所以此调用在 InitViews 中很靠前。

### 2.5 等待任务结束:`Finish`(行 722)

```cpp
// MobileShadingRenderer.cpp:718-722
if (bDynamicShadows)
{
    TaskDatas.DynamicShadows = InitDynamicShadows(GraphBuilder, InstanceCullingManager);
}
else
{
    PrepareViewVisibilityLists();  // 见 §3
}
...
TaskDatas.VisibilityTaskData->Finish();   // 行 722
```

- 对应 `SceneVisibility.h:39` `IVisibilityTaskData::Finish()`、`SceneVisibility.cpp:4600` `FVisibilityTaskData::Finish`。
- **必须** 在 `SetupMobileBasePassAfterShadowInit` 之前调用,否则 `ViewCommands.MeshCommands` 可能尚未写完。

### 2.6 取得每 View 命令:`GetViewCommandsPerView`(行 726)

```cpp
// MobileShadingRenderer.cpp:726
SetupMobileBasePassAfterShadowInit(
    BasePassDepthStencilAccess,
    TaskDatas.VisibilityTaskData->GetViewCommandsPerView(),
    InstanceCullingManager);
```

- 对应 `SceneVisibility.h:40`。
- 返回 `TArrayView<FViewCommands>`,每个 view 一个,包含该 view 的 `MeshCommands[EMeshPass::Num]`。
- 传给 `SetupMobileBasePassAfterShadowInit` 用于 `DispatchPassSetup` (行 410-425)。

### 2.7 使用 `FViewCommands` 的字段(行 420-425)

```cpp
// MobileShadingRenderer.cpp:419-425
View.NumVisibleDynamicMeshElements[EMeshPass::BasePass],
ViewCommands.DynamicMeshCommandBuildRequests[EMeshPass::BasePass],
ViewCommands.DynamicMeshCommandBuildFlags[EMeshPass::BasePass],
ViewCommands.NumDynamicMeshCommandBuildRequestElements[EMeshPass::BasePass],
ViewCommands.MeshCommands[EMeshPass::BasePass],
...
&ViewCommands.MeshCommands[EMeshPass::MobileBasePassCSM]);  // 行 425
```

- `FViewCommands` 在 `SceneVisibility.h:15-30` 定义,每个 view 一份,每个 `EMeshPass` 一组:
  - `MeshCommands[EMeshPass::Xxx]` — 该 pass 的 `FMeshCommandOneFrameArray`
  - `DynamicMeshCommandBuildRequests[EMeshPass::Xxx]` — 待 build 的 `FStaticMeshBatch*` 列表
  - `DynamicMeshCommandBuildFlags[EMeshPass::Xxx]` — 每个 request 的 `EMeshDrawCommandCullingPayloadFlags`
  - `NumDynamicMeshCommandBuildRequestElements[EMeshPass::Xxx]` — 每个 request 的 element 数

### 2.8 接口调用总表

| 行号 | 调用 | 来源 API |
|---|---|---|
| 925 | `OnRenderBegin(GraphBuilder)` 返回 `IVisibilityTaskData*` | `SceneVisibility.cpp` 的 `FSceneRenderer::OnRenderBegin` |
| 489 | `ProcessRenderThreadTasks()` | `SceneVisibility.h:36` / `SceneVisibility.cpp:4472` |
| 490 | `FinishGatherDynamicMeshElements(BasePassDepthStencilAccess, InstanceCullingManager, VirtualTextureUpdater)` | `SceneVisibility.h:38` |
| 722 | `Finish()` | `SceneVisibility.h:39` / `SceneVisibility.cpp:4600` |
| 726 | `GetViewCommandsPerView()` | `SceneVisibility.h:40` |

---

## 3. `FViewInfo` 上的可见性位图字段

SceneVisibility 写入这些位图,MobileShadingRenderer 读取它们做后续 dispatch。完整字段清单见 `SceneVisibility.md` §3.1,此处只列 MobileShadingRenderer 实际用到的:

### 3.1 `PrimitiveVisibilityMap`(行 367)

```cpp
// MobileShadingRenderer.cpp:367
MobileCSMVisibilityInfo.MobilePrimitiveCSMReceiverVisibilityMap.Init(
    false, View.PrimitiveVisibilityMap.Num());
```

- 用途:复制 `View.PrimitiveVisibilityMap` 的大小,初始化 CSM 接收者位图。
- 写入者:`SceneVisibility.cpp` 的 `FrustumCull` / `PrecomputedOcclusionCull` / `ComputeAndMarkRelevance`(位图本身在 SceneVisibility 之前的 InitViews 阶段被填充)。

### 3.2 `StaticMeshVisibilityMap`(行 370, 373)

```cpp
// MobileShadingRenderer.cpp:370-373
MobileCSMVisibilityInfo.MobileCSMStaticMeshVisibilityMap.Init(
    false, View.StaticMeshVisibilityMap.Num());

MobileCSMVisibilityInfo.MobileNonCSMStaticMeshVisibilityMap = View.StaticMeshVisibilityMap;
```

- 用途:
  - 行 370:为 CSM 静态 mesh 单独建一份位图。
  - 行 373:**直接拷贝** `View.StaticMeshVisibilityMap` 作为"非 CSM 静态 mesh"的默认列表(后续阴影/Cull pass 会从中剔除 CSM-only mesh)。
- 写入者:`SceneVisibility.cpp` 的 `FrustumCull` (`:687`) 与 `FComputeAndMarkRelevance::ComputeRelevance` (`:1299`)。

### 3.3 字段使用总表

| 行号 | 字段 | 类型 | 写入位置 |
|---|---|---|---|
| 367 | `View.PrimitiveVisibilityMap` (`.Num()`) | `FSceneBitArray` | SceneVisibility.cpp FrustumCull/OcclusionCull/Relevance |
| 370 | `View.StaticMeshVisibilityMap` (`.Num()`) | `FSceneBitArray` | 同上 |
| 373 | `View.StaticMeshVisibilityMap` (赋值) | `FSceneBitArray` | 同上 |
| 419 | `View.NumVisibleDynamicMeshElements[EMeshPass::BasePass]` | `TStaticArray<int32>` | SceneVisibility.cpp GDME |
| 417-418 | `View.DynamicMeshElements` / `DynamicMeshElementsPassRelevance` | `TArray<FMeshBatch>` / `TArray<FMeshPassMask>` | SceneVisibility.cpp GDME |

---

## 4. `FMobileCSMVisibilityInfo` —— Mobile 专属辅助

这是为 Mobile 端 CSM (Cascaded Shadow Map) 设计的额外可见性集合,**不在 SceneVisibility.h 中**,而是 `SceneRendering.h:109` 定义,在 `PrepareViewVisibilityLists` 中初始化。

### 4.1 定义位置(SceneRendering.h:109-126)

```cpp
// SceneRendering.h:109 (从 MobileShadingRenderer 的视角理解)
class FMobileCSMVisibilityInfo
{
    FSceneBitArray MobilePrimitiveCSMReceiverVisibilityMap;     // 接收 CSM 的 primitive
    FSceneBitArray MobileCSMStaticMeshVisibilityMap;           // CSM 静态 mesh
    FSceneBitArray MobileNonCSMStaticMeshVisibilityMap;        // 非 CSM 静态 mesh
    bool bMobileDynamicCSMInUse;
    bool bAlwaysUseCSM;
    FMobileCSMVisibilityInfo() : bMobileDynamicCSMInUse(false), bAlwaysUseCSM(false) {}
};
```

### 4.2 初始化位置(MobileShadingRenderer.cpp:359-375)

```cpp
// MobileShadingRenderer.cpp:359
void FMobileSceneRenderer::PrepareViewVisibilityLists()
{
    // Prepare view's visibility lists.
    // TODO: only do this when CSM + static is required.
    for (auto& View : Views)
    {
        FMobileCSMVisibilityInfo& MobileCSMVisibilityInfo = View.MobileCSMVisibilityInfo;
        // Init list of primitives that can receive Dynamic CSM.
        MobileCSMVisibilityInfo.MobilePrimitiveCSMReceiverVisibilityMap.Init(false, View.PrimitiveVisibilityMap.Num());

        // Init static mesh visibility info for CSM drawlist
        MobileCSMVisibilityInfo.MobileCSMStaticMeshVisibilityMap.Init(false, View.StaticMeshVisibilityMap.Num());

        // Init static mesh visibility info for default drawlist that excludes meshes in CSM only drawlist.
        MobileCSMVisibilityInfo.MobileNonCSMStaticMeshVisibilityMap = View.StaticMeshVisibilityMap;
    }
}
```

**三个位图的设计意图**:

| 位图 | 来源 | 用途 |
|---|---|---|
| `MobilePrimitiveCSMReceiverVisibilityMap` | 全 false,大小与 `PrimitiveVisibilityMap` 同 | 后由 CSM rendering pass 标记"接收 CSM"的 primitive |
| `MobileCSMStaticMeshVisibilityMap` | 全 false,大小与 `StaticMeshVisibilityMap` 同 | 收集要走 `EMeshPass::MobileBasePassCSM` 的静态 mesh |
| `MobileNonCSMStaticMeshVisibilityMap` | **直接拷贝** `View.StaticMeshVisibilityMap` | 收集要走 `EMeshPass::BasePass` 的静态 mesh(剔除 CSM-only) |

### 4.3 调用位置(行 718)

```cpp
// MobileShadingRenderer.cpp:707-720
if (bRendererOutputFinalSceneColor)
{
    const bool bDynamicShadows = ViewFamily.EngineShowFlags.DynamicShadows;
    if (bDynamicShadows)
    {
        // Setup dynamic shadows.
        TaskDatas.DynamicShadows = InitDynamicShadows(GraphBuilder, InstanceCullingManager);
    }
    else
    {
        // TODO: only do this when CSM + static is required.
        PrepareViewVisibilityLists();
    }
}
```

**调用条件**:只在**关闭 dynamic shadow**时才调用,因为 dynamic shadow 路径会自行处理 CSM。

---

## 5. `BeginOcclusionScope` —— 遮挡反馈(行 769)

```cpp
// MobileShadingRenderer.cpp:769-778
static void BeginOcclusionScope(FRDGBuilder& GraphBuilder, TArray<FViewInfo>& Views)
{
    for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ViewIndex++)
    {
        FViewInfo& View = Views[ViewIndex];
        if (View.ShouldRenderView() && View.ViewState && View.ViewState->OcclusionFeedback.IsInitialized())
        {
            View.ViewState->OcclusionFeedback.BeginOcclusionScope(GraphBuilder);
        }
    }
}
```

- 这是 `FOcclusionFeedback` 的包装,位于 `SceneOcclusion.h`(行 63 include)。
- 与 SceneVisibility.cpp 的 `FGPUOcclusionFeedback` 体系配合:SceneVisibility 在 `OcclusionCullPrimitive` (`:2454`) 收集反馈,这里在每帧 BasePass 之前打开"新 occlusion scope"以分配 ID。

### 5.1 调用位置(行 1037-1040)

```cpp
// MobileShadingRenderer.cpp:1037
if (bRendererOutputFinalSceneColor)
{
    BeginOcclusionScope(GraphBuilder, Views);
}
```

**调用时机**:`InitViews` 完成后,scene extensions 调度之前。

---

## 6. `SetupMobileBasePassAfterShadowInit` —— 消费 ViewCommands(行 377)

虽然函数名没出现"Visibility",但**这是 Mobile 端消费 SceneVisibility 输出的关键环节**。

### 6.1 函数签名与作用

```cpp
// MobileShadingRenderer.cpp:377
void FMobileSceneRenderer::SetupMobileBasePassAfterShadowInit(
    FExclusiveDepthStencil::Type BasePassDepthStencilAccess,
    TArrayView<FViewCommands> ViewCommandsPerView,         // ← 来自 SceneVisibility
    FInstanceCullingManager& InstanceCullingManager)
```

- 内部为每个 view:
  1. 创建 `EMeshPass::BasePass` 与 `EMeshPass::MobileBasePassCSM` 的 `FMeshPassProcessor` (行 388-390)。
  2. 排序 `FParallelMeshDrawCommandPass` (行 402-407)。
  3. **从 `ViewCommands` 取每个 pass 的 `MeshCommands`/`DynamicMeshCommandBuildRequests`/`Flags`/`NumElements`** (行 419-425),传给 `DispatchPassSetup` (行 410)。
- 这是把 SceneVisibility 写入的 `FViewCommands` 转化为 GPU 端 `FParallelMeshDrawCommandPass` 的桥梁。

### 6.2 调用位置(行 726)

```cpp
// MobileShadingRenderer.cpp:724-727
if (bRendererOutputFinalSceneColor)
{
    SetupMobileBasePassAfterShadowInit(
        BasePassDepthStencilAccess,
        TaskDatas.VisibilityTaskData->GetViewCommandsPerView(),
        InstanceCullingManager);
    ...
}
```

**调用条件**:`bRendererOutputFinalSceneColor` 为真,且必须在 `VisibilityTaskData->Finish()` 之后。

---

## 7. 一帧内 SceneVisibility 调用的时间线

```
T0  GameThread: Scene 提交 (PrimitiveSceneProxy 等)
    --- RT 切到 RT ---

T1  RT:  FMobileSceneRenderer::Render()         ← MobileShadingRenderer.cpp:910
T2  RT:     IVisibilityTaskData* = OnRenderBegin  ← 925 [触发 SceneVisibility.cpp:383 LaunchVisibilityTasks]
                  ↓ 任务图开始异步执行
T3  RT:     FInitViewTaskDatas InitViewTaskDatas(VisibilityTaskData)  ← 1027
T4  RT:     InitViews(GraphBuilder, ..., InstanceCullingManager, ..., InitViewTaskDatas)  ← 1033
T5  RT:        PreVisibilityFrameSetup                                     ← 451 (SceneVisibility.cpp:4635)
T6  RT:        VisibilityTaskData->ProcessRenderThreadTasks()             ← 489 (SceneVisibility.cpp:4472)
                  ↓ RT 端串行执行: occlusion cull / relevance / GDME
T7  RT:        VisibilityTaskData->FinishGatherDynamicMeshElements(...)    ← 490 (SceneVisibility.h:38)
                  ↓ ViewCommands.MeshCommands 已填充
T8  RT:        PostVisibilityFrameSetup(ILCTaskData)                      ← 496 (SceneVisibility.cpp:5398)

T9  RT:        InitDynamicShadows() 或 PrepareViewVisibilityLists()       ← 713/718 [后者建立 FMobileCSMVisibilityInfo]
T10 RT:        VisibilityTaskData->Finish()                                ← 722 [等待任务图结束]
T11 RT:        BeginOcclusionScope                                         ← 1039 [打开 occlusion feedback scope]
T12 RT:        SetupMobileBasePassAfterShadowInit(                        ← 726
                     ..., GetViewCommandsPerView(), InstanceCullingManager)
                  ↓ 把 ViewCommands 装入 FParallelMeshDrawCommandPass
T13 RT:        RDG Passes(BasePass / Translucency / Shadow / PostProcess)
```

---

## 8. 与桌面 DeferredShadingRenderer 的对比

| 阶段 | MobileShadingRenderer | DeferredShadingRenderer |
|---|---|---|
| 获取 IVisibilityTaskData | `OnRenderBegin(GraphBuilder)` (925) | `OnRenderBegin(GraphBuilder)` (1474) |
| 初始化 TaskDatas 容器 | `FInitViewTaskDatas InitViewTaskDatas(VisibilityTaskData)` (1027) | `FInitViewTaskDatas InitViewTaskDatas = OnRenderBegin(...)` (1474) |
| ProcessRenderThreadTasks | 行 489 | `DeferredShadingRenderer.cpp` 内对应位置 |
| FinishGatherDynamicMeshElements | 行 490 | 同上 |
| PostVisibilityFrameSetup | 行 496 | 同上 |
| **CSM 可见性特殊处理** | **`PrepareViewVisibilityLists()` (行 718)** | 无对应 |
| **Setup MeshPass** | **`SetupMobileBasePassAfterShadowInit()` (行 726)** | `SetupMeshPasses` 路径不同,直接调 `View.ParallelMeshDrawCommandPasses[...].DispatchPassSetup` |
| BeginOcclusionScope | 行 1039 | 同名函数 (`DeferredShadingRenderer.cpp`) |

**关键差异**:

1. **Mobile 多一道 CSM 可见性拆分**(`PrepareViewVisibilityLists`),因为 Mobile 端把 `EMeshPass::BasePass` 进一步拆成 `BasePass` 与 `MobileBasePassCSM` 两个 pass,需要独立的 visibility 位图。
2. **Mobile 端 BasePass 的 mesh pass processor 创建在 `SetupMobileBasePassAfterShadowInit` 中**,而不是在 `BeginInitViews` 期间直接做(后者是桌面端路径),目的是让 CSM 阴影初始化之后再排序 front-to-back。
3. **共享 API**:`IVisibilityTaskData` / `FViewCommands` / `FViewInfo` 字段读写完全一致。

---

## 9. 关键引用一览(代码行号表)

| 引用位置 | 内容 |
|---|---|
| MobileShadingRenderer.cpp:25 | `#include "SceneRendering.h"` |
| MobileShadingRenderer.cpp:26 | `#include "ScenePrivate.h"` |
| MobileShadingRenderer.cpp:58 | `#include "MobileBasePassRendering.h"` |
| MobileShadingRenderer.cpp:61 | `#include "InstanceCulling/InstanceCullingManager.h"` |
| MobileShadingRenderer.cpp:63 | `#include "SceneOcclusion.h"` |
| MobileShadingRenderer.cpp:359-375 | `FMobileSceneRenderer::PrepareViewVisibilityLists` |
| MobileShadingRenderer.cpp:367 | `View.PrimitiveVisibilityMap.Num()` |
| MobileShadingRenderer.cpp:370 | `View.StaticMeshVisibilityMap.Num()` |
| MobileShadingRenderer.cpp:373 | `View.StaticMeshVisibilityMap`(赋值) |
| MobileShadingRenderer.cpp:377-427 | `FMobileSceneRenderer::SetupMobileBasePassAfterShadowInit` |
| MobileShadingRenderer.cpp:388 | `EMeshPass::BasePass` |
| MobileShadingRenderer.cpp:390 | `EMeshPass::MobileBasePassCSM` |
| MobileShadingRenderer.cpp:417-425 | `FViewCommands` 的 MeshCommands/BuildRequests/BuildFlags 字段读取 |
| MobileShadingRenderer.cpp:419 | `View.NumVisibleDynamicMeshElements[EMeshPass::BasePass]` |
| MobileShadingRenderer.cpp:438 | `InitViews` 形参 `FInitViewTaskDatas& TaskDatas` |
| MobileShadingRenderer.cpp:489 | `TaskDatas.VisibilityTaskData->ProcessRenderThreadTasks()` |
| MobileShadingRenderer.cpp:490 | `TaskDatas.VisibilityTaskData->FinishGatherDynamicMeshElements(...)` |
| MobileShadingRenderer.cpp:718 | `PrepareViewVisibilityLists()` 调用点 |
| MobileShadingRenderer.cpp:722 | `TaskDatas.VisibilityTaskData->Finish()` |
| MobileShadingRenderer.cpp:726 | `SetupMobileBasePassAfterShadowInit(..., GetViewCommandsPerView(), ...)` |
| MobileShadingRenderer.cpp:769-778 | `BeginOcclusionScope` |
| MobileShadingRenderer.cpp:910 | `FMobileSceneRenderer::Render` 主入口 |
| MobileShadingRenderer.cpp:925 | `IVisibilityTaskData* VisibilityTaskData = OnRenderBegin(GraphBuilder)` |
| MobileShadingRenderer.cpp:1027 | `FInitViewTaskDatas InitViewTaskDatas(VisibilityTaskData)` |
| MobileShadingRenderer.cpp:1033 | `InitViews(GraphBuilder, SceneTexturesConfig, InstanceCullingManager, VirtualTextureUpdater.Get(), InitViewTaskDatas)` |
| MobileShadingRenderer.cpp:1039 | `BeginOcclusionScope(GraphBuilder, Views)` |
| **SceneVisibility.h:32** | `class IVisibilityTaskData` 定义 |
| **SceneVisibility.h:36** | `ProcessRenderThreadTasks()` 接口 |
| **SceneVisibility.h:38** | `FinishGatherDynamicMeshElements(...)` 接口 |
| **SceneVisibility.h:39** | `Finish()` 接口 |
| **SceneVisibility.h:40** | `GetViewCommandsPerView()` 接口 |
| **SceneVisibility.h:69** | `LaunchVisibilityTasks` 声明 |
| **SceneVisibility.cpp:383** | `LaunchVisibilityTasks` 定义 |
| **SceneVisibility.cpp:4089** | `FVisibilityTaskData::LaunchVisibilityTasks` |
| **SceneVisibility.cpp:4472** | `FVisibilityTaskData::ProcessRenderThreadTasks` |
| **SceneVisibility.cpp:4600** | `FVisibilityTaskData::Finish` |
| **SceneVisibility.cpp:4635** | `FSceneRenderer::PreVisibilityFrameSetup` |
| **SceneVisibility.cpp:5398** | `FSceneRenderer::PostVisibilityFrameSetup` |
| **SceneRendering.h:109** | `class FMobileCSMVisibilityInfo` 定义 |
| **SceneRendering.h:1381** | `FMobileCSMVisibilityInfo MobileCSMVisibilityInfo;` 字段 |
| **SceneRendering.h:2474** | `IVisibilityTaskData* OnRenderBegin(FRDGBuilder&)` 声明 |
| **SceneRendering.h:2669** | `struct FInitViewTaskDatas` 定义 |
| **SceneRendering.h:2679** | `void InitViews(...)` 声明 |

---

## 10. 配套参考

- `SceneVisibility.md` —— SceneVisibility.cpp 总体解析(本仓库)
- `AllwayRenderFront.md` —— Mobile Forward 延后渲染方案(`EMeshPass::MobileBasePassCSM` 在 MobileShadingRenderer.cpp:390 使用)
- `DeferredShadingRenderer.cpp:1474` —— 桌面端的 `OnRenderBegin` 调用方式
- `SceneHitProxyRendering.cpp:571/620` —— 另一种调用方式(Editor 选择/命中测试)
- `SceneVisibility.cpp:4635` / `:5398` —— `Pre/PostVisibilityFrameSetup` 的定义

---

## 11. 一句话总结

**`MobileShadingRenderer.cpp` 通过 `SceneRendering.h` 间接消费 `IVisibilityTaskData` 接口、读取 `FViewInfo` 可见性位图、初始化 `FMobileCSMVisibilityInfo` 三个 CSM 辅助位图,然后在 `SetupMobileBasePassAfterShadowInit` 中把 `FViewCommands` 转化为 `EMeshPass::BasePass` 与 `EMeshPass::MobileBasePassCSM` 的 GPU draw command —— 没有任何 SceneVisibility 的私有类型或内部辅助类泄漏到这个文件。**