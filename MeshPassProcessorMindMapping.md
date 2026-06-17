1. Runtime/Renderer/Private/MobileShadingRenderer.cpp:910Render中:1033调用InitViews(GraphBuilder, SceneTexturesConfig, InstanceCullingManager, VirtualTextureUpdater.Get(), InitViewTaskDatas);
2. Runtime/Renderer/Private/MobileShadingRenderer.cpp:433InitViews中:726调用SetupMobileBasePassAfterShadowInit
3. Runtime/Renderer/Private/MobileShadingRenderer.cpp:377726调用SetupMobileBasePassAfterShadowInit中:388调用FMeshPassProcessor* MeshPassProcessor = FPassProcessorManager::CreateMeshPassProcessor(EShadingPath::Mobile, EMeshPass::BasePass, Scene->GetFeatureLevel(), Scene, &View, nullptr);
4. Runtime/Renderer/Public/MeshPassProcessor.h:2194中进行查询Table
5. Runtime/Renderer/Private/MobileBasePass.cpp:1218中下方代码进行上方Table的注册，整理出他们之间的关系
    REGISTER_MESHPASSPROCESSOR_AND_PSOCOLLECTOR(MobileBasePassCSM,			CreateMobileBasePassCSMProcessor,		EShadingPath::Mobile, EMeshPass::MobileBasePassCSM, 	EMeshPassFlags::CachedMeshCommands | EMeshPassFlags::MainView);
    REGISTER_MESHPASSPROCESSOR_AND_PSOCOLLECTOR(MobileBasePass, 			CreateMobileBasePassProcessor, 			EShadingPath::Mobile, EMeshPass::BasePass, 		EMeshPassFlags::CachedMeshCommands | EMeshPassFlags::MainView);
    REGISTER_MESHPASSPROCESSOR_AND_PSOCOLLECTOR(MobileTranslucencyAllPass,		CreateMobileTranslucencyAllPassProcessor,	EShadingPath::Mobile, EMeshPass::TranslucencyAll, 	EMeshPassFlags::MainView);
    REGISTER_MESHPASSPROCESSOR_AND_PSOCOLLECTOR(MobileTranslucencyStandardPass,	CreateMobileTranslucencyStandardPassProcessor,	EShadingPath::Mobile, EMeshPass::TranslucencyStandard, 	EMeshPassFlags::MainView);
    REGISTER_MESHPASSPROCESSOR_AND_PSOCOLLECTOR(MobileTranslucencyAfterDOFPass,	CreateMobileTranslucencyAfterDOFProcessor,	EShadingPath::Mobile, EMeshPass::TranslucencyAfterDOF, 	EMeshPassFlags::MainView);
6. Runtime/Renderer/Private/MobileShadingRenderer.cpp:726SetupMobileBasePassAfterShadowInit中:410调用Pass.DispatchPassSetup传入MeshPassProcessor
7. Runtime/Renderer/Private/MeshDrawCommands.cpp:1334中DispatchPassSetup对TaskContext进行填充