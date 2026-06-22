//1. -----------------------------------------------------------------------
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
//这段代码修改成
//不管是BasePass还是MobileBasePassAfterTranslucent，都是靠bAfterTranslucent来判断的,是这样的吧
!bIsTranslucent && !bAfterTranslucent 

//2. -----------------------------------------------------------------------
4.5 Source/Runtime/Renderer/Private/SceneVisibility.cpp —— 收集到 ParallelMeshDrawCommandPasses
搜索 EMeshPass::TranslucencyStandard 在 SceneVisibility.cpp / SceneRendering.cpp 中的 setup 流程，把新 Pass 加入 View.ParallelMeshDrawCommandPasses：

在 FSceneRenderer::SetupMeshPass（或移动端等价位置 FMobileSceneRenderer::InitViews 内调用 SetupMeshPass） 中找到这一段：
case EMeshPass::TranslucencyStandard: { ... break; }
case EMeshPass::TranslucencyAll:      { ... break; }
case EMeshPass::TranslucencyAfterDOF: { ... break; }
添加：
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
真实代码可直接复制 case EMeshPass::BasePass 分支稍作改名即可，UE5.4 中所有 Pass 的 SetupMeshPass 模板都是同构的。
我并没有找到
case EMeshPass::TranslucencyStandard: { ... break; }
case EMeshPass::TranslucencyAll:      { ... break; }
case EMeshPass::TranslucencyAfterDOF: { ... break; }
这些，检查是否正确进行验证，给我正确的文件和行号
//3. -----------------------------------------------------------------------
4.6 Source/Runtime/Renderer/Private/MobileTranslucentRendering.cpp
在 FMobileSceneRenderer::RenderTranslucency 之后，新增一个新的成员函数：

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
这个地方没有写Editor下的渲染
Engine/Source/Runtime/Renderer/Private/MobileBasePassRendering.cpp:470
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
是否可以参考这个进行修改？


直接在AddMeshBatch开头做区分
void FMobileBasePassMeshProcessor::AddMeshBatch(const FMeshBatch &RESTRICT MeshBatch, uint64 BatchElementMask, const FPrimitiveSceneProxy *RESTRICT PrimitiveSceneProxy, int32 StaticMeshId)
{
    if (PrimitiveSceneProxy->IsRenderAfterTranslucent())
    {
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
//修改为👇
if (StaticMeshRelevance.bUseForMaterial && (ViewRelevance.bRenderInMainPass || ViewRelevance.bRenderCustomDepth))
{
    // Specific logic for mobile packets
    if (ShadingPath == EShadingPath::Mobile)
    {
        // Skydome must not be added to base pass bucket
        if (!StaticMeshRelevance.bUseSkyMaterial)
        {
            DrawCommandPacket.AddCommandsForMesh(PrimitiveIndex, PrimitiveSceneInfo, StaticMeshRelevance, StaticMesh, CullingPayloadFlags, Scene, bCanCache, EMeshPass::BasePass);
            DrawCommandPacket.AddCommandsForMesh(PrimitiveIndex, PrimitiveSceneInfo, StaticMeshRelevance, StaticMesh, CullingPayloadFlags, Scene, bCanCache, EMeshPass::MobileAfterTranslucencyPass);
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