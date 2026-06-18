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