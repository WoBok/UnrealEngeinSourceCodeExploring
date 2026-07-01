1. 对Engine/Source/Runtime/Renderer/Private/DepthRendering.cpp:1230进行改造
FMeshPassProcessor* CreateDepthPassProcessor(ERHIFeatureLevel::Type FeatureLevel, const FScene* Scene, const FSceneView* InViewIfDynamicMeshCommand, FMeshPassDrawListContext* InDrawListContext)
{
EDepthDrawingMode EarlyZPassMode;
bool bEarlyZPassMovable;
FScene::GetEarlyZPassMode(FeatureLevel, EarlyZPassMode, bEarlyZPassMovable);

	FMeshPassProcessorRenderState DepthPassState;
	SetupDepthPassState(DepthPassState);
		
	return new FDepthPassMeshProcessor(EMeshPass::DepthPass, Scene, FeatureLevel, InViewIfDynamicMeshCommand, DepthPassState, true, EarlyZPassMode, bEarlyZPassMovable, false, InDrawListContext);
}

FMeshPassProcessor* CreateMobileDepthPassProcessor(...



2. Engine/Source/Runtime/Renderer/Private/MobileShadingRenderer.cpp:377 SetupMobileBasePassAfterShadowInit中
   FMeshPassProcessor* MeshPassProcessor = FPassProcessorManager::CreateMeshPassProcessor(EShadingPath::Mobile, EMeshPass::BasePass, Scene->GetFeatureLevel(), Scene, &View, nullptr);
    和Engine/Source/Runtime/Renderer/Private/SceneRendering.cpp:4196 SetupMeshPass
   FMeshPassProcessor* MeshPassProcessor = FPassProcessorManager::CreateMeshPassProcessor(ShadingPath, PassType, Scene->GetFeatureLevel(), Scene, &View, nullptr);
    的关系？SceneRendering与移动端创建MeshPassProcessor有关系吗？
3. 
这个文档是我之前对移动端DepthPass的分析，
Engine/Source/Runtime/Renderer/Private/MobileShadingRenderer.cpp:377 SetupMobileBasePassAfterShadowInit中
FMeshPassProcessor* MeshPassProcessor = FPassProcessorManager::CreateMeshPassProcessor(EShadingPath::Mobile, EMeshPass::BasePass, Scene->GetFeatureLevel(), Scene, &View, nullptr);
和Engine/Source/Runtime/Renderer/Private/SceneRendering.cpp:4196 SetupMeshPass
FMeshPassProcessor* MeshPassProcessor = FPassProcessorManager::CreateMeshPassProcessor(ShadingPath, PassType, Scene->GetFeatureLevel(), Scene, &View, nullptr);
的关系？SceneRendering与移动端创建MeshPassProcessor有关系吗？
移动端是如何创建和使用DepthPass的，自顶向下梳理调用链路，最终整理一份Mermaid,附到
这个文档后面，将这个问题以引用的格式附在文档后面

Docs\DepthPass_Analysis_CX_GPT_7_1.md 这个文档是我整理的关于移动Forward渲染关于如何使用DepthPass，Docs\Plan1.md是我的计划，根据这两份文档梳理如何利用引擎代码完成我自己的Depth渲染