# UE5.4 移动端 Android Forward — 指定不透明物体延后渲染方案

> **目标平台**: Android OpenGL ES 3.2 / Vulkan,UE5.4 源码版 (`E:/Unreal Engine Work Projects/MR01_DaNaoTianGong_Main/Engine`),Mobile Forward Shading
> **文档版本**: v1.0  /  2026-06-18
> **作者**: Claude Code (MiniMax-M3,ultracode 模式)

---

## 0. 需求陈述

在 Mobile Forward 管线中:

1. **被指定的不透明物体** `P1` 在 **不透明阶段 (BasePass) 不渲染**。
2. **常规透明物体** `P2` 走标准 Translucency 通道,顺序不变。
3. `P1` 在 **Translucency 渲染完成之后** 渲染,使其 **始终显示在所有透明物体之上**。
4. `P1` 与 `P2` 自身之间仍按 UE 标准深度排序 (避免穿插穿模)。

效果示意:

```
帧渲染顺序:        场景
─────────────────────────────────────────
PrePass (深度)    : 地面 / 不透明建筑 (B0)
BasePass         : 其他不透明物体 (B1)        ← P1 在此被剔除
Decals / Fog     : 场景贴花
Translucency     : 玻璃、烟雾、粒子 (T0..Tn)
AllwayFrontPass  : P1 (在此阶段渲染)            ← ★ 新增 Pass
PostProcess      : 一切后处理
```

---

## 1. 现状与约束(基于源码研究)

### 1.1 Mobile Forward 渲染管线关键节点

| 文件:行号 | 节点 | 说明 |
|---|---|---|
| `MobileShadingRenderer.cpp:910` | `FMobileSceneRenderer::Render` | 整帧入口 |
| `MobileShadingRenderer.cpp:1169-1173` | `RenderCustomDepthPass` | CustomDepth 渲染(若开启) |
| `MobileShadingRenderer.cpp:1304-1307` | `PreRenderBasePass_RenderThread` | SVE: BasePass 前 RDG 钩子 |
| `MobileShadingRenderer.cpp:1503-1576` | `RenderForward` | 路由到 Single/Multi Pass |
| `MobileShadingRenderer.cpp:1578-1660` | **`RenderForwardSinglePass`** (Vulkan) | **1 个 RDG Pass + 3 subpass** (0=Base,1=Decals+Translucency,2=CustomResolve) |
| `MobileShadingRenderer.cpp:1609` | `RenderMobileBasePass` | BasePass 入口(在 subpass 0 内) |
| `MobileShadingRenderer.cpp:1612` | `PostRenderBasePass` | SVE 钩子(有 off-by-one bug,见 §7) |
| `MobileShadingRenderer.cpp:1614` | `RHICmdList.NextSubpass()` | 切到 subpass 1(Decals+Translucency) |
| `MobileShadingRenderer.cpp:1623` | `RenderTranslucency` | 透明物体入口(在 subpass 1 内) |
| `MobileShadingRenderer.cpp:1662-1749` | **`RenderForwardMultiPass`** (GLES/Metal) | **2 个独立 RDG Pass**: Pass1=Base,Pass2=Decals+Translucency |
| `MobileShadingRenderer.cpp:1735` | `RenderTranslucency` (Multi) | 在 Pass2 内 |
| `MobileBasePass.cpp:810-826` | `FMobileBasePassMeshProcessor` | BasePass Processor |
| `MobileBasePass.cpp:828-849` | `ShouldDraw` | 判定 Opaque / Translucent |
| `MeshPassProcessor.h:32-79` | `EMeshPass::Type` 枚举 | 现有 32 个值,NumBits=6 |
| `SceneVisibility.cpp:2198-2286` | Pass 分配 | 物体进哪些 Pass |
| `SceneVisibility.cpp:2211-2236` | BasePass 块 | `(bRenderInMainPass \|\| bRenderCustomDepth)` ⇒ BasePass |

### 1.2 关键限制

1. **Single Pass (Vulkan)** 共享 RT 与 depth buffer,**subpass 切换不能改 depth access**;若新 Pass 需 `DepthWrite`,必须拆出独立 RDG Pass(破坏 tile-based 优化)。
2. **`PostRenderBasePassMobile_RenderThread`** 钩子 `MobileShadingRenderer.cpp:2081` 有 `if (ViewFamily.ViewExtensions.Num() > 1)` off-by-one,单 SVE 项目不触发。
3. **Mobile 不支持** `PrePostProcessPass_RenderThread` 与 `SubscribeToPostProcessingPass`(只支持 Deferred 路径)。
4. **Stencil 通道** 0..255 只有 8 bit,`LightingChannel` / `CustomDepth Stencil` 都在用,不能滥用。
5. **`EMeshPass::Num=32`,`NumBits=6`**:可加 32 个新值。

---

## 2. 方案对比

| # | 方案 | 入侵性 | 可控性 | 兼容性 | 适用场景 |
|---|---|---|---|---|---|
| **A** | **新增 EMeshPass + 自定义 MeshPassProcessor + 新 RDG Pass** | 中 | **高** | 高 | **推荐**:通用、长期方案 |
| B | SceneViewExtension + RenderPostOpaqueExtensions | 低 | 中 | 中 | 轻量级,只用现成 RDG 钩子 |
| C | 复用 CustomDepth + Stencil 分流 | 低 | 低 | 低 | 极少量物体、临时方案 |
| D | 透明 + Holdout 技巧 | 低 | 低 | 低 | 仅装饰层,3D 顺序感丢失 |

**推荐方案 A**,原因:

- 完全复用 BasePass 的 Shader 变体,**无新 Shader、无新 PSO 编译**。
- 拥有独立 Pass,`SceneColor` / `SceneDepth` / `CustomDepth` 都可访问。
- 可指定深度测试模式 (`GreaterEqual`),保证始终在透明之上。
- 与 Translucency、Decal 物理隔离,符合"延后到 Translucency 之后"的精确语义。
- `EMeshPass` 枚举、`FPrimitiveViewRelevance` 位、`UPrimitiveComponent` 属性都是 UE 标准扩展点。

---

## 3. 推荐方案 A — 完整设计

### 3.1 整体架构

```
            ┌──────────── UPrimitiveComponent.bRenderInAllwayFrontPass = true
            │                  (用户设置)
            ▼
FPrimitiveSceneProxy::GetViewRelevance
            │  返回 bRenderInMainPass=false, bRenderInAllwayFrontPass=true
            ▼
SceneVisibility.cpp:2228 (新增)
            │  PassMask.Set(EMeshPass::AllwayFrontPass)
            ▼
MobileAllwayFrontMeshProcessor::ShouldDraw
            │  验证 bRenderInAllwayFrontPass && IsOpaque
            │  使用与 BasePass 共享的 Shader (复用 PSO)
            ▼
MobileShadingRenderer::RenderAllwayFrontPass (新增)
            │  在 RenderTranslucency 之后调用
            │  Single Pass: subpass 1 末尾 (next subpass 后)
            │  Multi Pass:  新建第 3 个 RDG Pass
            ▼
        SceneColor  ←  P1 渲染 (深度测试 GreaterEqual)
```

### 3.2 关键设计决策

1. **不写深度** — 新 Pass 保持 `DepthRead_StencilRead`,与 Translucency 一致,避免破坏 tile 缓存、避免覆盖场景深度。
2. **深度测试 = `CF_GreaterEqual`** — 永远绘制在已有像素之上,自然覆盖所有透明物体。
3. **剔除 `bRenderInMainPass`** — 在 ViewRelevance 上必须为 false,否则物体还会进 BasePass。
4. **复用 BasePass PSO** — 渲染状态几乎相同(不透明、DepthTest=LE, DepthWrite=Off),走 PSO cache 命中。
5. **阴影默认关闭** — 避免 ShadowMap 重复绘制;若需阴影,再扩展 `bCastShadowInAllwayFrontPass`。
6. **Multi Pass 模式新建独立 RDG Pass** — 用 `ELoad + DepthRead_StencilRead` 加载前一 Pass 输出,模仿 `DecalsAndTranslucency` (`:1721-1740`)。

---

## 4. 详细源码修改

### 4.1 修改 1 — `MeshPassProcessor.h`:新增 `EMeshPass::AllwayFrontPass`

**文件**: `Engine/Source/Runtime/Renderer/Public/MeshPassProcessor.h:32-79`

在 `MobileBasePassCSM,` 之后插入:

```cpp
namespace EMeshPass
{
    enum Type : uint8
    {
        DepthPass,                          // 0
        SecondStageDepthPass,               // 1
        BasePass,                           // 2
        AnisotropyPass,                     // 3
        SkyPass,                            // 4
        SingleLayerWaterPass,               // 5
        SingleLayerWaterDepthPrepass,       // 6
        CSMShadowDepth,                     // 7
        VSMShadowDepth,                     // 8
        Distortion,                         // 9
        Velocity,                           // 10
        TranslucentVelocity,                // 11
        TranslucencyStandard,               // 12
        TranslucencyStandardModulate,       // 13
        TranslucencyAfterDOF,               // 14
        TranslucencyAfterDOFModulate,       // 15
        TranslucencyAfterMotionBlur,        // 16
        TranslucencyAll,                    // 17
        LightmapDensity,                    // 18
        DebugViewMode,                      // 19
        CustomDepth,                        // 20
        MobileBasePassCSM,                  // 21
        // ★ 新增:延后渲染 Pass(在 Translucency 之后绘制,深度测试 GE 保证在所有像素之上)
        AllwayFrontPass,                    // 22  (新)
        VirtualTexture,                     // 23
        LumenCardCapture,                   // 24
        LumenCardNanite,                    // 25
        LumenTranslucencyRadianceCacheMark, // 26
        LumenFrontLayerTranslucencyGBuffer, // 27
        DitheredLODFadingOutMaskPass,       // 28
        NaniteMeshPass,                     // 29
        MeshDecal,                          // 30
        WaterInfoTextureDepthPass,          // 31
        WaterInfoTexturePass,               // 32
#if WITH_EDITOR
        HitProxy,                           // 33
        HitProxyOpaqueOnly,                 // 34
        EditorLevelInstance,                // 35
        EditorSelection,                    // 36
#endif
        Num,                                // = 33 (release) / 37 (editor)  ← 必须更新
        NumBits = 6,
    };
}
```

**同时更新** `GetMeshPassName` (`:83-135`) 在 switch 中添加:

```cpp
case EMeshPass::AllwayFrontPass:         return TEXT("AllwayFrontPass");
```

并更新 `static_assert(EMeshPass::Num == 33, ...)` (`:130` 附近)— 具体值由编译器报错提示。

### 4.2 修改 2 — `PrimitiveViewRelevance.h`:新增 `bRenderInAllwayFrontPass` 位

**文件**: `Engine/Source/Runtime/Engine/Public/PrimitiveViewRelevance.h:39-77`

在 `bRenderInSecondStageDepthPass` 之后(或任意空位)添加:

```cpp
struct FPrimitiveViewRelevance
{
    // ... 既有字段 ...
    uint32 bRenderInSecondStageDepthPass : 1;
    uint32 bRenderInAllwayFrontPass : 1;   // ★ 新增:是否进入 AllwayFrontPass
    // ... 既有字段 ...

    FPrimitiveViewRelevance()
    {
        // ... 既有清零 ...
        bRenderInSecondStageDepthPass = false;
        bRenderInAllwayFrontPass = false;  // ★ 新增
    }
};
```

### 4.3 修改 3 — `PrimitiveComponent.h`:新增 `bRenderInAllwayFrontPass` 属性

**文件**: `Engine/Source/Runtime/Engine/Classes/Components/PrimitiveComponent.h`

在 `bRenderInMainPass` 字段附近 (`:408`) 添加:

```cpp
/** If true, this component will be rendered in the main pass (z prepass, basepass, transparency) */
UPROPERTY(EditAnywhere, AdvancedDisplay, BlueprintReadOnly, Category = Rendering)
uint8 bRenderInMainPass:1;

/** ★ 新增:如果为 true,该物体在 BasePass 阶段不渲染,而在 Translucency 之后的 AllwayFrontPass 渲染(永远在最上层) */
UPROPERTY(EditAnywhere, AdvancedDisplay, BlueprintReadOnly, Category = Rendering)
uint8 bRenderInAllwayFrontPass:1;
```

在 `SetRenderInMainPass` (`:4457`) 附近添加 setter:

```cpp
/**
 * 设置物体是否进入"延后渲染通道"。开启后物体在 BasePass 不渲染,而在 Translucency 之后渲染并始终位于所有透明物体之上。
 * 默认 false。修改后会重建 SceneProxy。
 */
UFUNCTION(BlueprintCallable, Category = "Rendering")
ENGINE_API void SetRenderInAllwayFrontPass(bool bValue);
```

### 4.4 修改 4 — `PrimitiveComponent.cpp`:实现 setter + 同步到 Proxy

**文件**: `Engine/Source/Runtime/Engine/Private/Components/PrimitiveComponent.cpp`

在 `SetRenderInMainPass` (`:4457`) 附近添加:

```cpp
void UPrimitiveComponent::SetRenderInAllwayFrontPass(bool bValue)
{
    if (bRenderInAllwayFrontPass != bValue)
    {
        bRenderInAllwayFrontPass = bValue;
        // 同步:开启 AllwayFrontPass 时,自动从 MainPass 移除(避免重复渲染)
        if (bValue)
        {
            bRenderInMainPass = false;
        }
        MarkRenderStateDirty();
    }
}
```

修改 `CreateSceneProxy` 流程(无需新建函数,只需在 Proxy 构造时传值)。

### 4.5 修改 5 — `PrimitiveSceneProxy.h/.cpp`:镜像标志位

**文件**: `Engine/Source/Runtime/Engine/Public/PrimitiveSceneProxy.h`

在 `FPrimitiveSceneProxy` 类中添加镜像字段:

```cpp
class FPrimitiveSceneProxy
{
    // ... 既有字段 ...
    uint8 bRenderInAllwayFrontPass : 1;  // ★ 新增:RT 端镜像

    // 在构造函数初始化列表中添加(约 :393):
    , bRenderInAllwayFrontPass(InComponent ? InComponent->bRenderInAllwayFrontPass : false)
    // ...
};
```

在 `PrimitiveSceneProxy.cpp:740-743` 的 `GetViewRelevance` 默认实现中,**不要修改**。需要子类(如 `FStaticMeshSceneProxy`)在自身 `GetViewRelevance` 中添加:

```cpp
FPrimitiveViewRelevance FStaticMeshSceneProxy::GetViewRelevance(const FSceneView* View) const
{
    FPrimitiveViewRelevance Result = FStaticMeshSceneProxy::GetViewRelevance_Static ... ;
    // ... 既有逻辑 ...

    if (bRenderInAllwayFrontPass)
    {
        // 强制从 MainPass 移除,只进 AllwayFrontPass
        Result.bRenderInMainPass = false;
        Result.bRenderInAllwayFrontPass = true;
        Result.bRenderInDepthPass = false;     // 不进 DepthPass
        Result.bRenderCustomDepth = false;     // 不进 CustomDepth
    }

    return Result;
}
```

> **重要**:如果 `bRenderInAllwayFrontPass=true` 时不显式设置 `bRenderInMainPass=false`,物体将 **同时** 进入 BasePass 和 AllwayFrontPass,导致重复绘制。

### 4.6 修改 6 — `SceneVisibility.cpp`:PassMask 分配

**文件**: `Engine/Source/Runtime/Renderer/Private/SceneVisibility.cpp:2211-2236`

在 BasePass 块 (`:2211`) 之后增加新判断。注意:位置要保证 `bRenderInAllwayFrontPass` 单独存在时,BasePass 不会被启用。

当前代码逻辑:

```cpp
if (ViewRelevance.bRenderInMainPass || ViewRelevance.bRenderCustomDepth)
{
    PassMask.Set(EMeshPass::BasePass);
    // ...
}
```

**修改后**:

```cpp
const bool bRenderInAllwayFrontPass = ViewRelevance.bRenderInAllwayFrontPass;

if (bRenderInAllwayFrontPass)
{
    // ★ 走 AllwayFrontPass,绕过 BasePass
    PassMask.Set(EMeshPass::AllwayFrontPass);
    View.NumVisibleDynamicMeshElements[EMeshPass::AllwayFrontPass] += NumElements;
}
else if (ViewRelevance.bRenderInMainPass || ViewRelevance.bRenderCustomDepth)
{
    // ★ 原有逻辑(不变)
    PassMask.Set(EMeshPass::BasePass);
    if (ViewRelevance.bUsesSkyMaterial)        { PassMask.Set(EMeshPass::SkyPass); ... }
    if (ViewRelevance.bUsesAnisotropy)         { PassMask.Set(EMeshPass::AnisotropyPass); ... }
    if (ShadingPath == EShadingPath::Mobile)   { PassMask.Set(EMeshPass::MobileBasePassCSM); ... }
    if (ViewRelevance.bRenderCustomDepth)      { PassMask.Set(EMeshPass::CustomDepth); ... }
    if (bAddLightmapDensityCommands)           { PassMask.Set(EMeshPass::LightmapDensity); ... }
    // ...
}
```

**Translucency 块** (`:2288-2344`):保持原样,`bRenderInAllwayFrontPass=true` 的物体不会进 Translucency(因为材质是 Opaque)。

### 4.7 修改 7 — 新建 `MobileAllwayFrontPass.h/.cpp`

**新文件** 1: `Engine/Source/Runtime/Renderer/Private/MobileAllwayFrontPass.h`

```cpp
#pragma once

#include "CoreMinimal.h"
#include "MeshPassProcessor.h"

class FScene;
class FSceneView;
class FMeshPassProcessorRenderState;
class FStaticMeshBatch;
class FDynamicMeshBuilder;
class FLODSceneProxy;

/**
 * Mobile AllwayFrontPass 渲染器。
 *
 * 负责在 Translucency 完成之后,绘制 bRenderInAllwayFrontPass=true 的不透明物体。
 * 关键设计:
 *   - 深度测试 = CF_GreaterEqual:始终在已有像素之上,自然覆盖所有透明物体
 *   - 深度写入 = Off:不覆盖场景深度,避免破坏后续逻辑
 *   - Stencil  = Read:不污染现有 Stencil 通道
 *   - 复用 BasePass 的 Shader 变体:不增加 PSO 编译开销
 */
class FMobileAllwayFrontPassMeshProcessor : public FMeshPassProcessor
{
public:
    FMobileAllwayFrontPassMeshProcessor(
        const FScene* Scene,
        const FSceneView* InViewIfDynamicMeshCommand,
        const FMeshPassProcessorRenderState& InDrawRenderState,
        FMeshPassDrawListContext* InDrawListContext);

    virtual void AddMeshBatch(const FMeshBatch& MeshBatch, uint64 BatchElementMask, const FPrimitiveSceneProxy* PrimitiveSceneProxy, int32 StaticMeshId = INDEX_NONE) override;

private:
    bool ShouldDraw(const FMeshBatch& MeshBatch) const;
    bool TryAddMeshBatch(const FMeshBatch& MeshBatch, uint64 BatchElementMask, const FPrimitiveSceneProxy* PrimitiveSceneProxy, int32 StaticMeshId);
};

extern void CreateMobileAllwayFrontPassProcessor(
    ERHIFeatureLevel::Type FeatureLevel,
    const FScene* Scene,
    const FSceneView* InViewIfDynamicMeshCommand,
    FMeshPassProcessorRenderState& DrawRenderState,
    FMeshPassDrawListContext* InDrawListContext);

extern void RenderMobileAllwayFrontPass(
    FRHICommandList& RHICmdList,
    const FViewInfo& View,
    const FInstanceCullingDrawParams* InstanceCullingDrawParams);
```

**新文件** 2: `Engine/Source/Runtime/Renderer/Private/MobileAllwayFrontPass.cpp`

```cpp
#include "MobileAllwayFrontPass.h"

#include "DataDrivenShaderPlatformInfo.h"
#include "MeshPassProcessorRenderState.h"
#include "PrimitiveSceneProxy.h"
#include "PrimitiveViewRelevance.h"
#include "ScenePrivate.h"
#include "SceneRendering.h"

// =====================================================================
//  MeshPassProcessor
// =====================================================================

FMobileAllwayFrontPassMeshProcessor::FMobileAllwayFrontPassMeshProcessor(
    const FScene* Scene,
    const FSceneView* InViewIfDynamicMeshCommand,
    const FMeshPassProcessorRenderState& InDrawRenderState,
    FMeshPassDrawListContext* InDrawListContext)
    : FMeshPassProcessor(Scene, InViewIfDynamicMeshCommand, InDrawRenderState, InDrawListContext)
{
    // ★ 关键:用与 BasePass 几乎相同的渲染状态,确保 PSO cache 命中
    // 不同点:DepthTest 改为 CF_GreaterEqual(始终在最上面)
    // 不同点:DepthWrite 关闭(不污染 SceneDepth)
    // 不同点:Stencil 不写
    DrawRenderState.SetDepthStencilState(TStaticDepthStencilState<
        false,                                // DepthWrite = Off
        CF_GreaterEqual,                      // DepthTest = GE(永远在已有像素之上)
        true, CF_Always, SO_Keep, SO_Keep, SO_Keep,    // StencilRead = Always Keep
        false, CF_Always, SO_Keep, SO_Keep, SO_Keep
    >::GetRHI());
    DrawRenderState.SetBlendState(TStaticBlendState<>::GetRHI());   // 不透明:不混合
}

bool FMobileAllwayFrontPassMeshProcessor::ShouldDraw(const FMeshBatch& MeshBatch) const
{
    const FMaterial& Material = MeshBatch.MaterialRenderProxy->GetMaterial();
    const FPrimitiveSceneProxy* PrimitiveProxy = MeshBatch.PrimitiveSceneProxy;

    // 1) 必须标记为 AllwayFrontPass
    if (!PrimitiveProxy || !PrimitiveProxy->bRenderInAllwayFrontPass)
    {
        return false;
    }

    // 2) 仅接收不透明 / Masked(不接收 Translucent,因为 Translucent 走自己的通道)
    if (IsTranslucentBlendMode(Material.GetBlendMode()))
    {
        return false;
    }

    return true;
}

bool FMobileAllwayFrontPassMeshProcessor::TryAddMeshBatch(
    const FMeshBatch& MeshBatch,
    uint64 BatchElementMask,
    const FPrimitiveSceneProxy* PrimitiveSceneProxy,
    int32 StaticMeshId)
{
    if (!ShouldDraw(MeshBatch))
    {
        return false;
    }
    return Process(MeshBatch, BatchElementMask, PrimitiveSceneProxy, StaticMeshId);
}

void FMobileAllwayFrontPassMeshProcessor::AddMeshBatch(
    const FMeshBatch& MeshBatch,
    uint64 BatchElementMask,
    const FPrimitiveSceneProxy* PrimitiveSceneProxy,
    int32 StaticMeshId)
{
    TryAddMeshBatch(MeshBatch, BatchElementMask, PrimitiveSceneProxy, StaticMeshId);
}

// =====================================================================
//  Processor 工厂
// =====================================================================

void CreateMobileAllwayFrontPassProcessor(
    ERHIFeatureLevel::Type FeatureLevel,
    const FScene* Scene,
    const FSceneView* InViewIfDynamicMeshCommand,
    FMeshPassProcessorRenderState& DrawRenderState,
    FMeshPassDrawListContext* InDrawListContext)
{
    // ★ 渲染状态:不写深度、深度测试 GE、不写 Stencil
    DrawRenderState.SetDepthStencilAccess(FExclusiveDepthStencil::DepthRead_StencilRead);

    new (FMemory::Malloc(sizeof(FMobileAllwayFrontPassMeshProcessor), alignof(FMobileAllwayFrontPassMeshProcessor)))
        FMobileAllwayFrontPassMeshProcessor(Scene, InViewIfDynamicMeshCommand, DrawRenderState, InDrawListContext);
}

// =====================================================================
//  入口:由 MobileShadingRenderer 调用
// =====================================================================

void RenderMobileAllwayFrontPass(
    FRHICommandList& RHICmdList,
    const FViewInfo& View,
    const FInstanceCullingDrawParams* InstanceCullingDrawParams)
{
    if (!View.ParallelMeshDrawCommandPasses[EMeshPass::AllwayFrontPass].HasAnyDraw())
    {
        return;
    }

    RHICmdList.SetViewport(View.ViewRect);

    View.ParallelMeshDrawCommandPasses[EMeshPass::AllwayFrontPass].DispatchDraw(
        nullptr, RHICmdList, InstanceCullingDrawParams);
}

// =====================================================================
//  注册
// =====================================================================

REGISTER_MESHPASSPROCESSOR_AND_PSOCOLLECTOR(
    MobileAllwayFrontPass,
    CreateMobileAllwayFrontPassProcessor,
    EShadingPath::Mobile,
    EMeshPass::AllwayFrontPass,
    EMeshPassFlags::MainView);
```

### 4.8 修改 8 — `Renderer.Build.cs`:添加新文件

**文件**: `Engine/Source/Runtime/Renderer/Renderer.Build.cs`

找到 `PrivateIncludePaths` 或 `PublicDependencyModuleNames` 块,确认新文件被自动扫描(UE 的 Build.cs 默认会编译 Private/ 下所有 .cpp)。无需手动添加。

若需要显式控制,在 `PrivateIncludePaths` 后添加:

```csharp
PrivateIncludePathModuleNames.AddRange(new string[] { "Renderer", "RenderCore", "RHI" });
```

### 4.9 修改 9 — `MobileShadingRenderer.cpp`:集成新 Pass

**关键位置**: `MobileShadingRenderer.cpp:1578-1660` (Single Pass) 与 `:1662-1749` (Multi Pass)

#### 4.9.1 Single Pass 模式 (Vulkan)

在 `RenderForwardSinglePass` 的 RDG Pass lambda 中,`RenderTranslucency` (`:1623`) 之后添加:

```cpp
// ... 现有代码:RenderTranslucency(RHICmdList, View); ...
// ★ 新增:在 Translucency 之后渲染 AllwayFrontPass
RenderMobileAllwayFrontPass(RHICmdList, View, &PassParameters->InstanceCullingDrawParams);
// ... 现有代码:RenderMobileDebugPrimitives ...
```

**注意**:
- Single Pass 模式下,新调用与 Translucency 处于 **同一 RDG Pass + 同一 subpass**。这要求 `DepthRead_StencilRead` 的 access 在 Pass 开始时已声明。
- 现有 `PassParameters->RenderTargets.DepthStencil.SetDepthStencilAccess(...)` 调用 (`:1716` 类似位置) 已设置为 `DepthRead_StencilRead`,**无需修改**。
- 不要调用 `NextSubpass()`——tile 缓存可能丢失,影响性能。

#### 4.9.2 Multi Pass 模式 (GLES/Metal)

在 `RenderForwardMultiPass` (`:1662-1749`) 的第二个 RDG Pass `DecalsAndTranslucency` (`:1721`) 之后,**新增第三个 RDG Pass**:

```cpp
// ... 现有第二个 Pass (DecalsAndTranslucency) 结束 ...

// ★ 新增第三个 RDG Pass:AllwayFrontPass
if (View.ParallelMeshDrawCommandPasses[EMeshPass::AllwayFrontPass].HasAnyDraw())
{
    FMobileRenderPassParameters* AllwayParams = GraphBuilder.AllocParameters<FMobileRenderPassParameters>();
    AllwayParams->View = View.GetShaderParameters();
    AllwayParams->RenderTargets = PassParameters->RenderTargets;
    AllwayParams->RenderTargets[0].SetLoadAction(ELoad);  // 加载前一 Pass 的 SceneColor
    AllwayParams->RenderTargets.DepthStencil.SetDepthLoadAction(ELoad);
    AllwayParams->RenderTargets.DepthStencil.SetDepthStencilAccess(FExclusiveDepthStencil::DepthRead_StencilRead);
    AllwayParams->MobileBasePass = EMobileBasePass::Opaque;  // 复用 Opaque uniform

    GraphBuilder.AddPass(
        RDG_EVENT_NAME("AllwayFrontPass"),
        AllwayParams,
        ERDGPassFlags::Raster,
        [this, &View, AllwayParams](FRHICommandList& RHICmdList)
        {
            RenderMobileAllwayFrontPass(RHICmdList, View, &AllwayParams->InstanceCullingDrawParams);
        });
}
```

**重要**: 第二个 Pass 的 depth access 设置 (`:1716`) 必须保持 `DepthRead_StencilRead`,这样新 Pass 才能在 tile-based GPU 上正确读取深度。

### 4.10 修改 10 — `MobileShadingRenderer.cpp`:在 `BuildInstanceCullingDrawParams` 添加新 Pass

**位置**: `MobileShadingRenderer.cpp:1433-1446`

```cpp
void FMobileSceneRenderer::BuildInstanceCullingDrawParams(FRDGBuilder& GraphBuilder, ...)
{
    // ... 现有代码 ...
    for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ++ViewIndex)
    {
        FViewInfo& View = Views[ViewIndex];
        // ... 现有代码:BasePass / TranslucencyMeshPass / DebugViewMode / MobileBasePassCSM ...

        // ★ 新增:AllwayFrontPass(若启用)
        if (GetAllwayFrontPassEnabled())
        {
            View.ParallelMeshDrawCommandPasses[EMeshPass::AllwayFrontPass].BuildRenderingCommands(
                GraphBuilder, ...);
        }
    }
}
```

### 4.11 修改 11 — `MobileShadingRenderer.cpp`:CVar 控制开关

**位置**: 任意 .cpp 顶部,例如 `MobileShadingRenderer.cpp:65`

```cpp
static TAutoConsoleVariable<int32> CVarAllwayFrontPass(
    TEXT("r.Mobile.AllwayFrontPass"),
    1,
    TEXT("Whether to enable the AllwayFrontPass on mobile platforms. ")
    TEXT("When enabled, primitives with bRenderInAllwayFrontPass=true are rendered ")
    TEXT("after translucency and above all other geometry."),
    ECVF_RenderThreadSafe);
```

并在 `BuildInstanceCullingDrawParams` 中加 `if (CVarAllwayFrontPass.GetValueOnRenderThread() > 0)` 守卫。

### 4.12 修改 12 — `MobileShadingRenderer.cpp`:`RenderForward` 总入口分发

**位置**: `MobileShadingRenderer.cpp:1567-1574` (在 `RenderForward` 内部)

```cpp
void FMobileSceneRenderer::RenderForward(FRDGBuilder& GraphBuilder, ...)
{
    // ... 现有代码 ...
    if (bRequiresMultiPass)
    {
        RenderForwardMultiPass(GraphBuilder, ...);
    }
    else
    {
        RenderForwardSinglePass(GraphBuilder, ...);
    }
}
```

无需修改,集成点在 Single/Multi Pass 内部完成。

---

## 5. 完整修改文件清单

| # | 文件路径 | 类型 | 关键改动 |
|---|---|---|---|
| 1 | `Engine/Source/Runtime/Renderer/Public/MeshPassProcessor.h` | 修改 | 新增 `EMeshPass::AllwayFrontPass` (`:22`);更新 `GetMeshPassName` 与 `static_assert` |
| 2 | `Engine/Source/Runtime/Engine/Public/PrimitiveViewRelevance.h` | 修改 | 新增 `bRenderInAllwayFrontPass` 位 |
| 3 | `Engine/Source/Runtime/Engine/Classes/Components/PrimitiveComponent.h` | 修改 | 新增 `bRenderInAllwayFrontPass` 属性 + setter |
| 4 | `Engine/Source/Runtime/Engine/Private/Components/PrimitiveComponent.cpp` | 修改 | 实现 `SetRenderInAllwayFrontPass` |
| 5 | `Engine/Source/Runtime/Engine/Public/PrimitiveSceneProxy.h` | 修改 | 新增 `bRenderInAllwayFrontPass` 字段 |
| 6 | `Engine/Source/Runtime/Engine/Private/PrimitiveSceneProxy.cpp` | 修改 | 在子类 `GetViewRelevance` 添加标志位同步 |
| 7 | `Engine/Source/Runtime/Renderer/Private/SceneVisibility.cpp` | 修改 | `PassMask.Set(EMeshPass::AllwayFrontPass)` (`:2228`) |
| 8 | `Engine/Source/Runtime/Renderer/Private/MobileAllwayFrontPass.h` | **新建** | Processor 类定义 |
| 9 | `Engine/Source/Runtime/Renderer/Private/MobileAllwayFrontPass.cpp` | **新建** | Processor 实现 + 注册宏 |
| 10 | `Engine/Source/Runtime/Renderer/Private/MobileShadingRenderer.cpp` | 修改 | (a) `BuildInstanceCullingDrawParams` 集成; (b) `RenderForwardSinglePass` lambda 内调用; (c) `RenderForwardMultiPass` 新增第三 RDG Pass; (d) CVar |
| 11 | `Engine/Source/Runtime/Renderer/Private/StaticMeshSceneProxy.cpp` | 修改 | `GetViewRelevance` 同步 `bRenderInAllwayFrontPass` |
| 12 | `Engine/Source/Runtime/Renderer/Private/SkeletalMeshSceneProxy.cpp` | 修改 | 同上(Skeletal Mesh) |
| 13 | `Engine/Source/Runtime/Engine/Classes/Components/StaticMeshComponent.h` | 修改 | (可选) 暴露 `bRenderInAllwayFrontPass` BP 接口 |

**总计**:2 新建 + 11 修改。

---

## 6. 深度与 Stencil 行为详解

### 6.1 深度测试流程

| 阶段 | 写入深度 | 深度测试 | 备注 |
|---|---|---|---|
| PrePass | ✅ | LE | 写入最小深度 |
| BasePass | ✅ | LE | 标准不透明 |
| Decals | ❌ | LE | 仅深度测试 |
| **Translucency** | ❌ | LE | 仅深度测试 |
| **AllwayFrontPass** | ❌ | **GE** | **核心设计:始终在最上面** |

**为什么用 GE 而非 Always**: GE 保证 P1 物体之间仍按深度排序(避免穿插穿模),但都比已有像素更近或相等,即绘制在最上层。

### 6.2 Stencil 设计

新 Pass **不写 Stencil**。透明物体的 Stencil 也不被新 Pass 读取——因为深度测试已足够保证顺序。

若未来需要按 Stencil 分组(例如 P1 物体有不同的"层"),可用 P1 的 Stencil 值 0x80,但默认不开启。

### 6.3 与阴影的交互

**默认** `bRenderInAllwayFrontPass=true` 的物体 **不投射阴影**。理由:

- 阴影需在 Light Pass 期间绘制,时机比 AllwayFrontPass 更早
- 强行让 P1 投射阴影会破坏光照一致性

若需要 P1 物体投射阴影,可扩展:

```cpp
// 在 PrimitiveComponent.h 添加
UPROPERTY(EditAnywhere, AdvancedDisplay, BlueprintReadOnly, Category = Rendering,
          meta = (EditCondition = "bRenderInAllwayFrontPass"))
uint8 bCastShadowInAllwayFrontPass:1;
```

并修改 `FPrimitiveSceneProxy::GetLightingRelevance` 同步此标志。

---

## 7. 已知风险与缓解

| 风险 | 严重度 | 缓解 |
|---|---|---|
| `PostRenderBasePassMobile_RenderThread` 钩子不触发 | 低 | 不依赖此钩子,在 RDG Pass lambda 内直接调用 |
| Single Pass subpass 切换破坏 tile 优化 | **中** | 不要 `NextSubpass()`,保持在同一 subpass 内 |
| Multi Pass 新增 Pass 增加 render pass 切换 | 中 | 用 `ELoad` 而非 `Clear`,与 `DecalsAndTranslucency` 模式一致 |
| 物体同时进 BasePass 和 AllwayFrontPass | **高** | 强制 `bRenderInMainPass=false` 当 `bRenderInAllwayFrontPass=true` |
| 透明物体与 P1 物体重叠时穿模 | 中 | GE 测试已规避;若仍有,关闭 P1 的 `bRenderInDepthPass` |
| `EMeshPass::Num` 静态断言失败 | 低 | 同步更新 `static_assert` 与 `GetMeshPassName` switch |
| 多 View 立体声支持 | 中 | 确保 `BuildInstanceCullingDrawParams` 对所有 View 启用;`RenderMobileAllwayFrontPass` 接受 `FViewInfo` 即可 |
| MSAA 兼容性 | 中 | 复用 BasePass 的 MSAA 路径,SceneColor 是 multisample texture 时正确 |
| 与 Lumen / Nanite 交互 | 高 | 当前 LumenFrontLayerTranslucency 等已占用 24-27 位,新值 22 不会冲突;但需测试 Lumen 反射 |
| 与 Niagara GPU Particle 交互 | 中 | Niagara Opaque 仍走 BasePass;Translucent 走 Translucency;不受影响 |
| 与 CustomDepth 交互 | 低 | P1 物体不进 CustomDepth(在 ViewRelevance 强制 `bRenderCustomDepth=false`),不会污染 Outlines 等 |

---

## 8. 调试与验证方法

### 8.1 启用 CVar

```bash
# 启用 AllwayFrontPass
r.Mobile.AllwayFrontPass 1

# 关闭基线 BasePass 以对比
# r.Mobile.BasePass... (无对应 CVar;通过 r.Mobile.DisableBasePass 模拟)
```

### 8.2 RenderDoc 抓帧验证

1. 在场景中放置一个 `StaticMeshActor` (设为 `bRenderInAllwayFrontPass=true`)。
2. 抓取 Mobile Vulkan / GLES 帧。
3. 检查 `AllwayFrontPass` 事件出现在 `Translucency` 之后。
4. 检查深度 attachment 在 AllwayFrontPass 期间为 `ReadOnly`。
5. 验证 SceneColor 像素为 P1 物体颜色(覆盖在透明物体之上)。

### 8.3 验证清单

- [ ] P1 物体不在 BasePass 中绘制
- [ ] P1 物体在 Translucency 之后绘制
- [ ] P1 物体始终显示在所有透明物体之上
- [ ] P1 物体之间按深度正确排序
- [ ] 普通不透明物体行为不变
- [ ] 普通透明物体行为不变
- [ ] 阴影正常(非 P1 物体投射阴影,无 P1 物体阴影)
- [ ] MSAA 边缘不出现破绽
- [ ] 多 View (VR/立体声) 一致
- [ ] 性能:无明显帧率下降(PSO cache 命中)

---

## 9. 备选方案摘要

### 9.1 方案 B — SceneViewExtension + RenderPostOpaqueExtensions

**实现思路**:注册 SceneViewExtension,在 `RenderPostOpaqueExtensions` (`:1347`) 钩子中用 `GraphBuilder.AddPass` 手动收集并渲染 P1 物体。

**优点**:
- 入侵最小,无需修改 MeshPassProcessor 体系
- 适合 A/B 测试

**缺点**:
- 需自行管理 PSO 收集
- 需手动遍历 `Scene->PrimitiveSceneProxies` 过滤 P1
- 性能稍差(无 PSO cache 优化)

**适用**:临时验证、不便修改核心 Renderer。

### 9.2 方案 C — 复用 CustomDepth + Stencil 分流

**实现思路**:
- `bRenderInMainPass=false`, `bRenderCustomDepth=true`, `SetCustomDepthStencilValue(MyID)`
- 在 `PostProcess` Material 中读 `CustomDepth`,对 `MyID` 像素写自定义颜色

**优点**:
- 几乎零代码修改

**缺点**:
- 物体不能在场景中真实 3D 渲染,只在 PostProcess 中"重画"
- 多次 PSO、Stencil 位数有限
- 性能差

**适用**:装饰性叠加层(命名板、UI)。

### 9.3 方案 D — 透明 + Holdout 技巧

**实现思路**:把 P1 物体材质改为 `Translucent` 域,使用 `Holdout` 混合模式,配合自定义后处理。

**不推荐**:Holdout 仅对 GBuffer 模式有意义,Mobile Forward 下不适用。

---

## 10. 实施步骤(给开发者的操作清单)

### 第 1 天:基础结构
1. 修改 `MeshPassProcessor.h`,新增 `EMeshPass::AllwayFrontPass`,编译通过
2. 修改 `PrimitiveViewRelevance.h`,新增 `bRenderInAllwayFrontPass` 位
3. 编译验证(应无错误,仅 PassMask 分配处可能产生警告)

### 第 2 天:数据通路
4. 修改 `PrimitiveComponent.h/.cpp`,新增 `bRenderInAllwayFrontPass` 与 setter
5. 修改 `PrimitiveSceneProxy.h/.cpp`,添加镜像字段
6. 修改 `StaticMeshSceneProxy.cpp` 与 `SkeletalMeshSceneProxy.cpp`,同步 `GetViewRelevance`
7. 编译验证,运行简单场景确认属性编辑器出现 `bRenderInAllwayFrontPass` 复选框

### 第 3 天:Pass 实现
8. 新建 `MobileAllwayFrontPass.h/.cpp`,实现 Processor + 工厂 + 注册
9. 修改 `SceneVisibility.cpp`,添加 PassMask 分配
10. 编译验证(`REGISTER_*` 宏应正确触发 PSO collector)

### 第 4 天:渲染器集成
11. 修改 `MobileShadingRenderer.cpp`:
    - `BuildInstanceCullingDrawParams` 添加新 Pass 的 `BuildRenderingCommands`
    - `RenderForwardSinglePass` 在 `RenderTranslucency` 之后调用
    - `RenderForwardMultiPass` 新增第三 RDG Pass
    - 添加 CVar
12. 编译验证

### 第 5 天:测试与调优
13. 编写测试场景(包含 P1、不透明、透明物体)
14. RenderDoc 抓帧验证顺序
15. 性能 profiling,优化 PSO 命中率

---

## 11. 附录

### 11.1 关键源码引用汇总

| 文件:行号 | 内容 |
|---|---|
| `MobileShadingRenderer.cpp:1578-1660` | `RenderForwardSinglePass`(Vulkan subpass 结构) |
| `MobileShadingRenderer.cpp:1609` | BasePass 调用点 |
| `MobileShadingRenderer.cpp:1612` | `PostRenderBasePass` 钩子 |
| `MobileShadingRenderer.cpp:1623` | Translucency 调用点(Single) |
| `MobileShadingRenderer.cpp:1662-1749` | `RenderForwardMultiPass`(GLES/Metal) |
| `MobileShadingRenderer.cpp:1716` | Multi Pass depth access 设置 |
| `MobileShadingRenderer.cpp:1721-1740` | `DecalsAndTranslucency` 第二 Pass 模板 |
| `MobileShadingRenderer.cpp:1735` | Translucency 调用点(Multi) |
| `MobileShadingRenderer.cpp:2079-2090` | `PostRenderBasePass` SVE 钩子分发 |
| `MobileShadingRenderer.cpp:1433-1446` | `BuildInstanceCullingDrawParams` |
| `MobileBasePass.cpp:531-561` | `SetOpaqueRenderState` 参考 |
| `MobileBasePass.cpp:810-826` | `FMobileBasePassMeshProcessor` 构造 |
| `MobileBasePass.cpp:828-849` | `ShouldDraw` 判定模式 |
| `MeshPassProcessor.h:32-79` | `EMeshPass` 枚举 |
| `MeshPassProcessor.h:83-135` | `GetMeshPassName` switch |
| `MeshPassProcessor.h:2266-2272` | `REGISTER_MESHPASSPROCESSOR_AND_PSOCOLLECTOR` 宏 |
| `SceneVisibility.cpp:2198-2286` | Pass 分配核心逻辑 |
| `SceneVisibility.cpp:2211-2236` | BasePass 块 |
| `PrimitiveViewRelevance.h:39-77` | 标志位定义 |
| `PrimitiveComponent.h:408` | `bRenderInMainPass` |
| `PrimitiveComponent.cpp:4457` | `SetRenderInMainPass` |
| `PrimitiveSceneProxy.cpp:740-743` | 基类 `GetViewRelevance` |

### 11.2 命名空间规范

本方案所有新增符号采用 `AllwayFront` 前缀(对应文件名 `AllwayRenderFront.md`):

- `EMeshPass::AllwayFrontPass`
- `bRenderInAllwayFrontPass`
- `SetRenderInAllwayFrontPass`
- `FMobileAllwayFrontPassMeshProcessor`
- `RenderMobileAllwayFrontPass`
- `CVarAllwayFrontPass`
- `r.Mobile.AllwayFrontPass`

### 11.3 性能预估

- **新增 PSO 数量**:理论上 0(复用 BasePass PSO,只换 depth-stencil state;PSO cache 应能命中)
- **新增 RDG Pass**:Multi Pass 模式 +1(从 2 个变 3 个);Single Pass 模式 +0(同一 subpass 内)
- **GPU 时间开销**:每个 P1 物体 ≈ 1 次 BasePass 绘制(无阴影、DepthWrite Off)
- **CPU 时间开销**:仅 Setup 阶段多 1 次 `BuildRenderingCommands`(微秒级)

### 11.4 引用参考

- UE5.4 源码: `E:/Unreal Engine Work Projects/MR01_DaNaoTianGong_Main/Engine/Source/Runtime/Renderer/Private/MobileShadingRenderer.cpp`
- EPIC OpenColorIO 案例:`Engine/Plugins/Compositing/OpenColorIO/Source/OpenColorIO/Private/OpenColorIODisplayExtension.cpp`(SceneViewExtension + SubscribeToPostProcessingPass 参考)
- EPIC PPMChainGraph 案例:`Engine/Plugins/Experimental/PostProcessMaterialChainGraph/Source/PostProcessMaterialChainGraph/Private/PPMChainGraphSceneViewExtension.cpp`(PrePostProcessPass 模式参考)
- EPIC MediaCapture 案例:`Engine/Plugins/Media/MediaIOFramework/Source/MediaIOCore/Private/MediaCaptureSceneViewExtension.h`(只读 SceneColor 模式参考)

---

## 12. 版本历史

| 版本 | 日期 | 内容 |
|---|---|---|
| v1.0 | 2026-06-18 | 初版:基于 UE5.4 源码研究,推荐方案 A,完整实现步骤 |

---

**注意**:本文档为源码修改方案,实施前请:
1. 备份当前 `Engine/Source` 目录
2. 在独立分支上开发
3. 单元测试 SceneVisibility 与 MobileShadingRenderer 改动
4. RenderDoc 抓帧验证
5. 性能 profiling(目标:零帧率下降)
6. 跨平台验证(Android GLES + Vulkan)
