# Docs/Plan.md 源码方案问题分析

分析依据：当前工程源码路径为 `Engine` 根目录下的 `Source/...`。以下只记录存在问题或仍需继续修改的部分。

## 1. 新 DepthPass 会继续绑定 Mobile BasePass VS/PS，且现有状态可能被覆盖

问题：

- 方案中的 `MobileAfterTranslucencyDepthPass` 复用 `FMobileBasePassMeshProcessor`，即使设置 `CW_NONE`，仍会走 Mobile BasePass shader 绑定，不是纯 depth-only pass。
- 更严重的是，方案没有给新增 processor 设置 `FMobileBasePassMeshProcessor::EFlags::ForcePassDrawRenderState`，`Process()` 会根据材质重新调用 `SetOpaqueRenderState()`，可能覆盖方案里设置的 depth/blend state。尤其 `MobileAfterTranslucencyPass` 需要 `DepthRead_StencilRead` + `DepthWrite=false`，但在 Mobile HDR 或 deferred 状态下可能被改成 depth/stencil write。

源码佐证：

```cpp
// Source/Runtime/Renderer/Private/MobileBasePass.cpp:906-937
TMeshProcessorShaders<
	TMobileBasePassVSPolicyParamType<FUniformLightMapPolicy>,
	TMobileBasePassPSPolicyParamType<FUniformLightMapPolicy>> BasePassShaders;

if (!MobileBasePass::GetShaders(
	LightMapPolicyType,
	LocalLightSetting,
	MaterialResource,
	MeshBatch.VertexFactory->GetType(),
	bEnableSkyLight,
	BasePassShaders.VertexShader,
	BasePassShaders.PixelShader))
```

```cpp
// Source/Runtime/Renderer/Private/MobileBasePass.cpp:943-960
const bool bForcePassDrawRenderState = ((Flags & EFlags::ForcePassDrawRenderState) == EFlags::ForcePassDrawRenderState);

FMeshPassProcessorRenderState DrawRenderState(PassDrawRenderState);
if (!bForcePassDrawRenderState)
{
	...
	MobileBasePass::SetOpaqueRenderState(DrawRenderState, PrimitiveSceneProxy, MaterialResource, ShadingModels, bEnableReceiveDecalOutput && IsMobileHDR(), bPassUsesDeferredShading);
}
```

```cpp
// Source/Runtime/Renderer/Private/MobileBasePassRendering.h:463-474
enum class EFlags
{
	None = 0,
	CanUseDepthStencil = (1 << 0),
	CanReceiveCSM = (1 << 1),
	ForcePassDrawRenderState = (1 << 2),
	DoNotCache = (1 << 3)
};
```

```cpp
// Source/Runtime/Renderer/Private/MobileBasePass.cpp:531-560
void MobileBasePass::SetOpaqueRenderState(...)
{
	...
	if (bEnableReceiveDecalOutput || bUsesDeferredShading)
	{
		DrawRenderState.SetDepthStencilState(TStaticDepthStencilState<
				true, CF_DepthNearOrEqual,
				true, CF_Always, SO_Keep, SO_Keep, SO_Replace,
				...
```

需要继续修改：

- 如果继续复用 `FMobileBasePassMeshProcessor`，新增两个 processor 的 `Flags` 至少应包含 `ForcePassDrawRenderState`，确保 `CW_NONE`、`DepthWrite_StencilWrite`、`DepthRead_StencilRead` 等状态不被 `Process()` 覆盖。
- 如果目标是减少 depth pass 的 PS 成本，不应复用 Mobile BasePass。当前已有 `FDepthPassMeshProcessor` 和 `SetupDepthPassState()`，它会使用 depth-only shader 路径，且有机会走 position-only/null pixel shader。

```cpp
// Source/Runtime/Renderer/Private/DepthRendering.cpp:486-490
void SetupDepthPassState(FMeshPassProcessorRenderState& DrawRenderState)
{
	DrawRenderState.SetBlendState(TStaticBlendState<CW_NONE>::GetRHI());
	DrawRenderState.SetDepthStencilState(TStaticDepthStencilState<true, CF_DepthNearOrEqual>::GetRHI());
}
```

```cpp
// Source/Runtime/Renderer/Private/DepthRendering.cpp:985-991
const bool bSupportPositionOnlyStream = MeshBatch.VertexFactory->SupportsPositionOnlyStream();
const bool bVFTypeSupportsNullPixelShader = MeshBatch.VertexFactory->SupportsNullPixelShader();
...
if (ShouldRender(Material, bEvaluateWPO, bSupportPositionOnlyStream, bVFTypeSupportsNullPixelShader, bUseDefaultMaterial, bPositionOnly))
```

结论：直接复用移动端 deferred/base pass 不适合实现真正的 depth-only 优化；功能上可复用 Mobile BasePass，但必须强制固定 pass render state。

## 2. 方案只改了 `RenderForwardSinglePass`，遗漏 `RenderForwardMultiPass`

问题：

- 当前 forward 渲染会根据 `bRequiresMultiPass` 分支到 single-pass 或 multi-pass。方案只在 `RenderForwardSinglePass()` 中插入新增 pass，multi-pass 路径不会执行 `MobileAfterTranslucencyDepthPass` 和 `MobileAfterTranslucencyPass`。
- Android VR 如果运行在 Vulkan 或特定 framebuffer fetch 条件下通常可能是 single-pass，但源码仍保留 multi-pass 路径；方案说“移动端 Forward 渲染路径”，因此这里属于功能缺口。

源码佐证：

```cpp
// Source/Runtime/Renderer/Private/MobileShadingRenderer.cpp:1566-1574
// Split if we need to render translucency in a separate render pass
if (bRequiresMultiPass)
{
	RenderForwardMultiPass(GraphBuilder, PassParameters, ViewContext, SceneTextures);
}
else
{
	RenderForwardSinglePass(GraphBuilder, PassParameters, ViewContext, SceneTextures);
}
```

```cpp
// Source/Runtime/Renderer/Private/MobileShadingRenderer.cpp:1677-1685
// Depth pre-pass
RHICmdList.SetCurrentStat(GET_STATID(STAT_CLM_MobilePrePass));
RenderMaskedPrePass(RHICmdList, View);
// Opaque and masked
RHICmdList.SetCurrentStat(GET_STATID(STAT_CLMM_Opaque));
RenderMobileBasePass(RHICmdList, View, &PassParameters->InstanceCullingDrawParams);
RenderMobileDebugView(RHICmdList, View);
RHICmdList.PollOcclusionQueries();
PostRenderBasePass(RHICmdList, View);
```

```cpp
// Source/Runtime/Renderer/Private/MobileShadingRenderer.cpp:1729-1745
// scene depth is read only and can be fetched
RHICmdList.SetCurrentStat(GET_STATID(STAT_CLMM_Translucency));
RenderDecals(RHICmdList, View, &SecondPassParameters->InstanceCullingDrawParams);
RenderModulatedShadowProjections(RHICmdList, ViewContext.ViewIndex, View);
RenderFog(RHICmdList, View);
// Draw translucency.
RenderTranslucency(RHICmdList, View);

...
PreTonemapMSAA(RHICmdList, SceneTextures);
```

需要继续修改：

- 在 `RenderForwardMultiPass()` 的第一个 RDG pass 中，`RenderMobileBasePass()` 之后、`PostRenderBasePass()` 之前或之前合适位置调用 `RenderMobileAfterTranslucencyDepthPass()`。
- 在 `RenderForwardMultiPass()` 的第二个 RDG pass 中，`RenderTranslucency()` 之后、`RenderOcclusion()` / `PreTonemapMSAA()` 之前调用 `RenderMobileAfterTranslucencyPass()`。

## 3. 标记物体仍可能进入原始 DepthPass，导致深度写入时机早于“不透明后”

问题：

- 方案只在 BasePass 分流标记物体，没有排除原始 `EMeshPass::DepthPass`。
- 如果 full depth prepass、masked prepass 或动态 mesh relevance 触发，标记物体会在 `RenderMaskedPrePass()` / full prepass 阶段提前写入深度，而不是方案要求的“不透明 BasePass 之后再写深度”。
- 当 `bIsFullDepthPrepassEnabled` 为 true 时，forward base render target 的 depth access 是 `DepthRead_StencilWrite`，方案插入的 after depth pass 还可能无法在该 render pass 内写 depth。

源码佐证：

```cpp
// Source/Runtime/Renderer/Private/SceneVisibility.cpp:1530-1542
// Add depth commands.
if (StaticMeshRelevance.bUseForDepthPass && (bDrawDepthOnly || (bMobileMaskedInEarlyPass && ViewRelevance.bMasked)))
{
	...
	DrawCommandPacket.AddCommandsForMesh(..., EMeshPass::DepthPass);
}
```

```cpp
// Source/Runtime/Renderer/Private/SceneVisibility.cpp:2198-2209
if (ViewRelevance.bDrawRelevance && (ViewRelevance.bRenderInMainPass || ViewRelevance.bRenderCustomDepth || ViewRelevance.bRenderInDepthPass))
{
	...
	PassMask.Set(EMeshPass::DepthPass);
	View.NumVisibleDynamicMeshElements[EMeshPass::DepthPass] += NumElements;
}
```

```cpp
// Source/Runtime/Renderer/Private/MobileShadingRenderer.cpp:1604-1609
// Depth pre-pass
RHICmdList.SetCurrentStat(GET_STATID(STAT_CLM_MobilePrePass));
RenderMaskedPrePass(RHICmdList, View);
// Opaque and masked
RHICmdList.SetCurrentStat(GET_STATID(STAT_CLMM_Opaque));
RenderMobileBasePass(RHICmdList, View, &PassParameters->InstanceCullingDrawParams);
```

```cpp
// Source/Runtime/Renderer/Private/MobileShadingRenderer.cpp:1494-1496
BasePassRenderTargets.DepthStencil = bIsFullDepthPrepassEnabled ? 
	FDepthStencilBinding(SceneDepth, ERenderTargetLoadAction::ELoad, ERenderTargetLoadAction::ELoad, FExclusiveDepthStencil::DepthRead_StencilWrite) : 
	FDepthStencilBinding(SceneDepth, ERenderTargetLoadAction::EClear, ERenderTargetLoadAction::EClear, FExclusiveDepthStencil::DepthWrite_StencilWrite);
```

需要继续修改：

- 在 `SceneVisibility.cpp` 的静态 mesh depth command 分支中，移动 forward 且 `ViewRelevance.bRenderAfterTranslucency` 为 true 时，应跳过原始 `EMeshPass::DepthPass`，避免提前写深度。
- 在 `ComputeDynamicMeshRelevance()` 中同样要对标记物体跳过原始 `DepthPass`，否则 Skeletal Mesh / dynamic mesh 仍会提前写深度。
- 若项目可能启用 full depth prepass，还需要单独处理 render target depth access；否则 after depth pass 在 base render pass 内没有 depth write 权限。

## 4. `AddMeshBatch()` 中新增分流存在空指针风险

问题：

- 当前源码对 `PrimitiveSceneProxy` 调用 `ShouldRenderInMainPass()` 前做了空指针保护。
- 方案新增 `const bool bShouldRenderAfterTranslucency = PrimitiveSceneProxy->ShouldRenderAfterTranslucency();`，没有判空。如果某些动态 mesh 或编辑器路径传入 `nullptr`，会崩溃。

源码佐证：

```cpp
// Source/Runtime/Renderer/Private/MobileBasePass.cpp:867-872
void FMobileBasePassMeshProcessor::AddMeshBatch(..., const FPrimitiveSceneProxy* RESTRICT PrimitiveSceneProxy, ...)
{
	if (!MeshBatch.bUseForMaterial || 
		(Flags & FMobileBasePassMeshProcessor::EFlags::DoNotCache) == FMobileBasePassMeshProcessor::EFlags::DoNotCache ||
		(PrimitiveSceneProxy && !PrimitiveSceneProxy->ShouldRenderInMainPass()))
```

需要继续修改：

```cpp
const bool bShouldRenderAfterTranslucency =
	PrimitiveSceneProxy && PrimitiveSceneProxy->ShouldRenderAfterTranslucency();
```

## 5. 新增 GPU drawcall stat 只声明未定义，会导致编译/链接问题

问题：

- 方案在 `BasePassRendering.h` 里添加 `DECLARE_GPU_DRAWCALL_STAT_EXTERN(AfterTranslucency)` 和 `AfterTranslucencyDepth`，并在新函数里使用 `SCOPED_GPU_STAT`。
- 但当前 `Basepass` 除了声明外，还在 `BasePassRendering.cpp` 中有 `DEFINE_GPU_DRAWCALL_STAT(Basepass)`。方案没有补新增 stat 的 define。

源码佐证：

```cpp
// Source/Runtime/Renderer/Private/BasePassRendering.h:144
DECLARE_GPU_DRAWCALL_STAT_EXTERN(Basepass);
```

```cpp
// Source/Runtime/Renderer/Private/BasePassRendering.cpp:184
DEFINE_GPU_DRAWCALL_STAT(Basepass);
```

```cpp
// Source/Runtime/Renderer/Private/MobileBasePassRendering.cpp:472-475
CSV_SCOPED_TIMING_STAT_EXCLUSIVE(RenderBasePass);
SCOPED_DRAW_EVENT(RHICmdList, MobileBasePass);
SCOPE_CYCLE_COUNTER(STAT_BasePassDrawTime);
SCOPED_GPU_STAT(RHICmdList, Basepass);
```

需要继续修改：

- 在 `Source/Runtime/Renderer/Private/BasePassRendering.cpp` 中补：

```cpp
DEFINE_GPU_DRAWCALL_STAT(AfterTranslucency);
DEFINE_GPU_DRAWCALL_STAT(AfterTranslucencyDepth);
```

## 6. 方案跳过新增 Pass 的 PSO 预缓存，Android 上有运行时卡顿/PSO 缺失风险

问题：

- 方案在 `FMobileBasePassMeshProcessor::CollectPSOInitializers()` 开头对两个新增 pass 直接 `return`。
- 但方案又用 `REGISTER_MESHPASSPROCESSOR_AND_PSOCOLLECTOR` 注册了两个 pass，等于注册了 PSO collector 却不收集任何 PSO。移动端尤其 Android 上这会增加运行时 PSO 创建风险。

源码佐证：

```cpp
// Source/Runtime/Renderer/Private/MobileBasePass.cpp:1056-1066
void FMobileBasePassMeshProcessor::CollectPSOInitializers(...)
{
	static IConsoleVariable* PSOPrecacheTranslucencyAllPass = IConsoleManager::Get().FindConsoleVariable(TEXT("r.PSOPrecache.TranslucencyAllPass"));
	...
	if (!PreCacheParams.bRenderInMainPass || !ShouldDraw(Material))
	{
		return;
	}
```

```cpp
// Source/Runtime/Renderer/Private/MobileBasePass.cpp:1019-1053
TMeshProcessorShaders<
	TMobileBasePassVSPolicyParamType<FUniformLightMapPolicy>,
	TMobileBasePassPSPolicyParamType<FUniformLightMapPolicy>> BasePassShaders;
...
AddGraphicsPipelineStateInitializer(
	VertexFactoryData,
	MaterialResource,
	DrawRenderState,
	RenderTargetsInfo,
	BasePassShaders,
	...
	PSOInitializers);
```

```cpp
// Source/Runtime/Renderer/Private/MobileBasePass.cpp:1218-1222
REGISTER_MESHPASSPROCESSOR_AND_PSOCOLLECTOR(MobileBasePass, ...);
REGISTER_MESHPASSPROCESSOR_AND_PSOCOLLECTOR(MobileBasePassCSM, ...);
REGISTER_MESHPASSPROCESSOR_AND_PSOCOLLECTOR(MobileTranslucencyAllPass, ...);
REGISTER_MESHPASSPROCESSOR_AND_PSOCOLLECTOR(MobileTranslucencyStandardPass, ...);
REGISTER_MESHPASSPROCESSOR_AND_PSOCOLLECTOR(MobileTranslucencyAfterDOFPass, ...);
```

需要继续修改：

- 不要直接跳过两个新增 pass 的 PSO 预缓存。
- 如果继续复用 Mobile BasePass processor，应为 `MobileAfterTranslucencyDepthPass` 和 `MobileAfterTranslucencyPass` 分别收集与真实 render state 匹配的 PSO。
- 如果改用 `FDepthPassMeshProcessor` 实现 depth pass，则 depth pass 的 PSO 预缓存应走 `DepthRendering.cpp` 现有逻辑。

## 7. “复用移动端延迟渲染路径”不建议作为本功能实现路径

问题：

- 当前 `FMobileBasePassMeshProcessor` 构造时根据全局 mobile deferred 开关决定 `bPassUsesDeferredShading`。一旦走 deferred 相关路径，`SetOpaqueRenderState()` 会写 mobile shading model / lighting channel stencil，并假定 GBuffer/deferred subpass 语义。
- 用户需求明确是“移动端 Forward 渲染路径”，因此不应复用 mobile deferred pass 作为 after-translucency depth/color 的基础。

源码佐证：

```cpp
// Source/Runtime/Renderer/Private/MobileBasePass.cpp:810-824
FMobileBasePassMeshProcessor::FMobileBasePassMeshProcessor(...)
	: FMeshPassProcessor(...)
	, ...
	, bTranslucentBasePass(InTranslucencyPassType != ETranslucencyPass::TPT_MAX)
	, bDeferredShading(IsMobileDeferredShadingEnabled(GetFeatureLevelShaderPlatform(ERHIFeatureLevel::ES3_1)))
	, bPassUsesDeferredShading(bDeferredShading && !bTranslucentBasePass)
{
}
```

```cpp
// Source/Runtime/Renderer/Private/MobileBasePass.cpp:540-554
if (bUsesDeferredShading)
{
	uint8 ShadingModel = GetMobileShadingModelStencilValue(ShadingModels);
	StencilValue |= GET_STENCIL_MOBILE_SM_MASK(ShadingModel);
	StencilValue |= STENCIL_LIGHTING_CHANNELS_MASK(...);
}
		
if (bEnableReceiveDecalOutput || bUsesDeferredShading)
{
	DrawRenderState.SetDepthStencilState(TStaticDepthStencilState<
			true, CF_DepthNearOrEqual,
			true, CF_Always, SO_Keep, SO_Keep, SO_Replace,
```

需要继续修改：

- Forward-only 功能应显式固定新增 pass 的 render state，并避免进入 deferred shading/stencil 语义。
- Depth-only 优化应基于 `FDepthPassMeshProcessor` 或独立 processor，而不是复用 mobile deferred base pass。

## 8. `FMobileSceneRenderer::RenderPrePass` 逻辑梳理及 BasePass 后插入可行性
>Engine/Source/Runtime/Renderer/Private/DepthRendering.cpp:666中RenderPrePass，梳理其逻辑，其如
何过滤Mesh，其MeshProcess的逻辑，我能否对其修改，只渲染我标记的物体，将RenderPrePass放入
RenderForwardSinglePass和RenderForwardMultiPass的
RenderMobileBasePass（MobileShadingRenderer.cpp:1609行）之后
问题背景：

- `Source/Runtime/Renderer/Private/DepthRendering.cpp:666` 的 `FMobileSceneRenderer::RenderPrePass()` 看起来像是移动端 depth prepass 的入口，但它本身并不负责筛选 Mesh。
- 它只设置 viewport，然后派发已经构建好的 `EMeshPass::DepthPass` draw commands。
- 因此，如果目标是“BasePass 后只渲染标记物体的深度”，不能只修改或搬移 `RenderPrePass()`。

源码逻辑：

```cpp
// Source/Runtime/Renderer/Private/DepthRendering.cpp:660-664
bool FMobileSceneRenderer::ShouldRenderPrePass() const
{
	// Draw a depth pass to avoid overdraw in the other passes.
	return Scene->EarlyZPassMode == DDM_MaskedOnly || Scene->EarlyZPassMode == DDM_AllOpaque;
}
```

```cpp
// Source/Runtime/Renderer/Private/DepthRendering.cpp:666-679
void FMobileSceneRenderer::RenderPrePass(
	FRHICommandList& RHICmdList,
	const FViewInfo& View,
	const FInstanceCullingDrawParams* InstanceCullingDrawParams)
{
	checkSlow(RHICmdList.IsInsideRenderPass());

	SCOPED_NAMED_EVENT(FMobileSceneRenderer_RenderPrePass, FColor::Emerald);
	SCOPED_DRAW_EVENT(RHICmdList, MobileRenderPrePass);

	SCOPE_CYCLE_COUNTER(STAT_DepthDrawTime);
	CSV_SCOPED_TIMING_STAT_EXCLUSIVE(RenderPrePass);
	SCOPED_GPU_STAT(RHICmdList, Prepass);

	SetStereoViewport(RHICmdList, View);
	View.ParallelMeshDrawCommandPasses[EMeshPass::DepthPass].DispatchDraw(nullptr, RHICmdList, InstanceCullingDrawParams);
}
```

关键结论：

- `RenderPrePass()` 不遍历 primitive。
- `RenderPrePass()` 不判断材质、不判断是否 masked、不判断是否标记物体。
- 它派发的是 `View.ParallelMeshDrawCommandPasses[EMeshPass::DepthPass]`，而这个 pass 的内容已经在可见性阶段和 MeshPassProcessor 阶段准备好了。

现有调用点：

```cpp
// Source/Runtime/Renderer/Private/MobileShadingRenderer.cpp:849-854
void FMobileSceneRenderer::RenderMaskedPrePass(FRHICommandList& RHICmdList, const FViewInfo& View)
{
	if (bIsMaskedOnlyDepthPrepassEnabled)
	{
		RenderPrePass(RHICmdList, View, &DepthPassInstanceCullingDrawParams);
	}
}
```

```cpp
// Source/Runtime/Renderer/Private/MobileShadingRenderer.cpp:1604-1609
// Depth pre-pass
RHICmdList.SetCurrentStat(GET_STATID(STAT_CLM_MobilePrePass));
RenderMaskedPrePass(RHICmdList, View);
// Opaque and masked
RHICmdList.SetCurrentStat(GET_STATID(STAT_CLMM_Opaque));
RenderMobileBasePass(RHICmdList, View, &PassParameters->InstanceCullingDrawParams);
```

```cpp
// Source/Runtime/Renderer/Private/MobileShadingRenderer.cpp:1677-1682
// Depth pre-pass
RHICmdList.SetCurrentStat(GET_STATID(STAT_CLM_MobilePrePass));
RenderMaskedPrePass(RHICmdList, View);
// Opaque and masked
RHICmdList.SetCurrentStat(GET_STATID(STAT_CLMM_Opaque));
RenderMobileBasePass(RHICmdList, View, &PassParameters->InstanceCullingDrawParams);
```

Mesh 进入 `EMeshPass::DepthPass` 的主要位置：

```cpp
// Source/Runtime/Renderer/Private/SceneVisibility.cpp:1530-1542
// Add depth commands.
if (StaticMeshRelevance.bUseForDepthPass && (bDrawDepthOnly || (bMobileMaskedInEarlyPass && ViewRelevance.bMasked)))
{
	if (!(bIsMeshInVelocityPass && bVelocityPassWritesDepth))
	{
		if (ViewRelevance.bRenderInSecondStageDepthPass && ShadingPath != EShadingPath::Mobile)
		{
			DrawCommandPacket.AddCommandsForMesh(..., EMeshPass::SecondStageDepthPass);
		}
		else
		{
			DrawCommandPacket.AddCommandsForMesh(..., EMeshPass::DepthPass);
		}
	}
}
```

```cpp
// Source/Runtime/Renderer/Private/SceneVisibility.cpp:2198-2209
if (ViewRelevance.bDrawRelevance && (ViewRelevance.bRenderInMainPass || ViewRelevance.bRenderCustomDepth || ViewRelevance.bRenderInDepthPass))
{
	if (ViewRelevance.bRenderInSecondStageDepthPass && ShadingPath != EShadingPath::Mobile)
	{
		PassMask.Set(EMeshPass::SecondStageDepthPass);
		View.NumVisibleDynamicMeshElements[EMeshPass::SecondStageDepthPass] += NumElements;
	}
	else
	{
		PassMask.Set(EMeshPass::DepthPass);
		View.NumVisibleDynamicMeshElements[EMeshPass::DepthPass] += NumElements;
	}
}
```

`FDepthPassMeshProcessor` 的过滤逻辑：

```cpp
// Source/Runtime/Renderer/Private/DepthRendering.cpp:1021-1085
void FDepthPassMeshProcessor::AddMeshBatch(...)
{
	bool bDraw = MeshBatch.bUseForDepthPass;
	
	// Filter by occluder flags and settings if required.
	if (bDraw && bRespectUseAsOccluderFlag && !MeshBatch.bUseAsOccluder && EarlyZPassMode < DDM_AllOpaque)
	{
		if (PrimitiveSceneProxy)
		{
			bDraw = PrimitiveSceneProxy->ShouldUseAsOccluder()
				&& (!PrimitiveSceneProxy->IsMovable() || bEarlyZPassMovable);

			// Dynamic mesh command also has screen-size filtering here.
		}
	}

	if (bDraw)
	{
		const FMaterialRenderProxy* MaterialRenderProxy = MeshBatch.MaterialRenderProxy;
		while (MaterialRenderProxy)
		{
			const FMaterial* Material = MaterialRenderProxy->GetMaterialNoFallback(FeatureLevel);
			if (Material && Material->GetRenderingThreadShaderMap())
			{
				if (TryAddMeshBatch(MeshBatch, BatchElementMask, PrimitiveSceneProxy, StaticMeshId, *MaterialRenderProxy, *Material))
				{
					break;
				}
			}

			MaterialRenderProxy = MaterialRenderProxy->GetFallback(FeatureLevel);
		}
	}
}
```

```cpp
// Source/Runtime/Renderer/Private/DepthRendering.cpp:974-983
bool FDepthPassMeshProcessor::TryAddMeshBatch(...)
{
	const bool bIsTranslucent = IsTranslucentBlendMode(Material);
	bool ShouldRenderInDepthPass = (!PrimitiveSceneProxy || PrimitiveSceneProxy->ShouldRenderInDepthPass());

	if (!bIsTranslucent
		&& ShouldRenderInDepthPass
		&& ShouldIncludeDomainInMeshPass(Material.GetMaterialDomain())
		&& ShouldIncludeMaterialInDefaultOpaquePass(Material))
```

```cpp
// Source/Runtime/Renderer/Private/DepthRendering.cpp:939-971
bool FDepthPassMeshProcessor::ShouldRender(...)
{
	if (IsOpaqueBlendMode(Material)
		&& EarlyZPassMode != DDM_MaskedOnly
		&& bSupportPositionOnlyStream
		&& !bMaterialModifiesMeshPosition
		&& Material.WritesEveryPixel(false, bVFTypeSupportsNullPixelShader))
	{
		bShouldRender = true;
		bUseDefaultMaterial = true;
		bPositionOnly = true;
	}
	else
	{
		const bool bMaterialMasked = !Material.WritesEveryPixel(false, bVFTypeSupportsNullPixelShader)
			|| Material.IsTranslucencyWritingCustomDepth();

		if ((!bMaterialMasked && EarlyZPassMode != DDM_MaskedOnly)
			|| (bMaterialMasked && EarlyZPassMode != DDM_NonMaskedOnly))
		{
			bShouldRender = true;
			...
		}
	}

	return bShouldRender;
}
```

因此，Mesh 过滤顺序实际是：

1. `SceneVisibility.cpp` 决定静态或动态 Mesh 是否进入 `EMeshPass::DepthPass`。
2. `FDepthPassMeshProcessor::AddMeshBatch()` 根据 `bUseForDepthPass`、occluder、movable、屏幕尺寸继续过滤。
3. `TryAddMeshBatch()` 根据材质是否 translucent、是否允许 depth pass、material domain、default opaque pass 继续过滤。
4. `ShouldRender()` 决定 masked/non-masked、position-only、default material 等 shader 路径。
5. `Process()` 生成实际 mesh draw command。

能否修改为只渲染标记物体：

- 可以，但不应直接改现有 `RenderPrePass()`。
- 如果直接在 `RenderPrePass()` 后移并继续 dispatch `EMeshPass::DepthPass`，它会绘制所有已经进入原始 depth pass 的 Mesh，而不是只绘制标记物体。
- 如果直接把 `EMeshPass::DepthPass` 的 processor 改成只接受标记物体，会破坏正常 full depth prepass、masked-only prepass、occlusion 以及其他依赖 SceneDepth 的路径。

建议方案：

1. 保留现有 `RenderPrePass()` 不动。
2. 新增一个 mobile 专用 mesh pass，例如 `EMeshPass::MobileAfterBaseDepthPass` 或 `EMeshPass::MobileAfterTranslucencyDepthPass`。
3. 为这个新 pass 注册独立 processor。
4. 这个 processor 可以复用 `FDepthPassMeshProcessor` 的 depth-only shader 思路，但要额外过滤标记物体。
5. 在 `SceneVisibility.cpp` 中把标记物体加入新 pass，而不是依赖原始 `EMeshPass::DepthPass`。
6. 在 `BuildInstanceCullingDrawParams()` 中为新 pass 构建 instance culling draw params。
7. 在 `RenderForwardSinglePass()` 和 `RenderForwardMultiPass()` 的 `RenderMobileBasePass()` 之后 dispatch 新 pass。

processor 过滤应至少包含：

```cpp
if (!PrimitiveSceneProxy || !PrimitiveSceneProxy->ShouldRenderAfterTranslucency())
{
	return;
}
```

注意这里必须判空，不能直接调用：

```cpp
PrimitiveSceneProxy->ShouldRenderAfterTranslucency()
```

可新增类似渲染函数：

```cpp
void FMobileSceneRenderer::RenderMobileAfterBaseDepthPass(
	FRHICommandList& RHICmdList,
	const FViewInfo& View,
	const FInstanceCullingDrawParams* InstanceCullingDrawParams)
{
	checkSlow(RHICmdList.IsInsideRenderPass());
	SetStereoViewport(RHICmdList, View);
	View.ParallelMeshDrawCommandPasses[EMeshPass::MobileAfterBaseDepthPass]
		.DispatchDraw(nullptr, RHICmdList, InstanceCullingDrawParams);
}
```

插入位置：

```cpp
// RenderForwardSinglePass(), Source/Runtime/Renderer/Private/MobileShadingRenderer.cpp:1609 后
RenderMobileBasePass(RHICmdList, View, &PassParameters->InstanceCullingDrawParams);
RenderMobileAfterBaseDepthPass(RHICmdList, View, &AfterBaseDepthPassInstanceCullingDrawParams);
```

```cpp
// RenderForwardMultiPass(), Source/Runtime/Renderer/Private/MobileShadingRenderer.cpp:1682 后
RenderMobileBasePass(RHICmdList, View, &PassParameters->InstanceCullingDrawParams);
RenderMobileAfterBaseDepthPass(RHICmdList, View, &AfterBaseDepthPassInstanceCullingDrawParams);
```

但需要注意 full depth prepass：

```cpp
// Source/Runtime/Renderer/Private/MobileShadingRenderer.cpp:1494-1496
BasePassRenderTargets.DepthStencil = bIsFullDepthPrepassEnabled ? 
	FDepthStencilBinding(SceneDepth, ERenderTargetLoadAction::ELoad, ERenderTargetLoadAction::ELoad, FExclusiveDepthStencil::DepthRead_StencilWrite) : 
	FDepthStencilBinding(SceneDepth, ERenderTargetLoadAction::EClear, ERenderTargetLoadAction::EClear, FExclusiveDepthStencil::DepthWrite_StencilWrite);
```

当 `bIsFullDepthPrepassEnabled == true` 时，BasePass 的 depth access 是 `DepthRead_StencilWrite`，不能在同一个 render pass 内直接写 depth。若项目可能启用 full depth prepass，需要单独处理：

- 禁止该功能在 full depth prepass 模式下启用；
- 或者为 after-base depth pass 单独开一个 depth writable 的 RDG pass；
- 或者调整该段 render target binding 的 depth access，但这会影响 base pass 对 SceneDepth 的读取语义，需要谨慎验证。

还需要注意移动端 stencil：

```cpp
// Source/Runtime/Renderer/Private/DepthRendering.cpp:824-828
// Use StencilMask for DecalOutput on mobile
if (FeatureLevel == ERHIFeatureLevel::ES3_1 && !bShadowProjection)
{
	SetMobileDepthPassRenderState(PrimitiveSceneProxy, DrawRenderState, MeshBatch, IsMobileDeferredShadingEnabled(GetFeatureLevelShaderPlatform(FeatureLevel)));
}
```

`FDepthPassMeshProcessor` 在 mobile ES3_1 下不是纯粹只写 depth，它还会调用 `SetMobileDepthPassRenderState()` 写 stencil。对于“BasePass 后补标记物深度”的需求，需要确认是否希望写这些 stencil bit：

- forward 路径下会写 `MOBILE_CAST_CONTACT_SHADOW` 等 stencil 信息；
- deferred 路径下会写 mobile shading model / lighting channel stencil；
- 如果只想补 depth，不想修改 stencil，应给新 processor 增加开关，跳过 `SetMobileDepthPassRenderState()`，或实现一个独立 depth-only processor，使用 `SetupDepthPassState()` 风格的纯 depth write state。

最终建议：

- 不要把现有 `RenderPrePass()` 直接放到 `RenderMobileBasePass()` 后。
- 不要复用原始 `EMeshPass::DepthPass` 来实现“只渲染标记物体”。
- 应新增一个只收集标记物体的新 mobile depth mesh pass。
- 该 pass 的 shader/processor 优先基于 `FDepthPassMeshProcessor` 的 depth-only 路径，而不是 `FMobileBasePassMeshProcessor` 或 mobile deferred/base pass。
- SinglePass 和 MultiPass 都要插入，否则 multi-pass 路径功能缺失。
- full depth prepass 和 mobile stencil 写入是这个方案最需要提前处理的两个风险点。
