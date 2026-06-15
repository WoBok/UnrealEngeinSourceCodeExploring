# UE5 Android Forward 渲染调用链(Mermaid)

> 目标平台:Android / UE 5.4.4
> 覆盖范围:从 `UGameViewportClient::Draw` 到 `RenderMobileBasePass` 的完整 Forward 路径
> 图中所有节点均带 `文件:行号` 定位,可点击跳转

---

## 1. 完整调用链(Mermaid)

```mermaid
flowchart TD
    Start([Android 系统<br/>Java GameActivity / JNI]) --> AndroidMain

    subgraph BOOT["① 进程启动与 RHI 初始化 (Game Thread)"]
        AndroidMain["AndroidMain<br/>LaunchAndroid.cpp:550"]
        EngineInit["FEngineLoop::Init<br/>LaunchEngineLoop.cpp:4804"]
        RHIInit["RHIInit<br/>LaunchEngineLoop.cpp:3249"]
        PlatformRHI["PlatformCreateDynamicRHI<br/>AndroidDynamicRHI.cpp:8<br/>→ VulkanRHI / OpenGLDrv"]
        StartRT["StartRenderingThread<br/>LaunchEngineLoop.cpp:3410 →<br/>RenderingThread.cpp:750"]
        RTMain["RenderingThreadMain<br/>RenderingThread.cpp:354"]

        AndroidMain --> EngineInit --> RHIInit --> PlatformRHI
        StartRT --> RTMain
    end

    subgraph GAMETHREAD["② 帧绘制发起 (Game Thread → RT)"]
        Draw["UGameViewportClient::Draw<br/>GameViewportClient.cpp:1369"]
        UMGDraw["UViewport::Draw (UMG)<br/>Viewport.cpp:194"]

        Draw -->|Line 1847| BRVF
        UMGDraw -->|Line 194| BRVF
    end

    subgraph RTDRAW["③ 场景渲染分发 (Render Thread)"]
        BRVF["FRendererModule::BeginRenderingViewFamily<br/>SceneRendering.cpp:4965"]
        BRVFs["FRendererModule::BeginRenderingViewFamilies<br/>SceneRendering.cpp:4970"]
        CSR["FSceneRenderer::CreateSceneRenderers<br/>SceneRendering.cpp:4284"]
        Decision1{"ShadingPath ?<br/>SceneRendering.cpp:4287"}
        CreateMobile["new FMobileSceneRenderer<br/>MobileShadingRenderer.cpp:287<br/>(bDeferredShading=false)"]
        CreateDeferred["new FDeferredShadingSceneRenderer<br/>(PC 路径, 移动端不走)"]
        EnqCmd["ENQUEUE_RENDER_COMMAND(FDrawSceneCommand)<br/>SceneRendering.cpp:5113"]
        RVF["RenderViewFamilies_RenderThread<br/>SceneRendering.cpp:4743"]

        BRVF -->|Line 4967 转发| BRVFs
        BRVFs -->|Line 5087 构造| CSR
        CSR --> Decision1
        Decision1 -->|EShadingPath::Mobile<br/>Line 4304| CreateMobile
        Decision1 -->|EShadingPath::Deferred<br/>Line 4298| CreateDeferred
        BRVFs -->|Line 5113-5121 投递| EnqCmd
        EnqCmd -->|Lambda 触发| RVF
    end

    subgraph MOBILE_RENDER["④ FMobileSceneRenderer 渲染主循环 (Render Thread)"]
        MSR_Render["FMobileSceneRenderer::Render<br/>MobileShadingRenderer.cpp:910"]
        InitViews["InitViews<br/>MobileShadingRenderer.cpp:433"]
        Gather["GatherAndSortLights / ComputeLightGrid<br/>MobileShadingRenderer.cpp:1079"]
        Shadow["RenderShadowDepthMaps<br/>MobileShadingRenderer.cpp:1154"]
        Decision2{"bDeferredShading ?<br/>MobileShadingRenderer.cpp:~1160"}

        MSR_Render --> InitViews --> Gather --> Shadow --> Decision2
    end

    subgraph FORWARD_PATH["⑤ Forward 渲染路径(本报告重点)"]
        RenderFwd["FMobileSceneRenderer::RenderForward<br/>MobileShadingRenderer.cpp:1503"]
        InitRT["InitRenderTargetBindings_Forward<br/>MobileBasePassRendering.cpp:1448"]
        UpdDir["UpdateDirectionalLightUniformBuffers<br/>MobileShadingRenderer.cpp:2172"]
        CreateUB["CreateMobileBasePassUniformBuffer<br/>MobileBasePassRendering.cpp:248"]
        Decision3{"bRequiresMultiPass ?<br/>MobileShadingRenderer.cpp:~1565"}
        SinglePass["FMobileSceneRenderer::RenderForwardSinglePass<br/>MobileShadingRenderer.cpp:1578"]
        MultiPass["FMobileSceneRenderer::RenderForwardMultiPass<br/>MobileShadingRenderer.cpp:1662"]
        BasePass["FMobileSceneRenderer::RenderMobileBasePass ⭐<br/>MobileBasePassRendering.cpp:470"]
        Dispatch["DispatchDraw(EMeshPass::BasePass)<br/>调用 Pixel Shader:<br/>MobileBasePassPixelShader.usf:86-89<br/>FORWARD_SHADING 宏"]
        DebugView["RenderMobileDebugView<br/>MobileShadingRenderer.cpp:2092"]
        PostBP["PostRenderBasePass<br/>MobileShadingRenderer.cpp:2079"]
        NextSub1["NextSubpass()"]
        Decals["RenderDecals (D-Buffer)"]
        Fog["RenderFog"]
        Transluc["RenderTranslucency"]
        NextSub2{"bTonemapSubpassInline ?"}
        CustomResolve["RenderMobileCustomResolve<br/>(MSAA Resolve + Tonemap)"]
        PostProc["AddMobilePostProcessingPasses<br/>MobileShadingRenderer.cpp:1404"]

        Decision2 -->|"false (Forward 默认)"| RenderFwd
        Decision2 -->|true| RenderDeferred["FMobileSceneRenderer::RenderDeferred<br/>MobileShadingRenderer.cpp:1885"]

        RenderFwd --> InitRT
        RenderFwd --> UpdDir
        RenderFwd --> CreateUB
        RenderFwd --> Decision3
        Decision3 -->|"false (常见)"| SinglePass
        Decision3 -->|true| MultiPass

        SinglePass -->|Subpass 0| BasePass
        BasePass --> Dispatch
        SinglePass --> DebugView
        SinglePass --> PostBP
        SinglePass --> NextSub1
        NextSub1 --> Decals --> Fog --> Transluc
        Transluc --> NextSub2
        NextSub2 -->|true| CustomResolve
        NextSub2 -->|false| PostProc
        CustomResolve --> PostProc
    end

    subgraph DEFERRED_PATH["(对比) Mobile Deferred 路径"]
        RenderDeferred
        RDSingle["FMobileSceneRenderer::RenderDeferredSinglePass<br/>MobileShadingRenderer.cpp:1947"]
        RDMulti["FMobileSceneRenderer::RenderDeferredMultiPass<br/>MobileShadingRenderer.cpp:1996"]
        Decision2 -.->|true| RenderDeferred
        RenderDeferred --> RDSingle
        RenderDeferred --> RDMulti
    end

    subgraph PRESENT["⑥ Present 到 Android Surface"]
        SlateDraw["SlateRHIRenderer::DrawWindow_RenderThread<br/>SlateRHIRenderer.cpp:1087-1560"]
        EndVP["FRHICommandListImmediate::EndDrawingViewport<br/>RHICommandList.cpp:1287"]
        OpenGLPresent["eglSwapBuffers<br/>AndroidOpenGLFramePacer.cpp:488"]
        VulkanPresent["vkQueuePresentKHR / SwappyVk_queuePresent<br/>VulkanAndroidPlatform.cpp:670"]
        SurfaceFlinger([Android SurfaceFlinger<br/>合成并显示])

        PostProc --> SlateDraw
        SlateDraw --> EndVP
        EndVP -->|OpenGL| OpenGLPresent
        EndVP -->|Vulkan| VulkanPresent
        OpenGLPresent --> SurfaceFlinger
        VulkanPresent --> SurfaceFlinger
    end

    %% 让 RT 主循环接到 ③ 渲染分发
    RTMain -.->|异步消费| EnqCmd

    classDef decision fill:#ffd43b,stroke:#fab005,color:#000
    classDef entry fill:#51cf66,stroke:#2f9e44,color:#000
    classDef fwd fill:#4dabf7,stroke:#1971c2,color:#000
    classDef def fill:#ff8787,stroke:#c92a2a,color:#000
    classDef present fill:#da77f2,stroke:#862e9c,color:#000

    class Decision1,Decision2,Decision3,NextSub2 decision
    class Draw,UMGDraw,BRVF,BRVFs,CSR,MSR_Render,BasePass,Dispatch entry
    class RenderFwd,SinglePass,InitRT,UpdDir,CreateUB fwd
    class RenderDeferred,RDSingle,RDMulti def
    class EndVP,OpenGLPresent,VulkanPresent,SurfaceFlinger present
```

---

## 2. 关键调用源:`BeginRenderingViewFamily` 入口

`FRendererModule::BeginRenderingViewFamily` 有 **2 个主要调用方**:

| 调用方 | 位置 | 触发场景 |
|--------|------|----------|
| `UGameViewportClient::Draw` | `Source/Runtime/Engine/Private/GameViewportClient.cpp:1847` | 引擎主游戏窗口绘制(主路径) |
| `UViewport::Draw` | `Source/Runtime/UMG/Private/Components/Viewport.cpp:194` | UMG 视口控件内部渲染 |

> **主路径**:`UGameViewportClient::Draw` (Line 1369) → Line 1847 调用 `GetRendererModule().BeginRenderingViewFamily(SceneCanvas, &ViewFamily)`。

---

## 3. 关键决策点一览

| # | 位置 | 决策内容 | Forward 取值 |
|---|------|----------|--------------|
| ① | `SceneRendering.cpp:4287` | `EShadingPath = GetFeatureLevelShadingPath(FeatureLevel)` | ES3_1 → `EShadingPath::Mobile` |
| ② | `MobileShadingRenderer.cpp:290` | `bDeferredShading = IsMobileDeferredShadingEnabled(ShaderPlatform)` | `false`(默认) |
| ③ | `MobileShadingRenderer.cpp:~1160` | `if (bDeferredShading) RenderDeferred else RenderForward` | 走 `RenderForward` |
| ④ | `MobileShadingRenderer.cpp:1565` | `if (bRequiresMultiPass) Multi else Single` | `RenderForwardSinglePass` |
| ⑤ | `MobileShadingRenderer.cpp:1606` | `if (bTonemapSubpassInline) CustomResolve` | 启用 subpass tonemap |

---

## 4. Forward 关键函数速查(7 个核心)

| 函数 | 文件 | 行号 | 职责 |
|------|------|------|------|
| `FRendererModule::BeginRenderingViewFamily` | `SceneRendering.cpp` | 4965 | 入口包装(单 Family) |
| `FSceneRenderer::CreateSceneRenderers` | `SceneRendering.cpp` | 4284 | 决定构造哪种 SceneRenderer |
| `FMobileSceneRenderer::Render` | `MobileShadingRenderer.cpp` | 910 | 渲染主调度 |
| `FMobileSceneRenderer::RenderForward` | `MobileShadingRenderer.cpp` | 1503 | Forward 路径入口 |
| `FMobileSceneRenderer::RenderForwardSinglePass` | `MobileShadingRenderer.cpp` | 1578 | Subpass 0/1/2 调度 |
| `FMobileSceneRenderer::RenderMobileBasePass` | `MobileBasePassRendering.cpp` | 470 | ⭐ Base Pass 绘制(像素着色器计算光照) |
| `FRHICommandListImmediate::EndDrawingViewport` | `RHICommandList.cpp` | 1287 | 触发 eglSwapBuffers / vkQueuePresentKHR |

---

## 5. 调用链简要总结(精简版)

```
[Game Thread]                                            [Render Thread]
UGameViewportClient::Draw (GameViewportClient.cpp:1369)
   └→ BeginRenderingViewFamily (SceneRendering.cpp:4965)            ┐
        └→ BeginRenderingViewFamilies (SceneRendering.cpp:4970)     │
             ├→ CreateSceneRenderers (SceneRendering.cpp:4284)       │
             │    └→ new FMobileSceneRenderer (line 4304)            │
             │         (bDeferredShading=false, line 290)            │
             └→ ENQUEUE_RENDER_COMMAND ──────────────────────────────┤
                                                                       ↓
                              RenderViewFamilies_RenderThread (SceneRendering.cpp:4743)
                                  └→ FMobileSceneRenderer::Render (MobileShadingRenderer.cpp:910)
                                       ├─ InitViews
                                       ├─ GatherAndSortLights
                                       ├─ RenderShadowDepthMaps
                                       └─ ⭐ bDeferredShading=false
                                              └→ RenderForward (MobileShadingRenderer.cpp:1503)
                                                   └→ RenderForwardSinglePass (line 1578)
                                                        ├─ Subpass 0: RenderMobileBasePass (line 470) ⭐
                                                        │       └─ MobileBasePassPixelShader.usf (光照累加 + IBL)
                                                        ├─ Subpass 1: Decals / Fog / Translucency
                                                        └─ Subpass 2: CustomResolve (Tonemap + MSAA Resolve)
                                                                              ↓
                              AddMobilePostProcessingPasses
                                                                              ↓
                              SlateRHIRenderer::DrawWindow_RenderThread
                                  └→ EndDrawingViewport (RHICommandList.cpp:1287)
                                       ├─ OpenGL: eglSwapBuffers
                                       └─ Vulkan: vkQueuePresentKHR / SwappyVk_queuePresent
                                                                              ↓
                                                                 Android SurfaceFlinger → 屏幕
```
