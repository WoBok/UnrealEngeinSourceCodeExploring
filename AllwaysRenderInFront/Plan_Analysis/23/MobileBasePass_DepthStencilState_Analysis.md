# MobileBasePass 深度渲染分析

> 源码位置：`Engine/Source/Runtime/Renderer/Private/MobileBasePass.cpp:1151-1163`
>
> 关键行：
> ```cpp
> PassDrawRenderState.SetDepthStencilState(TStaticDepthStencilState<true, CF_DepthNearOrEqual>::GetRHI());
> ```

---

## 一、`SetDepthStencilState` 与 `TStaticDepthStencilState` 的作用

### 1. `TStaticDepthStencilState<...>` 是什么

定义在 `Engine/Source/Runtime/RenderCore/Public/RHIStaticStates.h:213-255`：

```cpp
class TStaticDepthStencilState : public TStaticStateRHI<
    TStaticDepthStencilState<
        bEnableDepthWrite,                // 模板参数 1
        DepthTest,                        // 模板参数 2
        bEnableFrontFaceStencil,
        FrontFaceStencilTest,
        FrontFaceStencilFailStencilOp,
        FrontFaceDepthFailStencilOp,
        FrontFacePassStencilOp,
        bEnableBackFaceStencil,
        BackFaceStencilTest,
        ...
        StencilReadMask,
        StencilWriteMask
        >,
    FDepthStencilStateRHIRef,
    FRHIDepthStencilState*
    >
{
public:
    static FDepthStencilStateRHIRef CreateRHI()
    {
        FDepthStencilStateInitializerRHI Initializer(
            bEnableDepthWrite, DepthTest,
            ...);
        return RHICreateDepthStencilState(Initializer);
    }
};
```

本质：一个模板化的"静态构造器"，把所有深度/模板状态编译期参数化，运行时通过 `RHICreateDepthStencilState` 一次性创建并缓存 `FDepthStencilStateRHIRef`。

### 2. `CF_DepthNearOrEqual` 的真正含义

定义在 `Engine/Source/Runtime/RHI/Public/RHIDefinitions.h:302`：

```cpp
enum ECompareFunction
{
    CF_Less,
    CF_LessEqual,
    CF_Greater,
    CF_GreaterEqual,
    CF_Equal,
    CF_NotEqual,
    CF_Never,
    CF_Always,
    ECompareFunction_Num,
    ECompareFunction_NumBits = 3,

    // Utility enumerations
    CF_DepthNearOrEqual   = (((int32)ERHIZBuffer::IsInverted != 0) ? CF_GreaterEqual : CF_LessEqual),
    CF_DepthNear          = (((int32)ERHIZBuffer::IsInverted != 0) ? CF_Greater      : CF_Less),
    CF_DepthFartherOrEqual= (((int32)ERHIZBuffer::IsInverted != 0) ? CF_LessEqual    : CF_GreaterEqual),
    CF_DepthFarther       = (((int32)ERHIZBuffer::IsInverted != 0) ? CF_Less         : CF_Greater),
};
```

- 正向 Z buffer：`CF_LessEqual`（更近或相等才通过）
- 反向 Z buffer：`CF_GreaterEqual`

`CF_DepthNearOrEqual` 即"近的/相等的才通过"，是常规不透明几何体使用的深度测试语义。

### 3. `MobileBasePass.cpp:1157` 模板参数语义

| 参数 | 值 | 含义 |
|------|----|------|
| `bEnableDepthWrite` | `true` | **深度写入开启** |
| `DepthTest` | `CF_DepthNearOrEqual` | 测试函数 = `LessEqual` 或 `GreaterEqual`（按 Z 方向） |
| `bEnableFrontFaceStencil` | `false`（默认） | 不开模板测试 |
| `...Stencil...` | 默认 | 模板禁用 |

综合语义：**深度测试开启 + 深度写入开启 + 通过条件为 NearOrEqual + 不动模板**。即"标准的、不透明几何体"渲染状态。

---

## 二、移动管线的深度渲染路径

### 1. `EarlyZPassMode` 决策

`Engine/Source/Runtime/Renderer/Private/RendererScene.cpp:4665-4709`：

```cpp
void FScene::GetEarlyZPassMode(ERHIFeatureLevel::Type InFeatureLevel,
                                EDepthDrawingMode & OutZPassMode,
                                bool& bOutEarlyZPassMovable)
{
    OutZPassMode = DDM_NonMaskedOnly;
    bOutEarlyZPassMovable = false;

    const EShaderPlatform ShaderPlatform = GetFeatureLevelShaderPlatform(InFeatureLevel);
    if (GetFeatureLevelShadingPath(InFeatureLevel) == EShadingPath::Deferred)
    {
        ...
    }
    else if (GetFeatureLevelShadingPath(InFeatureLevel) == EShadingPath::Mobile)
    {
        OutZPassMode = DDM_None;                              // ★ 默认无 prepass

        const bool bMaskedOnlyPrePass = MobileEarlyZPass == 2;
        if (bMaskedOnlyPrePass)
            OutZPassMode = DDM_MaskedOnly;

        if (MobileUsesFullDepthPrepass(ShaderPlatform))
            OutZPassMode = DDM_AllOpaque;                     // ★ 全不透明 prepass
    }
}
```

`MobileUsesFullDepthPrepass` 在 `Engine/Source/Runtime/RenderCore/Private/RenderUtils.cpp:616-619`：

```cpp
RENDERCORE_API bool MobileUsesFullDepthPrepass(const FStaticShaderPlatform Platform)
{
    return MobileUsesShadowMaskTexture(Platform)
        || IsMobileAmbientOcclusionEnabled(Platform)
        || IsUsingDBuffers(Platform)
        || FReadOnlyCVARCache::MobileEarlyZPass(Platform) == 1;
}
```

**结论**：Mobile 默认是 `DDM_None`，**没有 depth prepass**；只有显式开启 `MobileEarlyZPass=1` 或开启 AO / ShadowMask / DBuffer 时才有。

### 2. Depth Pass 入口

`Engine/Source/Runtime/Renderer/Private/DepthRendering.cpp:660-679`：

```cpp
bool FMobileSceneRenderer::ShouldRenderPrePass() const
{
    // Draw a depth pass to avoid overdraw in the other passes.
    return Scene->EarlyZPassMode == DDM_MaskedOnly
        || Scene->EarlyZPassMode == DDM_AllOpaque;
}

void FMobileSceneRenderer::RenderPrePass(FRHICommandList& RHICmdList,
                                         const FViewInfo& View,
                                         const FInstanceCullingDrawParams* InstanceCullingDrawParams)
{
    ...
    SetStereoViewport(RHICmdList, View);
    View.ParallelMeshDrawCommandPasses[EMeshPass::DepthPass].DispatchDraw(nullptr, RHICmdList, InstanceCullingDrawParams);
}
```

调用方在 `MobileShadingRenderer.cpp:824-836`（完整 depth prepass 路径）：

```cpp
View.ParallelMeshDrawCommandPasses[EMeshPass::DepthPass]
    .BuildRenderingCommands(GraphBuilder, Scene->GPUScene, PassParameters->InstanceCullingDrawParams);

GraphBuilder.AddPass(
    RDG_EVENT_NAME("FullDepthPrepass"),
    PassParameters,
    ERDGPassFlags::Raster,
    [this, PassParameters, &View, bDoOcclusionQueries](FRHICommandList& RHICmdList)
    {
        RenderPrePass(RHICmdList, View, &PassParameters->InstanceCullingDrawParams);
        ...
    });
```

### 3. Depth Pass 的状态设置（与 base pass 无关）

`DepthRendering.cpp:486-491`：

```cpp
void SetupDepthPassState(FMeshPassProcessorRenderState& DrawRenderState)
{
    // Disable color writes, enable depth tests and writes.
    DrawRenderState.SetBlendState(TStaticBlendState<CW_NONE>::GetRHI());
    DrawRenderState.SetDepthStencilState(TStaticDepthStencilState<true, CF_DepthNearOrEqual>::GetRHI());
}
```

`DepthRendering.cpp:753-784`：

```cpp
void SetMobileDepthPassRenderState(const FPrimitiveSceneProxy* PrimitiveSceneProxy,
                                   FMeshPassProcessorRenderState& DrawRenderState,
                                   const FMeshBatch& MeshBatch,
                                   bool bUsesDeferredShading)
{
    DrawRenderState.SetDepthStencilState(TStaticDepthStencilState<
        true, CF_DepthNearOrEqual,
        true, CF_Always, SO_Keep, SO_Keep, SO_Replace,
        false, CF_Always, SO_Keep, SO_Keep, SO_Keep,
        0x00, 0xff >::GetRHI());

    // ... stencil ref (RECEIVE_DECAL, MOBILE_SM_MASK, LIGHTING_CHANNELS, MOBILE_CAST_CONTACT_SHADOW)
    DrawRenderState.SetStencilRef(StencilValue);
}
```

Depth pass 处理器注册（`DepthRendering.cpp:1230-1243`）：

```cpp
FMeshPassProcessor* CreateDepthPassProcessor(...)
{
    EDepthDrawingMode EarlyZPassMode;
    bool bEarlyZPassMovable;
    FScene::GetEarlyZPassMode(FeatureLevel, EarlyZPassMode, bEarlyZPassMovable);

    FMeshPassProcessorRenderState DepthPassState;
    SetupDepthPassState(DepthPassState);

    return new FDepthPassMeshProcessor(EMeshPass::DepthPass, Scene, FeatureLevel,
                                       InViewIfDynamicMeshCommand, DepthPassState,
                                       true, EarlyZPassMode, bEarlyZPassMovable,
                                       false, InDrawListContext);
}

REGISTER_MESHPASSPROCESSOR_AND_PSOCOLLECTOR(DepthPass,      CreateDepthPassProcessor,
                                            EShadingPath::Deferred, EMeshPass::DepthPass, ...);
REGISTER_MESHPASSPROCESSOR_AND_PSOCOLLECTOR(MobileDepthPass, CreateDepthPassProcessor,
                                            EShadingPath::Mobile,   EMeshPass::DepthPass, ...);
```

**关键事实**：`EMeshPass::DepthPass` 使用**独立的** `FDepthPassMeshProcessor` + `SetupDepthPassState`，**完全不走** `CreateMobileBasePassProcessor`。所以 `MobileBasePass.cpp:1157` 那行与 depth pass 的运行状态没有直接关系——depth pass 渲染时不调用这里。

---

## 三、Base pass 与 Depth pass 的分流逻辑

### 1. Base pass 中按 mesh 切换 depth state

`Engine/Source/Runtime/Renderer/Private/MobileBasePass.cpp:945-961`：

```cpp
FMeshPassProcessorRenderState DrawRenderState(PassDrawRenderState);   // 拷贝默认 <true, CF_DepthNearOrEqual>
if (!bForcePassDrawRenderState)
{
    if (bTranslucentBasePass)
    {
        MobileBasePass::SetTranslucentRenderState(DrawRenderState, MaterialResource, ShadingModels);
        // 半透：<false, CF_DepthNearOrEqual>（只读深度）
    }
    // ★ 关键分支：同一 mesh 已被 depth pass 写过深度
    else if((MeshBatch.bUseForDepthPass && Scene->EarlyZPassMode == DDM_AllOpaque) || bMaskedInEarlyPass)
    {
        DrawRenderState.SetDepthStencilState(TStaticDepthStencilState<false, CF_Equal>::GetRHI());
        //                  bEnableDepthWrite=false, DepthTest=CF_Equal
        // 等值测试，不写深度（避免覆盖 depth pass 写出的有效深度）
    }
    else
    {
        const bool bEnableReceiveDecalOutput =
            ((Flags & EFlags::CanUseDepthStencil) == EFlags::CanUseDepthStencil);
        MobileBasePass::SetOpaqueRenderState(DrawRenderState, PrimitiveSceneProxy, MaterialResource,
                                             ShadingModels, bEnableReceiveDecalOutput && IsMobileHDR(),
                                             bPassUsesDeferredShading);
    }
}
```

其中 `bMaskedInEarlyPass` 的判定 (`MobileBasePass.cpp:942`)：

```cpp
const bool bMaskedInEarlyPass =
    (MaterialResource.IsMasked() || MeshBatch.bDitheredLODTransition)
    && Scene && MaskedInEarlyPass(Scene->GetShaderPlatform());
```

`MaskedInEarlyPass` (`RenderUtils.cpp:509-520`)：

```cpp
RENDERCORE_API bool MaskedInEarlyPass(const FStaticShaderPlatform Platform)
{
    if (IsMobilePlatform(Platform))
        return MobileEarlyZPass == 2 || MobileUsesFullDepthPrepass(Platform);
    ...
}
```

### 2. `SetOpaqueRenderState` 内部还会再次设置 depth state

`Engine/Source/Runtime/Renderer/Private/MobileBasePass.cpp:531-575`：

```cpp
void MobileBasePass::SetOpaqueRenderState(FMeshPassProcessorRenderState& DrawRenderState, ...)
{
    uint8 StencilValue = 0;
    if (bEnableReceiveDecalOutput)
        StencilValue |= GET_STENCIL_BIT_MASK(RECEIVE_DECAL, ReceiveDecals);

    if (bUsesDeferredShading)
    {
        StencilValue |= GET_STENCIL_MOBILE_SM_MASK(ShadingModel);
        StencilValue |= STENCIL_LIGHTING_CHANNELS_MASK(...);
    }

    if (bEnableReceiveDecalOutput || bUsesDeferredShading)
    {
        DrawRenderState.SetDepthStencilState(TStaticDepthStencilState<
                true, CF_DepthNearOrEqual,                                   // ★ 仍然开深度写
                true, CF_Always, SO_Keep, SO_Keep, SO_Replace,                // front face: 替换
                false, CF_Always, SO_Keep, SO_Keep, SO_Keep,
                0x00, 0xff >::GetRHI());
        DrawRenderState.SetStencilRef(StencilValue);
    }
    else
    {
        // default depth state should be already set
    }
    ...
}
```

也就是说：**只要不透明 + HDR/decal/deferred 任意一种条件**，`SetOpaqueRenderState` 都会用 `<true, CF_DepthNearOrEqual>` 显式再写一次 depth state。如果都不满足，则保留 `PassDrawRenderState` 的默认 `<true, CF_DepthNearOrEqual>`——仍然写深度。

### 3. Mesh 同时进入两个 Pass 的判定

`Engine/Source/Runtime/Renderer/Private/SceneVisibility.cpp:1318-1322, 1531-1542`：

```cpp
const bool bMobileMaskedInEarlyPass =
    (ShadingPath == EShadingPath::Mobile) && Scene.EarlyZPassMode == DDM_MaskedOnly;
...
if (StaticMeshRelevance.bUseForDepthPass && (bDrawDepthOnly || (bMobileMaskedInEarlyPass && ViewRelevance.bMasked)))
{
    if (!(bIsMeshInVelocityPass && bVelocityPassWritesDepth))
    {
        if (ViewRelevance.bRenderInSecondStageDepthPass && ShadingPath != EShadingPath::Mobile)
            DrawCommandPacket.AddCommandsForMesh(..., EMeshPass::SecondStageDepthPass);
        else
            DrawCommandPacket.AddCommandsForMesh(..., EMeshPass::DepthPass);
    }
}
```

含义：
- `Mobile` 平台 `DDM_AllOpaque`：所有 `bUseForDepthPass` 的 mesh 进 `DepthPass`
- `Mobile` 平台 `DDM_MaskedOnly`：只有 `bMasked` 的 mesh 进 `DepthPass`
- 其他情况：进 `DDM_None`，没有 prepass

`bUseForDepthPass` 字段定义在 `Engine/Source/Runtime/Engine/Public/MeshBatch.h:369`：

```cpp
uint32 bUseForDepthPass : 1;   // Whether it can be used in depth pass.
```

---

## 四、为什么 `CreateMobileBasePassProcessor` 默认设 `<true, CF_DepthNearOrEqual>`？

`Engine/Source/Runtime/Renderer/Private/MobileBasePass.cpp:1151-1163`：

```cpp
FMeshPassProcessor* CreateMobileBasePassProcessor(ERHIFeatureLevel::Type FeatureLevel,
                                                  const FScene* Scene,
                                                  const FSceneView* InViewIfDynamicMeshCommand,
                                                  FMeshPassDrawListContext* InDrawListContext)
{
    FMeshPassProcessorRenderState PassDrawRenderState;
    PassDrawRenderState.SetBlendState(TStaticBlendStateWriteMask<CW_RGBA>::GetRHI());
    const FExclusiveDepthStencil::Type DefaultBasePassDepthStencilAccess =
        FScene::GetDefaultBasePassDepthStencilAccess(FeatureLevel);
    PassDrawRenderState.SetDepthStencilAccess(DefaultBasePassDepthStencilAccess);
    PassDrawRenderState.SetDepthStencilState(TStaticDepthStencilState<true, CF_DepthNearOrEqual>::GetRHI());

    const FMobileBasePassMeshProcessor::EFlags Flags = FMobileBasePassMeshProcessor::EFlags::CanUseDepthStencil
        | (MobileBasePassAlwaysUsesCSM(...) ? FMobileBasePassMeshProcessor::EFlags::CanReceiveCSM
                                            : FMobileBasePassMeshProcessor::EFlags::None);

    return new FMobileBasePassMeshProcessor(EMeshPass::BasePass, Scene,
                                            InViewIfDynamicMeshCommand, PassDrawRenderState,
                                            InDrawListContext, Flags);
}
```

### 关键原因 1：Mobile 默认没有 prepass，base pass 必须自己写深度

- `GetEarlyZPassMode` 在 mobile 上**默认返回 `DDM_None`**
- 只有当 `MobileEarlyZPass=1` 或 AO / ShadowMask / DBuffer 强制打开时才升级为 `DDM_AllOpaque`
- `DDM_None` 时 `ShouldRenderPrePass()` 返回 false，根本不调用 `RenderPrePass`
- 这时 base pass 就是**第一个写入深度缓冲**的 pass，若不写深度，后续半透 / occlusion / SSR 全部失效

### 关键原因 2：开了 prepass 时，特定 mesh 的 depth state 会被覆盖

- `MobileBasePass.cpp:952-954` 显式覆盖为 `<false, CF_Equal>`——仅等值测试，不写深度
- 覆盖条件：`MeshBatch.bUseForDepthPass && EarlyZPassMode == DDM_AllOpaque` 或 `bMaskedInEarlyPass`
- 因此默认 `<true, ...>` 只在"该 mesh 没进 depth pass"时被使用，此时深度必须开

### 关键原因 3：和 `SetDepthStencilAccess` 语义一致

`RendererScene.cpp:4648-4663`：

```cpp
FExclusiveDepthStencil::Type FScene::GetDefaultBasePassDepthStencilAccess(ERHIFeatureLevel::Type InFeatureLevel)
{
    FExclusiveDepthStencil::Type BasePassDepthStencilAccess = FExclusiveDepthStencil::DepthWrite_StencilWrite;
    if (GetFeatureLevelShadingPath(InFeatureLevel) == EShadingPath::Deferred)
    {
        const EShaderPlatform ShaderPlatform = GetFeatureLevelShaderPlatform(InFeatureLevel);
        if (ShouldForceFullDepthPass(ShaderPlatform)
            && CVarBasePassWriteDepthEvenWithFullPrepass.GetValueOnAnyThread() == 0)
        {
            BasePassDepthStencilAccess = FExclusiveDepthStencil::DepthRead_StencilWrite;
        }
    }
    return BasePassDepthStencilAccess;
}
```

- Mobile 路径走 mobile 分支（不会进入 deferred 分支），**直接返回 `DepthWrite_StencilWrite`**
- 与 `bEnableDepthWrite=true` 完全匹配——base pass 在 mobile 上绝大多数情况下**要写深度**

### 关键原因 4：`SetOpaqueRenderState` 也保持 `<true, ...>`

`MobileBasePass.cpp:549-554` 在 decal/stencil/deferred 输出需求下再次显式写 `<true, CF_DepthNearOrEqual>`。可见整个 base pass 设计的一致原则就是"无 prepass 时由 base pass 自己负责写深度"。

---

## 五、`MobileBasePass.cpp:1157` 这一行的最终结论

### 它是什么

为 `EMeshPass::BasePass`（`FMobileBasePassMeshProcessor`）准备默认 PSO 输入状态里的 depth-stencil 状态。

### 它和 DepthPass 的关系

**`EMeshPass::DepthPass` 不走这里**。depth pass 使用：
- 独立的 `FDepthPassMeshProcessor`（`DepthRendering.cpp:1230` 注册）
- 独立的 `SetupDepthPassState` / `SetMobileDepthPassRenderState`（`DepthRendering.cpp:486, 753`）
- 自己的 blend state `TStaticBlendState<CW_NONE>`（关 color write）

两边通过 `bUseForDepthPass && EarlyZPassMode == DDM_AllOpaque` 这条**短路逻辑**衔接：
- 该 mesh 先在 depth pass 写好深度
- 进入 base pass 时被 `MobileBasePass.cpp:954` 覆盖为 `<false, CF_Equal>`——只做 z-test 等值比较、不写深度

### 它的作用（一句话总结）

为 mobile base pass 提供"**base pass 自己负责写深度**"的兜底默认深度状态；这是 mobile forward 路径**绝大多数场景**（`EarlyZPassMode == DDM_None`）下唯一实际生效的设置。当 `MobileBasePass.cpp:952-954` 的短路条件命中时（prepass 已写过深度）该默认状态会被覆盖为 `<false, CF_Equal>`，因此即使 prepass 开启也不会冲突。

---

## 六、参考证据汇总

| 关注点 | 文件 : 行 |
|--------|----------|
| `TStaticDepthStencilState` 定义 | `Engine/Source/Runtime/RenderCore/Public/RHIStaticStates.h:213-255` |
| `CF_DepthNearOrEqual` 定义 | `Engine/Source/Runtime/RHI/Public/RHIDefinitions.h:287-307` |
| base pass 默认 depth state | `Engine/Source/Runtime/Renderer/Private/MobileBasePass.cpp:1157` |
| base pass 内部覆盖 depth state | `Engine/Source/Runtime/Renderer/Private/MobileBasePass.cpp:952-954` |
| 半透 depth state | `Engine/Source/Runtime/Renderer/Private/MobileBasePass.cpp:1187, 1198, 1210` |
| `SetOpaqueRenderState` 显式设 depth state | `Engine/Source/Runtime/Renderer/Private/MobileBasePass.cpp:531-575` |
| `SetTranslucentRenderState` | `Engine/Source/Runtime/Renderer/Private/MobileBasePass.cpp:615+` |
| `bUseForDepthPass` 字段 | `Engine/Source/Runtime/Engine/Public/MeshBatch.h:369` |
| Mobile `GetEarlyZPassMode` | `Engine/Source/Runtime/Renderer/Private/RendererScene.cpp:4694-4708` |
| `MobileUsesFullDepthPrepass` | `Engine/Source/Runtime/RenderCore/Private/RenderUtils.cpp:616-619` |
| `MaskedInEarlyPass` | `Engine/Source/Runtime/RenderCore/Private/RenderUtils.cpp:509-520` |
| `GetDefaultBasePassDepthStencilAccess` | `Engine/Source/Runtime/Renderer/Private/RendererScene.cpp:4648-4663` |
| `RenderPrePass` (mobile) | `Engine/Source/Runtime/Renderer/Private/DepthRendering.cpp:660-679` |
| `SetupDepthPassState` | `Engine/Source/Runtime/Renderer/Private/DepthRendering.cpp:486-491` |
| `SetMobileDepthPassRenderState` | `Engine/Source/Runtime/Renderer/Private/DepthRendering.cpp:753-784` |
| `CreateDepthPassProcessor` 注册 | `Engine/Source/Runtime/Renderer/Private/DepthRendering.cpp:1230-1243` |
| `ShouldRenderPrePass` (mobile) | `Engine/Source/Runtime/Renderer/Private/DepthRendering.cpp:660-664` |
| `bIsFullDepthPrepassEnabled` | `Engine/Source/Runtime/Renderer/Private/MobileShadingRenderer.cpp:302` |
| MobileShadingRenderer 调用 DepthPass | `Engine/Source/Runtime/Renderer/Private/MobileShadingRenderer.cpp:824-836` |
| `bMobileMaskedInEarlyPass` 判断 | `Engine/Source/Runtime/Renderer/Private/SceneVisibility.cpp:1320, 1531-1542` |
| `FMobileBasePassMeshProcessor` flags | `Engine/Source/Runtime/Renderer/Private/MobileBasePassRendering.h:460-534` |