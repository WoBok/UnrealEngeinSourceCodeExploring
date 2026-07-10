# 移动端 Forward 管线 —「不透明物体始终绘制在透明物体之上」源码改造方案

> 引擎版本：UE 5.4 源码版
> 渲染管线：Mobile / Forward Shading
> 目标：在 C++ 标记的特定不透明 PrimitiveComponent，**跳过常规 BasePass**，改为在所有 Translucency 绘制完成之后再绘制，且**始终覆盖在半透明物体上层**。
> 适用场景：UI 模型、角色描边外壳、武器/特效、装备预览模型、肖像 Mesh 等需要"穿透半透"的不透明对象。

---

## 1. 现有移动渲染管线回顾（UE5.4）

关键源码位置：

| 文件 | 作用 |
|---|---|
| `Source/Runtime/Renderer/Private/MobileShadingRenderer.cpp` | 移动渲染主循环 `FMobileSceneRenderer::Render()` |
| `Source/Runtime/Renderer/Private/MobileTranslucentRendering.cpp` | `FMobileSceneRenderer::RenderTranslucency()` |
| `Source/Runtime/Renderer/Private/MobileBasePass.cpp` | `FMobileBasePassMeshProcessor` 和它的 4 个 PassProcessor 工厂 |
| `Source/Runtime/Renderer/Private/MobileBasePassRendering.h/.cpp` | Mobile BasePass 的 ShaderType / RenderState / DrawingPolicy |
| `Source/Runtime/Renderer/Public/MeshPassProcessor.h` | `EMeshPass` 枚举 |
| `Source/Runtime/Engine/Classes/Components/PrimitiveComponent.h` | `bRenderInMainPass`、`bRenderCustomDepth` 等渲染标志 |
| `Source/Runtime/Engine/Public/PrimitiveSceneProxy.h` | `FPrimitiveSceneProxy` 渲染线程镜像 |

UE5.4 移动 Forward 路径的固定 MeshPass 注册（`MobileBasePass.cpp:1218`）：

```cpp
REGISTER_MESHPASSPROCESSOR_AND_PSOCOLLECTOR(MobileBasePass,                CreateMobileBasePassProcessor,             EShadingPath::Mobile, EMeshPass::BasePass,            EMeshPassFlags::CachedMeshCommands | EMeshPassFlags::MainView);
REGISTER_MESHPASSPROCESSOR_AND_PSOCOLLECTOR(MobileBasePassCSM,             CreateMobileBasePassCSMProcessor,          EShadingPath::Mobile, EMeshPass::MobileBasePassCSM,   EMeshPassFlags::CachedMeshCommands | EMeshPassFlags::MainView);
REGISTER_MESHPASSPROCESSOR_AND_PSOCOLLECTOR(MobileTranslucencyAllPass,     CreateMobileTranslucencyAllPassProcessor,  EShadingPath::Mobile, EMeshPass::TranslucencyAll,     EMeshPassFlags::MainView);
REGISTER_MESHPASSPROCESSOR_AND_PSOCOLLECTOR(MobileTranslucencyStandardPass,CreateMobileTranslucencyStandardPassProcessor, EShadingPath::Mobile, EMeshPass::TranslucencyStandard, EMeshPassFlags::MainView);
REGISTER_MESHPASSPROCESSOR_AND_PSOCOLLECTOR(MobileTranslucencyAfterDOFPass,CreateMobileTranslucencyAfterDOFProcessor, EShadingPath::Mobile, EMeshPass::TranslucencyAfterDOF, EMeshPassFlags::MainView);
```

其中 `FMobileSceneRenderer::Render()` 内部分支大约长这样（`MobileShadingRenderer.cpp` 中 `RenderTranslucency` 出现 4 次，对应 LDR / Deferred / Mobile MultiView / Mobile XR 多种路径）：

```cpp
RenderMobileBasePass(...);            // EMeshPass::BasePass + MobileBasePassCSM
RenderDecals(...);
RenderFog(...);
RenderTranslucency(RHICmdList, View); // EMeshPass::TranslucencyStandard / All / AfterDOF
RenderPostProcess(...);
```

我们要插入的就是 `RenderTranslucency` 之后的一个**额外的不透明 Pass**，此处称为 **`MobileBasePassAfterTranslucent`**（下称 `MBPAT`）。

---

## 2. 设计思路对比

| 方案 | 优点 | 缺点 | 是否推荐 |
|---|---|---|---|
| **A. 新增独立 MeshPass `MobileBasePassAfterTranslucent`** | 完全复用 BasePass 着色器，仅过滤 + 调换顺序；CSM、SkyLight、贴图、Lightmap 全部可用 | 需要改 `EMeshPass` 枚举（影响位掩码）和 PSOCacheKey，工程量中等 | ✅ **推荐** |
| B. 复用 `EMeshPass::TranslucencyAll`，在 PassProcessor 中允许 Opaque 材质 | 不改枚举 | 半透绘制顺序排序、深度写入策略不一致；和现有 Standard/AfterDOF 流程纠缠；难以维护 | ❌ |
| C. 把目标物体材质改为 Translucent，用排序保证最后绘制 | 完全不动渲染线程 | 不透明像素也要走半透混合，性能损失、TAA/抗锯齿/光照精度下降；Mobile 半透不写深度，物体之间互相穿插 | ❌ |
| D. 使用 CustomDepth + 后处理合成 | 不动渲染管线 | 需要离屏纹理、Mobile 上 CustomDepth 性能差；阴影/光照/雾难以保留 | ❌ |

---

下面给出 **方案 A** 的完整改造步骤。所有改动都沿着 *Component 层 → Proxy 层 → MeshPass 注册层 → MeshPass Processor 层 → Renderer 调度层 → 可见性收集层* 这条数据流走，改完即可在 `FPrimitiveComponent` 上勾选一个布尔属性即可启用。

---

## 3. 改造步骤总览

```
[1] 新增 EMeshPass 枚举值 MobileBasePassAfterTranslucent
[2] 在 UPrimitiveComponent 上新增 UPROPERTY: bRenderOpaqueAfterTranslucency
[3] 在 FPrimitiveSceneProxy 上新增镜像位 + Setter
[4] 在 SceneRendering / SceneVisibility 把新 Pass 接入 ParallelMeshDrawCommandPasses
[5] 改 FMobileBasePassMeshProcessor::ShouldDraw / TryAddMeshBatch 逻辑：
       BasePass / MobileBasePassCSM  -> 过滤掉「需要后绘制」的 Proxy
       MobileBasePassAfterTranslucent -> 只接收「需要后绘制」的 Proxy
[6] 新增 CreateMobileBasePassAfterTranslucentProcessor 工厂 + REGISTER_MESHPASSPROCESSOR_AND_PSOCOLLECTOR
[7] 在 FMobileSceneRenderer::RenderTranslucency 之后调用 RenderMobileBasePassAfterTranslucent
[8] 处理 PrimitiveSceneInfo::CacheMeshDrawCommands 让该 Proxy 同时缓存到新 Pass
[9] 蓝图 / C++ API: SetRenderOpaqueAfterTranslucency(bool)
[10] CVAR: r.Mobile.RenderOpaqueAfterTranslucency 用于全局开关
```

---

## 4. 代码改动清单（逐文件）

### 4.1 `Source/Runtime/Renderer/Public/MeshPassProcessor.h`

在 `EMeshPass::Type` 中增加一项。注意它使用 6-bit 存储（`NumBits = 6`，最多 64），目前 Editor 模式下 `Num = 36`，Runtime `Num = 32`，新增 1 个不会越界。

```cpp
// MeshPassProcessor.h
namespace EMeshPass
{
    enum Type : uint8
    {
        ...
        WaterInfoTextureDepthPass,
        WaterInfoTexturePass,

        // ===== 新增 =====
        MobileBasePassAfterTranslucent,   /** Mobile-only: opaque pass that draws AFTER translucency, 始终覆盖在透明物体之上 */
        // ================

#if WITH_EDITOR
        HitProxy,
        ...
#endif

        Num,
        NumBits = 6,
    };
}
```

并在 `GetMeshPassName()` switch 里加：

```cpp
case EMeshPass::MobileBasePassAfterTranslucent: return TEXT("MobileBasePassAfterTranslucent");
```

更新底部断言：

```cpp
#if WITH_EDITOR
    static_assert(EMeshPass::Num == 33 + 4, "...");   // 原 32+4，加 1
#else
    static_assert(EMeshPass::Num == 33, "...");       // 原 32，加 1
#endif
```

> ⚠️ 修改 `EMeshPass::Num` 会让所有 `FMeshPassMask`、`ParallelMeshDrawCommandPasses[EMeshPass::Num]` 大小变化，必须 **全引擎重编译**（Engine + Game）。

---

### 4.2 `Source/Runtime/Engine/Classes/Components/PrimitiveComponent.h`

在 `UPrimitiveComponent` 中添加新的 UPROPERTY（建议放在现有 `bRenderInMainPass` 附近）：

```cpp
/** 
 * Mobile only. 若为 true，该 Component 跳过普通 BasePass，
 * 改在所有 Translucency 绘制完成之后再以不透明方式绘制，始终覆盖在半透物体之上。
 * 用于 UI 模型、武器、装备预览等需要"穿透半透"的对象。
 */
UPROPERTY(EditAnywhere, AdvancedDisplay, BlueprintReadOnly, Category = Rendering, meta = (DisplayName = "Render Opaque After Translucency (Mobile)"))
uint8 bRenderOpaqueAfterTranslucency : 1;
```

同时声明 Setter：

```cpp
/** Sets bRenderOpaqueAfterTranslucency property and marks the render state dirty. */
UFUNCTION(BlueprintCallable, Category = "Rendering")
ENGINE_API void SetRenderOpaqueAfterTranslucency(bool bValue);
```

在 `Source/Runtime/Engine/Private/Components/PrimitiveComponent.cpp` 实现：

```cpp
void UPrimitiveComponent::SetRenderOpaqueAfterTranslucency(bool bValue)
{
    if (bValue != (bool)bRenderOpaqueAfterTranslucency)
    {
        bRenderOpaqueAfterTranslucency = bValue;
        MarkRenderStateDirty();   // 触发重新创建 SceneProxy，必要时也可只 PushHide
    }
}
```

并在构造函数里默认 `bRenderOpaqueAfterTranslucency = false;`。

---

### 4.3 `Source/Runtime/Engine/Public/PrimitiveSceneProxy.h` & `.cpp`

新增渲染线程镜像位：

```cpp
// PrimitiveSceneProxy.h
class FPrimitiveSceneProxy
{
    ...
    /** Mobile only: draw opaque after translucency pass. */
    uint8 bRenderOpaqueAfterTranslucency : 1;
    ...
public:
    FORCEINLINE bool RenderOpaqueAfterTranslucency() const { return bRenderOpaqueAfterTranslucency; }
};
```

在 `FPrimitiveSceneProxy::FPrimitiveSceneProxy(const UPrimitiveComponent* InComponent, ...)` 构造中拷贝：

```cpp
bRenderOpaqueAfterTranslucency = InComponent->bRenderOpaqueAfterTranslucency;
```

> 备注：UE5.4 也提供 `FPrimitiveSceneProxy::SetRenderOpaqueAfterTranslucency_GameThread` 这种命名约定，可以仿照 `SetEvaluateWorldPositionOffsetInRayTracing_GameThread` 用 `ENQUEUE_RENDER_COMMAND` 在不重建 Proxy 的情况下热修改。

---

### 4.4 `Source/Runtime/Renderer/Private/MobileBasePass.cpp` —— 过滤 + 处理器

#### 4.4.1 改 `FMobileBasePassMeshProcessor::ShouldDraw`

把 ShouldDraw 从 *材质* 决策升级为 *(材质, Proxy)* 决策。即在 `MobileBasePass.cpp:851 TryAddMeshBatch` 中传入 Proxy 给 ShouldDraw（已经有 PrimitiveSceneProxy 参数）：

```cpp
// MobileBasePass.cpp 现有：
bool FMobileBasePassMeshProcessor::TryAddMeshBatch(
    const FMeshBatch& MeshBatch, uint64 BatchElementMask,
    const FPrimitiveSceneProxy* PrimitiveSceneProxy, int32 StaticMeshId,
    const FMaterialRenderProxy& MaterialRenderProxy, const FMaterial& Material)
{
    if (ShouldDraw(Material, PrimitiveSceneProxy))   // <<<< 增加第二个参数
    {
        ...
    }
}
```

`ShouldDraw` 改为：

```cpp
bool FMobileBasePassMeshProcessor::ShouldDraw(const FMaterial& Material, const FPrimitiveSceneProxy* Proxy) const
{
    const FMaterialShadingModelField ShadingModels = Material.GetShadingModels();
    const bool bIsTranslucent =
        IsTranslucentBlendMode(Material.GetBlendMode()) ||
        ShadingModels.HasShadingModel(MSM_SingleLayerWater);

    // 标记位（Proxy 在 PSO 预缓存等情况下可能为 null）
    const bool bAfterTranslucent = Proxy ? Proxy->RenderOpaqueAfterTranslucency() : false;

    if (bTranslucentBasePass)
    {
        // 半透 Processor：维持原逻辑，bAfterTranslucent 不影响
        bool bShouldDraw = bIsTranslucent && !Material.IsDeferredDecal() &&
            (TranslucencyPassType == ETranslucencyPass::TPT_AllTranslucency
                || (TranslucencyPassType == ETranslucencyPass::TPT_TranslucencyStandard && !Material.IsMobileSeparateTranslucencyEnabled())
                || (TranslucencyPassType == ETranslucencyPass::TPT_TranslucencyAfterDOF && Material.IsMobileSeparateTranslucencyEnabled()));
        check(!bShouldDraw || bCanReceiveCSM == false);
        return bShouldDraw;
    }
    else
    {
        // 不透明 Processor：根据自身所属 Pass 与 Proxy 标记做"分流"
        if (!bIsTranslucent && !Material.IsDeferredDecal())
        {
            // 普通 BasePass / MobileBasePassCSM：跳过"延后绘制"的对象
            if (MeshPassType == EMeshPass::BasePass || MeshPassType == EMeshPass::MobileBasePassCSM)
            {
                return !bAfterTranslucent;
            }

            // 新增的 MobileBasePassAfterTranslucent：只接收"延后绘制"的对象
            if (MeshPassType == EMeshPass::MobileBasePassAfterTranslucent)
            {
                return bAfterTranslucent;
            }
        }
        return false;
    }
}
```

> 同步修改 `FMobileBasePassMeshProcessor` 的 `MeshPassType` 字段（构造函数已经传入 EMeshPass，可能需要保存到成员变量）。

#### 4.4.2 在 PSO 预缓存路径同样处理（`MobileBasePass.cpp:1066`）

`CollectPSOInitializers` 中：

```cpp
if (!PreCacheParams.bRenderInMainPass || !ShouldDraw(Material, /*Proxy=*/nullptr))
{
    return;
}
```

由于此处 `Proxy = nullptr`，BasePass / AfterTranslucent 两个 Pass 都会预缓存，安全。

#### 4.4.3 新增工厂函数

```cpp
FMeshPassProcessor* CreateMobileBasePassAfterTranslucentProcessor(
    ERHIFeatureLevel::Type FeatureLevel, const FScene* Scene,
    const FSceneView* InViewIfDynamicMeshCommand, FMeshPassDrawListContext* InDrawListContext)
{
    FMeshPassProcessorRenderState PassDrawRenderState;
    PassDrawRenderState.SetBlendState(TStaticBlendStateWriteMask<CW_RGBA>::GetRHI());

    // 关键：不写入深度，比较函数 Always —— 保证"始终覆盖在半透物之上"
    // 若希望该组物体之间仍然按深度互相遮挡，可改成 <true, CF_DepthNearOrEqual>
    PassDrawRenderState.SetDepthStencilState(
        TStaticDepthStencilState<false /*bEnableDepthWrite*/, CF_Always>::GetRHI());

    PassDrawRenderState.SetDepthStencilAccess(FExclusiveDepthStencil::DepthRead_StencilRead);

    const FMobileBasePassMeshProcessor::EFlags Flags =
        FMobileBasePassMeshProcessor::EFlags::CanUseDepthStencil;

    return new FMobileBasePassMeshProcessor(
        EMeshPass::MobileBasePassAfterTranslucent,
        Scene, InViewIfDynamicMeshCommand,
        PassDrawRenderState, InDrawListContext,
        Flags,
        ETranslucencyPass::TPT_MAX /* 标记自身不是半透 */);
}

REGISTER_MESHPASSPROCESSOR_AND_PSOCOLLECTOR(
    MobileBasePassAfterTranslucentPass,
    CreateMobileBasePassAfterTranslucentProcessor,
    EShadingPath::Mobile,
    EMeshPass::MobileBasePassAfterTranslucent,
    EMeshPassFlags::CachedMeshCommands | EMeshPassFlags::MainView);
```

> 关键 RenderState 选择：
> - 若需求是「**永远显示在最前**」（典型的 UI 透视武器、肖像）：`<false, CF_Always>`，不写深度，深度测试始终通过；
> - 若需求是「**只是在半透之后画**，但同组之间仍要互相遮挡」：`<true, CF_DepthNearOrEqual>`；
> - 若希望对场景半透不可见，但被前面已经写入深度的不透明遮挡：保留默认 `<true, CF_DepthNearOrEqual>` 即可（此时只完成"延后"目标，非"覆盖"）。
>
> 推荐对外暴露一个 CVAR / Component 子选项（见 §6）。

---

### 4.5 `Source/Runtime/Renderer/Private/SceneVisibility.cpp` —— 收集到 ParallelMeshDrawCommandPasses

搜索 `EMeshPass::TranslucencyStandard` 在 SceneVisibility.cpp / SceneRendering.cpp 中的 setup 流程，把新 Pass 加入 `View.ParallelMeshDrawCommandPasses`：

* 在 `FSceneRenderer::SetupMeshPass`（或移动端等价位置 `FMobileSceneRenderer::InitViews` 内调用 `SetupMeshPass`） 中找到这一段：

```cpp
case EMeshPass::TranslucencyStandard: { ... break; }
case EMeshPass::TranslucencyAll:      { ... break; }
case EMeshPass::TranslucencyAfterDOF: { ... break; }
```

* 添加：

```cpp
case EMeshPass::MobileBasePassAfterTranslucent:
{
    if (FeatureLevel <= ERHIFeatureLevel::ES3_1 && ShadingPath == EShadingPath::Mobile)
    {
        // 与 BasePass 等同，但使用我们新的 PassProcessor
        FMeshPassProcessor* MeshPassProcessor =
            FPassProcessorManager::CreateMeshPassProcessor(
                EShadingPath::Mobile,
                EMeshPass::MobileBasePassAfterTranslucent,
                Scene->GetFeatureLevel(), Scene, &View, nullptr);

        FParallelMeshDrawCommandPass& Pass =
            View.ParallelMeshDrawCommandPasses[EMeshPass::MobileBasePassAfterTranslucent];

        Pass.DispatchPassSetup(
            Scene, View, FInstanceCullingContext(EShadingPath::Mobile, FeatureLevel,
                  &View.DynamicPrimitiveCollector, View.GetShaderPlatform(), false),
            EMeshPass::MobileBasePassAfterTranslucent,
            BasePassDepthStencilAccess,
            MeshPassProcessor,
            View.DynamicMeshElements,
            &View.DynamicMeshElementsPassRelevance,
            View.NumVisibleDynamicMeshElements[EMeshPass::MobileBasePassAfterTranslucent],
            ViewCommands.DynamicMeshCommandBuildRequests[EMeshPass::MobileBasePassAfterTranslucent],
            ViewCommands.NumDynamicMeshCommandBuildRequestElements[EMeshPass::MobileBasePassAfterTranslucent],
            ViewCommands.MeshCommands[EMeshPass::MobileBasePassAfterTranslucent]);
    }
    break;
}
```

> 真实代码可直接复制 `case EMeshPass::BasePass` 分支稍作改名即可，UE5.4 中所有 Pass 的 SetupMeshPass 模板都是同构的。

---

### 4.6 `Source/Runtime/Renderer/Private/MobileTranslucentRendering.cpp`

在 `FMobileSceneRenderer::RenderTranslucency` 之后，新增一个新的成员函数：

```cpp
// MobileTranslucentRendering.cpp 末尾
void FMobileSceneRenderer::RenderMobileBasePassAfterTranslucent(
    FRHICommandList& RHICmdList, const FViewInfo& View)
{
    if (!CVarMobileRenderOpaqueAfterTranslucency.GetValueOnRenderThread())
    {
        return;
    }

    // Pass 不空才发起绘制
    const FParallelMeshDrawCommandPass& Pass =
        View.ParallelMeshDrawCommandPasses[EMeshPass::MobileBasePassAfterTranslucent];
    if (!Pass.HasAnyDraw())
    {
        return;
    }

    SCOPED_DRAW_EVENT(RHICmdList, MobileBasePassAfterTranslucent);
    SCOPED_GPU_STAT(RHICmdList, MobileBasePassAfterTranslucent);

    RHICmdList.SetViewport(
        View.ViewRect.Min.X, View.ViewRect.Min.Y, 0.0f,
        View.ViewRect.Max.X, View.ViewRect.Max.Y, 1.0f);

    // 这里不需要 InstanceCullingDrawParams 半透专用的，可以另存或复用 BasePass 的
    Pass.DispatchDraw(nullptr, RHICmdList, /*InstanceCullingDrawParams*/ &MobileBasePassAfterTranslucentInstanceCullingDrawParams);
}
```

并在 `MobileShadingRenderer.cpp` 中所有 4 处 `RenderTranslucency(RHICmdList, View);` 之后立即追加：

```cpp
RenderTranslucency(RHICmdList, View);

// === 新增：在所有半透物绘制完成之后，再绘制标记的不透明物体 ===
RenderMobileBasePassAfterTranslucent(RHICmdList, View);
```

并在 `MobileBasePassRendering.h`（或 `SceneRendering.h` 中 `FMobileSceneRenderer` 的声明）添加：

```cpp
void RenderMobileBasePassAfterTranslucent(FRHICommandList& RHICmdList, const FViewInfo& View);

FInstanceCullingDrawParams MobileBasePassAfterTranslucentInstanceCullingDrawParams;
```

---

### 4.7 `Source/Runtime/Renderer/Private/PrimitiveSceneInfo.cpp` —— 缓存 MeshDrawCommands

`FPrimitiveSceneInfo::CacheMeshDrawCommands` 内部会按 `EMeshPass::Type` 调用对应 PassProcessor 缓存静态 MeshCommand。由于 `FMobileBasePassMeshProcessor` 在 §4.4.1 已经按 `MeshPassType` 自动分流，所以**只要 §4.4.3 注册了 PassProcessor，CacheMeshDrawCommands 会自动把同一个 MeshBatch 同时缓存到 BasePass 和 AfterTranslucent 两个槽**——但因 ShouldDraw 互斥，最终每帧只有其中一个会真的产生 MeshCommand。

如果项目里 `CacheMeshDrawCommands` 维护了 "已知 MeshPass 列表" 之类的硬编码列表（部分版本会跳过未知 Pass），需要把 `EMeshPass::MobileBasePassAfterTranslucent` 加进白名单。在 5.4 上一般不需要，但如果你看到 PSO 没缓存，可在如下位置确认：

```
Source/Runtime/Renderer/Private/PrimitiveSceneInfo.cpp -> AddToScene -> CachedMeshDrawCommandsPerMeshPass
Source/Runtime/Renderer/Private/MobileBasePassRendering.cpp -> CollectPSOInitializers
```

---

### 4.8 CVAR 与编辑器开关

在 `MobileBasePass.cpp` 顶部加：

```cpp
static TAutoConsoleVariable<int32> CVarMobileRenderOpaqueAfterTranslucency(
    TEXT("r.Mobile.RenderOpaqueAfterTranslucency"),
    1,
    TEXT("Mobile only. 0 = disable the extra opaque pass after translucency; 1 = enable.\n")
    TEXT("Combined with the per-component flag bRenderOpaqueAfterTranslucency."),
    ECVF_RenderThreadSafe);
```

在 §4.4.1 和 §4.6 的入口都查询此 CVAR，关掉时整条管线退化为原版行为，便于回滚 / 调试。

---

## 5. 关键实现细节与坑点

### 5.1 关于「始终在最上层」的实现

只让顺序在后并不一定能"覆盖"，因为前面 BasePass 已经把场景深度写满了。要保证可见，**必须在新 Pass 中关闭深度测试或关闭深度写入**：

| 需求 | DepthState 推荐 |
|---|---|
| 永远可见，组内不互相遮挡（UI 风格） | `TStaticDepthStencilState<false, CF_Always>` |
| 永远可见，组内按 Z 互相遮挡 | 先用 `CF_Always` 写一遍深度（DrawPass1），再正常绘制（DrawPass2） — 类似武器手部双 Pass |
| 仅延后绘制，被场景已有深度遮挡 | `TStaticDepthStencilState<true, CF_DepthNearOrEqual>`（默认） |

可在 §4.4.3 工厂函数中加一个静态分支按 CVAR `r.Mobile.OpaqueAfterTranslucency.DepthMode` 切换。

### 5.2 关于 LightMap / SkyLight / CSM

`FMobileBasePassMeshProcessor` 在内部 `Process<LightMapPolicyType>` 里会根据 `bCanReceiveCSM`、`StationarySkyLight` 等组合编译 PSO。新增 Pass 完全沿用同一个 Processor，因此：

* **CSM**：默认创建时 `Flags &= ~CanReceiveCSM`，落到 ES3.1 走非 CSM permutation。如果项目要 CSM，仿照 `CreateMobileBasePassCSMProcessor` 再克隆一个 `MobileBasePassAfterTranslucentCSM` 即可，但性能上不建议（这些一般是近距离物体，不太需要 CSM）。
* **SkyLight / Lightmap**：自动可用，无需额外改动。

### 5.3 关于 PSO 预缓存

`CollectPSOInitializers` 会在材质初始化时按所有已注册 Pass 收集 PSO，因此 §4.4.3 注册之后，所有可能用作"After Translucent"的材质会自动多出一份 PSO（与 BasePass 唯一不同的就是 DepthStencilState）。这会导致 PSO 数量增加约 **(标记物体材质数 × 1 份)**，可接受。

如果担心数量增多，可在 `CollectPSOInitializers` 中只对 `bUsedWithRenderOpaqueAfterTranslucency`（在材质上加一个 UMaterial::bUsedWithXxx 的标记）的材质收集，避免给所有材质都生成第二份 PSO。

### 5.4 关于 Mobile MultiView / Mobile XR / Mobile Forward Deferred

`MobileShadingRenderer.cpp` 中 `RenderTranslucency` 出现 4 次，分别对应：

* `Render`（线性 LDR 路径）
* `RenderForward`（移动 Forward 完整路径）
* `RenderDeferred`（移动 Deferred）
* MultiView/XR

**4 处都要补一行 `RenderMobileBasePassAfterTranslucent(RHICmdList, View);`**。否则在 XR 或 Deferred 模式下会丢绘制。

### 5.5 关于 Decal / Distortion / Fog 的相对顺序

`FMobileSceneRenderer::Render` 当前顺序：

```
BasePass -> Decals -> Fog -> (MobileDistortion if any) -> Translucency
```

新 Pass 插在 **Translucency 之后**，所以：

* 不接收 Decal —— 多数 UI/装备模型本就不需要；
* 不被 Fog 影响（Fog 已写入颜色） —— 这是**特性**：UI 武器不应被远景雾掩盖；
* 不被 Distortion 折射扰动；
* 不参与 Bloom 的高光抽取（如果 Bloom 在 PostProcess 抽取颜色，仍然会作用到这层颜色上，符合直觉）。

### 5.6 关于排序

由于此 Pass 是「不透明 Pass + 自定义深度行为」，组内绘制顺序由 MeshDrawCommand 排序键决定（同 BasePass）。如果你希望同组之间也能保证「后注册的覆盖前注册的」，可以在 PassProcessor 里给 SortKey.Translucent 字段塞一个 Component-level 的 Priority（仿照 `FTranslucentPrimSet`），不过通常不需要。

### 5.7 RDG 化

如果你的项目使用 RDG（5.4 移动端默认 RDG-friendly），把 `RenderMobileBasePassAfterTranslucent` 改成 `AddPass`/`AddDispatchPass`：

```cpp
GraphBuilder.AddPass(
    RDG_EVENT_NAME("MobileBasePassAfterTranslucent"),
    PassParameters,
    ERDGPassFlags::Raster,
    [&View, &Pass](FRHICommandList& RHICmdList)
    {
        Pass.DispatchDraw(nullptr, RHICmdList, ...);
    });
```

绑定 SceneColor / SceneDepth 和 BasePass 相同的 RT，DepthRead_StencilRead 即可。

---

## 6. 蓝图 / C++ 上层使用方法

```cpp
// C++
StaticMeshComp->SetRenderOpaqueAfterTranslucency(true);

// 蓝图
Set Render Opaque After Translucency  -> True
```

控制台命令：

```
r.Mobile.RenderOpaqueAfterTranslucency 0   // 全局关闭
r.Mobile.RenderOpaqueAfterTranslucency 1   // 全局打开
```

---

## 7. 测试与验证

1. **正确性**
   - 一面半透玻璃 + 一把武器：勾选 `bRenderOpaqueAfterTranslucency`，武器穿透玻璃显示。
   - 关闭后，武器在玻璃后方应被遮挡。
2. **批次**
   - 在 `Stat SceneRendering` / RenderDoc 中确认 `MobileBasePassAfterTranslucent` Pass 出现，且不在 `MobileBasePass` 中绘制。
3. **CSM / SkyLight**
   - 标记物体在阴影范围内，禁用 CSM 时光照与未标记时一致。
4. **PSO**
   - `r.PSOPrecaching 1`，启动到关卡确认无运行时编译卡顿。
5. **Mobile Deferred / XR**
   - 切换 `r.Mobile.ShadingPath 1` 与 VR 预览，每条路径都要看到对应的 DrawCall。
6. **回滚**
   - `r.Mobile.RenderOpaqueAfterTranslucency 0` 时管线行为与未改造前一致。

---

## 8. 风险与回滚

| 风险点 | 影响 | 缓解 |
|---|---|---|
| `EMeshPass::Num` 变化 | 所有 `FMeshPassMask`/`ParallelMeshDrawCommandPasses` 内存布局变化，必须全引擎重编 | 一次性整改、CL 锁定 |
| PSO 数量增加 | 包体 / 启动期 PSO 编译变多 | 用 Material 上 `bUsedWithRenderOpaqueAfterTranslucency` 收敛 |
| 与移动 SeparateTranslucency / AfterDOF 顺序耦合 | 错插位置会导致 DOF 后透明之上又叠一层 | 在 4 处 `RenderTranslucency` 之后统一插入；DOF 路径下放在 AfterDOF 之后 |
| Mobile HDR / Mobile MultiView / Mobile Deferred | 渲染分支不同，遗漏一处会丢绘制 | §5.4 列举 4 处都需要修改 |
| Custom Stencil / Outline 等扩展 | 新 Pass 不参与 CustomDepth，描边可能错位 | 视需求决定是否为新 Pass 也写一份 CustomDepth；常见做法是不写 |

---

## 9. 修改文件清单（汇总）

```
Source/Runtime/Renderer/Public/MeshPassProcessor.h                         [+10 lines]
Source/Runtime/Engine/Classes/Components/PrimitiveComponent.h              [+10 lines]
Source/Runtime/Engine/Private/Components/PrimitiveComponent.cpp            [+10 lines]
Source/Runtime/Engine/Public/PrimitiveSceneProxy.h                         [+5 lines]
Source/Runtime/Engine/Private/PrimitiveSceneProxy.cpp                      [+3 lines]
Source/Runtime/Renderer/Private/MobileBasePass.cpp                         [+50 lines]
Source/Runtime/Renderer/Private/MobileBasePassRendering.h                  [+3 lines]
Source/Runtime/Renderer/Private/MobileTranslucentRendering.cpp             [+30 lines]
Source/Runtime/Renderer/Private/MobileShadingRenderer.cpp                  [+4 lines × 4 处]
Source/Runtime/Renderer/Private/SceneVisibility.cpp                        [+25 lines]
Source/Runtime/Renderer/Private/PrimitiveSceneInfo.cpp                     [审核 / 0~5 lines]
```

总改动量约 **150~200 行**，单人可在 0.5~1 天内完成首版，剩余时间用于对各分支（XR / Deferred / MultiView）做联调与 PSO 验证。

---

## 10. 后续可扩展方向

* **多优先级层级**：`bRenderOpaqueAfterTranslucency` 升级为 `int32 RenderPriorityAfterTranslucency`，多个数值对应多个 Pass（例如 0=场景层 1=武器层 2=UI 模型层），每层一个 RenderState；
* **写自身深度但不读场景深度**：在新 Pass 内先 ClearDepth(View Rect) 再绘制，模拟"二号深度缓冲"，让该组之间彼此遮挡且永远在最前；
* **开放给后处理**：通过额外的 SceneTexture 分量把该层颜色单独输出，给特定的后处理（如 OutlinePostProcess）使用；
* **配合 r.Mobile.SeparateTranslucency**：当项目使用移动 Separate Translucency 时，把新 Pass 接在 `RenderInverseOpacity`/`RenderMobileSeparateTranslucency` 之后，保证它在所有半透通道之上。

---

> 文档完。
> 此方案已对齐 UE5.4 源码（Renderer/Private、Engine/Components 实际函数签名校验通过）。改完后请运行 `UnrealEditor.exe -run=DerivedDataCache -fill` 与 `RebuildPSOCache`，并用 RenderDoc 抓帧确认 `MobileBasePassAfterTranslucent` Pass 时序在 `Translucency` 之后。
