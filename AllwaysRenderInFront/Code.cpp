// 在构造函数中添加bIsAfterranslucencyPass的初始化
void FMobileBasePassMeshProcessor::AddMeshBatch(const FMeshBatch &RESTRICT MeshBatch, uint64 BatchElementMask, const FPrimitiveSceneProxy *RESTRICT PrimitiveSceneProxy, int32 StaticMeshId)
{
    bool bAfterTranslucencyBasePass = PrimitiveSceneProxy->ShouldRenderAfterTranslucency();
    if (bIsAfterranslucencyPass)
    {
        if (!bAfterTranslucencyBasePass)
            return;
    }
    else
    {
        if (bAfterTranslucencyBasePass)
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