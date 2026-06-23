我现在正在UE5.4中开发Android端VR游戏，我现在有这样一个需求，在渲染不透明物体的阶段，我标记的物体不进行渲染，当渲染完成透明物体后再渲染我标记的物体，我的想法是模仿移动端不透明物体渲染的逻辑，在透明物体渲染完成后再进行一次
我标记物体的渲染，沿用不透明物体渲染深度测试等逻辑即可，因为我在透明物体之后渲染，透明物体不写入深度，所以我可以把透明物体遮挡住（这是我的核心需求），我只需要让Mesh和Skeletal Mesh生效即可，我也不需要CustomDepth，
我只需要移动端，Forward渲染路径的修改即可，行号不用做出太多纠正，代码只要在正确的文件中，正确的作用域即可，以下是我的引擎修改方案，结合当前工程源码对此方案进行分析，是否有错误存在，是否有潜在问题，是否有完成此功能需要修改的部分但未进行修改

1. Engine/Source/Runtime/Renderer/Public/MeshPassProcessor.h:32在EMeshPass中添加MobileAfterTranslucencyPass

```c++
namespace EMeshPass
{
	enum Type : uint8
	{
		DepthPass,
		SecondStageDepthPass,
		BasePass,
		AnisotropyPass,
		SkyPass,
		SingleLayerWaterPass,
		SingleLayerWaterDepthPrepass,
		CSMShadowDepth,
		VSMShadowDepth,
		Distortion,
		Velocity,
		TranslucentVelocity,
		TranslucencyStandard,
		TranslucencyStandardModulate,
		TranslucencyAfterDOF,
		TranslucencyAfterDOFModulate,
		TranslucencyAfterMotionBlur,
		TranslucencyAll, /** Drawing all translucency, regardless of separate or standard.  Used when drawing translucency outside of the main renderer, eg FRendererModule::DrawTile. */
		LightmapDensity,
		DebugViewMode, /** Any of EDebugViewShaderMode */
		CustomDepth,
		MobileBasePassCSM,  /** Mobile base pass with CSM shading enabled */
		VirtualTexture,
		LumenCardCapture,
		LumenCardNanite,
		LumenTranslucencyRadianceCacheMark,
		LumenFrontLayerTranslucencyGBuffer,
		DitheredLODFadingOutMaskPass, /** A mini depth pass used to mark pixels with dithered LOD fading out. Currently only used by ray tracing shadows. */
		NaniteMeshPass,
		MeshDecal,
		WaterInfoTextureDepthPass,
		WaterInfoTexturePass,
		MobileAfterTranslucencyPass,//RenderAfterTranslucency Added

#if WITH_EDITOR
		HitProxy,
		HitProxyOpaqueOnly,
		EditorLevelInstance,
		EditorSelection,
#endif

		Num,
		NumBits = 6,
	};
}
```

:83在GetMeshPassName中添加case EMeshPass::MobileAfterTranslucencyPass: return TEXT("
MobileAfterTranslucencyPass");并更新底部断言EMeshPass::Num == 33 + 4与EMeshPass::Num == 33

```c++
inline const TCHAR* GetMeshPassName(EMeshPass::Type MeshPass)
{
	switch (MeshPass)
	{
	case EMeshPass::DepthPass: return TEXT("DepthPass");
	case EMeshPass::SecondStageDepthPass: return TEXT("SecondStageDepthPass");
	case EMeshPass::BasePass: return TEXT("BasePass");
	case EMeshPass::AnisotropyPass: return TEXT("AnisotropyPass");
	case EMeshPass::SkyPass: return TEXT("SkyPass");
	case EMeshPass::SingleLayerWaterPass: return TEXT("SingleLayerWaterPass");
	case EMeshPass::SingleLayerWaterDepthPrepass: return TEXT("SingleLayerWaterDepthPrepass");
	case EMeshPass::CSMShadowDepth: return TEXT("CSMShadowDepth");
	case EMeshPass::VSMShadowDepth: return TEXT("VSMShadowDepth");
	case EMeshPass::Distortion: return TEXT("Distortion");
	case EMeshPass::Velocity: return TEXT("Velocity");
	case EMeshPass::TranslucentVelocity: return TEXT("TranslucentVelocity");
	case EMeshPass::TranslucencyStandard: return TEXT("TranslucencyStandard");
	case EMeshPass::TranslucencyStandardModulate: return TEXT("TranslucencyStandardModulate");
	case EMeshPass::TranslucencyAfterDOF: return TEXT("TranslucencyAfterDOF");
	case EMeshPass::TranslucencyAfterDOFModulate: return TEXT("TranslucencyAfterDOFModulate");
	case EMeshPass::TranslucencyAfterMotionBlur: return TEXT("TranslucencyAfterMotionBlur");
	case EMeshPass::TranslucencyAll: return TEXT("TranslucencyAll");
	case EMeshPass::LightmapDensity: return TEXT("LightmapDensity");
	case EMeshPass::DebugViewMode: return TEXT("DebugViewMode");
	case EMeshPass::CustomDepth: return TEXT("CustomDepth");
	case EMeshPass::MobileBasePassCSM: return TEXT("MobileBasePassCSM");
	case EMeshPass::VirtualTexture: return TEXT("VirtualTexture");
	case EMeshPass::LumenCardCapture: return TEXT("LumenCardCapture");
	case EMeshPass::LumenCardNanite: return TEXT("LumenCardNanite");
	case EMeshPass::LumenTranslucencyRadianceCacheMark: return TEXT("LumenTranslucencyRadianceCacheMark");
	case EMeshPass::LumenFrontLayerTranslucencyGBuffer: return TEXT("LumenFrontLayerTranslucencyGBuffer");
	case EMeshPass::DitheredLODFadingOutMaskPass: return TEXT("DitheredLODFadingOutMaskPass");
	case EMeshPass::NaniteMeshPass: return TEXT("NaniteMeshPass");
	case EMeshPass::MeshDecal: return TEXT("MeshDecal");
	case EMeshPass::WaterInfoTextureDepthPass: return TEXT("WaterInfoTextureDepthPass");
	case EMeshPass::WaterInfoTexturePass: return TEXT("WaterInfoTexturePass");
	case EMeshPass::MobileAfterTranslucencyPass: return TEXT("MobileAfterTranslucencyPass");//RenderAfterTranslucency Added
#if WITH_EDITOR
	case EMeshPass::HitProxy: return TEXT("HitProxy");
	case EMeshPass::HitProxyOpaqueOnly: return TEXT("HitProxyOpaqueOnly");
	case EMeshPass::EditorLevelInstance: return TEXT("EditorLevelInstance");
	case EMeshPass::EditorSelection: return TEXT("EditorSelection");
#endif
	}

#if WITH_EDITOR//RenderAfterTranslucency Changed
	static_assert(EMeshPass::Num == 33 + 4, "Need to update switch(MeshPass) after changing EMeshPass"); // GUID to prevent incorrect auto-resolves, please change when changing the expression: {674D7D62-CFD8-4971-9A8D-CD91E5612CD8}
#else
	static_assert(EMeshPass::Num == 33, "Need to update switch(MeshPass) after changing EMeshPass"); // GUID to prevent incorrect auto-resolves, please change when changing the expression: {674D7D62-CFD8-4971-9A8D-CD91E5612CD8}
#endif

	checkf(0, TEXT("Missing case for EMeshPass %u"), (uint32)MeshPass);
	return nullptr;
}
```

2. Engine/Source/Runtime/Engine/Classes/Components/PrimitiveComponent.h:407附近添加bRenderAfterTranslucency字段

```c++
//...
    UPROPERTY(EditAnywhere, AdvancedDisplay, BlueprintReadOnly, Category = Rendering)
    uint8 bRenderInMainPass:1;
    
    UPROPERTY(EditAnywhere, AdvancedDisplay, BlueprintReadOnly, Category = Rendering, meta = (DisplayName = "Render Opaque After Translucency (Mobile)"))
    uint8 bRenderAfterTranslucency : 1;//RenderAfterTranslucency Added
//...
```

在:1917附近添加Setter

```c++
	UFUNCTION(BlueprintCallable, Category = "Rendering")
	ENGINE_API void SetRenderInMainPass(bool bValue);
	
	//RenderAfterTranslucency Added
	UFUNCTION(BlueprintCallable, Category = "Rendering")
	ENGINE_API void SetRenderAfterTranslucency(bool bValue);
```

Engine/Source/Runtime/Engine/Private/Components/PrimitiveComponent.cpp:4457附近实现Setter

```c++
void UPrimitiveComponent::SetRenderInMainPass(bool bValue)
{
	if (bRenderInMainPass != bValue)
	{
		bRenderInMainPass = bValue;
		MarkRenderStateDirty();
	}
}

//RenderAfterTranslucency Added
void UPrimitiveComponent::SetRenderAfterTranslucency(bool bValue)
{
	if (bRenderAfterTranslucency != bValue)
	{
		bRenderAfterTranslucency = bValue;
		MarkRenderStateDirty();
	}
}
```

:333附近添加bRenderAfterTranslucency = false

```c++
    bRenderInMainPass = true;
    bRenderAfterTranslucency = false;//RenderAfterTranslucency Added
```

3. Engine/Source/Runtime/Engine/Public/PrimitiveSceneProxy.h:1200附近添加bRenderAfterTranslucency

```c++
   uint8 bRenderInMainPass : 1;
   uint8 bRenderAfterTranslucency : 1;//RenderAfterTranslucency Added
```

:700附近添加ShouldRenderAfterTranslucency()

```c++
    inline bool ShouldRenderInMainPass() const { return bRenderInMainPass; }
    inline bool ShouldRenderAfterTranslucency() const { return bRenderAfterTranslucency; }//RenderAfterTranslucency Added
```

Engine/Source/Runtime/Engine/Private/PrimitiveSceneProxy.cpp的InitializeFrom中:277附近添加初始化bRenderAfterTranslucency

```c++
    bRenderInMainPass = InComponent->bRenderInMainPass;
    bRenderAfterTranslucency = InComponent->bRenderAfterTranslucency;//RenderAfterTranslucency Added
```

:428附近添加初始化bRenderAfterTranslucency

```c++
    bRenderInMainPass(InProxyDesc.bRenderInMainPass),
    bRenderAfterTranslucency(InProxyDesc.bRenderAfterTranslucency),//RenderAfterTranslucency Added
```

Engine/Source/Runtime/Engine/Public/PrimitiveSceneProxyDesc.h:93附近添加bRenderAfterTranslucency

```c++
    uint32 bRenderInMainPass : 1;
    uint32 bRenderAfterTranslucency : 1;//RenderAfterTranslucency Added
```

:25附近添加bRenderAfterTranslucency = false

```c++
    bRenderInMainPass = true;
    bRenderAfterTranslucency = false;//RenderAfterTranslucency Added
```

4. Engine/Source/Runtime/Renderer/Private/MobileBasePassRendering.h:533附近添加bAfterTranslucencyBasePass

```c++
    const bool bPassUsesDeferredShading; 
    const bool bAfterTranslucencyBasePass; //RenderAfterTranslucency Added
```

MobileBasePassRendering.h:480构造函数添加bool bAfterTranslucencyBasePass，并设置默认值为false

```c++
	FMobileBasePassMeshProcessor(
		EMeshPass::Type InMeshPassType,
		const FScene* InScene,
		const FSceneView* InViewIfDynamicMeshCommand,
		const FMeshPassProcessorRenderState& InDrawRenderState,
		FMeshPassDrawListContext* InDrawListContext,
		EFlags Flags,
		ETranslucencyPass::Type InTranslucencyPassType = ETranslucencyPass::TPT_MAX,
		bool bAfterTranslucencyBasePass = false);//RenderAfterTranslucency Added
```

Engine/Source/Runtime/Renderer/Private/MobileBasePass.cpp:810附近添加构造函数初始化bAfterTranslucencyBasePass(
IsAfterTranslucencyBasePass)

```c++
   FMobileBasePassMeshProcessor::FMobileBasePassMeshProcessor(
       EMeshPass::Type InMeshPassType,
       const FScene* Scene,
       const FSceneView* InViewIfDynamicMeshCommand,
       const FMeshPassProcessorRenderState& InDrawRenderState,
       FMeshPassDrawListContext* InDrawListContext,
       EFlags InFlags,
       ETranslucencyPass::Type InTranslucencyPassType,
       bool IsAfterTranslucencyBasePass)
       : FMeshPassProcessor(InMeshPassType, Scene, ERHIFeatureLevel::ES3_1, InViewIfDynamicMeshCommand, InDrawListContext)
       , PassDrawRenderState(InDrawRenderState)
       , TranslucencyPassType(InTranslucencyPassType)
       , Flags(InFlags)
       , bTranslucentBasePass(InTranslucencyPassType != ETranslucencyPass::TPT_MAX)
       , bDeferredShading(IsMobileDeferredShadingEnabled(GetFeatureLevelShaderPlatform(ERHIFeatureLevel::ES3_1)))
       , bPassUsesDeferredShading(bDeferredShading && !bTranslucentBasePass)
       , bAfterTranslucencyBasePass(IsAfterTranslucencyBasePass)//RenderAfterTranslucency Added
   {
   }
```

Engine/Source/Runtime/Renderer/Private/MobileBasePass.cpp:867处修改AddMeshBatch函数，通过构造函数传入的bAfterTranslucencyBasePass与PrimitiveSceneProxy中的ShouldRenderAfterTranslucency做Pass分流

```c++
void FMobileBasePassMeshProcessor::AddMeshBatch(const FMeshBatch &RESTRICT MeshBatch, uint64 BatchElementMask, const FPrimitiveSceneProxy *RESTRICT PrimitiveSceneProxy, int32 StaticMeshId)
{
    if (!MeshBatch.bUseForMaterial ||
        (Flags & FMobileBasePassMeshProcessor::EFlags::DoNotCache) == FMobileBasePassMeshProcessor::EFlags::DoNotCache ||
        (PrimitiveSceneProxy && !PrimitiveSceneProxy->ShouldRenderInMainPass()))
    {
        return;
    }
    //RenderAfterTranslucency Added
    bool bShouldRenderAfterTranslucency = PrimitiveSceneProxy->ShouldRenderAfterTranslucency();
    if (bAfterTranslucencyBasePass)
    {
        if (!bShouldRenderAfterTranslucency)
            return;
    }
    else
    {
        if (bShouldRenderAfterTranslucency)
            return;
    }

    const FMaterialRenderProxy *MaterialRenderProxy = MeshBatch.MaterialRenderProxy;
    while (MaterialRenderProxy)
    {
        const FMaterial *Material = MaterialRenderProxy->GetMaterialNoFallback(FeatureLevel);
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
```

在:1151处模仿CreateMobileBasePassProcessor加入

```c++
FMeshPassProcessor* CreateMobileBasePassProcessor(ERHIFeatureLevel::Type FeatureLevel, const FScene* Scene, const FSceneView* InViewIfDynamicMeshCommand, FMeshPassDrawListContext* InDrawListContext)
{
	FMeshPassProcessorRenderState PassDrawRenderState;
	PassDrawRenderState.SetBlendState(TStaticBlendStateWriteMask<CW_RGBA>::GetRHI());
	const FExclusiveDepthStencil::Type DefaultBasePassDepthStencilAccess = FScene::GetDefaultBasePassDepthStencilAccess(FeatureLevel);
	PassDrawRenderState.SetDepthStencilAccess(DefaultBasePassDepthStencilAccess);
	PassDrawRenderState.SetDepthStencilState(TStaticDepthStencilState<true, CF_DepthNearOrEqual>::GetRHI());

	const FMobileBasePassMeshProcessor::EFlags Flags = FMobileBasePassMeshProcessor::EFlags::CanUseDepthStencil
		| (MobileBasePassAlwaysUsesCSM(GShaderPlatformForFeatureLevel[FeatureLevel]) ? FMobileBasePassMeshProcessor::EFlags::CanReceiveCSM : FMobileBasePassMeshProcessor::EFlags::None);

	return new FMobileBasePassMeshProcessor(EMeshPass::BasePass, Scene, InViewIfDynamicMeshCommand, PassDrawRenderState, InDrawListContext, Flags);
}
//...
//RenderAfterTranslucency Added
FMeshPassProcessor* CreateMobileAfterTranslucencyPassProcessor(ERHIFeatureLevel::Type FeatureLevel, const FScene* Scene, const FSceneView* InViewIfDynamicMeshCommand, FMeshPassDrawListContext* InDrawListContext)
{
	FMeshPassProcessorRenderState PassDrawRenderState;
	//PassDrawRenderState.SetBlendState(TStaticBlendStateWriteMask<CW_RGBA>::GetRHI());//是否还需要？
	PassDrawRenderState.SetDepthStencilAccess(FExclusiveDepthStencil::DepthRead_StencilRead);
	PassDrawRenderState.SetDepthStencilState(TStaticDepthStencilState<false, CF_DepthNearOrEqual>::GetRHI());//这里使用false就可以了吧？SetDepthStencilState和TStaticDepthStencilState的作用是什么？

	const FMobileBasePassMeshProcessor::EFlags Flags = FMobileBasePassMeshProcessor::EFlags::CanUseDepthStencil;//这里直接赋值CanUseDepthStencil就可以了吧？不需要CreateMobileBasePassProcessor中那么多判断吧？

	return new FMobileBasePassMeshProcessor(EMeshPass::MobileAfterTranslucencyPass, Scene, InViewIfDynamicMeshCommand, PassDrawRenderState, InDrawListContext, Flags, true);
}
```

在:1223处加入

```c++
REGISTER_MESHPASSPROCESSOR_AND_PSOCOLLECTOR(MobileTranslucencyAfterDOFPass,	CreateMobileTranslucencyAfterDOFProcessor,	EShadingPath::Mobile, EMeshPass::TranslucencyAfterDOF, 	EMeshPassFlags::MainView);
//RenderAfterTranslucency Added
REGISTER_MESHPASSPROCESSOR_AND_PSOCOLLECTOR(MobileAfterTranslucencyPass, CreateMobileAfterTranslucencyPassProcessor, EShadingPath::Mobile, EMeshPass::MobileAfterTranslucencyPass, EMeshPassFlags::CachedMeshCommands | EMeshPassFlags::MainView);
```

5. Engine/Source/Runtime/Renderer/Private/MobileBasePassRendering.cpp:492附近添加RenderMobileAfterTranslucencyPass()

```c++
void FMobileSceneRenderer::RenderMobileBasePass(FRHICommandList& RHICmdList, const FViewInfo& View, const FInstanceCullingDrawParams* InstanceCullingDrawParams)
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
//...
//RenderAfterTranslucency Added
void FMobileSceneRenderer::RenderMobileAfterTranslucencyPass(FRHICommandList& RHICmdList, const FViewInfo& View, const FInstanceCullingDrawParams* InstanceCullingDrawParams)
{
	CSV_SCOPED_TIMING_STAT_EXCLUSIVE(RenderAfterTranslucency);
	SCOPED_DRAW_EVENT(RHICmdList, MobileAfterTranslucencyPass);
	SCOPE_CYCLE_COUNTER(STAT_AfterTranslucencyDrawTime);
	SCOPED_GPU_STAT(RHICmdList, AfterTranslucency);

	RHICmdList.SetViewport(View.ViewRect.Min.X, View.ViewRect.Min.Y, 0, View.ViewRect.Max.X, View.ViewRect.Max.Y, 1);
	View.ParallelMeshDrawCommandPasses[EMeshPass::MobileAfterTranslucencyPass].DispatchDraw(nullptr, RHICmdList, InstanceCullingDrawParams);
}
```
Engine/Source/Runtime/Renderer/Private/SceneRendering.h的FMobileSceneRenderer中（RenderMobileBasePass声明附近，约 2695 行）添加：
```c++
	void RenderMobileBasePass(FRHICommandList& RHICmdList, const FViewInfo& View, const FInstanceCullingDrawParams* InstanceCullingDrawParams);
    void RenderMobileAfterTranslucencyPass(FRHICommandList& RHICmdList, const FViewInfo& View, const FInstanceCullingDrawParams* InstanceCullingDrawParams);
```
Engine/Source/Runtime/RenderCore/Private/RenderCore.cpp:65附近添加 DEFINE_STAT(STAT_AfterTranslucencyDrawTime)

```c++
DEFINE_STAT(STAT_BasePassDrawTime);
DEFINE_STAT(STAT_AfterTranslucencyDrawTime);
```
在 RenderCore.h 中（STAT_BasePassDrawTime 声明附近，约 44 行）添加：
```c++
DECLARE_CYCLE_STAT_EXTERN(TEXT("Base pass drawing"),STAT_BasePassDrawTime,STATGROUP_SceneRendering, RENDERCORE_API);
DECLARE_CYCLE_STAT_EXTERN(TEXT("After translucency drawing"), STAT_AfterTranslucencyDrawTime, STATGROUP_SceneRendering, RENDERCORE_API);
```
Engine/Source/Runtime/Renderer/Private/BasePassRendering.h:144附近
```c++
DECLARE_GPU_DRAWCALL_STAT_EXTERN(Basepass);
DECLARE_GPU_DRAWCALL_STAT_EXTERN(AfterTranslucency);
```
在Engine/Source/Runtime/Renderer/Private/SceneRendering.h:2796处添加AfterTranslucencyInstanceCullingDrawParams
```c++
	FInstanceCullingDrawParams TranslucencyInstanceCullingDrawParams;	
	FInstanceCullingDrawParams AfterTranslucencyInstanceCullingDrawParams;
```
在Engine/Source/Runtime/Renderer/Private/MobileShadingRenderer.cpp:1433BuildInstanceCullingDrawParams中添加
```c++
void FMobileSceneRenderer::BuildInstanceCullingDrawParams(FRDGBuilder& GraphBuilder, FViewInfo& View, FMobileRenderPassParameters* PassParameters)
{
	if (Scene->GPUScene.IsEnabled())
	{
		if (!bIsFullDepthPrepassEnabled)
		{
			View.ParallelMeshDrawCommandPasses[EMeshPass::DepthPass].BuildRenderingCommands(GraphBuilder, Scene->GPUScene, DepthPassInstanceCullingDrawParams);
		}
		View.ParallelMeshDrawCommandPasses[EMeshPass::BasePass].BuildRenderingCommands(GraphBuilder, Scene->GPUScene, PassParameters->InstanceCullingDrawParams);
		View.ParallelMeshDrawCommandPasses[EMeshPass::SkyPass].BuildRenderingCommands(GraphBuilder, Scene->GPUScene, SkyPassInstanceCullingDrawParams);
		View.ParallelMeshDrawCommandPasses[StandardTranslucencyMeshPass].BuildRenderingCommands(GraphBuilder, Scene->GPUScene, TranslucencyInstanceCullingDrawParams);
		View.ParallelMeshDrawCommandPasses[EMeshPass::DebugViewMode].BuildRenderingCommands(GraphBuilder, Scene->GPUScene, DebugViewModeInstanceCullingDrawParams);
		View.ParallelMeshDrawCommandPasses[EMeshPass::MobileAfterTranslucencyPass].BuildRenderingCommands(GraphBuilder, Scene->GPUScene, AfterTranslucencyInstanceCullingDrawParams);
	}
}
```
Engine/Source/Runtime/Renderer/Private/MobileShadingRenderer.cpp:1624处RenderForwardSinglePass中添加RenderMobileAfterTranslucencyPass()调用

```c++
//...
RenderTranslucency(RHICmdList, View);
RenderMobileAfterTranslucencyPass(RHICmdList, View, &AfterTranslucencyInstanceCullingDrawParams);//RenderAfterTranslucency Added
```

:1736处RenderForwardMultiPass中添加RenderMobileAfterTranslucencyPass()调用

```c++
//...
RenderTranslucency(RHICmdList, View);
RenderMobileAfterTranslucencyPass(RHICmdList, View, &AfterTranslucencyInstanceCullingDrawParams);//RenderAfterTranslucency Added
```

6. Engine/Source/Runtime/Engine/Public/PrimitiveViewRelevance.h:54附近添加bRenderAfterTranslucency

```c++
    uint32 bRenderInMainPass : 1;
    uint32 bRenderAfterTranslucency : 1;//RenderAfterTranslucency Added
```

在:103附近添加bRenderAfterTranslucency = false

```c++
	FPrimitiveViewRelevance()
	{
		// the class is only storing bits, the following avoids code redundancy
		uint8 * RESTRICT p = (uint8*)this;
		for(uint32 i = 0; i < sizeof(*this); ++i)
		{
			*p++ = 0;
		}

		// only exceptions (bugs we need to fix?):

		bOpaque = true;
		// without it BSP doesn't render
		bRenderInMainPass = true;
	}
	//修改为↓
		FPrimitiveViewRelevance()
	{
		// the class is only storing bits, the following avoids code redundancy
		uint8 * RESTRICT p = (uint8*)this;
		for(uint32 i = 0; i < sizeof(*this); ++i)
		{
			*p++ = 0;
		}

		// only exceptions (bugs we need to fix?):

		bOpaque = true;
		// without it BSP doesn't render
		bRenderInMainPass = true;
		bRenderAfterTranslucency = false;//RenderAfterTranslucency Added
	}
```

Engine/Source/Runtime/Engine/Private/StaticMeshRender.cpp:2055 GetViewRelevance中添加

```c++
    Result.bRenderInMainPass = ShouldRenderInMainPass();
    Result.bRenderAfterTranslucency = ShouldRenderAfterTranslucency();
```

Engine/Source/Runtime/Engine/Private/SkeletalMesh.cpp:7107 GetViewRelevance中添加

```c++
    Result.bRenderInMainPass = ShouldRenderInMainPass();
    Result.bRenderAfterTranslucency = ShouldRenderAfterTranslucency();
```

在Runtime/Renderer/Private/SceneVisibility.cpp:1564附近添加

```c++
if (StaticMeshRelevance.bUseForMaterial && (ViewRelevance.bRenderInMainPass || ViewRelevance.bRenderCustomDepth))
{
    // Specific logic for mobile packets
    if (ShadingPath == EShadingPath::Mobile)
    {
        // Skydome must not be added to base pass bucket
        if (!StaticMeshRelevance.bUseSkyMaterial)
        {
            DrawCommandPacket.AddCommandsForMesh(PrimitiveIndex, PrimitiveSceneInfo, StaticMeshRelevance, StaticMesh, CullingPayloadFlags, Scene, bCanCache, EMeshPass::BasePass);
            if (!bMobileBasePassAlwaysUsesCSM)
            {
                DrawCommandPacket.AddCommandsForMesh(PrimitiveIndex, PrimitiveSceneInfo, StaticMeshRelevance, StaticMesh, CullingPayloadFlags, Scene, bCanCache, EMeshPass::MobileBasePassCSM);
            }
        }
        else
        {
            DrawCommandPacket.AddCommandsForMesh(PrimitiveIndex, PrimitiveSceneInfo, StaticMeshRelevance, StaticMesh, CullingPayloadFlags, Scene, bCanCache, EMeshPass::SkyPass);
        }
        // bUseSingleLayerWaterMaterial is added to BasePass on Mobile. No need to add it to SingleLayerWaterPass

        MarkMask |= EMarkMaskBits::StaticMeshVisibilityMapMask;
    }
//修改为↓
if (StaticMeshRelevance.bUseForMaterial && (ViewRelevance.bRenderInMainPass || ViewRelevance.bRenderCustomDepth))
{
    // Specific logic for mobile packets
    if (ShadingPath == EShadingPath::Mobile)
    {
        // Skydome must not be added to base pass bucket
        if (!StaticMeshRelevance.bUseSkyMaterial)
        {
            if(ViewRelevance.bRenderAfterTranslucency)
            {
                DrawCommandPacket.AddCommandsForMesh(PrimitiveIndex, PrimitiveSceneInfo, StaticMeshRelevance, StaticMesh, CullingPayloadFlags, Scene, bCanCache, EMeshPass::MobileAfterTranslucencyPass);//RenderAfterTranslucency Added
            }else
            {
                DrawCommandPacket.AddCommandsForMesh(PrimitiveIndex, PrimitiveSceneInfo, StaticMeshRelevance, StaticMesh, CullingPayloadFlags, Scene, bCanCache, EMeshPass::BasePass);
            }
            if (!bMobileBasePassAlwaysUsesCSM)
            {
                DrawCommandPacket.AddCommandsForMesh(PrimitiveIndex, PrimitiveSceneInfo, StaticMeshRelevance, StaticMesh, CullingPayloadFlags, Scene, bCanCache, EMeshPass::MobileBasePassCSM);
            }
        }
        else
        {
            DrawCommandPacket.AddCommandsForMesh(PrimitiveIndex, PrimitiveSceneInfo, StaticMeshRelevance, StaticMesh, CullingPayloadFlags, Scene, bCanCache, EMeshPass::SkyPass);
        }
        // bUseSingleLayerWaterMaterial is added to BasePass on Mobile. No need to add it to SingleLayerWaterPass

        MarkMask |= EMarkMaskBits::StaticMeshVisibilityMapMask;
    }
```
Engine/Source/Runtime/Renderer/Private/SceneVisibility.cpp:2186 ComputeDynamicMeshRelevance中，:2211附近
```c++
    if (ViewRelevance.bRenderInMainPass || ViewRelevance.bRenderCustomDepth)
    {
    	PassMask.Set(EMeshPass::BasePass);
    	View.NumVisibleDynamicMeshElements[EMeshPass::BasePass] += NumElements;
    //...
//修改为↓
    if (ViewRelevance.bRenderInMainPass || ViewRelevance.bRenderCustomDepth)
    {
        if(ShadingPath == EShadingPath::Mobile && ViewRelevance.bRenderAfterTranslucency)
        {
            PassMask.Set(EMeshPass::MobileAfterTranslucencyPass);
            View.NumVisibleDynamicMeshElements[EMeshPass::MobileAfterTranslucencyPass] += NumElements;
        }else
        {
            PassMask.Set(EMeshPass::BasePass);
            View.NumVisibleDynamicMeshElements[EMeshPass::BasePass] += NumElements;
        }
```
---
3.3 设计问题 — bAfterTranslucencyBasePass 成员冗余
计划添加了 bool bAfterTranslucencyBasePass 成员，但同样的信息可通过 InMeshPassType == EMeshPass::MobileAfterTranslucencyPass 获得。在 AddMeshBatch 中可简化为：

// 在 AddMeshBatch 中，用 MeshPassType 替换 bAfterTranslucencyBasePass 分支：
const bool bAfterTranslucencyPass = (MeshPassType == EMeshPass::MobileAfterTranslucencyPass);
if (bAfterTranslucencyPass != PrimitiveSceneProxy->ShouldRenderAfterTranslucency())
{
return;
}
或者直接比较 MeshPassType 以完全去掉这个成员。