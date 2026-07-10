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

Plan1.md:
可以将我创建的Pass放入查找表中：像Engine/Source/Runtime/Renderer/Private/MobileBasePass.cpp:1218
REGISTER_MESHPASSPROCESSOR_AND_PSOCOLLECTOR(MobileBasePass, 			CreateMobileBasePassProcessor, 			EShadingPath::Mobile, EMeshPass::BasePass, 		EMeshPassFlags::CachedMeshCommands | EMeshPassFlags::MainView);
REGISTER_MESHPASSPROCESSOR_AND_PSOCOLLECTOR(MobileBasePassCSM,			CreateMobileBasePassCSMProcessor,		EShadingPath::Mobile, EMeshPass::MobileBasePassCSM, 	EMeshPassFlags::CachedMeshCommands | EMeshPassFlags::MainView);
REGISTER_MESHPASSPROCESSOR_AND_PSOCOLLECTOR(MobileTranslucencyAllPass,		CreateMobileTranslucencyAllPassProcessor,	EShadingPath::Mobile, EMeshPass::TranslucencyAll, 	EMeshPassFlags::MainView);
REGISTER_MESHPASSPROCESSOR_AND_PSOCOLLECTOR(MobileTranslucencyStandardPass,	CreateMobileTranslucencyStandardPassProcessor,	EShadingPath::Mobile, EMeshPass::TranslucencyStandard, 	EMeshPassFlags::MainView);
REGISTER_MESHPASSPROCESSOR_AND_PSOCOLLECTOR(MobileTranslucencyAfterDOFPass,	CreateMobileTranslucencyAfterDOFProcessor,	EShadingPath::Mobile, EMeshPass::TranslucencyAfterDOF, 	EMeshPassFlags::MainView);
这些代码的实现

在DepthRendering.cpp中的PassMeshProcessor中AddMeshBatch相关逻辑中过滤我标记的Mesh


利用Engine/Source/Runtime/Renderer/Private/DepthRendering.cpp:666中RenderPrePass，或创建我自己的CustomDepthPass,将RenderPrePass或我自己创建的CustomDepthPass放入
RenderForwardSinglePass和RenderForwardMultiPass的
RenderMobileBasePass（MobileShadingRenderer.cpp:1609行）之后进行渲染逻辑

结合我的计划中进行分析如何完成这个DepthPass
相关修改我未考虑完全的请继续补充