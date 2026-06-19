1. Engine/Source/Runtime/Renderer/Public/MeshPassProcessor.h:32在EMeshPass中添加MobileBasePassAfterTranslucency

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

#if WITH_EDITOR
		HitProxy,
		HitProxyOpaqueOnly,
		EditorLevelInstance,
		EditorSelection,
#endif

		Num,
		NumBits = 6,
		MobileBasePassAfterTranslucency,//RenderAfterTranslucency Added
	};
}
```

:83在GetMeshPassName中添加case EMeshPass::MobileBasePassAfterTranslucency: return TEXT("
MobileBasePassAfterTranslucency");并更新底部断言EMeshPass::Num == 33 + 4与EMeshPass::Num == 33

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
	case EMeshPass::MobileBasePassAfterTranslucency: return TEXT("MobileBasePassAfterTranslucency");//RenderAfterTranslucency Added
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

Engine/Source/Runtime/Engine/Private/PrimitiveSceneProxy.cpp:277附近添加初始化bRenderAfterTranslucency

```c++
    bRenderInMainPass = InComponent->bRenderInMainPass;
    bRenderAfterTranslucency = InComponent->bRenderAfterTranslucency;//RenderAfterTranslucency Added
```

:428附近添加初始化bRenderAfterTranslucency

```c++
    bRenderInMainPass(InProxyDesc.bRenderInMainPass),
    bRenderAfterTranslucency(InProxyDesc.bRenderAfterTranslucency),//RenderAfterTranslucency Added
```

4. Engine/Source/Runtime/Renderer/Private/MobileBasePassRendering.h:533附近添加bAfterTranslucencyBasePass

```c++
    const bool bPassUsesDeferredShading; 
    const bool bAfterTranslucencyBasePass; //RenderAfterTranslucency Added
```

MobileBasePassRendering.h:480附近构造函数添加,bool bAfterTranslucencyBasePass

```c++
	FMobileBasePassMeshProcessor(
		EMeshPass::Type InMeshPassType,
		const FScene* InScene,
		const FSceneView* InViewIfDynamicMeshCommand,
		const FMeshPassProcessorRenderState& InDrawRenderState,
		FMeshPassDrawListContext* InDrawListContext,
		EFlags Flags,
		ETranslucencyPass::Type InTranslucencyPassType = ETranslucencyPass::TPT_MAX,
		bool bAfterTranslucencyBasePass);//RenderAfterTranslucency Added
```

Engine/Source/Runtime/Renderer/Private/MobileBasePass.cpp

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
       , bPassUsesDeferredShading(bDeferredShading && !bTranslucentBasePass
       , bAfterTranslucencyBasePass(IsAfterTranslucencyBasePass))//RenderAfterTranslucency Added
   {
   }
```

Engine/Source/Runtime/Renderer/Private/MobileBasePass.cpp:
867处修改AddMeshBatch函数，通过构造函数传入的bAfterTranslucencyBasePass与PrimitiveSceneProxy中的ShouldRenderAfterTranslucency做Pass分流

```c++
void FMobileBasePassMeshProcessor::AddMeshBatch(const FMeshBatch &RESTRICT MeshBatch, uint64 BatchElementMask, const FPrimitiveSceneProxy *RESTRICT PrimitiveSceneProxy, int32 StaticMeshId)
{
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
    if (!MeshBatch.bUseForMaterial ||
        (Flags & FMobileBasePassMeshProcessor::EFlags::DoNotCache) == FMobileBasePassMeshProcessor::EFlags::DoNotCache ||
        (PrimitiveSceneProxy && !PrimitiveSceneProxy->ShouldRenderInMainPass()))
    {
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

---
PrimitiveViewRelevance.h?