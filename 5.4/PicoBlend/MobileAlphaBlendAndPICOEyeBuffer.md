# 移动端透明材质 Alpha 混合与 PICO Eye Buffer 分析

> 基于本仓库 `Engine/Source`（UE 5.4 代码库）与 `PICO-Unreal-Integration-SDK/UE_5.4` 的实际代码调查。
> PICO 官方文档（<https://developer-cn.picoxr.com/document/unreal/seethrough/>）为 JS 渲染页面无法直接抓取，文中对文档的引用以官方搜索结果摘要与 SDK 代码互相印证。

## 一、移动端透明材质的 Alpha 是如何混合进 RT 的

### 1. 渲染路径

移动渲染器（ES3.1 / Vulkan）不经过桌面的 `FTranslucencyDrawingPolicyFactory`，透明物体作为 mesh pass 直接画进 SceneColor：

- `Engine/Source/Runtime/Renderer/Private/MobileTranslucentRendering.cpp:7-20` — `FMobileSceneRenderer::RenderTranslucency` 只是派发 `View.ParallelMeshDrawCommandPasses[StandardTranslucencyMeshPass]`。
- 调用点：`MobileShadingRenderer.cpp:1623`（单 pass，切换子 pass 以便 fetch 深度）、`:1735`、`:2068`（多 pass）。
- 每个材质的混合状态由 `MobileBasePass::SetTranslucentRenderState()` 设置：`Engine/Source/Runtime/Renderer/Private/MobileBasePass.cpp:615`，由 mobile base-pass 的 mesh processor 调用（同文件 :950、:1103）。

### 2. 精确混合公式（BLEND_Translucent，最常见情况）

`MobileBasePass.cpp:666`：

```cpp
TStaticBlendState<CW_RGBA, BO_Add, BF_SourceAlpha, BF_InverseSourceAlpha,
                  BO_Add, BF_Zero, BF_InverseSourceAlpha, ...>
```

即：

```
RGB' = Src.rgb · Src.a + Dst.rgb · (1 - Src.a)
A'   = Src.a · 0       + Dst.a   · (1 - Src.a)
```

**关键结论：目标（RT）的 Alpha 既不会被保留原样，也不会被强制写 1——它会乘以 `(1 - SrcAlpha)`。**

像素着色器输出 `OutColor = half4(FoggedColor, Opacity)`（`Engine/Shaders/Private/MobileBasePassPixelShader.usf:1061-1062`），因此每叠一层半透明，RT 的 Alpha 就向 0 衰减一次。

### 3. 各 BlendMode 对 Alpha 的行为（`MobileBasePass.cpp:676-719`）

| BlendMode | 位置 | 对 Alpha 的行为 |
|---|---|---|
| `BLEND_Translucent` | :666 | `Dst.a · (1 - Src.a)`（Alpha 衰减） |
| `BLEND_Additive` | :678 | 同上乘法衰减（`BF_Zero / BF_InverseSourceAlpha`） |
| `BLEND_Modulate` | :689 | `CW_RGB` 写掩码，**Alpha 通道不写、保持原值** |
| `BLEND_AlphaComposite` | :700 | 预乘式，Alpha 同样 `·(1 - Src.a)` 衰减 |
| `BLEND_AlphaHoldout` | :711 | `A' = Src.a + Dst.a·(1 - Src.a)`，写入 holdout Alpha（VR 透视"抠洞"用） |
| Substrate 路径 | :641 | 预乘，Alpha 同样衰减 |

特殊情况：`ShouldWriteOnlyAlpha()`（`MobileBasePass.cpp:655`）只写 `CW_ALPHA`，`A' = Src.a`；Thin-translucent / `BLEND_TranslucentColoredTransmittance` 走 dual-source blending 或 framebuffer-fetch（`MobileBasePassRendering.cpp:69-121`、`MobileBasePass.cpp:585-613`）。

## 二、最终展示到屏幕的 RT 默认 Alpha 是多少

### 1. SceneColor 的 Alpha：默认全 0

- 格式由 `GetMobileSceneColorFormat()` 决定（`Engine/Source/Runtime/Engine/Private/SceneTexturesConfig.cpp:53-94`）：
  - HDR 移动端默认 **`PF_FloatR11G11B10`——根本没有 Alpha 通道**；只有 `r.Mobile.PropagateAlpha=1`（默认 0，`MobileBasePassRendering.cpp:52-57`）时才改用 `PF_FloatRGBA`（`SceneTextures.cpp:400`）。
  - LDR / XR 一体机走 `GetDefaultMobileSceneColorLowPrecisionFormat()`：XR 用 swapchain 格式（`PF_R8G8B8A8` 等，带 Alpha 字节）。
- 清屏值：`FSceneTexturesConfig::ColorClearValue = FClearValueBinding::Black`，**Alpha = 0**（`SceneTexturesConfig.h:180`、`SceneTexturesConfig.cpp:268`；创建见 `SceneTextures.cpp:488-489`）。
- 不透明像素也把 **Alpha 写成 0.0**：`MobileBasePassPixelShader.usf:1071-1073`（注释说明平面反射/SceneCapture 用 Alpha 标记已渲染区域）。
- 对比：桌面延迟渲染路径强制清屏 Alpha=1（`kSceneColorClearAlpha = 1.0f`，`BasePassRendering.cpp:123`），**移动端不用这条路径**。

### 2. 最终 Backbuffer / Swapchain

- 默认 backbuffer 格式是 8 位 RGBA：`EDefaultBackBufferPixelFormat::DBBPF_B8G8R8A8 = 0`（`Engine/Classes/Engine/RendererSettings.h:159-170`），Android Vulkan swapchain 同样有 Alpha 通道，但普通 App 的系统合成器忽略它。
- 最终写入的 Alpha 取决于后处理路径：
  - **Tonemap Subpass 路径**（`r.Mobile.TonemapSubpass`，Vulkan HDR 默认开）：`MobileCustomResolve_MainPS` 强制 **`OutColor.a = 1.0`**（`Engine/Shaders/Private/PostProcessTonemap.usf:702-710`）。
  - **标准 Tonemapper 路径**：`bWriteAlphaChannel = AAM_FXAA || r.PostProcessing.PropagateAlpha || ...`（`PostProcessing.cpp:2730`），`r.PostProcessing.PropagateAlpha` 默认 0；不写 Alpha 时用 `CW_RGB` 写掩码（`PostProcessTonemap.cpp:1016`），目标 Alpha 保持清屏值。
  - **立体（XR）眼图特例**：`ShouldWriteAlphaChannel()` 对立体眼视图返回 true（`PostProcessTonemap.cpp:550-557`，注释提到 Oculus runtime 的修复），此时 `POST_PROCESS_ALPHA == 0` 会往 eye buffer 里**写 Alpha = 0**。

**总结：移动端 SceneColor 的 Alpha 默认全为 0（清屏 0、不透明写 0、半透明乘 (1-a) 衰减）；最终屏幕 RT 的 Alpha 在 Tonemap Subpass 路径下被强制为 1.0，其他路径下为 0。** 引擎没有在任何通用 blit 中把最终 Alpha 强制为 1——除非开启 `r.Mobile.PropagateAlpha=1` / `r.PostProcessing.PropagateAlpha=1`。

## 三、PICO 的 Eye Buffer 是不是就是这张 RT？

**是，就是同一张。** PICO SDK 中没有独立的"最终 RT"——XR 视口的最终渲染目标本身就是 Eye Buffer swapchain 纹理。

证据链（均在 `PICO-Unreal-Integration-SDK/UE_5.4/Plugins/PICOXR/Source/PICOXRHMD/` 下）：

1. Eye Buffer 即 **layer 0**（`PXR_LAYER_PROJECTION` 原生投影层），`PXR_HMD.cpp:1164-1168` 创建为 UE 立体层。
2. 层描述填格式：`PXR_StereoLayer.cpp:1238` — `PxrLayerCreateParam.format = VK_FORMAT_R8G8B8A8_SRGB / UNORM`，**固定带 Alpha 通道**。
3. 包成 UE swapchain：`PXR_StereoLayer.cpp:656` — `CreateSwapChain_RenderThread(..., PF_R8G8B8A8, ...)`，实现见 `PXR_HMDRenderBridge.cpp:61-76`。
4. `FPICOXRHMD::AllocateRenderTargetTexture`（`PXR_HMD.cpp:2585-2612`）直接把这个 swapchain 纹理交回给 UE 渲染器当渲染目标——UE 移动端渲染器**直接渲染进 Eye Buffer**，没有像普通手机 App 那样的 present/mirror 步骤（`NeedsNativePresent()` 返回 false，`PXR_HMDRenderBridge.cpp:27-30`）。
5. 每帧以 `PxrLayerProjection2` 提交给 PXR 合成器：`FPICOXRStereoLayer::SubmitLayer_RHIThread`（`PXR_StereoLayer.cpp:808+`）。之后由 **PXR runtime** 把 Eye Buffer 与 VST（视频透视）相机画面做合成——合成发生在引擎之外。

所以官方文档里说的 Eye Buffer，指的就是这张"最终展示用的 RT"，只是它的消费者不是手机屏幕而是 PICO 合成器，Alpha 通道也因此在合成时有实际意义。

## 四、Eye Buffer 的 Alpha 如何参与透视合成

由 CVar `r.Mobile.PICO.BlendModeSetting` 控制（定义于 `PXR_HMD.cpp:90-96`，**默认 1**），混合因子在 `PXR_StereoLayer.cpp:824-846` 提交：

| 模式 | 值 | 混合因子 | 效果 |
|---|---|---|---|
| CoveringMode | 0 | `src=ONE, dst=ONE` | VST 覆盖全屏 |
| **ClipMode（默认）** | 1 | `srcColor=ONE_MINUS_SRC_ALPHA, dstColor=SRC_ALPHA` | Eye Buffer 与 VST 按 Alpha 裁剪 |
| AdditiveMode | 2 | `src=ONE, dst=SRC_ALPHA` | Eye Buffer 不按 Alpha 裁剪 |

官方描述（PICO 分支使用指南）："`r.Mobile.PICO.BlendModeSetting=1`（默认）：VST 层与 Eye Buffer 均会根据 Alpha 值做裁剪"。

默认 ClipMode 的合成公式按混合因子字面展开为：

```
Final = EyeBuffer.rgb · (1 - EyeBuffer.a) + VST.rgb · EyeBuffer.a
```

即 **Alpha = 1 的位置显示透视相机画面，Alpha = 0 的位置显示渲染的 VR 内容**（语义与经典 "over" 混合相反）。SDK 本身从不写 Eye Buffer 的 Alpha——Alpha 完全来自 App/渲染器写进 RGBA8 目标的值；SDK 只提供带 Alpha 的表面和混合因子。

### 需要特别注意的"坑"

把上面两节合起来看：

- UE 移动端默认往 SceneColor/Eye Buffer 里写的 Alpha 是 **0**（清屏 0、不透明像素写 0、半透明乘 `(1-a)` 衰减）。
- 而 PICO ClipMode 下 Alpha=0 恰好是"显示渲染内容"。**这两个默认值是自洽的**：默认状态下整个画面都显示 VR 内容，透视不生效。
- 想做 MR 透视"抠洞"（让某区域露出真实世界），需要在该区域写入 Alpha=1，手段包括：
  - 使用 `BLEND_AlphaHoldout` 材质（写 `Src.a`，正是为此设计的）；
  - 或开启 `r.Mobile.PropagateAlpha=1`（SceneColor 改用 `PF_FloatRGBA`，并在后处理链路中传播 Alpha）。
- 注意 OpenXR 通用路径语义相反：`xr.OpenXREnvironmentBlendMode=3`（ALPHA_BLEND）下 Alpha=0 表示透明/透视（`OpenXRHMD.cpp:85-88, 3602`）。**PICO 原生 SDK 的 ClipMode 与 OpenXR ALPHA_BLEND 的 Alpha 语义是相反的，迁移代码时不要混用假设。**

## 五、关键文件索引

| 主题 | 文件 |
|---|---|
| 移动端透明混合状态 | `Engine/Source/Runtime/Renderer/Private/MobileBasePass.cpp:615-719` |
| 移动端像素着色器输出 | `Engine/Shaders/Private/MobileBasePassPixelShader.usf:1061-1073` |
| SceneColor 格式与清屏 | `Engine/Source/Runtime/Engine/Private/SceneTexturesConfig.cpp:53-94`、`Public/SceneTexturesConfig.h:180` |
| PropagateAlpha CVar | `Engine/Source/Runtime/Renderer/Private/MobileBasePassRendering.cpp:52-57` |
| Tonemap 强制 Alpha=1 | `Engine/Shaders/Private/PostProcessTonemap.usf:702-710` |
| 立体眼图写 Alpha=0 | `Engine/Source/Runtime/Renderer/Private/PostProcess/PostProcessTonemap.cpp:550-557` |
| Backbuffer 格式 | `Engine/Source/Runtime/Engine/Classes/Engine/RendererSettings.h:159-170` |
| PICO Eye Buffer 创建 | `PICO-Unreal-Integration-SDK/UE_5.4/Plugins/PICOXR/Source/PICOXRHMD/Private/PXR_StereoLayer.cpp:656, 1238` |
| PICO 渲染目标接管 | `PICO-Unreal-Integration-SDK/UE_5.4/Plugins/PICOXR/Source/PICOXRHMD/Private/PXR_HMD.cpp:2585-2612` |
| PICO 混合因子提交 | `PICO-Unreal-Integration-SDK/UE_5.4/Plugins/PICOXR/Source/PICOXRHMD/Private/PXR_StereoLayer.cpp:808-848` |
| BlendModeSetting CVar | `PICO-Unreal-Integration-SDK/UE_5.4/Plugins/PICOXR/Source/PICOXRHMD/Private/PXR_HMD.cpp:90-96` |
| 开启 VST（Android manifest） | `PICO-Unreal-Integration-SDK/UE_5.4/Plugins/PICOXR/Source/PICOXRHMD/PICOXR_UPL.xml:96-107` |

## 六、补充验证：最终 RT 默认 Alpha 与"A' 是否恒为 0"

> 针对疑问：最终展示到屏幕的 RT 默认 Alpha 是多少？如果初始为 0，那么 `A' = Src.a·0 + Dst.a·(1 - Src.a)` 中 A' 岂不是永远为 0？以下为逐条代码验证结论（2026-07 复核，行号已按当前代码校准）。

### 1. "A' 会一直为 0" —— 数学与代码双重成立

数学上：`A' = Src.a·0 + Dst.a·(1 - Src.a)`，只要 `Dst.a = 0`，无论叠多少层半透明，`A' ≡ 0`（`0 · (1 - Src.a) = 0`）。Alpha 不是"衰减"，而是**从 0 出发永远无法离开 0**——只有显式写入非零 Alpha 的路径才能打破它。

代码上，初始与每像素写入确实都让 `Dst.a` 保持 0：

- **初始清屏 Alpha = 0**：SceneColor 清屏值 `FClearValueBinding::Black`（`SceneTexturesConfig.cpp:268`、`SceneTexturesConfig.h:182`）；Vulkan 最终 backbuffer / XR swapchain 包装纹理用默认构造的 `FClearValueBinding()`，即 (0,0,0,0)（`RHIResources.h:230-237`，`VulkanViewport.cpp:17-18`）。**没有任何路径以 Alpha=1 清屏**（桌面延迟路径的 `kSceneColorClearAlpha = 1.0f` 在 `BasePassRendering.cpp:123`，移动端不走）。
- **不透明像素也写 Alpha = 0**：`MobileBasePassPixelShader.usf:1072-1076`，`OutColor.a = 0.0;`（注释：平面反射/SceneCapture 用 scene color alpha 标记已渲染区域）。
- **半透明按公式叠加**：混合状态 `MobileBasePass.cpp:666`（alpha 部分 `BO_Add, BF_Zero, BF_InverseSourceAlpha`），着色器输出 `half4(..., Opacity)`（`MobileBasePassPixelShader.usf:1065`，Translucent 分支）。`0 · (1 - Src.a) = 0`，恒为 0。

例外（能让 Alpha 脱离 0 的路径，均非默认）：

- `BLEND_AlphaHoldout`：`A' = Src.a + Dst.a·(1 - Src.a)`（`MobileBasePass.cpp:711`，alpha 源因子 `BF_One`）——这正是 MR"抠洞"的正规手段。
- `BLEND_Modulate`：`CW_RGB` 写掩码 + alpha 目标因子 `BF_One`，**保留**目标 alpha 但不增加（`MobileBasePass.cpp:689`，注释 "preserve destination alpha"）。
- `r.Mobile.PropagateAlpha=1`（默认 0，`MobileBasePassRendering.cpp:52-57`，`ECVF_ReadOnly`）：SceneColor 改用 `PF_FloatRGBA`（`SceneTextures.cpp:400` → `SceneTexturesConfig.cpp:63`），且 tonemapper 走 `POST_PROCESS_ALPHA=2` 传播 Alpha（`ShaderCompiler.cpp:8585-8600`）。

### 2. 最终展示到屏幕的 RT 默认 Alpha —— 分两条路径

- **Tonemap Subpass 路径**（`r.Mobile.TonemapSubpass`，Vulkan HDR 默认开，判定见 `MobileShadingRenderer.cpp:102-113, 1398-1400`）：`MobileCustomResolve_MainPS` **无条件**写 `SceneColor.a = 1.0` 和 `OutColor.a = 1.0`（`PostProcessTonemap.usf:702-710`）。此路径下最终 RT Alpha = **1**。
- **标准 Tonemapper 路径**（含 XR 非 subpass）：`ShouldWriteAlphaChannel()` 对立体眼视图强制返回 true（`PostProcessTonemap.cpp:550-557`，注释提到 Oculus runtime 的 bug）；于是用 `CW_RGBA` 全通道写（`PostProcessTonemap.cpp:1016`、`ScreenPass.h:363`），但着色器在默认 `POST_PROCESS_ALPHA=0` 下不传播 Alpha，`OutColor` 初始化为 0（`PostProcessTonemap.usf:454`）——**实际往 eye buffer 写入的是 Alpha = 0**。除 subpass 路径外，引擎没有任何代码把最终 Alpha 强制为 1。

即：**默认配置下，最终 RT 的 Alpha 要么是 subpass 强制的 1，要么是 tonemapper 写出的 0——两者都与半透明叠加无关；半透明叠加本身永远无法把 Alpha 从 0 变成非 0。**

### 3. PICO Eye Buffer 就是这张 RT —— 验证属实

逐项复核 `PICO-Unreal-Integration-SDK/UE_5.4/Plugins/PICOXR/Source/PICOXRHMD/`：

- Eye Buffer 即 layer 0：`PXR_HMD.cpp:1164-1168` 创建 UE 立体层并 `check(EyeLayerId == 0)`；`PXR_LAYER_PROJECTION` 的赋值在 `PXR_StereoLayer.cpp:1227`（`SetEyeLayerDesc`，由 `PXR_HMD.cpp:1097` 调用）。
- 格式固定带 Alpha：`PXR_StereoLayer.cpp:1238` — `VK_FORMAT_R8G8B8A8_SRGB / UNORM`，无任何不带 Alpha 的分支。
- swapchain 创建：`PXR_StereoLayer.cpp:656`（`PF_R8G8B8A8`），实现 `PXR_HMDRenderBridge.cpp:61-77`。
- 渲染目标接管：`FPICOXRHMD::AllocateRenderTargetTexture`（`PXR_HMD.cpp:2585-2606`）直接把 eye layer swapchain 纹理作为 `OutTargetableTexture` 返回——UE 渲染器**直接渲染进 Eye Buffer**。
- 无 present/mirror：`NeedsNativePresent()` 返回 false（`PXR_HMDRenderBridge.cpp:27-30`），`Present()` 只做帧结束回调（:32-59）。
- 混合因子提交：`PXR_StereoLayer.cpp:819-848` 填 `PxrLayerBlend`（`PxrBlendFactor::PXR_BLEND_FACTOR_*` 枚举），ClipMode（默认，`PXR_HMD.cpp:90-96` CVar 默认 1）为 `srcColor=ONE_MINUS_SRC_ALPHA, dstColor=SRC_ALPHA`，`srcAlpha=dstAlpha=ONE`；实际提交在 `PXR_StereoLayer.cpp:951` `SubmitLayer2((PxrLayerHeader2*)&layerProjection)`。

**"SDK 本身从不写 Eye Buffer 的 Alpha" 复核成立**：

- eye layer 无 `LayerDesc.Texture`，不走 `PXRLayersCopy_RenderThread` 的 TransferImage 路径（`PXR_StereoLayer.cpp:356-394`）。
- 模块内唯一清屏函数 `ClearTexture_RHIThread`（`PXR_HMD.cpp:2383-2400`）对 Vulkan 直接早退且**无任何调用者**（死代码）。
- Vulkan swapchain 纹理包装（`PXR_HMDRenderBridge_Vulkan.cpp:48-79`）传默认 `FClearValueBinding()`（ENoneBound），**创建后不清屏**；初始内容由 PICO runtime 分配，首帧即被 UE 渲染完整覆盖。
- `bNeedDrawBlackEye` 黑屏不画进 eye buffer，而是让提交的 `colorScale` 为全 0（`PXR_StereoLayer.cpp:850-853`），由合成器显示黑。

### 4. 对本疑问的最终回答

最终展示到屏幕的 RT 默认 Alpha **取决于后处理路径**（Tonemap Subpass 强制 1，否则为 0），但**半透明混合链中的 `Dst.a` 起点恒为 0**（清屏 0 + 不透明写 0），因此 `A' = Dst.a·(1 - Src.a)` 在数学上恒为 0——这不是 bug 而是设计：Alpha 通道在移动端默认不被当作有效数据维护，只有在 MR 抠洞（`BLEND_AlphaHoldout`）或显式开启 `r.Mobile.PropagateAlpha` 时才有意义。PICO 的 Eye Buffer 正是这张 RT 本身，SDK 只提供带 Alpha 的表面和合成混合因子，从不代为写入 Alpha。
