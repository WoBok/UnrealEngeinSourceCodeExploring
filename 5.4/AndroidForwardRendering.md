# UE5 Android Forward 前向渲染完整逻辑分析

> 目标平台:Android / UE 5.4.4
> 引擎源码路径:`Engine/Source/Runtime/`
> 分析范围:从 Android 进程启动入口到最终 Present 到 Surface 的完整渲染流程

---

## 0. 关键架构发现(必读)

> **UE5 移动端重大变化**: 移动端 **没有** `FForwardShadingSceneRenderer` 类(那是 PC 高配 Forward 使用的)。
> 移动端 **Forward 和 Deferred 都统一由 `FMobileSceneRenderer` 处理**,通过构造函数中的 `bDeferredShading` 标志位决定走哪条路径。
> 这是理解 UE5 移动渲染流程的关键。

---

## 1. Forward vs Deferred 决策配置

### 1.1 CVar 定义(决定项)

`Source/Runtime/Core/Private/HAL/ConsoleManager.cpp:3475-3480`
```cpp
static TAutoConsoleVariable<int32> CVarMobileShadingPath(
    TEXT("r.Mobile.ShadingPath"),
    0,    // 0 = Forward (默认)
    TEXT("0: Forward shading (default)\n"
         "1: Deferred shading (Mobile HDR is required for Deferred)"),
    ECVF_RenderThreadSafe | ECVF_ReadOnly);
```

OpenGL 的独立开关 `ConsoleManager.cpp:3482-3487`:
```cpp
static TAutoConsoleVariable<int32> CVarMobileAllowDeferredShadingOpenGL(
    TEXT("r.Mobile.AllowDeferredShadingOpenGL"), 0, ...);  // OpenGL 默认不允许 Deferred
```

### 1.2 项目设置枚举

`Source/Runtime/Engine/Classes/Engine/RendererSettings.h:217-226`
```cpp
namespace EMobileShadingPath
{
    enum Type : int
    {
        Forward = 0  UMETA(DisplayName = "Forward Shading"),  // 默认
        Deferred = 1 UMETA(DisplayName = "Deferred Shading"),
    };
}
```

`Source/Runtime/Engine/Classes/Engine/RendererSettings.h:302-306`
```cpp
UPROPERTY(config, EditAnywhere, Category = Mobile, meta = (
    ConsoleVariable = "r.Mobile.ShadingPath", DisplayName = "Mobile Shading",
    ToolTip = "The shading path to use on mobile platforms. Changing this setting requires restarting the editor. Mobile HDR is required for Deferred Shading.",
    ConfigRestartRequired = true))
TEnumAsByte<EMobileShadingPath::Type> MobileShadingPath;
```

### 1.3 ShadingPath 运行时查询

`Source/Runtime/Engine/Public/SceneUtils.h:27-37`
```cpp
enum class EShadingPath { Mobile, Deferred, Num };

inline EShadingPath GetFeatureLevelShadingPath(FStaticFeatureLevel InFeatureLevel)
{
    return (InFeatureLevel >= ERHIFeatureLevel::SM5) 
        ? EShadingPath::Deferred : EShadingPath::Mobile;
}
```

`Source/Runtime/RenderCore/Public/RenderUtils.h:284-292`
```cpp
inline bool IsMobileDeferredShadingEnabled(const FStaticShaderPlatform Platform) {
    return FReadOnlyCVARCache::MobileDeferredShading(Platform) && IsMobileHDR();
}
inline bool MobileForwardEnableLocalLights(const FStaticShaderPlatform Platform) {
    return FReadOnlyCVARCache::MobileForwardLocalLights(Platform) > 0;
}
```

`Source/Runtime/RenderCore/Private/ReadOnlyCVARCache.cpp:40-52`
```cpp
int32 FReadOnlyCVARCache::MobileForwardLocalLightsIniValue(EShaderPlatform Platform) {
    static FShaderPlatformCachedIniValue<int32> CVar(TEXT("r.Mobile.Forward.EnableLocalLights"));
    return CVar.Get(Platform);
}
bool FReadOnlyCVARCache::MobileDeferredShadingIniValue(EShaderPlatform Platform) {
    static FShaderPlatformCachedIniValue<bool> MobileShadingPathIniValue(TEXT("r.Mobile.ShadingPath"));
    return MobileShadingPathIniValue.Get(Platform) && bSupportedPlatform;
}
```

### 1.4 关键 Forward 相关 CVar 汇总

| CVar | 作用 |
|------|------|
| `r.Mobile.ShadingPath` | 选择 Forward/Deferred(0=Forward 默认) |
| `r.Mobile.AllowDeferredShadingOpenGL` | OpenGL 下允许 Deferred(默认禁止) |
| `r.Mobile.Forward.EnableLocalLights` | Forward 启用聚类局部光 |
| `r.Mobile.Forward.EnableClusteredReflections` | Forward 启用聚类反射 |
| `r.Mobile.UseHWsRGBEncoding` / `r.Mobile.HDR` | LDR/HDR 切换 |
| `r.MSAACount` | MSAA 采样数(Forward 独有,Deferred 不支持) |
| `r.Forward.LightGridPixelSize` | 聚类 Tile 像素大小 |
| `r.ForwardShading` | **PC 端**的 Forward 开关,移动端不使用 |

---

## 2. Android 端启动与 RHI 初始化

### 2.1 启动入口链

```
Java GameActivity 启动
    ↓ JNI
[Launch/Private/Android/LaunchAndroid.cpp:990] android_main(android_app* state)
    ↓
[LaunchAndroid.cpp:550] AndroidMain()
    ├─ ANativeActivity_setWindowFormat(RGBA_8888)        [Line 560]
    ├─ 等待 GResumeMainInit (Java onCreate 完成)         [Line 630-640]
    ├─ InitCommandLine()                                  [Line 644]
    ├─ GEngineLoop.PreInit(...)                           [Line 687]
    ├─ InitHMDs()                                         [Line 705]
    └─ GEngineLoop.Init()                                 [Line 716]
          ↓
[FEngineLoop::Init] LaunchEngineLoop.cpp:4804
    ├─ GEngine = NewObject<UGameEngine>
    ├─ GEngine->ParseCommandline()
    └─ RHIInit(bHasEditorToken)                          [Line 3249]
              ↓
[RHI/Private/DynamicRHI.cpp:269] RHIInit()
    └─ PlatformCreateDynamicRHI()
              ↓
[RHI/Private/Android/AndroidDynamicRHI.cpp:8] PlatformCreateDynamicRHI()
    ├─ ShouldUseVulkan()? → LoadModuleChecked("VulkanRHI")
    │      └─ CreateRHI(ES3_1)  ← 移动端典型 FeatureLevel
    └─ else → LoadModuleChecked("OpenGLDrv")
           └─ CreateRHI(ES3_1)
```

> **关键**: 移动端 Forward 通常使用 `ERHIFeatureLevel::ES3_1`,`GetFeatureLevelShadingPath(ES3_1)` 返回 `EShadingPath::Mobile`。

### 2.2 AndroidMain 详细实现

`Source/Runtime/Launch/Private/Android/LaunchAndroid.cpp:550-720`
```cpp
int32 AndroidMain(struct android_app* state)
{
    // 1. 设置 ANativeActivity 窗口格式 8888
    ANativeActivity_setWindowFormat(state->activity, WINDOW_FORMAT_RGBA_8888);
    
    // 2. 等待 JNI 侧 onCreate 完成 (GResumeMainInit)
    while (!GResumeMainInit) { ... }
    
    // 3. 初始化命令行
    InitCommandLine();
    GEventHandlerInitialized = true;
    
    // 4. 初始化文件系统
    IPlatformFile::GetPlatformPhysical().Initialize(...);
    
    // 5. PreInit 引擎循环
    int32 PreInitResult = GEngineLoop.PreInit(0, NULL, FCommandLine::Get());
    
    // 6. HMDs 初始化
    InitHMDs();
    
    // 7. 引擎主 Init
    int32 ErrorLevel = GEngineLoop.Init();
    
    // 8. Tick 循环 (在 LaunchEngineLoop 中)
}
```

### 2.3 FEngineLoop::Init 路径

`Source/Runtime/Launch/Private/LaunchEngineLoop.cpp:4804-5016`
```cpp
int32 FEngineLoop::Init() {
    // 1. 创建 GEngine = NewObject<UGameEngine>(...)
    GEngine = NewObject<UEngine>(GetTransientPackage(), EngineClass);
    // 2. 解析命令行
    GEngine->ParseCommandline();
    // ...
}
```

`Source/Runtime/Launch/Private/LaunchEngineLoop.cpp:3246-3250` (RHIInit 阶段)
```cpp
{
    SCOPED_BOOT_TIMING("RHIInit");
    RHIInit(bHasEditorToken);  // 触发 GDynamicRHI 创建
}
```

### 2.4 RHIInit & Dynamic RHI 选择

`Source/Runtime/RHI/Private/DynamicRHI.cpp:269-318`
```cpp
void RHIInit(bool bHasEditorToken) {
    // ...
    GDynamicRHI = PlatformCreateDynamicRHI();  // 平台相关
    if (GDynamicRHI) {
        GDynamicRHI->Init();
        // ...
    }
}
```

`Source/Runtime/RHI/Private/Android/AndroidDynamicRHI.cpp:8-56`
```cpp
FDynamicRHI* PlatformCreateDynamicRHI() {
    if (FPlatformMisc::ShouldUseVulkan() || FPlatformMisc::ShouldUseDesktopVulkan()) {
        // Vulkan 优先,如果 Vulkan 不支持则降级到 OpenGL
        FAndroidAppEntry::ReleaseEGL();  // 销毁 FAndroidAppEntry::PlatformInit 创建的 EGL
        DynamicRHIModule = &FModuleManager::LoadModuleChecked<IDynamicRHIModule>(TEXT("VulkanRHI"));
        if (!DynamicRHIModule->IsSupported()) {
            DynamicRHIModule = &FModuleManager::LoadModuleChecked<IDynamicRHIModule>(TEXT("OpenGLDrv"));
            GraphicsRHI = TEXT("OpenGL");
        } else {
            // Feature level 关键: Vulkan SM5 或 ES3.1
            RequestedFeatureLevel = FPlatformMisc::ShouldUseDesktopVulkan() 
                ? ERHIFeatureLevel::SM5 : ERHIFeatureLevel::ES3_1;
            GraphicsRHI = TEXT("Vulkan");
        }
    } else {
        DynamicRHIModule = &FModuleManager::LoadModuleChecked<IDynamicRHIModule>(TEXT("OpenGLDrv"));
        GraphicsRHI = TEXT("OpenGL");
    }
    // ...
    DynamicRHI = DynamicRHIModule->CreateRHI(RequestedFeatureLevel);
}
```

### 2.5 EGL / NativeWindow 初始化

`Source/Runtime/OpenGLDrv/Private/Android/AndroidOpenGL.cpp:1127-1141`
```cpp
void FAndroidAppEntry::PlatformInit() {
    // 早期创建一个 ES3.2 EGL 用于 GPU 查询,避免重建
    AndroidEGL::GetInstance()->Init(AndroidEGL::AV_OpenGLES, 3, 2);
}
void FAndroidAppEntry::ReleaseEGL() {
    // 销毁该 EGL,让 Vulkan 接管
    EGL->DestroyBackBuffer();
    EGL->Terminate();
}
```

`Source/Runtime/OpenGLDrv/Private/Android/AndroidEGL.cpp:277-340` (`CreateEGLRenderSurface`)
```cpp
void AndroidEGL::CreateEGLRenderSurface(ANativeWindow* InWindow, bool bCreateWndSurface) {
    if (bCreateWndSurface) {
        // 关键:用 ANativeWindow 创建 EGL surface(在 Vulkan 中是 VkSurface)
        PImplData->eglSurface = eglCreateWindowSurface(
            PImplData->eglDisplay, PImplData->eglConfigParam, InWindow, NULL);
        // ...
        eglQuerySurface(... EGL_WIDTH, EGL_HEIGHT, ...);  // 获取 back buffer 尺寸
    }
}
```

`Source/Runtime/Launch/Private/Android/AndroidEventManager.cpp:243-264` (NativeWindow 传递)
```cpp
void FAppEventManager::HandleWindowCreated_EventThread(void* InWindow) {
    FAndroidWindow::AcquireWindowRef((ANativeWindow*)InWindow);
    FAndroidWindow::SetHardwareWindow_EventThread(InWindow);
    EnqueueAppEvent(APP_EVENT_STATE_WINDOW_CREATED, FAppEventData((ANativeWindow*)InWindow));
}
```

---

## 3. 渲染线程启动(Render Thread Startup)

### 3.1 StartRenderingThread

`Source/Runtime/Launch/Private/LaunchEngineLoop.cpp:3409-3411`
```cpp
SCOPED_BOOT_TIMING("StartRenderingThread");
StartRenderingThread();
```

`Source/Runtime/RenderCore/Private/RenderingThread.cpp:750-829`
```cpp
void StartRenderingThread() {
    // 1. 暂停纹理流送
    SuspendTextureStreamingRenderTasks();
    FlushRenderingCommands();
    
    // 2. (可选)启动 RHI Thread
    if (GUseRHIThread_InternalUseOnly) {
        FRHIThread::Get().Start();
        GIsRunningRHIInDedicatedThread_InternalUseOnly = true;
    }
    
    // 3. 创建渲染线程
    GIsThreadedRendering = true;
    GRenderingThreadRunnable = new FRenderingThread();
    GRenderingThread = FRunnableThread::Create(
        GRenderingThreadRunnable, 
        *BuildRenderingThreadName(ThreadCount), 0,
        FPlatformAffinity::GetRenderingThreadPriority(),
        FPlatformAffinity::GetRenderingThreadMask(),
        FPlatformAffinity::GetRenderingThreadFlags());
    
    // 4. 等待渲染线程绑定 TaskGraph
    GRenderingThreadRunnable->TaskGraphBoundSyncEvent->Wait();
}
```

### 3.2 RenderingThreadMain

`Source/Runtime/RenderCore/Private/RenderingThread.cpp:354-429`
```cpp
void RenderingThreadMain(FEvent* TaskGraphBoundSyncEvent) {
    ENamedThreads::Type RenderThread = ENamedThreads::Type(ENamedThreads::ActualRenderingThread);
    ENamedThreads::SetRenderThread(RenderThread);
    FTaskGraphInterface::Get().AttachToThread(RenderThread);
    
    // 通知主线程渲染线程已就绪
    TaskGraphBoundSyncEvent->Trigger();
    
    FPlatformProcess::SetRealTimeMode();
    FCoreDelegates::PostRenderingThreadCreated.Broadcast();
    
    {
        FTaskTagScope TaskTagScope(ETaskTag::ERenderingThread);
        FScopedRHIThreadOwnership ThreadOwnershipScope;  // 获取 RHI 线程所有权(若不独立线程)
        FTaskGraphInterface::Get().ProcessThreadUntilRequestReturn(RenderThread);
        // 上面这行会处理所有 ENQUEUE_RENDER_COMMAND 投递的 lambda
    }
    
    FCoreDelegates::PreRenderingThreadDestroyed.Broadcast();
}
```

### 3.3 ENQUEUE_RENDER_COMMAND 宏

`Source/Runtime/RenderCore/Public/RenderingThread.h` (宏定义)
```cpp
#define ENQUEUE_RENDER_COMMAND(Type) \
    EnqueueUniqueRenderCommand_RHIRENDERTHREAD<Type>(...)
```

将 lambda 插入渲染线程的任务队列,RT 主循环会消费它们。

---

## 4. FSceneRenderer 创建与调度(核心分发)

### 4.1 BeginRenderingViewFamily 入口(Game Thread)

`Source/Runtime/Renderer/Private/SceneRendering.cpp:4965-5130`
```cpp
void FRendererModule::BeginRenderingViewFamily(FCanvas* Canvas, FSceneViewFamily* ViewFamily) {
    BeginRenderingViewFamilies(Canvas, TArrayView<FSceneViewFamily*>(&ViewFamily, 1));
}

void FRendererModule::BeginRenderingViewFamilies(FCanvas* Canvas, TArrayView<FSceneViewFamily*> ViewFamilies) {
    // ...
    FSceneRenderer::CreateSceneRenderers(ViewFamiliesConst, Canvas->GetHitProxyConsumer(), SceneRenderers);
    
    // 关键:ENQUEUE_RENDER_COMMAND 把整个绘制过程投递到渲染线程
    ENQUEUE_RENDER_COMMAND(FDrawSceneCommand)(
        [LocalSceneRenderers = CopyTemp(SceneRenderers), DrawSceneEnqueue]
        (FRHICommandListImmediate& RHICmdList) {
            RenderViewFamilies_RenderThread(RHICmdList, LocalSceneRenderers);
        });
}
```

### 4.2 ⭐ CreateSceneRenderers(关键决策点)

`Source/Runtime/Renderer/Private/SceneRendering.cpp:4284-4310` ⭐
```cpp
void FSceneRenderer::CreateSceneRenderers(
    const TArray<const FSceneViewFamily*>& InViewFamilies,
    FHitProxyConsumer* HitProxyConsumer,
    TArray<FSceneRenderer*>& OutSceneRenderers) 
{
    const FSceneInterface* Scene = InViewFamilies[0]->Scene;
    check(Scene);

    // 决策点:根据 FeatureLevel 决定 ShadingPath
    EShadingPath ShadingPath = GetFeatureLevelShadingPath(Scene->GetFeatureLevel());
    
    for (int32 FamilyIndex = 0; FamilyIndex < InViewFamilies.Num(); FamilyIndex++) {
        const FSceneViewFamily* InViewFamily = InViewFamilies[FamilyIndex];
        
        if (ShadingPath == EShadingPath::Deferred) {
            // 走 PC 的 DeferredShadingRenderer
            FDeferredShadingSceneRenderer* SceneRenderer = 
                new FDeferredShadingSceneRenderer(InViewFamily, HitProxyConsumer);
            OutSceneRenderers.Add(SceneRenderer);
        } else {
            // 走 Mobile (可能是 Forward 或 Mobile Deferred)
            check(ShadingPath == EShadingPath::Mobile);
            OutSceneRenderers.Add(new FMobileSceneRenderer(InViewFamily, HitProxyConsumer));
        }
        // ...
    }
}
```

> **关键决策**: Android 平台若用 ES3_1 FeatureLevel,一定走 `FMobileSceneRenderer`。**Mobile Forward 渲染没有独立的 `FForwardShadingSceneRenderer` 类**(那是 PC 的)。

### 4.3 RenderViewFamilies_RenderThread(RT)

`Source/Runtime/Renderer/Private/SceneRendering.cpp:4743-4830`
```cpp
static void RenderViewFamilies_RenderThread(FRHICommandListImmediate& RHICmdList, 
    const TArray<FSceneRenderer*>& SceneRenderers) {
    // 1. 更新 Lumen 场景等
    // 2. 对每个 SceneRenderer 调用 Render()
    for (FSceneRenderer* SceneRenderer : SceneRenderers) {
        if (ViewFamily.bIsSceneTextureInitialized) { ... }
        // ...
    }
    // 3. FRDGBuilder 触发 GPU 工作
}
```

---

## 5. FMobileSceneRenderer(Forward 入口)

### 5.1 构造(决定 bDeferredShading)

`Source/Runtime/Renderer/Private/MobileShadingRenderer.cpp:287-310` ⭐
```cpp
FMobileSceneRenderer::FMobileSceneRenderer(const FSceneViewFamily* InViewFamily, 
                                            FHitProxyConsumer* HitProxyConsumer)
    : FSceneRenderer(InViewFamily, HitProxyConsumer)
    , bGammaSpace(!IsMobileHDR())
    , bDeferredShading(IsMobileDeferredShadingEnabled(ShaderPlatform))  // ⭐ 关键字段
    , bRequiresDBufferDecals(bDeferredShading ? false : IsUsingDBuffers(ShaderPlatform))
    , bUseVirtualTexturing(...)
{
    // ...
    bRenderToSceneColor = false;
    bRequiresMultiPass = false;
    bEnableClusteredLocalLights = MobileForwardEnableLocalLights(ShaderPlatform);  // Forward 特性
    bEnableClusteredReflections = MobileForwardEnableClusteredReflections(ShaderPlatform);
    // ...
}
```

**`bDeferredShading` 决定了后续调用 `RenderForward()` 还是 `RenderDeferred()`。**

### 5.2 ⭐ Render 主调度

`Source/Runtime/Renderer/Private/MobileShadingRenderer.cpp:910-1431`
```cpp
void FMobileSceneRenderer::Render(FRDGBuilder& GraphBuilder) {
    // 1. 系统纹理初始化
    GSystemTextures.InitializeTextures(...);
    
    // 2. 可见性计算 (Instance Culling)
    FInstanceCullingManager& InstanceCullingManager = ...;
    InitViews(GraphBuilder, SceneTexturesConfig, InstanceCullingManager, ...);
    
    // 3. 光源排序 / 聚类(若启用)
    if (bDeferredShading || bEnableClusteredLocalLights || bEnableClusteredReflections) {
        GatherAndSortLights(SortedLightSet, ...);
        if (bCullLightsToGrid) {
            ComputeLightGrid(GraphBuilder, bEnableClusteredLocalLights, SortedLightSet);
        }
    } else {
        SetDummyForwardLightUniformBufferOnViews(...);  // 非聚类 Forward
    }
    
    // 4. 阴影深度图
    RenderShadowDepthMaps(GraphBuilder, ...);
    
    // 5. ⭐ 关键分发:Forward 还是 Deferred?
    //    实际位置在 RenderViewFamilies_RenderThread 内
    if (bDeferredShading) {
        // ...
    } else {
        RenderForward(GraphBuilder, ViewFamilyTexture, SceneTextures, DBufferTextures);
    }
    
    // 6. 后处理
    if (ViewFamily.bResolveScene && bRenderToSceneColor) {
        AddMobilePostProcessingPasses(...);
    }
}
```

### 5.3 ⭐ RenderForward 主体

`Source/Runtime/Renderer/Private/MobileShadingRenderer.cpp:1503-1660` ⭐
```cpp
void FMobileSceneRenderer::RenderForward(FRDGBuilder& GraphBuilder, 
    FRDGTextureRef ViewFamilyTexture, FSceneTextures& SceneTextures, 
    FDBufferTextures& DBufferTextures) 
{
    // 初始化 Render Targets
    FRenderTargetBindingSlots BasePassRenderTargets = 
        InitRenderTargetBindings_Forward(ViewFamilyTexture, SceneTextures);
    
    // 更新方向光 UniformBuffer
    UpdateDirectionalLightUniformBuffers(GraphBuilder, View);
    
    // 设置 MobileBasePass Uniform Buffer
    FMobileBasePassTextures MobileBasePassTextures{};
    PassParameters->MobileBasePass = CreateMobileBasePassUniformBuffer(
        GraphBuilder, View, EMobileBasePass::Opaque, SetupMode, MobileBasePassTextures);
    
    // 分发 Single Pass / Multi Pass
    if (bRequiresMultiPass) {
        RenderForwardMultiPass(GraphBuilder, PassParameters, ViewContext, SceneTextures);
    } else {
        RenderForwardSinglePass(GraphBuilder, PassParameters, ViewContext, SceneTextures);
    }
}
```

### 5.4 ⭐ RenderForwardSinglePass(Subpass 调度)

`Source/Runtime/Renderer/Private/MobileShadingRenderer.cpp:1578-1660` ⭐
```cpp
void FMobileSceneRenderer::RenderForwardSinglePass(FRDGBuilder& GraphBuilder,
    FMobileRenderPassParameters* PassParameters, FRenderViewContext& ViewContext, 
    FSceneTextures& SceneTextures) 
{
    GraphBuilder.AddPass(
        RDG_EVENT_NAME("SceneColorRendering"),
        PassParameters,
        ERDGPassFlags::Raster | ERDGPassFlags::NeverMerge,
        [...](FRHICommandList& RHICmdList) {
            // 1. Depth pre-pass
            RenderMaskedPrePass(RHICmdList, View);
            // 2. ⭐ Base Pass:不透明 + 蒙版
            RenderMobileBasePass(RHICmdList, View, ...);
            // 3. Debug view
            RenderMobileDebugView(RHICmdList, View);
            // 4. Occlusion queries
            RHICmdList.PollOcclusionQueries();
            // 5. Post render base pass
            PostRenderBasePass(RHICmdList, View);
            
            // 6. Subpass 2: Translucency
            RHICmdList.NextSubpass();
            RenderDecals(RHICmdList, View, ...);
            RenderModulatedShadowProjections(RHICmdList, ...);
            RenderFog(RHICmdList, View);
            RenderTranslucency(RHICmdList, View);
            // ...
            
            // 7. Subpass 3: 解析(MSAA resolve / tonemap)
            if (bTonemapSubpassInline) {
                RHICmdList.NextSubpass();
                RenderMobileCustomResolve(RHICmdList, View, NumMSAASamples, SceneTextures);
            }
        });
}
```

---

## 6. Forward Base Pass 细节

### 6.1 RenderMobileBasePass

`Source/Runtime/Renderer/Private/MobileBasePassRendering.cpp:470-491`
```cpp
void FMobileSceneRenderer::RenderMobileBasePass(FRHICommandList& RHICmdList, 
    const FViewInfo& View, 
    const FInstanceCullingDrawParams* InstanceCullingDrawParams) {
    CSV_SCOPED_TIMING_STAT_EXCLUSIVE(RenderBasePass);
    SCOPED_DRAW_EVENT(RHICmdList, MobileBasePass);
    
    RHICmdList.SetViewport(View.ViewRect.Min.X, View.ViewRect.Min.Y, 0, 
                            View.ViewRect.Max.X, View.ViewRect.Max.Y, 1);
    
    // 关键:分发到 EMeshPass::BasePass 的 mesh draw commands
    View.ParallelMeshDrawCommandPasses[EMeshPass::BasePass]
        .DispatchDraw(nullptr, RHICmdList, InstanceCullingDrawParams);
    
    // SkyPass
    if (View.Family->EngineShowFlags.Atmosphere) {
        View.ParallelMeshDrawCommandPasses[EMeshPass::SkyPass]
            .DispatchDraw(...);
    }
    // ...
}
```

### 6.2 ⭐ SetupMobileBasePassUniformParameters(Forward 关键数据)

`Source/Runtime/Renderer/Private/MobileBasePassRendering.cpp:248-340` ⭐
```cpp
void SetupMobileBasePassUniformParameters(FRDGBuilder& GraphBuilder,
    const FViewInfo& View, EMobileBasePass BasePass, ...) 
{
    SetupFogUniformParameters(GraphBuilder, View, ...);
    
    // ⭐ Forward 光照数据 (FForwardLightData)
    if (View.ForwardLightingResources.ForwardLightData) {
        BasePassParameters.Forward = *View.ForwardLightingResources.ForwardLightData;
    } else {
        SetupDummyForwardLightUniformParameters(...);
    }
    
    // Forward 多视图(VR)
    if (View.bIsMobileMultiViewEnabled && InstancedForwardLightData) {
        BasePassParameters.ForwardMMV = *InstancedForwardLightData;
    }
    
    // IBL/Reflection capture
    const FPlanarReflectionSceneProxy* ReflectionSceneProxy = 
        Scene ? Scene->GetForwardPassGlobalPlanarReflection() : nullptr;
    SetupPlanarReflectionUniformParameters(View, ReflectionSceneProxy, ...);
    
    // Sky light 处理
    SetupSkyIblUniformParameters(...);
    // ...
}
```

### 6.3 着色器:FORWARD_SHADING 宏

`Shaders/Private/MobileBasePassPixelShader.usf:86-89`
```hlsl
// 在禁用 MOBILE_DEFERRED_SHADING 时强制启用 FORWARD_SHADING
#ifndef FORWARD_SHADING
#define FORWARD_SHADING (!MOBILE_DEFERRED_SHADING)
#endif
```

`Shaders/Private/MobileBasePassPixelShader.usf:414-430` (Subsurface / Skin)
```hlsl
#if (MATERIAL_SHADINGMODEL_SUBSURFACE || MATERIAL_SHADINGMODEL_PREINTEGRATED_SKIN || 
     MATERIAL_SHADINGMODEL_SUBSURFACE_PROFILE || MATERIAL_SHADINGMODEL_TWOSIDED_FOLIAGE || 
     MATERIAL_SHADINGMODEL_CLOTH || MATERIAL_SHADINGMODEL_EYE) 
    if (ShadingModelID == SHADINGMODELID_SUBSURFACE || ...) {
        half4 SubsurfaceData = GetMaterialSubsurfaceData(PixelMaterialInputs);
        if (ShadingModelID == SHADINGMODELID_SUBSURFACE || ...) {
            SubsurfaceColor = SubsurfaceData.rgb * ResolvedView.DiffuseOverrideParameter.w 
                + ResolvedView.DiffuseOverrideParameter.xyz;
        }
        SubsurfaceProfile = SubsurfaceData.a;
    }
#endif
```

### 6.4 InitRenderTargetBindings_Forward

`Source/Runtime/Renderer/Private/MobileBasePassRendering.cpp:1448-1501`
```cpp
FRenderTargetBindingSlots FMobileSceneRenderer::InitRenderTargetBindings_Forward(
    FRDGTextureRef ViewFamilyTexture, FSceneTextures& SceneTextures) {
    FRenderTargetBindingSlots BasePassRenderTargets;
    
    bool bMobileMSAA = NumMSAASamples > 1;
    if (!bRenderToSceneColor) {
        if (bMobileMSAA) {
            SceneColor = SceneTextures.Color.Target;
            SceneColorResolve = ViewFamilyTexture;  // ⭐ MSAA 解析到 back buffer
        } else {
            SceneColor = ViewFamilyTexture;  // ⭐ 直接渲染到 back buffer
        }
    }
    
    BasePassRenderTargets[0] = FRenderTargetBinding(SceneColor, SceneColorResolve, 
        ERenderTargetLoadAction::EClear);
    // 深度/模板:有 Z-prepass 时为 DepthRead_StencilWrite,否则 DepthWrite_StencilWrite
    BasePassRenderTargets.DepthStencil = bIsFullDepthPrepassEnabled ? 
        FDepthStencilBinding(SceneDepth, ELoad, ELoad, FExclusiveDepthStencil::DepthRead_StencilWrite) : 
        FDepthStencilBinding(SceneDepth, EClear, EClear, FExclusiveDepthStencil::DepthWrite_StencilWrite);
    return BasePassRenderTargets;
}
```

> **关键差异 (Forward vs Deferred)**:
> - Forward 可以直接渲染到 back buffer(无 GBuffer)
> - Forward 支持 MSAA(`r.MSAACount`)
> - Forward 用 subpass 内联做 translucency(避免后处理)
> - Forward 用 subpass 做 MSAA 解析

---

## 7. Mobile Forward 光照计算

### 7.1 聚类(Clustered) vs 非聚类

`Source/Runtime/Renderer/Private/MobileShadingRenderer.cpp:1079-1099`
```cpp
if (bDeferredShading || bEnableClusteredLocalLights || bEnableClusteredReflections) {
    // 启用聚类:Grid-based 光源剔除
    GatherAndSortLights(SortedLightSet, bShadowedLightsInClustered);
    int32 NumReflectionCaptures = ...;
    if (bCullLightsToGrid) {
        ComputeLightGrid(GraphBuilder, bEnableClusteredLocalLights, SortedLightSet);
    }
} else {
    // 简单 Forward:所有局部光直接传入 UB
    SetDummyForwardLightUniformBufferOnViews(GraphBuilder, ShaderPlatform, Views);
}
```

### 7.2 ⭐ RenderMobileLocalLightsBuffer(聚类 Forward 局部光)

`Source/Runtime/Renderer/Private/MobileLocalLightsBuffer.cpp:202-360` ⭐
```cpp
void FMobileSceneRenderer::RenderMobileLocalLightsBuffer(FRDGBuilder& GraphBuilder, 
    FSceneTextures& SceneTextures, const FSortedLightSetSceneInfo& SortedLights) 
{
    if (!CompileShaderPermutationsForMobileLocalLightsBuffer(ShaderPlatform) || 
        IsMobileDeferredShadingEnabled(ShaderPlatform)) {
        return;  // 只用于 Forward 聚类模式
    }
    
    // 1. 计算 tile 大小
    static const auto LightGridPixelSizeCVar = ...;  // r.Forward.LightGridPixelSize
    const FIntPoint GroupSize(...);
    
    // 2. Compute Pass: 计算每个 tile 的光源信息
    auto* PassParameters = GraphBuilder.AllocParameters<FLocalLightBufferCS::FParameters>();
    PassParameters->RWTileInfo = TileInfoBufferUAV;
    FComputeShaderUtils::AddPass(GraphBuilder, "RenderMobileLocalLights_TiledInfoCS", 
        ERDGPassFlags::Compute, ComputeShader, PassParameters, ...);
    
    // 3. 渲染到 MobileLocalLightTextureA/B
    FLocalLightBufferPrepassParameters* PassParameters = ...;
    PassParameters->RenderTargets[0] = FRenderTargetBinding(
        SceneTextures.MobileLocalLightTextureA, EClear);
    PassParameters->RenderTargets[1] = FRenderTargetBinding(
        SceneTextures.MobileLocalLightTextureB, EClear);
    // ...
}
```

### 7.3 着色器端直接光照计算

`Shaders/Private/MobileBasePassPixelShader.usf` 中,`#define ForwardLightData MobileBasePass.Forward` 后,`FForwardLightData` 包含:
- `NumLocalLights` — 局部光数量
- `NumReflectionCaptures` — 反射捕获数量
- `HasDirectionalLight` — 方向光标志
- `ForwardDirectLighting` — 预计算光
- `TransmissionShadowMapTexture` — 次表面光影
- 等等

GPU 端计算逻辑(简化):
```hlsl
// Direct lighting
FGBufferData GBuffer = ...; // 从材质输入
FDirectLighting Lighting = AccumulateDirectLighting(
    GBuffer, View, GetSceneData().SceneData, ForwardLightData, ...);
// AccumulateDirectLighting 会遍历所有 local lights,应用 shadow / IBL
// 最终颜色 = Direct + Indirect (来自 IBL/Sky)
half3 OutColor = Lighting.Diffuse + Lighting.Specular + GBuffer.Emissive 
               + GetIBLContribution(GBuffer, ...);
```

### 7.4 间接光照(IBL)

`Source/Runtime/Renderer/Private/MobileBasePassRendering.cpp:443-467` (SkyIBL 设置)
```cpp
// Forward 使用 Reflection Capture 或 SkyLight 做 IBL
// SkyLight:
CaptureTexture = SkyLight->ProcessedTexture;
SkyMaxMipIndex = FMath::Log2(static_cast<float>(CaptureTexture->GetSizeX()));
Parameters.Params = FVector4f(Brightness, SkyMaxMipIndex, 
    bSkyLightIsDynamic ? 1.0f : 0.0f, BlendFraction);
Parameters.Texture = CaptureTexture->TextureRHI;
// ...
```

> **关键差异 (Forward vs Deferred)**:
> - Forward: **每个像素** 都累加所有局部光(若启用聚类则用 Light Grid 剔除)
> - Deferred: G-Buffer 写完后,**屏幕空间**采样 G-Buffer 计算光照
> - Forward 支持简单透射阴影(`TransmissionShadowMapTexture`)
> - Deferred 支持 IES profiles / light functions / lit deferred decals(这些 Forward 不支持)

---

## 8. 输出到 SwapChain / Present 到 Android Surface

### 8.1 渲染到 ViewFamilyTexture

`Source/Runtime/Renderer/Private/MobileShadingRenderer.cpp:1189-1191`
```cpp
// ViewFamilyTexture 即为 back buffer / FBO (vulkan 中是 SwapChain image)
FRDGTextureRef ViewFamilyTexture = TryCreateViewFamilyTexture(GraphBuilder, ViewFamily);
```

Base Pass 直接渲染到该纹理(当 `!bRenderToSceneColor`),或者解析到该纹理(MSAA 模式)。

### 8.2 SlateRHIRenderer::DrawWindow_RenderThread(Back Buffer Present)

`Source/Runtime/SlateRHIRenderer/Private/SlateRHIRenderer.cpp:1087-1560` ⭐
```cpp
const FRHIGPUMask PresentingGPUMask = FRHIGPUMask::FromIndex(
    RHIGetViewportNextPresentGPUIndex(ViewportInfo.ViewportRHI));
SCOPED_GPU_MASK(RHICmdList, PresentingGPUMask);
// ... Slate UI 绘制到 BackBuffer ...

// 关键的 present 调用
RHICmdList.EndDrawingViewport(ViewportInfo.ViewportRHI, true /*bPresent*/, 
    DrawCommandParams.bLockToVsync);
```

### 8.3 ⭐ EndDrawingViewport(RHI CommandList)

`Source/Runtime/RHI/Private/RHICommandList.cpp:1287-1325` ⭐
```cpp
void FRHICommandListImmediate::EndDrawingViewport(FRHIViewport* Viewport, 
    bool bPresent, bool bLockToVsync) {
    // ...
    if (Bypass()) {
        GetContext().RHIEndDrawingViewport(Viewport, bPresent, bLockToVsync);
    } else {
        ALLOC_COMMAND(FRHICommandEndDrawingViewport)(Viewport, bPresent, bLockToVsync);
        FRHICommandListExecutor::GetImmediateCommandList()
            .ImmediateFlush(EImmediateFlushType::DispatchToRHIThread);
    }
    // 推进 frame,获取下一帧 back buffer
    RHIAdvanceFrameForGetViewportBackBuffer(Viewport);
}
```

### 8.4 OpenGL Android Present(eglSwapBuffers)

`Source/Runtime/OpenGLDrv/Private/Android/AndroidOpenGL.cpp:228-282` ⭐
```cpp
bool PlatformBlitToViewport(FPlatformOpenGLDevice* Device, 
    const FOpenGLViewport& Viewport, uint32 BackbufferSizeX, uint32 BackbufferSizeY, 
    bool bPresent, bool bLockToVsync) 
{
    FPlatformOpenGLContext* const Context = Viewport.GetGLContext();
    
    if (bPresent && AndroidEGL::GetInstance()->IsOfflineSurfaceRequired()) {
        // 离线 surface
    }
    
    if (bPresent && Viewport.GetCustomPresent()) {
        bPresent = Viewport.GetCustomPresent()->Present(SyncInterval);
    }
    
    if (bPresent) {
        AndroidEGL::GetInstance()->UpdateBuffersTransform();
        // ⭐ 真正呈现到 Android surface
        FAndroidPlatformRHIFramePacer::SwapBuffers(bLockToVsync);
    }
    // ...
}
```

`Source/Runtime/OpenGLDrv/Private/Android/AndroidOpenGLFramePacer.cpp:233-490`
```cpp
bool FAndroidOpenGLFramePacer::SwapBuffers(bool bLockToVsync) {
    // 帧率控制逻辑(Android Choreographer / Frame Pacing API)
    // ...
    QUICK_SCOPE_CYCLE_COUNTER(STAT_eglSwapBuffers);
    // ⭐ 真正的 Present
    if (eglSurface == NULL || !eglSwapBuffers(eglDisplay, eglSurface)) {
        // ... 处理失败 ...
    }
}
```

### 8.5 Vulkan Android Present(vkQueuePresentKHR)

`Source/Runtime/VulkanRHI/Private/Android/VulkanAndroidPlatform.cpp:670-682` ⭐
```cpp
VkResult FVulkanAndroidPlatform::Present(VkQueue Queue, VkPresentInfoKHR& PresentInfo) {
#if USE_ANDROID_SWAPPY
    if (FAndroidPlatformRHIFramePacer::CVarUseSwappyForFramePacing.GetValueOnRenderThread() != 0) {
        // Android Frame Pacing (Swappy) 库
        return SwappyVk_queuePresent(Queue, &PresentInfo);
    }
#endif
    return VulkanRHI::vkQueuePresentKHR(Queue, &PresentInfo);
}
```

`Source/Runtime/VulkanRHI/Private/Android/VulkanAndroidPlatform.cpp:684-720` (Swapchain 创建)
```cpp
VkResult FVulkanAndroidPlatform::CreateSwapchainKHR(void* WindowHandle, ...) {
    Result = VulkanRHI::vkCreateSwapchainKHR(Device, CreateInfo, Allocator, Swapchain);
    if (Result == VK_SUCCESS) {
#if USE_ANDROID_SWAPPY
        if (CVarUseSwappyForFramePacing.GetValueOnAnyThread() != 0) {
            JNIEnv* Env = FAndroidApplication::GetJavaEnv();
            WindowHandle = GetHardwareWindowHandle();
            SwappyVk_initAndGetRefreshCycleDuration(Env, 
                FJavaWrapper::GameActivityThis, PhysicalDevice, Device, 
                *Swapchain, &RefreshDuration);
            SwappyVk_setWindow(Device, *Swapchain, (ANativeWindow*)WindowHandle);
            SwappyVk_setAutoSwapInterval(false);
        }
#endif
    }
}
```

### 8.6 ANativeWindow 回到 Java

最终,`ANativeWindow`(Java 层 `SurfaceView/Surface`)接受 EGL/Vulkan 的 present,SurfaceFlinger 合成后显示到屏幕。

---

## 9. Forward vs Deferred 关键差异点(代码层面)

| 维度 | Mobile Forward | Mobile Deferred |
|------|---------------|-----------------|
| **SceneRenderer 类** | `FMobileSceneRenderer` (bDeferredShading=false) | `FMobileSceneRenderer` (bDeferredShading=true) |
| **CVar** | `r.Mobile.ShadingPath=0` (默认) | `r.Mobile.ShadingPath=1` |
| **HDR 必需** | 否(LDR/HDR 皆可) | 是(`IsMobileHDR()`) |
| **MSAA 支持** | 是(`r.MSAACount`) | 否 |
| **G-Buffer** | 无 | 有(`MobileGBufferA/B/C`) |
| **光照计算** | Base Pass 内逐像素累加 | G-Buffer 写完 + 屏幕空间 shading pass |
| **聚类局部光** | 可选(`r.Mobile.Forward.EnableLocalLights`) | 必需 |
| **IES Profiles** | 不支持 | 支持 |
| **Light Functions** | 不支持 | 支持(`RenderLightFunctionAtlas`) |
| **Deferred Decals** | D-Buffer (简单) | Lit Deferred Decals |
| **深度图利用** | 单一 Depth,Subpass 内 DepthRead | G-Buffer 含 Depth |
| **Translucency** | Subpass 内联 | 独立 pass |
| **Custom Resolve** | 支持(bTonemapSubpassInline) | 不需要(已有 HDR) |
| **bIsFullDepthPrepassEnabled** | 取决于 `r.Mobile.EarlyZPass` | 强制 |
| **bRequiresDBufferDecals** | `IsUsingDBuffers(ShaderPlatform)` | 强制 false |
| **Shader Permutation** | `_MobFwd` (可选 `_MobFCR`) | `_MobDSh` / `_MobDShEx` |
| **代码入口** | `RenderForwardSinglePass` | `RenderDeferredSinglePass` |
| **PostProcess** | `AddMobilePostProcessingPasses` | 同样,但 LDR/Mobile 路径 |

### 9.1 主要决策代码片段汇总

- `Source/Runtime/Renderer/Private/SceneRendering.cpp:4287-4305` — SceneRenderer 构造选择
- `Source/Runtime/Renderer/Private/MobileShadingRenderer.cpp:290` — `bDeferredShading` 初始化
- `Source/Runtime/Renderer/Private/MobileShadingRenderer.cpp:1503-1570` — `RenderForward` 主体
- `Source/Runtime/Renderer/Private/MobileBasePassRendering.cpp:470-491` — `RenderMobileBasePass`
- `Source/Runtime/Renderer/Private/MobileShadingRenderer.cpp:1578-1660` — `RenderForwardSinglePass` (Subpass 调度)
- `Source/Runtime/RenderCore/Private/ReadOnlyCVARCache.cpp:40-52` — Forward Local Lights CVar 读取

---

## 10. 完整调用链(Call Chain)

```
[Android 系统]
   Java GameActivity 启动
        ↓ JNI
[Launch/Private/Android]
   android_main(android_app* state)                                    [LaunchAndroid.cpp:990]
        ↓
   AndroidMain(struct android_app*)                                    [LaunchAndroid.cpp:550]
        ├─ ANativeActivity_setWindowFormat(RGBA_8888)                  [LaunchAndroid.cpp:560]
        ├─ 等待 GResumeMainInit (Java onCreate 完成)                   [LaunchAndroid.cpp:630-640]
        ├─ InitCommandLine()                                            [LaunchAndroid.cpp:644]
        ├─ FPlatformMisc::LowLevelOutputDebugString ...
        ├─ GEngineLoop.PreInit(...)                                     [LaunchAndroid.cpp:687]
        ├─ InitHMDs()                                                   [LaunchAndroid.cpp:705]
        └─ GEngineLoop.Init()                                           [LaunchAndroid.cpp:716]
              ↓
[Launch/Private/LaunchEngineLoop.cpp]
   FEngineLoop::Init()                                                  [LaunchEngineLoop.cpp:4804]
        ├─ GEngine = NewObject<UGameEngine>
        ├─ GEngine->ParseCommandline()
        └─ (PreInitPostStartupScreen)

   FEngineLoop::PreInit 后阶段
        ├─ RHIInit(bHasEditorToken)                                     [LaunchEngineLoop.cpp:3249]
        │       ↓
        │   [RHI/Private/DynamicRHI.cpp]
        │   RHIInit()                                                   [DynamicRHI.cpp:269]
        │       └─ PlatformCreateDynamicRHI()                           [DynamicRHI.cpp:290]
        │              ↓
        │           [RHI/Private/Android/AndroidDynamicRHI.cpp]
        │           PlatformCreateDynamicRHI()                          [AndroidDynamicRHI.cpp:8]
        │              ├─ ShouldUseVulkan()? → LoadModule("VulkanRHI")
        │              │      └─ VulkanRHI::CreateRHI(ES3_1 或 SM5)
        │              └─ else → LoadModule("OpenGLDrv")
        │                     └─ OpenGLDrv::CreateRHI(ES3_1)
        │
        ├─ FSlateApplication::Get().InitializeRenderer(SlateRenderer)   [LaunchEngineLoop.cpp:3429]
        ├─ StartRenderingThread()                                        [LaunchEngineLoop.cpp:3410]
        │       ↓
        │   [RenderCore/Private/RenderingThread.cpp]
        │   StartRenderingThread()                                       [RenderingThread.cpp:750]
        │       └─ FRunnableThread::Create(FRenderingThread)            [RenderingThread.cpp:803]
        │              ↓
        │   void RenderingThreadMain(FEvent*)                            [RenderingThread.cpp:354]
        │       └─ FTaskGraphInterface::ProcessThreadUntilRequestReturn [RenderingThread.cpp:413]
        │              ↓ (RT 消费 ENQUEUE_RENDER_COMMAND 投递的任务)
        │
        └─ FEngineLoop::Tick() 循环                                      [LaunchEngineLoop.cpp 后续]

[UGameViewportClient::Draw]
   FRendererModule::BeginRenderingViewFamily(Canvas, ViewFamily)         [SceneRendering.cpp:4965]
        ↓
   FRendererModule::BeginRenderingViewFamilies(...)                       [SceneRendering.cpp:4970]
        ├─ Scene->IncrementFrameNumber()
        ├─ ENQUEUE_RENDER_COMMAND(UpdateFastVRamConfig)
        ├─ FSceneRenderer::CreateSceneRenderers(...)                      [SceneRendering.cpp:4280]
        │       ↓
        │   EShadingPath = GetFeatureLevelShadingPath(Scene->GetFeatureLevel())
        │       ↓ (ES3_1 移动端 → EShadingPath::Mobile)
        │   new FMobileSceneRenderer(ViewFamily, HitProxyConsumer)       [MobileShadingRenderer.cpp:287]
        │       └─ bDeferredShading = IsMobileDeferredShadingEnabled(ShaderPlatform)
        │
        └─ ENQUEUE_RENDER_COMMAND(FDrawSceneCommand)                      [SceneRendering.cpp:5113]
              [Lambda captures SceneRenderers]
              ↓ (RT 拉起)
              RenderViewFamilies_RenderThread(RHICmdList, SceneRenderers)  [SceneRendering.cpp:4743]
                  ↓
                  for SceneRenderer: SceneRenderer->Render(GraphBuilder)
                       ↓
                  FMobileSceneRenderer::Render(GraphBuilder)               [MobileShadingRenderer.cpp:910]
                       ├─ GSystemTextures.InitializeTextures
                       ├─ InitViews(GraphBuilder, ...)                    [MobileShadingRenderer.cpp:433]
                       │     ├─ PreVisibilityFrameSetup
                       │     ├─ PostVisibilityFrameSetup
                       │     ├─ ComputeViewVisibility (FViewInfo 创建)
                       │     ├─ FSceneTextures::InitializeViewFamily
                       │     └─ Shadow / Z-Prepass 决策
                       │
                       ├─ GatherAndSortLights / ComputeLightGrid          [MobileShadingRenderer.cpp:1086-1092]
                       ├─ RenderShadowDepthMaps                            [MobileShadingRenderer.cpp:1154]
                       │
                       ├─ ⭐ 决策点:
                       │   if (bDeferredShading) {
                       │       RenderDeferred(...);  [MobileShadingRenderer.cpp:1885]
                       │   } else {
                       │       RenderForward(...);   [MobileShadingRenderer.cpp:1503]  ← 移动端 Forward 入口
                       │   }
                       │
                       ↓ (Forward 路径)
                       FMobileSceneRenderer::RenderForward(...)             [MobileShadingRenderer.cpp:1503]
                           ├─ InitRenderTargetBindings_Forward             [MobileBasePassRendering.cpp:1448]
                           ├─ UpdateDirectionalLightUniformBuffers
                           ├─ CreateMobileBasePassUniformBuffer
                           └─ (Multi/Single) Pass 选择
                                ↓
                           RenderForwardSinglePass (或 MultiPass)           [MobileShadingRenderer.cpp:1578]
                                ├─ Subpass 0: MobileBasePass
                                │      └─ FMobileSceneRenderer::RenderMobileBasePass  [MobileBasePassRendering.cpp:470]
                                │              └─ DispatchDraw(EMeshPass::BasePass)
                                │                     ↓
                                │              MeshDrawCommands 调用 Pixel Shader:
                                │              MobileBasePassPixelShader.usf
                                │              ├─ Subsurface / Skin / Clearcoat 计算
                                │              ├─ Direct Lighting 累加(方向光 + 局部光)
                                │              ├─ IBL / Reflection Capture 采样
                                │              └─ Output to SceneColor / ViewFamilyTexture
                                │
                                ├─ Subpass 1: Translucency
                                │      ├─ RenderDecals (D-Buffer)
                                │      ├─ RenderModulatedShadowProjections
                                │      ├─ RenderFog
                                │      └─ RenderTranslucency
                                │
                                └─ Subpass 2 (可选): Custom Resolve
                                       └─ RenderMobileCustomResolve (Tonemap + MSAA Resolve)
                           
                       ↓
                       AddMobilePostProcessingPasses                       [MobileShadingRenderer.cpp:1404]
                       (Tonemap, Bloom, FSR/TSR upscale, etc.)
                       
[Slate / SwapChain Present]
   FSlateApplication::DrawWindowAndChildren                              [SlateRHIRenderer.cpp]
        └─ DrawWindow_RenderThread                                       [SlateRHIRenderer.cpp:1560+]
              ├─ Slate UI 渲染到 BackBuffer
              ├─ ⭐ RHICmdList.EndDrawingViewport(Viewport, true, bLockToVsync)
              │       ↓
              │   FRHICommandListImmediate::EndDrawingViewport            [RHICommandList.cpp:1287]
              │       └─ GetContext().RHIEndDrawingViewport(...)
              │              ├─ [Vulkan] vkQueuePresentKHR
              │              │       └─ [Android Vulkan] FVulkanAndroidPlatform::Present
              │              │              └─ SwappyVk_queuePresent (Frame Pacing) [VulkanAndroidPlatform.cpp:675]
              │              │              └─ 或 vkQueuePresentKHR(Queue, &PresentInfo) [VulkanAndroidPlatform.cpp:680]
              │              │
              │              └─ [OpenGL] eglSwapBuffers
              │                      └─ FAndroidPlatformRHIFramePacer::SwapBuffers  [AndroidOpenGLFramePacer.cpp:233]
              │                             └─ eglSwapBuffers(eglDisplay, eglSurface)  [AndroidOpenGLFramePacer.cpp:488]
              │                                    ↓
              │                          Android SurfaceFlinger 合成并显示
              │
              └─ OnBackBufferReadyToPresentDelegate.Broadcast  [SlateRHIRenderer.cpp:1502]
```

---

## 11. 关键文件路径汇总(供深入查阅)

### C++ (UE Source)
- `Engine/Source/Runtime/Launch/Private/Android/LaunchAndroid.cpp` — Android main
- `Engine/Source/Runtime/Launch/Private/Android/AndroidEventManager.cpp` — Window 事件
- `Engine/Source/Runtime/Launch/Private/LaunchEngineLoop.cpp` — Engine init & RHIInit
- `Engine/Source/Runtime/RHI/Private/Android/AndroidDynamicRHI.cpp` — RHI module 选择
- `Engine/Source/Runtime/RHI/Private/DynamicRHI.cpp` — RHIInit
- `Engine/Source/Runtime/OpenGLDrv/Private/Android/AndroidOpenGL.cpp` — OpenGL/EGL setup & Present
- `Engine/Source/Runtime/OpenGLDrv/Private/Android/AndroidEGL.cpp` — EGL surface 创建
- `Engine/Source/Runtime/OpenGLDrv/Private/Android/AndroidOpenGLFramePacer.cpp` — Frame pacing
- `Engine/Source/Runtime/VulkanRHI/Private/Android/VulkanAndroidPlatform.cpp` — Vulkan present & swappy
- `Engine/Source/Runtime/RenderCore/Private/RenderingThread.cpp` — 渲染线程主循环
- `Engine/Source/Runtime/RenderCore/Private/ReadOnlyCVARCache.cpp` — CVar 缓存
- `Engine/Source/Runtime/Engine/Public/SceneUtils.h` — EShadingPath 枚举
- `Engine/Source/Runtime/Engine/Classes/Engine/RendererSettings.h` — MobileShadingPath 设置
- `Engine/Source/Runtime/RenderCore/Public/RenderUtils.h` — IsMobileDeferredShadingEnabled
- `Engine/Source/Runtime/Core/Private/HAL/ConsoleManager.cpp` — CVar 定义
- `Engine/Source/Runtime/Renderer/Private/SceneRendering.cpp` — FSceneRenderer::CreateSceneRenderers
- `Engine/Source/Runtime/Renderer/Private/MobileShadingRenderer.cpp` — FMobileSceneRenderer (核心)
- `Engine/Source/Runtime/Renderer/Private/MobileBasePassRendering.cpp` — Mobile Base Pass
- `Engine/Source/Runtime/Renderer/Private/MobileBasePassRendering.h` — Mobile Base Pass header
- `Engine/Source/Runtime/Renderer/Private/MobileLocalLightsBuffer.cpp` — 聚类局部光
- `Engine/Source/Runtime/Renderer/Private/MobileDeferredShadingPass.cpp` — Deferred 对比
- `Engine/Source/Runtime/Renderer/Private/Renderer.cpp` — FRendererModule
- `Engine/Source/Runtime/ApplicationCore/Private/Android/AndroidWindow.cpp` — Window setup
- `Engine/Source/Runtime/RHI/Private/RHICommandList.cpp` — EndDrawingViewport
- `Engine/Source/Runtime/SlateRHIRenderer/Private/SlateRHIRenderer.cpp` — Slate present

### Shaders
- `Engine/Shaders/Private/MobileBasePassPixelShader.usf` — Forward pixel shader
- `Engine/Shaders/Private/MobileBasePassCommon.ush` — Base pass 通用代码
- `Engine/Shaders/Private/MobileBasePassVertexShader.usf` — Vertex shader

---

## 12. 关键发现 / 关键决策点(Summary)

1. **架构变化 (UE5 关键)**: 移动端**没有** `FForwardShadingSceneRenderer`(那是 PC 的)。`FMobileSceneRenderer` 统一处理 Forward 与 Deferred,`bDeferredShading` 字段决定走哪条路径。

2. **Forward 是移动端默认**: `r.Mobile.ShadingPath=0` (Default), `EMobileShadingPath::Forward = 0`。

3. **核心分发点**:
   - `SceneRendering.cpp:4296-4304` — 决定 SceneRenderer 种类
   - `MobileShadingRenderer.cpp:290` — 决定 `bDeferredShading` 标志

4. **Forward 入口**: `FMobileSceneRenderer::RenderForward` (`MobileShadingRenderer.cpp:1503`)→ `RenderForwardSinglePass` (line 1578)→ `RenderMobileBasePass` (`MobileBasePassRendering.cpp:470`)。

5. **Forward Base Pass 特点**: 直接渲染到 `ViewFamilyTexture` (back buffer);支持 MSAA;支持 Subpass 内联(translucency + tonemap)。

6. **Forward 光照特点**:
   - 聚类 (Clustered): `bEnableClusteredLocalLights` / `ComputeLightGrid` → `RenderMobileLocalLightsBuffer`
   - 非聚类:所有光直接放入 `FForwardLightData` uniform buffer
   - 直接光照在 base pass 像素着色器中累加
   - IBL 通过 Reflection Capture 或 SkyLight 实现

7. **Present 路径**:
   - OpenGL: `eglSwapBuffers(eglDisplay, eglSurface)` (EGL surface 由 `ANativeWindow` 创建)
   - Vulkan: `vkQueuePresentKHR` 或 `SwappyVk_queuePresent` (Android Frame Pacing 库)
   - 两者最终都把帧交给 Android SurfaceFlinger 合成并显示

8. **Subpass 设计**(Forward 独有):
   - Subpass 0: MobileBasePass (GBuffer-free)
   - Subpass 1: Translucency / Decals / Fog
   - Subpass 2 (可选): MSAA Resolve + Tonemap
   - 这样避免了多 RenderTarget 切换开销

9. **关键文件位置总结**:
   - Forward 决策 + 入口: `Source/Runtime/Renderer/Private/SceneRendering.cpp:4280-4310`, `Source/Runtime/Renderer/Private/MobileShadingRenderer.cpp:287-310`, `Source/Runtime/Renderer/Private/MobileShadingRenderer.cpp:1503`
   - Base Pass: `Source/Runtime/Renderer/Private/MobileBasePassRendering.cpp:470`
   - Forward 着色器: `Shaders/Private/MobileBasePassPixelShader.usf:86-89` (FORWARD_SHADING 宏)
   - Local Lights: `Source/Runtime/Renderer/Private/MobileLocalLightsBuffer.cpp:202`
   - Present: `Source/Runtime/OpenGLDrv/Private/Android/AndroidOpenGL.cpp:228` / `Source/Runtime/VulkanRHI/Private/Android/VulkanAndroidPlatform.cpp:670`
