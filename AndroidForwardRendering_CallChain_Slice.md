# UE5 Android Forward 渲染调用链 - 指定片段

> 目标平台:Android / UE 5.4.4
> **范围限定**:从 `BeginRenderingViewFamily` 的调用方 → `RenderMobileBasePass`
> 覆盖节点:**11 个函数 + 2 个调用方 + 3 个决策点**

---

## Mermaid 调用链

```mermaid
flowchart TD
    %% ① 调用方
    Caller1["UGameViewportClient::Draw<br/>GameViewportClient.cpp:1369<br/>(主游戏窗口绘制)"]
    Caller2["UViewport::Draw<br/>Viewport.cpp:194<br/>(UMG 视口控件)"]

    %% ② 入口
    BRVF["FRendererModule::BeginRenderingViewFamily<br/>SceneRendering.cpp:4965"]
    BRVFs["FRendererModule::BeginRenderingViewFamilies<br/>SceneRendering.cpp:4970"]

    %% ③ SceneRenderer 构造(决策点 ①)
    CSR["FSceneRenderer::CreateSceneRenderers<br/>SceneRendering.cpp:4284<br/>━━━━━━━━━━━━<br/>EShadingPath ShadingPath =<br/>&nbsp;&nbsp;GetFeatureLevelShadingPath(<br/>&nbsp;&nbsp;&nbsp;&nbsp;Scene->GetFeatureLevel());<br/>━━━━━━━━━━━━"]
    D1{"ShadingPath ==<br/>EShadingPath::Mobile ?<br/>SceneRendering.cpp:4287<br/>GetFeatureLevelShadingPath()"}
    NewMobile["new FMobileSceneRenderer<br/>━━━━━━━━━━━━<br/>OutSceneRenderers.Add(<br/>&nbsp;&nbsp;new FMobileSceneRenderer(<br/>&nbsp;&nbsp;&nbsp;&nbsp;InViewFamily,<br/>&nbsp;&nbsp;&nbsp;&nbsp;HitProxyConsumer));<br/>━━━━━━━━━━━━<br/>MobileShadingRenderer.cpp:287<br/>(调用点 SceneRendering.cpp:4304)<br/>bDeferredShading=false"]

    %% ④ 投递到 RT
    EnqCmd["ENQUEUE_RENDER_COMMAND(FDrawSceneCommand)<br/>SceneRendering.cpp:5113"]

    %% ⑤ RT 渲染分发
    RVF["RenderViewFamilies_RenderThread<br/>SceneRendering.cpp:4743"]

    %% ⑥ Mobile 主渲染
    MSR["FMobileSceneRenderer::Render<br/>MobileShadingRenderer.cpp:910"]
    D2{"bDeferredShading ?<br/>MobileShadingRenderer.cpp:1311"}

    %% ⑦ Forward 入口
    RenderFwd["FMobileSceneRenderer::RenderForward<br/>MobileShadingRenderer.cpp:1503"]
    D3{"bRequiresMultiPass ?<br/>MobileShadingRenderer.cpp:~1565"}
    SinglePass["FMobileSceneRenderer::RenderForwardSinglePass<br/>MobileShadingRenderer.cpp:1578"]

    %% ⑧ ⭐ Base Pass
    BasePass["FMobileSceneRenderer::RenderMobileBasePass ⭐<br/>MobileBasePassRendering.cpp:470"]

    %% 边:调用方 → 入口
    Caller1 -->|"Line 1847<br/>GetRendererModule().BeginRenderingViewFamily()"| BRVF
    Caller2 -->|"Line 194"| BRVF

    %% 入口分发
    BRVF -->|"Line 4967 转发单 Family"| BRVFs
    BRVFs -->|"Line 5087 构造 SceneRenderer"| CSR

    %% 决策 ①
    CSR --> D1
    D1 -->|"EShadingPath::Mobile<br/>(ES3_1 默认)<br/>Line 4304"| NewMobile

    %% 投递到 RT
    BRVFs -->|"Line 5113 异步投递<br/>lambda 内调 RVF"| EnqCmd
    EnqCmd -.->|"RT 消费后触发<br/>(虚线:异步)"| RVF

    %% RT 渲染
    RVF -->|"for each SceneRenderer"| MSR

    %% 决策 ②
    MSR --> D2
    D2 -->|"false (Forward 默认)"| RenderFwd

    %% Forward
    RenderFwd --> D3
    D3 -->|"false (常见)"| SinglePass

    %% SinglePass → BasePass
    SinglePass -->|"Subpass 0 内调用"| BasePass

    classDef caller fill:#ffd8a8,stroke:#e8590c,color:#000
    classDef entry fill:#51cf66,stroke:#2f9e44,color:#000
    classDef decision fill:#ffd43b,stroke:#fab005,color:#000
    classDef fwd fill:#4dabf7,stroke:#1971c2,color:#000
    classDef target fill:#ff6b6b,stroke:#c92a2a,color:#fff

    class Caller1,Caller2 caller
    class BRVF,BRVFs,CSR,NewMobile,EnqCmd,RVF,MSR entry
    class D1,D2,D3 decision
    class RenderFwd,SinglePass fwd
    class BasePass target
```

---

## 节点清单(11 个函数 + 2 调用方 + 3 决策)

| # | 类型 | 名称 | 文件:行号 |
|---|------|------|----------|
| 1 | 调用方 | `UGameViewportClient::Draw` | `GameViewportClient.cpp:1369` (调用 1847) |
| 2 | 调用方 | `UViewport::Draw` | `Viewport.cpp:194` |
| 3 | 入口 | `FRendererModule::BeginRenderingViewFamily` | `SceneRendering.cpp:4965` |
| 4 | 入口 | `FRendererModule::BeginRenderingViewFamilies` | `SceneRendering.cpp:4970` |
| 5 | 入口 | `FSceneRenderer::CreateSceneRenderers`<br/>调用 `OutSceneRenderers.Add(new FMobileSceneRenderer(...))` | `SceneRendering.cpp:4284`<br/>(构造语句在 Line 4304) |
| 6 | 构造 | `new FMobileSceneRenderer`<br/>`bDeferredShading = IsMobileDeferredShadingEnabled(ShaderPlatform)` | `MobileShadingRenderer.cpp:287` |
| 7 | 入口 | `ENQUEUE_RENDER_COMMAND(FDrawSceneCommand)` | `SceneRendering.cpp:5113` |
| 8 | 入口 | `RenderViewFamilies_RenderThread` | `SceneRendering.cpp:4743` |
| 9 | 入口 | `FMobileSceneRenderer::Render` | `MobileShadingRenderer.cpp:910` |
| 10 | Forward | `FMobileSceneRenderer::RenderForward` | `MobileShadingRenderer.cpp:1503` |
| 11 | Forward | `FMobileSceneRenderer::RenderForwardSinglePass` | `MobileShadingRenderer.cpp:1578` |
| 12 | ⭐ 终点 | `FMobileSceneRenderer::RenderMobileBasePass` | `MobileBasePassRendering.cpp:470` |

| # | 决策点 | 位置 | Forward 取值 |
|---|--------|------|--------------|
| ① | `GetFeatureLevelShadingPath(FeatureLevel)` | `SceneRendering.cpp:4287` | `EShadingPath::Mobile` |
| ② | `if (bDeferredShading)` | `MobileShadingRenderer.cpp:~1160` | `false` |
| ③ | `if (bRequiresMultiPass)` | `MobileShadingRenderer.cpp:~1565` | `false` (走 SinglePass) |

---

## 调用链一句话总结

> `UGameViewportClient::Draw` 在第 1847 行调用 `BeginRenderingViewFamily` → 转发到 `BeginRenderingViewFamilies` → 在第 5087 行通过 `CreateSceneRenderers`(决策 `EShadingPath = GetFeatureLevelShadingPath(FeatureLevel)`,ES3_1 → `Mobile`)→ **在 Line 4304 执行 `OutSceneRenderers.Add(new FMobileSceneRenderer(InViewFamily, HitProxyConsumer))` 构造 `FMobileSceneRenderer`**(构造函数 Line 287,`bDeferredShading = false`)→ 第 5113 行 `ENQUEUE_RENDER_COMMAND` 投递到 RT → RT 消费后调用 `RenderViewFamilies_RenderThread` → `FMobileSceneRenderer::Render` → 决策 `bDeferredShading=false` → 走 `RenderForward` → 决策 `bRequiresMultiPass=false` → 走 `RenderForwardSinglePass` → Subpass 0 中调用 `RenderMobileBasePass` 完成最终像素着色与光照累加。
