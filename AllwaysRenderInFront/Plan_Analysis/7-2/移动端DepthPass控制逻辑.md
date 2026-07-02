# 移动端 Depth Pass 控制逻辑（SceneVisibility.cpp）

> 本文档整理 `Engine/Source/Runtime/Renderer/Private/SceneVisibility.cpp` 中**移动端（`EShadingPath::Mobile`）**如何控制 Depth Pass（Early-Z 预渲染 / 深度预处理）的逻辑。
> 涉及的关键外部源码：`RenderCore/Private/RenderUtils.cpp`、`Renderer/Private/RendererScene.cpp`、`Renderer/Private/DepthRendering.h`、`Engine/Public/PrimitiveViewRelevance.h`。

---

## 1. 概述

移动端的 Depth Pass 与 Deferred 路径有明显差异，核心区别有三点：

1. **移动端默认不做深度预渲染**（`DDM_None`），而 Deferred 默认 `DDM_NonMaskedOnly`。
2. **移动端禁用“按屏幕尺寸选择性写入深度”**（`bDrawDepthOnly` 的尺寸阈值分支被 `ShadingPath != EShadingPath::Mobile` 关掉）。也就是说，移动端要么全画，要么不画，不存在“大物体才画深度”的中间档。
3. **移动端从不使用 `SecondStageDepthPass`**（两阶段深度）——所有相关入口都被 `ShadingPath != EShadingPath::Mobile` 守卫挡住，移动端只会走 `EMeshPass::DepthPass`。

最终移动端 Depth Pass 落到三种模式之一，由 `Scene.EarlyZPassMode` 决定：

| 模式 | `EDepthDrawingMode` | 触发条件 | 效果 |
|---|---|---|---|
| 无 Prepass | `DDM_None` | 默认（`r.Mobile.EarlyZPass==0` 且无强制特性） | 不收集任何 Depth Pass 网格命令 |
| 仅 Masked | `DDM_MaskedOnly` | `r.Mobile.EarlyZPass==2` | 只把 masked（alpha test）物体加入 Depth Pass |
| 完整 Prepass | `DDM_AllOpaque` | `MobileUsesFullDepthPrepass` 为真（见 §3.2） | 所有可深度物体全部写入 Depth Pass |

---

## 2. 关键数据来源

### 2.1 `EDepthDrawingMode` 枚举
定义于 `Renderer/Private/DepthRendering.h:20`：

```cpp
enum EDepthDrawingMode
{
    DDM_None              = 0, // 不做深度预渲染
    DDM_NonMaskedOnly     = 1, // 仅不透明
    DDM_AllOccluders      = 2, // 不透明+masked，但排除 bUseAsOccluder=false 的对象
    DDM_AllOpaque         = 3, // 完整 prepass，每像素深度需与 base pass 一致
    DDM_MaskedOnly        = 4, // 仅 masked 材质（移动端 masked early pass 用）
    DDM_AllOpaqueNoVelocity= 5,// 完整 prepass，但动态几何交给 Velocity pass 写深度
};
```

移动端只关心 `DDM_None` / `DDM_MaskedOnly` / `DDM_AllOpaque` 三种。`DDM_AllOpaqueNoVelocity` 在移动端分支里**不会**被选中。

### 2.2 View Relevance 标志
定义于 `Engine/Public/PrimitiveViewRelevance.h`：

- `bRenderInDepthPass`（`:52`）——该 primitive 是否参与 Depth Pass。
- `bRenderInSecondStageDepthPass`（`:70`）——是否参与第二阶段深度（**移动端恒不使用**）。
- `FStaticMeshBatchRelevance::bUseForDepthPass`——静态网格批次是否参与 Depth Pass。

---

## 3. 控制流程详解

### 3.1 `Scene.EarlyZPassMode` 的确定（移动端分支）
`Renderer/Private/RendererScene.cpp:4665` `FScene::GetEarlyZPassMode`，移动端分支（`:4694-4708`）：

```cpp
else if (GetFeatureLevelShadingPath(InFeatureLevel) == EShadingPath::Mobile)
{
    OutZPassMode = DDM_None;                       // 默认：不做深度预渲染

    const bool bMaskedOnlyPrePass = FReadOnlyCVARCache::MobileEarlyZPass(ShaderPlatform) == 2;
    if (bMaskedOnlyPrePass)
    {
        OutZPassMode = DDM_MaskedOnly;             // r.Mobile.EarlyZPass == 2
    }

    if (MobileUsesFullDepthPrepass(ShaderPlatform))
    {
        OutZPassMode = DDM_AllOpaque;               // 强制完整 prepass，覆盖上面的 masked-only
    }
}
```

**优先级**：`MobileUsesFullDepthPrepass` > `r.Mobile.EarlyZPass==2` > 默认 `DDM_None`。

### 3.2 `bFullEarlyZPass` 的设置
`SceneVisibility.cpp:1059`，`FFilterStaticMeshesForViewData` 构造函数：

```cpp
bFullEarlyZPass = ShouldForceFullDepthPass(View.GetShaderPlatform());
```

`ShouldForceFullDepthPass`（`RenderCore/Private/RenderUtils.cpp:621`）对移动平台直接委托给 `MobileUsesFullDepthPrepass`：

```cpp
RENDERCORE_API bool ShouldForceFullDepthPass(const FStaticShaderPlatform Platform)
{
    if (IsMobilePlatform(Platform))
    {
        return MobileUsesFullDepthPrepass(Platform);   // ← 移动端走这里
    }
    else { /* Nanite / AO / DBuffer / VT / StencilLODDither / ... */ }
}
```

`MobileUsesFullDepthPrepass`（`RenderUtils.cpp:616`）——任一为真即强制完整 prepass：

```cpp
return MobileUsesShadowMaskTexture(Platform)        // 需要阴影 mask 纹理
    || IsMobileAmbientOcclusionEnabled(Platform)    // 移动端 AO 开启
    || IsUsingDBuffers(Platform)                     // DBuffer 贴花
    || FReadOnlyCVARCache::MobileEarlyZPass(Platform) == 1;  // r.Mobile.EarlyZPass == 1
```

> ⚠️ `ShouldForceFullDepthPass` 影响静态 draw list 的归属，其依赖项**运行时不可变**，修改对应 cvar 后需 `FGlobalComponentRecreateRenderStateContext` 才能生效（见 `RenderUtils.cpp:641` 注释）。

### 3.3 逐帧局部标志（`FRelevancePacket::ComputeRelevance`）
`SceneVisibility.cpp:1320-1322`：

```cpp
const bool bMobileMaskedInEarlyPass     = (ShadingPath == EShadingPath::Mobile) && Scene.EarlyZPassMode == DDM_MaskedOnly;
const bool bMobileBasePassAlwaysUsesCSM  = (ShadingPath == EShadingPath::Mobile) && MobileBasePassAlwaysUsesCSM(Scene.GetShaderPlatform());
const bool bVelocityPassWritesDepth     = Scene.EarlyZPassMode == DDM_AllOpaqueNoVelocity;
```

- `bMobileMaskedInEarlyPass`：移动端 + `DDM_MaskedOnly` → masked 物体进入 Depth Pass 的开关。
- `bMobileBasePassAlwaysUsesCSM`：与 CSM 着色剔除相关（见 §3.6，非深度本身）。
- `bVelocityPassWritesDepth`：仅 `DDM_AllOpaqueNoVelocity` 时为真（移动端不会取到该模式，故对移动端恒为 false）。

### 3.4 ★核心决策：`bDrawDepthOnly`
`SceneVisibility.cpp:1423`：

```cpp
const bool bDrawDepthOnly =
      ViewData.bFullEarlyZPass
   || ((ShadingPath != EShadingPath::Mobile)                                // ← 移动端此分支恒 false
       && (FMath::Square(Bounds.BoxSphereBounds.SphereRadius)
           > GMinScreenRadiusForDepthPrepass * GMinScreenRadiusForDepthPrepass * LODFactorDistanceSquared));
```

**关键结论**：
- Deferred 路径下，大物体（屏幕半径 > `GMinScreenRadiusForDepthPrepass` 阈值）也会被画入 Depth Pass，小物体跳过——这是性能/收益权衡。
- 移动端把这一行用 `ShadingPath != Mobile` 显式关闭。因此移动端的 `bDrawDepthOnly` **只可能**因 `bFullEarlyZPass` 为真而变真；一旦 `bFullEarlyZPass` 为假，`bDrawDepthOnly` 恒为假（没有“按尺寸选画”这一档）。

### 3.5 深度命令收集
`SceneVisibility.cpp:1530-1543`：

```cpp
// Add depth commands.
if (StaticMeshRelevance.bUseForDepthPass
    && (bDrawDepthOnly || (bMobileMaskedInEarlyPass && ViewRelevance.bMasked)))
{
    if (!(bIsMeshInVelocityPass && bVelocityPassWritesDepth))
    {
        if (ViewRelevance.bRenderInSecondStageDepthPass && ShadingPath != EShadingPath::Mobile)  // 移动端恒 false
        {
            DrawCommandPacket.AddCommandsForMesh(..., EMeshPass::SecondStageDepthPass);
        }
        else
        {
            DrawCommandPacket.AddCommandsForMesh(..., EMeshPass::DepthPass);                    // ← 移动端走这里
        }
    }
    // (RayTracing dithered LOD fading 分支略)
}
```

进入 Depth Pass 的条件（移动端）：
1. `bUseForDepthPass` 为真（静态网格批次声明参与深度）；
2. 且满足以下其一：
   - `bDrawDepthOnly` 为真 → 即 `bFullEarlyZPass` 为真（完整 prepass 模式，所有可深度物体都进）；
   - **或** `bMobileMaskedInEarlyPass && ViewRelevance.bMasked` → 即处于 `DDM_MaskedOnly` 模式且该网格是 masked。
3. 排除已被 Velocity pass 写过深度的网格（`bVelocityPassWritesDepth`，移动端恒 false，故不触发）。
4. 因 `ShadingPath != Mobile` 守卫，移动端**永远不会**走 `SecondStageDepthPass`，统一进 `EMeshPass::DepthPass`。

> 注：外层还有一道门 `SceneVisibility.cpp:1500-1502`，要求 `bUseForMaterial || bUseAsOccluder` 且 `bRenderInMainPass || bRenderCustomDepth || bRenderInDepthPass`，且未被 HLOD fade 隐藏。

### 3.6 移动端 Base Pass 特例（`SceneVisibility.cpp:1558-1577`）
这部分不属于 Depth Pass，但与深度命令收集同处一个 `if`，列出以避免混淆：

```cpp
if (ShadingPath == EShadingPath::Mobile)
{
    if (!StaticMeshRelevance.bUseSkyMaterial)               // 非天穹
    {
        DrawCommandPacket.AddCommandsForMesh(..., EMeshPass::BasePass);
        if (!bMobileBasePassAlwaysUsesCSM)                  // CSM 不做单独 pass 时才加 MobileBasePassCSM
            DrawCommandPacket.AddCommandsForMesh(..., EMeshPass::MobileBasePassCSM);
    }
    else
    {
        DrawCommandPacket.AddCommandsForMesh(..., EMeshPass::SkyPass);   // 天穹单独走 SkyPass
    }
    MarkMask |= EMarkMaskBits::StaticMeshVisibilityMapMask;
}
```

`MobileBasePassAlwaysUsesCSM`（`RenderUtils.cpp:602`）：移动延迟渲染恒为真；前向路径下仅当 `r.Mobile.Shadow.CSMShaderCullingMethod==5` 且开启移动距离场时为真。

### 3.7 `SecondStageDepthPass` 在移动端被禁用（共 3 处）
两阶段深度是 Deferred 专属优化，移动端三处守卫一致关闭：

1. `SceneVisibility.cpp:1221`（`FRelevancePacket::Finalize`）：
   ```cpp
   WriteView.bUsesSecondStageDepthPass |= bUsesSecondStageDepthPass && ShadingPath != EShadingPath::Mobile;
   ```
2. `SceneVisibility.cpp:1912`（静态 relevance 累计）：
   ```cpp
   bUsesSecondStageDepthPass |= ViewRelevance.bRenderInSecondStageDepthPass && ShadingPath != EShadingPath::Mobile;
   ```
3. `SceneVisibility.cpp:2200`（动态网格 `ComputeDynamicMeshRelevance`）：
   ```cpp
   if (ViewRelevance.bRenderInSecondStageDepthPass && ShadingPath != EShadingPath::Mobile) {
       PassMask.Set(EMeshPass::SecondStageDepthPass);
       View.NumVisibleDynamicMeshElements[EMeshPass::SecondStageDepthPass] += NumElements;
   } else {
       PassMask.Set(EMeshPass::DepthPass);                       // ← 移动端走这里
       View.NumVisibleDynamicMeshElements[EMeshPass::DepthPass] += NumElements;
   }
   ```
   动态网格元素同样：移动端恒进 `EMeshPass::DepthPass`，不会进 `SecondStageDepthPass`。

### 3.8 Stencil Dither（LOD 抖动用模板）
`SceneVisibility.cpp:5437-5444`，`FDeferredShadingSceneRenderer::PreVisibilityFrameSetup`：

```cpp
for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ViewIndex++)
{
    FViewInfo& View = Views[ViewIndex];
    View.bAllowStencilDither = DepthPass.bDitheredLODTransitionsUseStencil;
}
```

该标志在所有路径（含移动端）都会被设置，控制 LOD 抖动过渡是否在 Depth Pass 阶段复用 stencil 通道。移动端是否真正启用还取决于 `DepthPass.bDitheredLODTransitionsUseStencil` 的取值（由 `r.StencilForLODDither` 等驱动）。

---

## 4. 数据流总结

```
                    ┌─────────────────────────────────────────────┐
   cvar/特性        │  GetEarlyZPassMode (RendererScene.cpp:4665) │
                    │  —— 移动端分支 ——                            │
                    └─────────────────────────────────────────────┘
   r.Mobile.EarlyZPass==0          ─► DDM_None          (默认)
   r.Mobile.EarlyZPass==2          ─► DDM_MaskedOnly
   MobileUsesFullDepthPrepass==真 ─► DDM_AllOpaque      (覆盖)
        (ShadowMaskTexture / MobileAO / DBuffer / r.Mobile.EarlyZPass==1)
                                   │
                    Scene.EarlyZPassMode
                                   ▼
   ┌──────────────────────────────────────────────────────────────┐
   │ FFilterStaticMeshesForViewData (SceneVisibility.cpp:1047)     │
   │   bFullEarlyZPass = ShouldForceFullDepthPass(Platform)        │
   │      (移动端 ⇔ MobileUsesFullDepthPrepass)                    │
   └──────────────────────────────────────────────────────────────┘
                                   ▼
   ┌──────────────────────────────────────────────────────────────┐
   │ FRelevancePacket::ComputeRelevance (SceneVisibility.cpp:1299)│
   │   bMobileMaskedInEarlyPass = Mobile && EarlyZPassMode==Masked│
   │   bDrawDepthOnly = bFullEarlyZPass                            │
   │                  || ((ShadingPath!=Mobile) && size>threshold) │  ← 移动端关闭尺寸分支
   └──────────────────────────────────────────────────────────────┘
                                   ▼
   ┌──────────────────────────────────────────────────────────────┐
   │ 深度命令收集 (SceneVisibility.cpp:1531)                       │
   │  if (bUseForDepthPass && (bDrawDepthOnly                      │
   │                          || (bMobileMaskedInEarlyPass&&Masked)))│
   │     → EMeshPass::DepthPass   (移动端恒不进 SecondStageDepthPass)│
   └──────────────────────────────────────────────────────────────┘
```

**移动端三种模式下的实际行为**：

| `EarlyZPassMode` | `bFullEarlyZPass` | `bMobileMaskedInEarlyPass` | `bDrawDepthOnly` | 进入 DepthPass 的网格 |
|---|---|---|---|---|
| `DDM_None` | false | false | false | 无（不收集） |
| `DDM_MaskedOnly` | false | true | false | 仅 masked 网格 |
| `DDM_AllOpaque` | true | false | true | 所有 `bUseForDepthPass` 网格 |

---

## 5. 相关 CVar 汇总

| CVar | 取值 | 含义 |
|---|---|---|
| `r.Mobile.EarlyZPass` | 0 | 不做深度预渲染（默认） |
| `r.Mobile.EarlyZPass` | 1 | 强制完整 prepass（`MobileUsesFullDepthPrepass`） |
| `r.Mobile.EarlyZPass` | 2 | 仅 masked 物体 prepass（`DDM_MaskedOnly`） |
| `r.Mobile.AmbientOcclusion` | 1 | 移动端 AO，隐式强制完整 prepass |
| `r.DBuffer` | 1 | DBuffer 贴花，隐式强制完整 prepass |
| `r.Mobile.Shadow.CSMShaderCullingMethod` | 5 | 配合距离场时 `MobileBasePassAlwaysUsesCSM`（影响 base pass，非深度） |
| `r.StencilForLODDither` | 1 | 模板 LOD 抖动，影响 `bAllowStencilDither` |

> `r.Mobile.EarlyZPass` 经 `FReadOnlyCVARCache::MobileEarlyZPass` 缓存读取。

---

## 6. 关键源码位置索引

| 位置 | 内容 |
|---|---|
| `SceneVisibility.cpp:1047-1060` | `FFilterStaticMeshesForViewData` 构造，设置 `bFullEarlyZPass` |
| `SceneVisibility.cpp:1196-1221` | `FRelevancePacket::Finalize`，`:1221` 守卫 `bUsesSecondStageDepthPass` |
| `SceneVisibility.cpp:1299-1322` | `ComputeRelevance`，移动端局部标志（`bMobileMaskedInEarlyPass` 等） |
| `SceneVisibility.cpp:1423` | ★`bDrawDepthOnly` 决策（移动端关闭尺寸分支） |
| `SceneVisibility.cpp:1500-1502` | 深度命令外层门控（material/depthpass/customdepth） |
| `SceneVisibility.cpp:1530-1543` | ★深度命令收集，移动端恒走 `EMeshPass::DepthPass` |
| `SceneVisibility.cpp:1558-1577` | 移动端 Base Pass 特例（SkyPass / MobileBasePassCSM） |
| `SceneVisibility.cpp:1912` | 静态 relevance 累计 `bUsesSecondStageDepthPass` 守卫 |
| `SceneVisibility.cpp:2186-2209` | `ComputeDynamicMeshRelevance`，动态网格深度 pass 选择 |
| `SceneVisibility.cpp:5437-5444` | `PreVisibilityFrameSetup`，`bAllowStencilDither` |
| `RenderUtils.cpp:602-614` | `MobileBasePassAlwaysUsesCSM` |
| `RenderUtils.cpp:616-619` | `MobileUsesFullDepthPrepass` |
| `RenderUtils.cpp:621-644` | `ShouldForceFullDepthPass`（移动端委托给上一条） |
| `RendererScene.cpp:4665-4708` | `FScene::GetEarlyZPassMode`，移动端分支 `:4694-4708` |
| `DepthRendering.h:20-34` | `EDepthDrawingMode` 枚举定义 |
| `PrimitiveViewRelevance.h:52,70` | `bRenderInDepthPass` / `bRenderInSecondStageDepthPass` |

---

## 7. 一句话总结

> 移动端 Depth Pass 是**三选一**的：默认不做（`DDM_None`）；`r.Mobile.EarlyZPass=2` 时只画 masked（`DDM_MaskedOnly`）；当需要 ShadowMask / AO / DBuffer 或 `r.Mobile.EarlyZPass=1` 时强制全画（`DDM_AllOpaque`）。移动端**关闭按屏幕尺寸选择性写深度**，且**永不使用两阶段深度**（`SecondStageDepthPass`），所有深度命令统一进 `EMeshPass::DepthPass`。
