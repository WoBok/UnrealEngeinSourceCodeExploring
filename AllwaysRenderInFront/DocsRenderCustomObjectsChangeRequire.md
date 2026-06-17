# 渲染定制物体 — 三处疑问的源码验证与修正

> 针对 `AllwayRenderFront.md` 方案中三处存疑点的源码级复核（UE5.4 源码版，分支 5.4）。
> 结论：**疑问 2、3 指出的错误成立，原文档需修正；疑问 1 的简化不成立（极性相反），需保留分支。**

---

## 疑问 1 —— `ShouldDraw` 能否简化为 `!bIsTranslucent && !bAfterTranslucent`？

### 结论：**不能。两个 Pass 对 `bAfterTranslucent` 的过滤极性相反，必须分支。**

### 源码事实

`FMobileBasePassMeshProcessor::ShouldDraw` 的**真实签名**（`MobileBasePass.cpp:828`）只接收材质，**没有 Proxy 参数**：

```cpp
bool FMobileBasePassMeshProcessor::ShouldDraw(const FMaterial& Material) const
{
    ...
    else
    {
        // opaque materials.
        return !bIsTranslucent;
    }
}
```

原 `AllwayRenderFront.md` 写的 `ShouldDraw(Material, Proxy)` 是**虚构签名**，源码里不存在。`bAfterTranslucent` 在 `ShouldDraw` 内根本拿不到 Proxy，必须改在能拿到 Proxy 的 `TryAddMeshBatch` / `AddMeshBatch` 里做分流。

`FMobileBasePassMeshProcessor` 构造函数（`MobileBasePass.cpp:810`）把 `InMeshPassType` 透传给基类 `FMeshPassProcessor`，基类成员为 `MeshPassType`（`MeshPassProcessor.h:2046`），可访问。但子类没有把它存成具名成员，所以若要在子类里判断"自己是哪个 Pass"，**需要新增一个 bool 成员**（推荐），或读基类 `MeshPassType`。

### 为什么单一表达式不成立

两个 Pass 由**两个不同的 Processor 实例**服务（`CreateMobileBasePassProcessor` vs `CreateMobileBasePassAfterTranslucentProcessor`），它们对同一个 `bAfterTranslucent` 标记要做**相反**的过滤：

| Processor 实例 | 期望行为 | 正确条件 |
|---|---|---|
| BasePass / MobileBasePassCSM | **排除**被标记物体 | `!bIsTranslucent && !bAfterTranslucent` |
| MobileBasePassAfterTranslucent | **只收**被标记物体 | `!bIsTranslucent &&  bAfterTranslucent` |

若两个实例都套用 `!bIsTranslucent && !bAfterTranslucent`：
- BasePass 实例：正确（排除标记物）✅
- AfterTranslucent 实例：被标记物 `bAfterTranslucent=true` → `!bAfterTranslucent=false` → **整 Pass 一条都不画** ❌

所以"两个 Pass 都靠 `bAfterTranslucent` 判断"的直觉是对的，但**极性必须翻转**，不能合并成一条表达式。

### 正确写法（推荐：不改 `ShouldDraw` 签名，在 `TryAddMeshBatch` 里分流）

`MobileBasePass.h` 给 `FMobileBasePassMeshProcessor` 加一个成员：

```cpp
// 构造时置位：仅 MobileBasePassAfterTranslucent 实例为 true
bool bAfterTranslucentPass = false;
```

构造函数（`MobileBasePass.cpp:810`）补一行：

```cpp
, bAfterTranslucentPass(InMeshPassType == EMeshPass::MobileBasePassAfterTranslucent)
```

`TryAddMeshBatch`（`MobileBasePass.cpp:851`，此处已有 `PrimitiveSceneProxy` 参数）开头插入分流，**仅在非半透 Processor 上生效**（半透 Processor `bTranslucentBasePass==true`，不参与此逻辑）：

```cpp
bool FMobileBasePassMeshProcessor::TryAddMeshBatch(
    const FMeshBatch& RESTRICT MeshBatch, uint64 BatchElementMask,
    const FPrimitiveSceneProxy* RESTRICT PrimitiveSceneProxy, int32 StaticMeshId,
    const FMaterialRenderProxy& MaterialRenderProxy, const FMaterial& Material)
{
    // ===== 新增：不透明物体的"延后绘制"分流 =====
    if (!bTranslucentBasePass)
    {
        const bool bAfterTranslucent =
            PrimitiveSceneProxy && PrimitiveSceneProxy->RenderOpaqueAfterTranslucency();

        // BasePass 实例(bAfterTranslucentPass=false) 只画未标记物；
        // AfterTranslucent 实例(bAfterTranslucentPass=true) 只画标记物。
        // 极性相反 —— 这正是不能用单一表达式的原因。
        if (bAfterTranslucent != bAfterTranslucentPass)
        {
            return true; // 不归属本 Pass，跳过
        }
    }
    // ============================================

    if (ShouldDraw(Material))   // 签名保持不变
    {
        ... // 原有 Process(...) 逻辑
    }
    return true;
}
```

极简等价形式（若你想写成一行）：

```cpp
// 仅当 !bTranslucentBasePass 时
!bIsTranslucent && (bAfterTranslucent == bAfterTranslucentPass)
```

> 注意 `CollectPSOInitializers`（`MobileBasePass.cpp:1066` 附近）里调用 `ShouldDraw` 时 Proxy 为 null，PSO 预缓存会**同时为两个 Pass 都生成 PSO**（这是期望行为，运行期再由上面的分流决定实际画哪个），无需改动。

---

## 疑问 2 —— `SceneVisibility.cpp` 里找不到那三个 `case EMeshPass::TranslucencyXxx`

### 结论：**原文档 §4.5 描述错误。`SetupMeshPass` 是通用循环，没有 per-pass `case`。**

### 源码事实

`FSceneRenderer::SetupMeshPass`（`SceneRendering.cpp:4196`）是一个**遍历 `EMeshPass::Num` 的通用 for 循环**，对每个带 `EMeshPassFlags::MainView` 标志的 Pass 自动 `CreateMeshPassProcessor` + `DispatchPassSetup`：

```cpp
// SceneRendering.cpp:4196
void FSceneRenderer::SetupMeshPass(FViewInfo& View, ...)
{
    const EShadingPath ShadingPath = GetFeatureLevelShadingPath(Scene->GetFeatureLevel());
    for (int32 PassIndex = 0; PassIndex < EMeshPass::Num; PassIndex++)
    {
        const EMeshPass::Type PassType = (EMeshPass::Type)PassIndex;
        if ((FPassProcessorManager::GetPassFlags(ShadingPath, PassType) & EMeshPassFlags::MainView) != EMeshPassFlags::None)
        {
            // Mobile: BasePass 和 MobileBasePassCSM 被跳过(它们在别处合并排序)
            if (ShadingPath == EShadingPath::Mobile && (PassType == EMeshPass::BasePass || PassType == EMeshPass::MobileBasePassCSM))
                continue;

            FMeshPassProcessor* MeshPassProcessor = FPassProcessorManager::CreateMeshPassProcessor(ShadingPath, PassType, ...);
            FParallelMeshDrawCommandPass& Pass = View.ParallelMeshDrawCommandPasses[PassIndex];
            Pass.DispatchPassSetup(Scene, View, ..., PassType, BasePassDepthStencilAccess, MeshPassProcessor, ...);
        }
    }
}
```

**含义**：只要新 Pass 满足两点，`SetupMeshPass` 会**自动**把它接入 `View.ParallelMeshDrawCommandPasses`，**无需手写任何 `case`**：

1. 加进 `EMeshPass` 枚举（疑问 2 原文档已对）；
2. 用 `REGISTER_MESHPASSPROCESSOR_AND_PSOCOLLECTOR(..., EMeshPassFlags::CachedMeshCommands | EMeshPassFlags::MainView)` 注册（带上 `MainView`）。

且 Mobile 的特殊跳过列表（`SceneRendering.cpp:4209`）只跳 `BasePass`/`MobileBasePassCSM`，新 Pass 不在内，会被正常 setup。✅

### 真正需要改的是"路由"——把标记物体喂给新 Pass

`SetupMeshPass` 只负责"为每个 Pass 建好绘制命令容器"，但**容器里有没有东西**取决于可见性收集阶段是否把 Mesh 加进该 Pass。这才是要改的地方，分两条路径：

#### 路径 A：动态 Mesh（`ComputeDynamicMeshRelevance`）

函数位于 `SceneVisibility.cpp:2186`，设置 `PassMask`。不透明物体当前在 `SceneVisibility.cpp:2211-2232` 被加进 `BasePass` / `MobileBasePassCSM`：

```cpp
// SceneVisibility.cpp:2211
if (ViewRelevance.bRenderInMainPass || ViewRelevance.bRenderCustomDepth)
{
    PassMask.Set(EMeshPass::BasePass);                                  // 2213
    ...
    if (ShadingPath == EShadingPath::Mobile)
    {
        PassMask.Set(EMeshPass::MobileBasePassCSM);                     // 2230
        View.NumVisibleDynamicMeshElements[EMeshPass::MobileBasePassCSM] += NumElements;
    }
    ...
}
```

需在此按 Proxy 标记分流。`PrimitiveSceneInfo` 在该函数参数里（`SceneVisibility.cpp:2193`），可取 `PrimitiveSceneInfo->Proxy`：

```cpp
if (ViewRelevance.bRenderInMainPass || ViewRelevance.bRenderCustomDepth)
{
    const bool bAfterTranslucent =
        ShadingPath == EShadingPath::Mobile
        && PrimitiveSceneInfo->Proxy
        && PrimitiveSceneInfo->Proxy->RenderOpaqueAfterTranslucency();

    if (bAfterTranslucent)
    {
        // 标记物体：只进新 Pass，不进 BasePass/CSM
        PassMask.Set(EMeshPass::MobileBasePassAfterTranslucent);
        View.NumVisibleDynamicMeshElements[EMeshPass::MobileBasePassAfterTranslucent] += NumElements;
    }
    else
    {
        PassMask.Set(EMeshPass::BasePass);
        View.NumVisibleDynamicMeshElements[EMeshPass::BasePass] += NumElements;

        if (ShadingPath == EShadingPath::Mobile)
        {
            PassMask.Set(EMeshPass::MobileBasePassCSM);
            View.NumVisibleDynamicMeshElements[EMeshPass::MobileBasePassCSM] += NumElements;
        }
        ... // 其余 SkyPass / Anisotropy / CustomDepth 等保持原样
    }
}
```

#### 路径 B：静态 Mesh（`AddCommandsForMesh` 显式调用）

静态缓存命令是**逐 Pass 显式 `AddCommandsForMesh`** 的，不是走 PassMask 循环。Mobile 不透明物体在 `SceneVisibility.cpp:1559-1569`：

```cpp
// SceneVisibility.cpp:1559
if (ShadingPath == EShadingPath::Mobile)
{
    if (!StaticMeshRelevance.bUseSkyMaterial)
    {
        DrawCommandPacket.AddCommandsForMesh(..., EMeshPass::BasePass);          // 1564
        if (!bMobileBasePassAlwaysUsesCSM)
        {
            DrawCommandPacket.AddCommandsForMesh(..., EMeshPass::MobileBasePassCSM); // 1567
        }
    }
    ...
}
```

这里需要拿到该静态 Mesh 所属 Primitive 的标记。`PrimitiveSceneInfo` 在作用域内（同函数参数），分流：

```cpp
if (ShadingPath == EShadingPath::Mobile)
{
    const bool bAfterTranslucent =
        PrimitiveSceneInfo && PrimitiveSceneInfo->Proxy
        && PrimitiveSceneInfo->Proxy->RenderOpaqueAfterTranslucency();

    if (!StaticMeshRelevance.bUseSkyMaterial)
    {
        if (bAfterTranslucent)
        {
            DrawCommandPacket.AddCommandsForMesh(..., EMeshPass::MobileBasePassAfterTranslucent);
        }
        else
        {
            DrawCommandPacket.AddCommandsForMesh(..., EMeshPass::BasePass);
            if (!bMobileBasePassAlwaysUsesCSM)
            {
                DrawCommandPacket.AddCommandsForMesh(..., EMeshPass::MobileBasePassCSM);
            }
        }
    }
    ...
}
```

> ⚠️ 静态路径的前提：`FPrimitiveSceneInfo::CacheMeshDrawCommands` 在缓存阶段必须已为新 Pass 生成过命令。由于 §疑问1 的 `TryAddMeshBatch` 分流在缓存时同样生效（缓存也走 `AddMeshBatch`/`TryAddMeshBatch`，且 Proxy 可用），标记物体会被缓存到 `MobileBasePassAfterTranslucent` 槽而非 `BasePass` 槽，`AddCommandsForMesh(..., MobileBasePassAfterTranslucent)` 才能取到命令。两处分流极性一致，闭环成立。

### 修正后的 §4.5 结论

| 原文档说法 | 实际情况 |
|---|---|
| "在 `SetupMeshPass` 里找到 `case EMeshPass::TranslucencyStandard` 加 case" | ❌ 不存在该 case；`SetupMeshPass` 是通用循环，注册带 `MainView` 即自动接入，**无需改 `SetupMeshPass`** |
| 需要改 SceneVisibility | ✅ 但改的是**动态 PassMask（2213 行附近）**和**静态 AddCommandsForMesh（1564 行附近）**，不是 setup |

---

## 疑问 3 —— `RenderMobileBasePassAfterTranslucent` 没写 Editor 渲染，且 `InstanceCullingDrawParams` 用法有误

### 结论：**两个问题都成立。应参照 `RenderMobileBasePass` 重写，但 Editor 图元不应重复绘制；`InstanceCullingDrawParams` 必须作为参数传入并先 `BuildRenderingCommands`。**

### 源码事实

`FMobileSceneRenderer::RenderMobileBasePass`（`MobileBasePassRendering.cpp:470`）真实结构：

```cpp
void FMobileSceneRenderer::RenderMobileBasePass(FRHICommandList& RHICmdList, const FViewInfo& View,
    const FInstanceCullingDrawParams* InstanceCullingDrawParams)
{
    CSV_SCOPED_TIMING_STAT_EXCLUSIVE(RenderBasePass);
    SCOPED_DRAW_EVENT(RHICmdList, MobileBasePass);
    SCOPE_CYCLE_COUNTER(STAT_BasePassDrawTime);
    SCOPED_GPU_STAT(RHICmdList, Basepass);

    RHICmdList.SetViewport(View.ViewRect.Min.X, View.ViewRect.Min.Y, 0, View.ViewRect.Max.X, View.ViewRect.Max.Y, 1);
    View.ParallelMeshDrawCommandPasses[EMeshPass::BasePass].DispatchDraw(nullptr, RHICmdList, InstanceCullingDrawParams);

    if (View.Family->EngineShowFlags.Atmosphere)
    {
        View.ParallelMeshDrawCommandPasses[EMeshPass::SkyPass].DispatchDraw(nullptr, RHICmdList, &SkyPassInstanceCullingDrawParams);
    }

    // editor primitives
    FMeshPassProcessorRenderState DrawRenderState;
    DrawRenderState.SetBlendState(TStaticBlendStateWriteMask<CW_RGBA>::GetRHI());
    DrawRenderState.SetDepthStencilAccess(Scene->DefaultBasePassDepthStencilAccess);
    DrawRenderState.SetDepthStencilState(TStaticDepthStencilState<true, CF_DepthNearOrEqual>::GetRHI());
    RenderMobileEditorPrimitives(RHICmdList, View, DrawRenderState, InstanceCullingDrawParams);
}
```

原文档 `RenderMobileBasePassAfterTranslucent` 的两个错误：

1. **`InstanceCullingDrawParams` 来源虚构**：原文用了不存在的成员 `MobileBasePassAfterTranslucentInstanceCullingDrawParams`，且从未给它 `BuildRenderingCommands`，`DispatchDraw` 拿到的是空数据。
2. **未处理 Editor 图元**（用户指出的点）——但方向需斟酌（见下）。

### `InstanceCullingDrawParams` 的正确链路

`FParallelMeshDrawCommandPass::DispatchDraw` 的第三参可为 `nullptr`，但开了 GPUScene 实例裁剪时必须传有效的 `InstanceCullingDrawParams`，且该 params 要先由 `BuildRenderingCommands` 填充（`MeshDrawCommands.h:152`）。Mobile 在 `BuildInstanceCullingDrawParams`（`MobileShadingRenderer.cpp:1433`）里为每个 Pass 调一次：

```cpp
// MobileShadingRenderer.cpp:1433
void FMobileSceneRenderer::BuildInstanceCullingDrawParams(FRDGBuilder& GraphBuilder, FViewInfo& View, FMobileRenderPassParameters* PassParameters)
{
    if (Scene->GPUScene.IsEnabled())
    {
        ...
        View.ParallelMeshDrawCommandPasses[EMeshPass::BasePass].BuildRenderingCommands(GraphBuilder, Scene->GPUScene, PassParameters->InstanceCullingDrawParams);
        View.ParallelMeshDrawCommandPasses[EMeshPass::SkyPass].BuildRenderingCommands(GraphBuilder, Scene->GPUScene, SkyPassInstanceCullingDrawParams);
        View.ParallelMeshDrawCommandPasses[StandardTranslucencyMeshPass].BuildRenderingCommands(GraphBuilder, Scene->GPUScene, TranslucencyInstanceCullingDrawParams);
        ...
    }
}
```

`HasAnyDraw()` 存在（`MeshDrawCommands.h:169`：`bool HasAnyDraw() const { return MaxNumDraws > 0; }`），可用。

**正确做法**：新 Pass 复用 BasePass 的 `PassParameters->InstanceCullingDrawParams`（同一个 uniform buffer 即可，实例裁剪数据是视图级共享的），并在 `BuildInstanceCullingDrawParams` 里为它 `BuildRenderingCommands` 一次；`RenderMobileBasePassAfterTranslucent` 把它当**参数**接收（与 `RenderMobileBasePass` 同构）。

### Editor 图元要不要画？

`RenderMobileEditorPrimitives`（`MobileBasePassRendering.cpp:493`）在主 `RenderMobileBasePass` 里**已经画过一次**编辑器图元（选择框、Gizmo、调试线等）。若在新 Pass 里再画一次 → **重复绘制 / 双倍开销 / 描边叠色**。

因此结论与用户直觉相反：**新 Pass 不应调用 `RenderMobileEditorPrimitives`**。它只是一个"在半透之上的不透明 overlay"，编辑器图元已在 BasePass 阶段完成，无需也不应在此重画。若确有"某些编辑器图元也要盖在半透之上"的需求，那是另一条独立特性（可另开一个 editor-primitive overlay pass），不在本次范围内。

### 修正后的 `RenderMobileBasePassAfterTranslucent`

`MobileBasePassRendering.h` 声明（与 `RenderMobileBasePass` 同构，带参数）：

```cpp
void RenderMobileBasePassAfterTranslucent(
    FRHICommandList& RHICmdList,
    const FViewInfo& View,
    const FInstanceCullingDrawParams* InstanceCullingDrawParams);
```

`MobileBasePassRendering.cpp` 实现：

```cpp
void FMobileSceneRenderer::RenderMobileBasePassAfterTranslucent(
    FRHICommandList& RHICmdList,
    const FViewInfo& View,
    const FInstanceCullingDrawParams* InstanceCullingDrawParams)
{
    if (!CVarMobileRenderOpaqueAfterTranslucency.GetValueOnRenderThread())
    {
        return;
    }

    const FParallelMeshDrawCommandPass& Pass =
        View.ParallelMeshDrawCommandPasses[EMeshPass::MobileBasePassAfterTranslucent];
    if (!Pass.HasAnyDraw())
    {
        return;
    }

    SCOPED_DRAW_EVENT(RHICmdList, MobileBasePassAfterTranslucent);
    SCOPED_GPU_STAT(RHICmdList, MobileBasePassAfterTranslucent);

    RHICmdList.SetViewport(
        View.ViewRect.Min.X, View.ViewRect.Min.Y, 0,
        View.ViewRect.Max.X, View.ViewRect.Max.Y, 1);

    // 深度行为由 PassProcessor 的 PassDrawRenderState 决定
    // （CF_Always + 不写深度 = 始终覆盖在半透之上）
    Pass.DispatchDraw(nullptr, RHICmdList, InstanceCullingDrawParams);

    // 注意：此处不调用 RenderMobileEditorPrimitives。
    // 编辑器图元已在主 RenderMobileBasePass 中绘制，重复绘制会导致叠加/双倍开销。
}
```

`BuildInstanceCullingDrawParams` 补一行（`MobileShadingRenderer.cpp:1444` 之后）：

```cpp
View.ParallelMeshDrawCommandPasses[EMeshPass::MobileBasePassAfterTranslucent]
    .BuildRenderingCommands(GraphBuilder, Scene->GPUScene, PassParameters->InstanceCullingDrawParams);
```

### 调用点（4 处 `RenderTranslucency` 之后）

`MobileShadingRenderer.cpp` 中 4 处 `RenderTranslucency(RHICmdList, View);`（行 **1623 / 1735 / 1985 / 2068**）之后各加一行，**传入对应作用域的 `InstanceCullingDrawParams`**：

| 行号 | 上下文 | 传入参数 |
|---|---|---|
| 1623 后 | 主 Forward 路径 | `&PassParameters->InstanceCullingDrawParams` |
| 1735 后 | 第二 Pass（SecondPassParameters） | `&SecondPassParameters->InstanceCullingDrawParams` |
| 1985 后 | 另一路径 | `&PassParameters->InstanceCullingDrawParams` |
| 2068 后 | 另一路径（SecondPassParameters） | `&SecondPassParameters->InstanceCullingDrawParams` |

例如 1623 处：

```cpp
// Draw translucency.
RenderTranslucency(RHICmdList, View);

// === 新增：半透之上的不透明 overlay ===
RenderMobileBasePassAfterTranslucent(RHICmdList, View, &PassParameters->InstanceCullingDrawParams);
```

> 这 4 处中，1623 位于 `NextSubpass()`（行 1614）之后的半透 subpass 内，新 overlay 在同一 subpass 内追加绘制颜色即可；深度状态由 Processor 的 `PassDrawRenderState`（`CF_Always` + 不写深度）保证"覆盖"。其余 3 处同理确认其 subpass/RT 绑定上下文后再插入。

---

## 汇总：原文档需修正项

| 位置 | 原文档问题 | 修正 |
|---|---|---|
| §4.4.1 `ShouldDraw` | 虚构 `ShouldDraw(Material, Proxy)` 签名；试图用单一 `!bAfterTranslucent` 表达式 | 保持 `ShouldDraw(Material)` 不变；在 `TryAddMeshBatch` 用 `bAfterTranslucent == bAfterTranslucentPass` 分流（极性相反，必须分支） |
| §4.5 `SetupMeshPass` | 称有 `case EMeshPass::TranslucencyStandard` 需加 case | `SetupMeshPass` 是通用循环，注册带 `MainView` 即自动接入，**不改 setup**；改的是 `SceneVisibility.cpp:2213`（动态 PassMask）与 `:1564`（静态 AddCommandsForMesh） |
| §4.6 `RenderMobileBasePassAfterTranslucent` | 用不存在的成员 `MobileBasePassAfterTranslucentInstanceCullingDrawParams`；未提 Editor | 改为接收 `InstanceCullingDrawParams` 参数；在 `BuildInstanceCullingDrawParams`(`:1444`) 补 `BuildRenderingCommands`；**不调用** `RenderMobileEditorPrimitives`（已在主 BasePass 画过） |
| §4.6 调用点 | 仅说"4 处之后追加"未给参数 | 4 处（`:1623/:1735/:1985/:2068`）分别传 `&PassParameters->InstanceCullingDrawParams` / `&SecondPassParameters->InstanceCullingDrawParams` |

---

## 附：关键源码定位（UE5.4，已逐行核对）

```
MobileBasePass.cpp:810     FMobileBasePassMeshProcessor 构造（透传 InMeshPassType 给基类）
MobileBasePass.cpp:828     ShouldDraw(const FMaterial&) —— 无 Proxy 参数
MobileBasePass.cpp:851     TryAddMeshBatch(..., PrimitiveSceneProxy, ...) —— Proxy 可用，分流在此做
MobileBasePass.cpp:867     AddMeshBatch(..., PrimitiveSceneProxy, ...)
MeshPassProcessor.h:2046   FMeshPassProcessor::MeshPassType 基类成员
MeshPassProcessor.h:2042   class FMeshPassProcessor
MeshDrawCommands.h:115     class FParallelMeshDrawCommandPass
MeshDrawCommands.h:152     BuildRenderingCommands
MeshDrawCommands.h:165     DispatchDraw(..., InstanceCullingDrawParams = nullptr)
MeshDrawCommands.h:169     HasAnyDraw()

SceneRendering.cpp:4196    FSceneRenderer::SetupMeshPass —— 通用 for 循环，无 per-pass case
SceneRendering.cpp:4209    Mobile 跳过 BasePass/MobileBasePassCSM 的特例

SceneVisibility.cpp:1559   静态 Mesh：Mobile 不透明 AddCommandsForMesh(BasePass/CSM)
SceneVisibility.cpp:1564   AddCommandsForMesh(..., EMeshPass::BasePass)
SceneVisibility.cpp:1567   AddCommandsForMesh(..., EMeshPass::MobileBasePassCSM)
SceneVisibility.cpp:2186   static void ComputeDynamicMeshRelevance(...)
SceneVisibility.cpp:2213   PassMask.Set(EMeshPass::BasePass)  —— 动态分流点
SceneVisibility.cpp:2230   PassMask.Set(EMeshPass::MobileBasePassCSM)

MobileBasePassRendering.cpp:470  RenderMobileBasePass(RHICmdList, View, InstanceCullingDrawParams)
MobileBasePassRendering.cpp:490  RenderMobileEditorPrimitives(...) —— 已在主 BasePass 执行
MobileBasePassRendering.cpp:493  RenderMobileEditorPrimitives 定义
MobileShadingRenderer.cpp:1433   BuildInstanceCullingDrawParams —— 需为新 Pass 补 BuildRenderingCommands
MobileShadingRenderer.cpp:1441   BasePass.BuildRenderingCommands(..., PassParameters->InstanceCullingDrawParams)
MobileShadingRenderer.cpp:1609   RenderMobileBasePass(..., &PassParameters->InstanceCullingDrawParams)
MobileShadingRenderer.cpp:1623   RenderTranslucency —— 调用点 1
MobileShadingRenderer.cpp:1735   RenderTranslucency —— 调用点 2（SecondPassParameters）
MobileShadingRenderer.cpp:1985   RenderTranslucency —— 调用点 3
MobileShadingRenderer.cpp:2068   RenderTranslucency —— 调用点 4（SecondPassParameters）
```
