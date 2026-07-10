1. Scene->GetShaderPlatform()不可用

2. 这里如何做PSO预热，如何修改
void FMobileBasePassMeshProcessor::CollectPSOInitializers(const FSceneTexturesConfig& SceneTexturesConfig, const FMaterial& Material, const FPSOPrecacheVertexFactoryData& VertexFactoryData, const FPSOPrecacheParams& PreCacheParams, TArray<FPSOPrecacheData>& PSOInitializers)
{
	//RenderAfterTranslucency Added: 暂不为该Pass做PSO预热（SetupPrecachePSOParams不感知标记，收集到的状态也不匹配）。
	// 代价：标记物体首次绘制时运行时编译PSO，可能有一次卡顿；如需消除，可参照本函数按颜色Pass实际RenderState收集。
	if (MeshPassType == EMeshPass::MobileAfterTranslucencyPass)
	{
		return;
	}
	static IConsoleVariable* PSOPrecacheTranslucencyAllPass = IConsoleManager::Get().FindConsoleVariable(TEXT("r.PSOPrecache.TranslucencyAllPass"));
	// PSO precaching enabled for TranslucencyAll
	if (MeshPassType == EMeshPass::TranslucencyAll && PSOPrecacheTranslucencyAllPass->GetInt() == 0)
	{
		return;
	}

我现在正在UE5.4中开发Android端VR游戏，我现在有这样一个需求，在渲染不透明物体的阶段，我标记的物体不进行渲染颜色，只写入深度，当渲染完成透明物体后再渲染我标记的物体，因为我在透明物体之后渲染，透明物体不写入深度，所以我可以把透明物体遮挡住（这是我的核心需求），而且可以读到我标记物体的深度
我修改源码的思路是我在不透明渲染Pass之后添加MobileAfterTranslucencyDepthPass写入我标记物体的深度，但不写入颜色，在透明物体渲染Pass之后添加MobileAfterTranslucencyPass绘制我标记的物体，这时已经有深度了，可以直接读取我只需要让Mesh和Skeletal Mesh生效即可，我也不需要CustomDepth，我只需要移动端，Forward渲染路径的修改即可

帮我验证是否都修改好了，