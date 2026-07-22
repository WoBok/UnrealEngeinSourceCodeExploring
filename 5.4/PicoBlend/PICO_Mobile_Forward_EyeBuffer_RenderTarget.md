# PICO SDK：移动端前向渲染下 Eye Buffer 与 Unreal 最终颜色 RT 的关系

## 1. 适用范围

本文结论基于当前仓库中的以下版本：

- Unreal Engine：5.4.4，分支 `5.4`，提交 `847de5e2553`
- PICOXR 插件：3.2.3
- PICO SDK 路径：`Engine/PICO-Unreal-Integration-SDK/UE_5.4/Plugins/PICOXR`
- 渲染路径：Android Mobile Forward，Vulkan/OpenGL ES

文中需要区分三个经常都被简称为“最终 RT”的对象：

1. `SceneTextures.Color.Target`：Mobile Forward 的中间 Scene Color。
2. `ViewFamilyTexture`：Unreal Renderer 本帧最终写入的 View Family 输出纹理。
3. PICO Runtime/系统合成器最终写到物理面板的图像：应用不可直接持有的系统输出。

## 2. 直接结论

| 比较对象 | 是否同一个 | 结论 |
|---|---:|---|
| PICO 当前 Eye Buffer 与 UE 的 `ViewFamilyTexture` | 是 | 它们是同一个底层 PICO swapchain 原生图像；UE 侧只是把当前 `VkImage`/GL texture 包装成 `FRHITexture` 并注册到 RDG。 |
| PICO Eye Buffer 与 Mobile Forward 的实际 `SV_Target0` | 条件决定 | PICO 的 UE 内置 HMD 畸变关闭。LDR、无 MSAA 且没有 Upscale 等额外条件时，Base Pass/Translucency 直接写 Eye Buffer；MSAA 时先写 MSAA Scene Color 并 Resolve 到 Eye Buffer；Mobile HDR/Upscale 等路径先写中间 Scene Color。 |
| PICO Eye Buffer 与物理屏幕/面板 backbuffer | 否 | UE 将 Eye Layer 提交给 PICO Runtime；Runtime 再做畸变、重投影、VST/其他 Layer 合成并扫描到面板。PICO Custom Present 会阻止 Android RHI 的普通窗口 Present。 |

因此，对“Eye Buffer 和引擎最终输出颜色 RT 是不是同一个”必须按术语回答：

- 如果“引擎最终输出颜色 RT”指 `ViewFamilyTexture`：**是，同一个当前 Eye swapchain 图像。**
- 如果“最终 RT”指 Base Pass 当前绑定的 `SV_Target0`：**要看 `bRenderToSceneColor` 和 MSAA，不能只凭“这是 XR”判断。**
- 如果“最终 RT”指物理面板 backbuffer：**不是。该图像属于 PICO 系统合成器。**

完整颜色链如下：

```text
PICO Runtime CreateLayer/GetLayerImage
        ↓ 返回原生 VkImage / GL texture
PICOXR 用 FRHITexture 包装并组成 FXRSwapChain
        ↓ AllocateRenderTargetTexture
FSceneViewport.RenderTargetTextureRHI
        ↓ TryCreateViewFamilyTexture
ViewFamilyTexture 〔同一张当前 PICO Eye Buffer〕

Mobile Base Pass / 普通 Translucency
        ↓ InitRenderTargetBindings_Forward 条件分支
        ├─ LDR、1x（无 MSAA）、无其他强制条件：直接写 ViewFamilyTexture/Eye Buffer
        ├─ LDR、MSAA：写 SceneTextures.Color.Target，Resolve 到 Eye Buffer
        └─ Mobile HDR/Upscale/其他条件：写中间 Scene Color
                                      ↓ Tonemap / Mobile Post Processing
                              ViewFamilyTexture 〔PICO Eye Buffer〕
        ↓ SubmitLayer2 + EndFrame
PICO Runtime Compositor
        ↓ 畸变、时间重投影、VST/Overlay 合成
物理面板输出 〔不是应用 Eye Buffer〕
```

## 3. PICO Eye Buffer 从哪里来

### 3.1 主 Eye Layer 是 ID 0 的 Layer

PICOXR 启动时创建优先级最低、持续更新的主 Eye Layer，并要求它的 Unreal Layer ID 为 0：

```cpp
// Engine/PICO-Unreal-Integration-SDK/UE_5.4/Plugins/PICOXR/
// Source/PICOXRHMD/Private/PXR_HMD.cpp:1164-1168
IStereoLayers::FLayerDesc EyeLayerDesc;
EyeLayerDesc.Priority = INT_MIN;
EyeLayerDesc.Flags = LAYER_FLAG_TEX_CONTINUOUS_UPDATE;
const uint32 EyeLayerId = CreateLayer(EyeLayerDesc);
check(EyeLayerId == 0);
```

主层会被描述为 `PXR_LAYER_PROJECTION`。Mobile Multi-View 开启时，`ArraySize == 2`，Eye Buffer 是双层 `Texture2DArray`；否则为 double-wide 2D texture：

```cpp
// .../PXR_StereoLayer.cpp:1225-1238
void FPICOXRStereoLayer::SetEyeLayerDesc(
    uint32 SizeX, uint32 SizeY, uint32 ArraySize,
    uint32 NumMips, uint32 NumSamples, FString RHIString,
    bool EnableSubSampled)
{
    PxrLayerCreateParam.layerShape = PXR_LAYER_PROJECTION;
    PxrLayerCreateParam.width = SizeX;
    PxrLayerCreateParam.height = SizeY;
    PxrLayerCreateParam.faceCount = 1;
    PxrLayerCreateParam.mipmapCount = NumMips;
    PxrLayerCreateParam.sampleCount = NumSamples;
    PxrLayerCreateParam.arraySize = ArraySize;
    PxrLayerCreateParam.layerLayout = ArraySize == 2
        ? PXR_LAYER_LAYOUT_ARRAY
        : PXR_LAYER_LAYOUT_DOUBLE_WIDE;
```

PICO HMD 设置 Eye Layer 时传入 `NumSamples = 1`：

```cpp
// .../PXR_HMD.cpp:1096-1098
const bool EnableSubsampled = /* ... */;
EyeLayer->SetEyeLayerDesc(
    GameSettings->RenderTargetSize.X,
    GameSettings->RenderTargetSize.Y,
    Layout, 1, 1, RHIString, EnableSubsampled);
```

这意味着 **PICO Eye swapchain 自身是单采样纹理**。如果 UE Mobile Renderer 使用 MSAA，MSAA 发生在中间 `SceneTextures.Color.Target`，之后再 Resolve/Tonemap 到 Eye Buffer。

不开启 Mobile Multi-View 时，PICO 会把理想单眼宽度乘 2：

```cpp
// .../PXR_HMD.cpp:1283-1287
// Allocate double space for both eyes (if multiview disabled)
if (!bIsUsingMobileMultiView)
{
    RenderTargetSize.X *= 2;
}
```

### 3.2 原生图像由 PICO Runtime 创建

PICOXR 调用 Runtime 创建 Layer，再从 Runtime 取得每个 swapchain image 的原生句柄：

```cpp
// .../PXR_StereoLayer.cpp:566-586
ExecuteOnRHIThread([&]()
{
#if PLATFORM_ANDROID
    PxrLayerCreateParam.layerId = PxrLayerID = PxrLayerIDCounter;
    if (FPICOXRHMDModule::GetPluginWrapper().bIsSessionInitialized &&
        (FPICOXRHMDModule::GetPluginWrapper().CreateLayer(&PxrLayerCreateParam) == 0))
    {
        PxrLayerIDCounter++;
        uint32_t ImageCounts = 0;
        uint64_t LayerImages[2][3] = {};

        FPICOXRHMDModule::GetPluginWrapper().GetLayerImageCount(
            PxrLayerID, PXR_EYE_RIGHT, &ImageCounts);

        for (uint32_t i = 0; i < ImageCounts; i++)
        {
            FPICOXRHMDModule::GetPluginWrapper().GetLayerImage(
                PxrLayerID, PXR_EYE_RIGHT, i, &LayerImages[1][i]);
            TextureResources.Add(LayerImages[1][i]);
        }
```

随后这些原生图像被标记为可渲染、可采样和可 Resolve，并组成 UE 的 `FXRSwapChain`：

```cpp
// .../PXR_StereoLayer.cpp:638-656
ERHIResourceType ResourceType;
if (PxrLayerCreateParam.arraySize == 2)
{
    ResourceType = RRT_Texture2DArray;
}
else
{
    ResourceType = RRT_Texture2D;
}

ETextureCreateFlags Flags = TexCreate_None;
ETextureCreateFlags TargetableTextureFlags = TexCreate_None;
Flags = TargetableTextureFlags |=
    TexCreate_RenderTargetable |
    TexCreate_ShaderResource |
    TexCreate_ResolveTargetable |
    (IsMobileColorsRGB() ? TexCreate_SRGB : TexCreate_None);

SwapChain = CustomPresent->CreateSwapChain_RenderThread(
    ID, PxrLayerID, ResourceType, TextureResources, PF_R8G8B8A8,
    PxrLayerCreateParam.width, PxrLayerCreateParam.height,
    PxrLayerCreateParam.arraySize, PxrLayerCreateParam.mipmapCount,
    PxrLayerCreateParam.sampleCount, Flags,
    TargetableTextureFlags, MSAAValue);
```

### 3.3 UE 只是包装 PICO 的原生图像，不复制颜色

`CreateSwapChain_RenderThread` 对每个原生句柄创建 RHI wrapper，然后建立一个会随当前 image index 切换的 aliased texture：

```cpp
// .../PXR_HMDRenderBridge.cpp:61-76
for (int32 TextureIndex = 0; TextureIndex < NativeTextures.Num(); ++TextureIndex)
{
    FTextureRHIRef TexRef = CreateTexture_RenderThread(
        RHIResourceType, NativeTextures[TextureIndex], Format,
        SizeX, SizeY, NumMips, NumSamples,
        TargetableTextureFlags, MSAAValue);
    RHITextureSwapChain.Add(TexRef);
}

RHITexture = GDynamicRHI->RHICreateAliasedTexture(RHITextureSwapChain[0]);
return CreateXRSwapChain(MoveTemp(RHITextureSwapChain), RHITexture);
```

Vulkan 路径将同一个 `VkImage` 包装成 UE texture：

```cpp
// .../PXR_HMDRenderBridge_Vulkan.cpp:63-69
switch (RHIResourceType)
{
case RRT_Texture2D:
    return GVulkanRHI->RHICreateTexture2DFromResource(
        (EPixelFormat)Format, SizeX, SizeY, NumMips, NumSamples,
        (VkImage)InTexture, TargetableTextureFlags).GetReference();

case RRT_Texture2DArray:
    return GVulkanRHI->RHICreateTexture2DArrayFromResource(
        (EPixelFormat)Format, SizeX, SizeY, 2, NumMips, NumSamples,
        (VkImage)InTexture, TargetableTextureFlags,
        ColorTextureBinding).GetReference();
}
```

OpenGL ES 路径同理，传入的 `(GLuint)InTexture` 仍是 PICO Runtime 创建的 texture：

```cpp
// .../PXR_HMDRenderBridge_OpenGL.cpp:16-25
case RRT_Texture2D:
    return DynamicRHI->RHICreateTexture2DFromResource(
        (EPixelFormat)Format, SizeX, SizeY, NumMips, NumSamples,
        MSAAValue, FClearValueBinding::Black,
        (GLuint)InTexture, TargetableTextureFlags).GetReference();

case RRT_Texture2DArray:
    return DynamicRHI->RHICreateTexture2DArrayFromResource(
        (EPixelFormat)Format, SizeX, SizeY, 2, NumMips, NumSamples,
        MSAAValue, FClearValueBinding::Black,
        (GLuint)InTexture, TargetableTextureFlags).GetReference();
```

这里没有 `CopyTexture` 或全屏 Blit。RHI texture 和 PICO Eye Buffer 指向相同的原生 GPU image。

### 3.4 每帧切到 Runtime 指定的当前 image

PICO Runtime 返回下一张可写 image 的 index，插件将 UE 的 `FXRSwapChain` alias 切到同一个 index：

```cpp
// .../PXR_StereoLayer.cpp:765-781
if (SwapChain && SwapChain.IsValid())
{
    int32 index = 0;
#if PLATFORM_ANDROID
    FPICOXRHMDModule::GetPluginWrapper().GetLayerNextImageIndex(
        PxrLayerID, &index);
#endif
    while (index != SwapChain->GetSwapChainIndex_RHIThread())
    {
        SwapChain->IncrementSwapChainIndex_RHIThread();
    }
}
```

所以“同一个”是指：**本帧 `ViewFamilyTexture` 指向 PICO Runtime 为本帧选中的当前 Eye swapchain image。** `FRHITexture` 可能是 aliased wrapper，不能只靠某一帧的 C++ wrapper 地址判断底层原生 image 是否相同。

## 4. PICO Eye Buffer 如何成为 Unreal 的最终 ViewFamily RT

### 4.1 PICO 把当前 swapchain texture 同时返回为 RTV 和 SRV

这是两者同源的最直接证据：

```cpp
// .../PXR_HMD.cpp:2585-2605
bool FPICOXRHMD::AllocateRenderTargetTexture(
    uint32 Index, uint32 SizeX, uint32 SizeY, uint8 Format,
    uint32 NumMips, ETextureCreateFlags Flags,
    ETextureCreateFlags TargetableTextureFlags,
    FTexture2DRHIRef& OutTargetableTexture,
    FTexture2DRHIRef& OutShaderResourceTexture,
    uint32 NumSamples)
{
    check(IsInRenderingThread());
    check(Index == 0);

    if (PXRLayerMap[0].IsValid() && PXREyeLayer_RenderThread.IsValid())
    {
        const FXRSwapChainPtr& SwapChain =
            PXREyeLayer_RenderThread->GetSwapChain();
        if (SwapChain.IsValid())
        {
            OutTargetableTexture = OutShaderResourceTexture =
                SwapChain->GetTexture2DArray()
                    ? SwapChain->GetTexture2DArray()
                    : SwapChain->GetTexture2D();
            bNeedReAllocateViewportRenderTarget = false;
            return true;
        }
    }
    return false;
}
```

`OutTargetableTexture == OutShaderResourceTexture` 不是“内容相同的两张图”，而是同一个 `FTexture2DRHIRef`。

### 4.2 `FSceneViewport` 接受 HMD 分配的纹理

SceneViewport 优先让 XR/HMD 分配 RT；只有分配失败才自行 `RHICreateTexture`：

```cpp
// Engine/Source/Runtime/Engine/Private/Slate/SceneViewport.cpp:2141-2161
// try to allocate texture via StereoRenderingDevice; if not successful,
// use the default way
if (bHMDAllocatedSeparateRenderTargets)
{
    RTRHI = BufferedRTRHI[i];
    SRVRHI = BufferedSRVRHI[i];
}
else if (StereoRenderTargetManager == nullptr ||
         !StereoRenderTargetManager->AllocateRenderTargetTexture(
             i, TexSizeX, TexSizeY, SceneTargetFormat, 1,
             TexCreate_None, TexCreate_RenderTargetable,
             RTRHI, SRVRHI))
{
    // 只有 HMD 没有提供纹理时才走这个 fallback。
    RTRHI = SRVRHI = RHICreateTexture(Desc);
}

BufferedRenderTargetsRHI[i] = RTRHI;
BufferedShaderResourceTexturesRHI[i] = SRVRHI;
```

随后 Viewport 的 render target 指向该 HMD/PICO texture：

```cpp
// .../SceneViewport.cpp:2180-2182
CurrentBufferedTargetIndex = 0;
NextBufferedTargetIndex =
    (CurrentBufferedTargetIndex + 1) % BufferedSlateHandles.Num();
RenderTargetTextureRHI =
    BufferedShaderResourceTexturesRHI[CurrentBufferedTargetIndex];
```

### 4.3 Renderer 将它注册为 `ViewFamilyTexture`

```cpp
// Engine/Source/Runtime/Renderer/Private/ScreenPass.cpp:33-42
FRDGTextureRef TryCreateViewFamilyTexture(
    FRDGBuilder& GraphBuilder,
    const FSceneViewFamily& ViewFamily)
{
    FRHITexture* TextureRHI =
        ViewFamily.RenderTarget->GetRenderTargetTexture();
    FRDGTextureRef Texture = nullptr;
    if (TextureRHI)
    {
        Texture = RegisterExternalTexture(
            GraphBuilder, TextureRHI, TEXT("ViewFamilyTexture"));
        GraphBuilder.SetTextureAccessFinal(Texture, ERHIAccess::RTV);
    }
    return Texture;
}
```

Mobile Renderer 的调用点：

```cpp
// Engine/Source/Runtime/Renderer/Private/MobileShadingRenderer.cpp:1189
FRDGTextureRef ViewFamilyTexture =
    TryCreateViewFamilyTexture(GraphBuilder, ViewFamily);
```

结合 4.1、4.2、4.3 可得：

```text
ViewFamilyTexture
  = ViewFamily.RenderTarget.RenderTargetTextureRHI
  = FSceneViewport.BufferedShaderResourceTexturesRHI[当前项]
  = PICO AllocateRenderTargetTexture 返回的 FXRSwapChain 当前 texture
  = 当前 PICO Eye Buffer 原生 image
```

## 5. Base Pass/Translucency 何时直接写 Eye Buffer

### 5.1 PICO 关闭 UE 内置 HMD Distortion

PICO 的畸变和重投影由 Runtime 完成，所以插件明确返回 `false`：

```cpp
// Engine/PICO-Unreal-Integration-SDK/UE_5.4/Plugins/PICOXR/
// Source/PICOXRHMD/Private/PXR_HMD.h:167-170
/** FHeadMountedDisplayBase interface */
virtual bool GetHMDDistortionEnabled(
    EShadingPath ShadingPath) const override
{
    return false;
}
```

XRBase 会把该返回值写入 View Family ShowFlag：

```cpp
// Engine/Plugins/Runtime/XRBase/Source/XRBase/
// Private/DefaultXRCamera.cpp:211-222
void FDefaultXRCamera::SetupViewFamily(
    FSceneViewFamily& InViewFamily)
{
    const IHeadMountedDisplay* const HMD =
        TrackingSystem->GetHMDDevice();

    if (InViewFamily.Views.Num() > 0 &&
        !InViewFamily.Views[0]->bIsSceneCapture)
    {
        InViewFamily.EngineShowFlags.HMDDistortion =
            HMD != nullptr
                ? HMD->GetHMDDistortionEnabled(
                    InViewFamily.Scene->GetShadingPath())
                : false;
    }
    InViewFamily.EngineShowFlags.StereoRendering =
        bCurrentFrameIsStereoRendering;
}
```

所以正常 PICO 视图是：

```text
StereoRendering = true
HMDDistortion    = false
bStereoRenderingAndHMD = false
```

不能把普通 XR 中“Stereo + UE HMD Distortion”的中间 RT 规则直接套到 PICO。

### 5.2 `bRenderToSceneColor` 的真实条件

Mobile Renderer 的完整条件是：

```cpp
// Engine/Source/Runtime/Renderer/Private/MobileShadingRenderer.cpp:498-519
FIntPoint RenderTargetSize = ViewFamily.RenderTarget->GetSizeXY();
EPixelFormat RenderTargetPixelFormat = PF_Unknown;
if (ViewFamily.RenderTarget->GetRenderTargetTexture().IsValid())
{
    RenderTargetSize =
        ViewFamily.RenderTarget->GetRenderTargetTexture()->GetSizeXY();
    RenderTargetPixelFormat =
        ViewFamily.RenderTarget->GetRenderTargetTexture()->GetFormat();
}

const bool bStereoRenderingAndHMD =
    ViewFamily.EngineShowFlags.StereoRendering &&
    ViewFamily.EngineShowFlags.HMDDistortion;

bRenderToSceneColor =
       !bGammaSpace
    || bStereoRenderingAndHMD
    || bRequiresUpscale
    || bShouldCompositeEditorPrimitives
    || Views[0].bIsSceneCapture
    || Views[0].bIsReflectionCapture
    || /* MSAA/format 等其他条件 */;
```

`bGammaSpace` 在构造函数中等于 `!IsMobileHDR()`：

```cpp
// .../MobileShadingRenderer.cpp:287-294
FMobileSceneRenderer::FMobileSceneRenderer(/* ... */)
    : FSceneRenderer(InViewFamily, HitProxyConsumer)
    , bGammaSpace(!IsMobileHDR())
    // ...
{
    bRenderToSceneColor = false;
}
```

因此 PICO 的常见分支是：

| 条件 | `bRenderToSceneColor` | Base Pass 主颜色去向 |
|---|---:|---|
| Mobile HDR 关闭、1x（无 MSAA）、无 Upscale/Full Prepass 等额外条件 | `false` | 直接绑定 `ViewFamilyTexture`，即当前 Eye Buffer。 |
| Mobile HDR 关闭、MSAA > 1、且平台允许该 direct-resolve 分支 | `false` | 写 `SceneTextures.Color.Target` 的 MSAA surface，Resolve attachment 是 Eye Buffer。 |
| Mobile HDR 开启 | `true`，因为 `!bGammaSpace` | 写中间 Scene Color，再由 Tonemap/后处理写 Eye Buffer。 |
| Upscale、Scene Capture、Reflection、特定 MSAA/格式条件或 Full Depth Prepass | `true` | 写中间 Scene Color，再输出到 Eye Buffer。 |

### 5.3 `SV_Target0` 的实际绑定

```cpp
// .../MobileShadingRenderer.cpp:1448-1480
FRenderTargetBindingSlots
FMobileSceneRenderer::InitRenderTargetBindings_Forward(
    FRDGTextureRef ViewFamilyTexture,
    FSceneTextures& SceneTextures)
{
    FRDGTextureRef SceneColor = nullptr;
    FRDGTextureRef SceneColorResolve = nullptr;

    if (!bRenderToSceneColor)
    {
        if (bMobileMSAA)
        {
            SceneColor = SceneTextures.Color.Target;
            SceneColorResolve = ViewFamilyTexture;
        }
        else
        {
            SceneColor = ViewFamilyTexture;
        }
    }
    else
    {
        SceneColor = SceneTextures.Color.Target;
        SceneColorResolve =
            bMobileMSAA ? SceneTextures.Color.Resolve : nullptr;
    }

    FRenderTargetBindingSlots BasePassRenderTargets;
    BasePassRenderTargets[0] = FRenderTargetBinding(
        SceneColor, SceneColorResolve,
        ERenderTargetLoadAction::EClear);
```

由这段代码可直接得到三种情况：

```text
1. !bRenderToSceneColor && !bMobileMSAA
   BasePassRenderTargets[0] = ViewFamilyTexture = PICO Eye Buffer

2. !bRenderToSceneColor && bMobileMSAA
   BasePassRenderTargets[0]         = SceneTextures.Color.Target (MSAA)
   BasePassRenderTargets[0].Resolve = ViewFamilyTexture = PICO Eye Buffer

3. bRenderToSceneColor
   BasePassRenderTargets[0] = SceneTextures.Color.Target
   最后的 Resolve/Tonemap/Post Process 再写 Eye Buffer
```

因此，PICO XR 的 LDR 1x 路径完全可能直接把 Eye Buffer 绑定为 `SV_Target0`；“XR 一定先写 Scene Color”在这个插件上不成立。

### 5.4 Opaque 和普通 Translucency 使用同一组绑定

在 Mobile Forward single-pass 路径中，`PassParameters->RenderTargets` 已经是上面的 `BasePassRenderTargets`。Opaque 与 Translucency 都在这个 raster pass 内执行：

```cpp
// .../MobileShadingRenderer.cpp:1590-1623
GraphBuilder.AddPass(
    RDG_EVENT_NAME("SceneColorRendering"),
    PassParameters,
    ERDGPassFlags::Raster | ERDGPassFlags::NeverMerge,
    [this, PassParameters, ViewContext, &SceneTextures]
    (FRHICommandList& RHICmdList)
{
    FViewInfo& View = *ViewContext.ViewInfo;

    // Opaque and masked
    RenderMobileBasePass(
        RHICmdList, View,
        &PassParameters->InstanceCullingDrawParams);

    RHICmdList.NextSubpass();

    // Draw translucency.
    RenderTranslucency(RHICmdList, View);
});
```

Multi-pass 路径会为 Translucency 建第二个 pass，但它复制同一个 `PassParameters`，只把 RT0 的 LoadAction 改成 `ELoad`，仍然写同一张 Scene Color：

```cpp
// .../MobileShadingRenderer.cpp:1707-1715
FMobileRenderPassParameters* SecondPassParameters =
    GraphBuilder.AllocParameters<FMobileRenderPassParameters>();
*SecondPassParameters = *PassParameters;
SecondPassParameters->MobileBasePass =
    CreateMobileBasePassUniformBuffer(
        GraphBuilder, View, EMobileBasePass::Translucent, SetupMode);
SecondPassParameters->RenderTargets[0].SetLoadAction(
    ERenderTargetLoadAction::ELoad);
```

所以你列出的 Mobile Base Pass Pixel Shader 的 `OutColor` 写入哪个对象，取决于本帧 `BasePassRenderTargets[0]`：

- PICO LDR、1x、无额外条件：**直接写 Eye Buffer**。
- PICO LDR + MSAA：写 MSAA Scene Color，随后 Resolve 到 Eye Buffer。
- Mobile HDR/Upscale 等：写中间 Scene Color，最后再输出到 Eye Buffer。

## 6. 你给出的半透明代码究竟修改哪张纹理

### 6.1 Pixel Shader 输出 RGB 和 Opacity

当前源码的准确行号是 1061-1062：

```hlsl
// Engine/Shaders/Private/MobileBasePassPixelShader.usf:1061-1062
#elif MATERIALBLENDING_TRANSLUCENT
    OutColor = half4(
        Color * VertexFog.a + VertexFog.rgb,
        Opacity);
```

此处的 `OutColor` 最终对应 `SV_Target0`。它没有固定绑定到某个名为 Eye Buffer 的全局对象；它写入哪张纹理完全由当前 render pass 的 attachment 0 决定。

根据第 5 节的绑定链，在 PICO XR Mobile Forward 中可能是：

```text
OutColor/SV_Target0 → BasePassRenderTargets[0]
                    ├─ LDR 1x：ViewFamilyTexture / Eye Buffer
                    ├─ LDR MSAA：MSAA Scene Color，随后 Resolve 到 Eye Buffer
                    └─ HDR/Upscale 等：中间 Scene Color，随后后处理到 Eye Buffer
```

### 6.2 Blend State 的 RGB/Alpha 方程

当前源码与你给出的代码一致：

```cpp
// Engine/Source/Runtime/Renderer/Private/MobileBasePass.cpp:652-673
case BLEND_Translucent:
    if (Material.ShouldWriteOnlyAlpha())
    {
        // ...
    }
    else
    {
        DrawRenderState.SetBlendState(TStaticBlendState<
            CW_RGBA,
            BO_Add, BF_SourceAlpha, BF_InverseSourceAlpha,
            BO_Add, BF_Zero,        BF_InverseSourceAlpha,
            // 其他 MRT 为 CW_NONE ...
        >::GetRHI());
    }
    break;
```

令 Pixel Shader 输出为：

```text
Src.rgb = Color * VertexFog.a + VertexFog.rgb
Src.a   = Opacity
```

硬件混合结果为：

```text
Out.rgb = Src.rgb * Src.a + Dst.rgb * (1 - Src.a)
Out.a   = Src.a   * 0     + Dst.a   * (1 - Src.a)
        = Dst.a * (1 - Opacity)
```

因此你说的“用 `InverseSourceAlpha` 衰减目标 Alpha”是准确的。被衰减的是 **当前绑定的 attachment 0 的 Alpha**：LDR 1x direct path 中就是 Eye Buffer Alpha；MSAA/HDR 等路径中则先是中间 Scene Color Alpha。

### 6.3 这个 Alpha 不是常规的“累计不透明度”，而是背景透过率

UE 源码直接说明 Scene Color Alpha 的语义：1 表示显示背景，0 表示前景完全存在。

```cpp
// Engine/Source/Runtime/Renderer/Private/BasePassRendering.cpp:122-123
// Scene color alpha is used during scene captures and planar reflections.
// 1 indicates background should be shown,
// 0 indicates foreground is fully present.
static const float kSceneColorClearAlpha = 1.0f;
```

Scene Color 默认使用 `FClearValueBinding::Black`：

```cpp
// Engine/Source/Runtime/Engine/Private/SceneTexturesConfig.cpp:267-269
ColorFormat    = PF_Unknown;
ColorClearValue = FClearValueBinding::Black;
DepthClearValue = FClearValueBinding::DepthFar;
```

而 `Black` 的 Alpha 确实是 1：

```cpp
// Engine/Source/Runtime/RHI/Private/RHI.cpp:105
const FClearValueBinding FClearValueBinding::Black(
    FLinearColor(0.0f, 0.0f, 0.0f, 1.0f));
```

普通 Opaque Mobile Base Pass 会写 Alpha 0：

```hlsl
// Engine/Shaders/Private/MobileBasePassPixelShader.usf:1068-1073
#else
    OutColor.rgb = Color * VertexFog.a + VertexFog.rgb;

    #if !MATERIAL_USE_ALPHA_TO_COVERAGE
        OutColor.a = 0.0;
```

当目标初始 Alpha 为 1 时，这套 Alpha 是“还剩多少背景/VST 可以显示”：

- 清屏区域：`A = 1`，完全显示背景。
- Opaque 区域：`A = 0`，完全显示虚拟前景。
- 一个 Opacity 为 `a` 的透明表面覆盖清屏：`A = 1 × (1-a)`。
- 多个透明层：`A = ∏(1-a_i)`。

这也解释了 Alpha 分量为什么使用：

```text
SrcFactorAlpha = 0
DstFactorAlpha = 1 - SrcAlpha
```

它是在累积 **transmittance/背景透过率**，而不是累积常规 coverage alpha。

### 6.4 当前 PICO 代码中，Vulkan 与 OpenGL 的 Eye Buffer Clear Alpha 不同

上面的 `SceneTexturesConfig.ColorClearValue = Black` 证明的是 **UE 中间 Scene Color** 默认清成 Alpha 1。它不能自动证明 direct path 下 PICO Eye Buffer 也清成 Alpha 1。

OpenGL wrapper 明确使用 `FClearValueBinding::Black`，即 Alpha 1：

```cpp
// .../PXR_HMDRenderBridge_OpenGL.cpp:16-25
case RRT_Texture2D:
    return DynamicRHI->RHICreateTexture2DFromResource(
        /* ... */, FClearValueBinding::Black,
        (GLuint)InTexture, TargetableTextureFlags).GetReference();

case RRT_Texture2DArray:
    return DynamicRHI->RHICreateTexture2DArrayFromResource(
        /* ... */, FClearValueBinding::Black,
        (GLuint)InTexture, TargetableTextureFlags).GetReference();
```

Vulkan 2D wrapper 没有传 clear binding，因此使用接口默认值 `Transparent`；2DArray wrapper 传入默认构造的 `FClearValueBinding()`：

```cpp
// .../PXR_HMDRenderBridge_Vulkan.cpp:48-69
FClearValueBinding ColorTextureBinding = FClearValueBinding();

case RRT_Texture2D:
    return GVulkanRHI->RHICreateTexture2DFromResource(
        /* ... */, (VkImage)InTexture,
        TargetableTextureFlags).GetReference();

case RRT_Texture2DArray:
    return GVulkanRHI->RHICreateTexture2DArrayFromResource(
        /* ... */, (VkImage)InTexture,
        TargetableTextureFlags,
        ColorTextureBinding).GetReference();
```

Vulkan 接口的默认值是 `Transparent`：

```cpp
// Engine/Source/Runtime/VulkanRHI/Public/IVulkanDynamicRHI.h:54-55
virtual FTexture2DRHIRef RHICreateTexture2DFromResource(
    /* ... */,
    const FClearValueBinding& ClearValueBinding =
        FClearValueBinding::Transparent) = 0;
```

并且默认构造的 `FClearValueBinding()` 也是全 0：

```cpp
// Engine/Source/Runtime/RHI/Public/RHIResources.h:230-236
FClearValueBinding()
    : ColorBinding(EClearBinding::EColorBound)
{
    Value.Color[0] = 0.0f;
    Value.Color[1] = 0.0f;
    Value.Color[2] = 0.0f;
    Value.Color[3] = 0.0f;
}
```

常量定义进一步确认：

```cpp
// Engine/Source/Runtime/RHI/Private/RHI.cpp:105,108
const FClearValueBinding FClearValueBinding::Black(
    FLinearColor(0.0f, 0.0f, 0.0f, 1.0f));
const FClearValueBinding FClearValueBinding::Transparent(
    FLinearColor(0.0f, 0.0f, 0.0f, 0.0f));
```

所以当前插件代码下：

- OpenGL direct Eye path 的绑定 clear alpha 是 1。
- Vulkan direct Eye path 的绑定 clear alpha 是 0。
- 若 Vulkan direct path 真正以 0 清屏，则 `Aout = Adst × (1-Opacity)` 会一直保持 0；不能仅靠半透明 Blend State 从 0 生成背景透过率。

如果目标是用 PICO Clip Mode 显示 VST，必须在真机 capture 中确认 Eye Buffer 的实际清屏 Alpha。需要背景区域为 1 时，应让 Eye Buffer clear binding/显式 clear 使用 Alpha 1；这是当前 Vulkan wrapper 与中间 Scene Color 路径的一个实质差异。

## 7. 中间 Scene Color 如何进入 PICO Eye Buffer

本节只适用于 `bRenderToSceneColor == true` 的 Mobile HDR/Upscale 等路径。LDR 1x direct path 已经把 Eye Buffer 作为 attachment 0，不需要这次后处理写回；LDR MSAA direct-resolve 路径则在 render pass resolve 时进入 Eye Buffer。

Mobile Renderer 把 `ViewFamilyTexture` 作为移动后处理的最终输出：

```cpp
// Engine/Source/Runtime/Renderer/Private/MobileShadingRenderer.cpp:1378-1405
if (ViewFamily.bResolveScene)
{
    if (bRenderToSceneColor && !bTonemapSubpassInline)
    {
        FMobilePostProcessingInputs PostProcessingInputs;
        PostProcessingInputs.ViewFamilyTexture = ViewFamilyTexture;
        PostProcessingInputs.SceneTextures =
            CreateMobileSceneTextureUniformBuffer(
                GraphBuilder, &SceneTextures,
                EMobileSceneTextureSetupMode::All);

        AddMobilePostProcessingPasses(
            GraphBuilder, Scene, Views[ViewIndex],
            GetSceneUniforms(), PostProcessingInputs,
            InstanceCullingManager);
    }
}
```

移动后处理用 `SceneColorTexture` 作为输入，并把 `ViewFamilyTexture` 建成最终输出：

```cpp
// Engine/Source/Runtime/Renderer/Private/PostProcess/PostProcessing.cpp:2108-2117
const FScreenPassRenderTarget ViewFamilyOutput =
    FScreenPassRenderTarget::CreateViewFamilyOutput(
        Inputs.ViewFamilyTexture, View);

// Scene color is updated incrementally through the post process pipeline.
FScreenPassTexture SceneColor(
    (*Inputs.SceneTextures)->SceneColorTexture,
    FinalOutputViewRect);
```

最后一个有效后处理 pass 会通过 `PassSequence.AcceptOverrideIfLastPass(...)` 被直接定向到 `ViewFamilyOutput`。常见的最后一步是 Tonemap：

```cpp
// .../PostProcessing.cpp:2705-2736
FTonemapInputs TonemapperInputs;
PassSequence.AcceptOverrideIfLastPass(
    EPass::Tonemap, TonemapperInputs.OverrideOutput);

// This is the view family render target.
if (TonemapperInputs.OverrideOutput.Texture)
{
    TonemapperInputs.OverrideOutput.ViewRect = OutputViewRect;
    TonemapperInputs.OverrideOutput.LoadAction = OutputLoadAction;
}

TonemapperInputs.SceneColor =
    FScreenPassTextureSlice::CreateFromScreenPassTexture(
        GraphBuilder, SceneColor);

SceneColor = AddTonemapPass(
    GraphBuilder, View, TonemapperInputs);
```

中间路径的颜色关系是：

```text
材质 OutColor
  → 中间 SceneTextures.Color.Target
  → 可能经过 MSAA Resolve、Tonemap、Upscale、FXAA、后处理材质等
  → 最后一个 pass 写 ViewFamilyTexture
  → 同一个当前 PICO Eye Buffer
```

Base Pass 材质颜色最终会影响 Eye Buffer；在该中间路径中不是 Base Pass draw 直接写 Eye Buffer。LDR 1x direct path 则相反，Base Pass draw 就直接写 Eye Buffer。

## 8. Alpha 能否从 Scene Color 保留到 Eye Buffer

这是 PICO Clip/VST 合成中最容易漏掉的一层，但只在存在中间 Scene Color/Tonemap 时需要。Direct path 的问题不是“后处理是否传播”，而是 Eye Buffer 自身的 clear alpha 和材质 draw 如何写 alpha。

### 8.1 `r.Mobile.PropagateAlpha` 默认关闭

```cpp
// Engine/Source/Runtime/Renderer/Private/MobileBasePassRendering.cpp:52-57
static TAutoConsoleVariable<int32> CVarMobilePropagateAlpha(
    TEXT("r.Mobile.PropagateAlpha"),
    0,
    TEXT("0: Disabled")
    TEXT("1: Propagate Full Alpha Propagate"),
    ECVF_ReadOnly | ECVF_RenderThreadSafe);
```

它是 ReadOnly/着色器编译配置，应放在目标平台配置中并重新编译 shader，不能把它当成普通运行时开关。

中间 Scene Color 是否要求带 Alpha 通道也由它决定：

```cpp
// Engine/Source/Runtime/Renderer/Private/SceneTextures.cpp:395-407
void InitializeSceneTexturesConfig(
    FSceneTexturesConfig& Config,
    const FSceneViewFamily& ViewFamily)
{
    EShadingPath ShadingPath =
        GetFeatureLevelShadingPath(ViewFamily.GetFeatureLevel());

    bool bRequiresAlphaChannel =
        ShadingPath == EShadingPath::Mobile
            ? IsMobilePropagateAlphaEnabled(
                ViewFamily.GetShaderPlatform())
            : false;
```

移动平台开启该配置时，Shader Compiler 将 `POST_PROCESS_ALPHA` 定义为 2：

```cpp
// Engine/Source/Runtime/Engine/Private/ShaderCompiler/ShaderCompiler.cpp:8585-8600
static IConsoleVariable* CVar =
    IConsoleManager::Get().FindConsoleVariable(
        TEXT("r.PostProcessing.PropagateAlpha"));
int32 PropagateAlpha = CVar->GetInt();

if (bIsMobilePlatform)
{
    static FShaderPlatformCachedIniValue<int32>
        MobilePropagateAlphaIniValue(
            TEXT("r.Mobile.PropagateAlpha"));
    int MobilePropagateAlphaIniValueInt =
        MobilePropagateAlphaIniValue.Get(
            (EShaderPlatform)ShaderPlatform);
    PropagateAlpha =
        MobilePropagateAlphaIniValueInt > 0 ? 2 : 0;
}

SET_SHADER_DEFINE(
    Input.Environment, POST_PROCESS_ALPHA, PropagateAlpha);
```

Tonemap Shader 只在相应 Alpha 模式下复制 Scene Color Alpha：

```hlsl
// Engine/Shaders/Private/PostProcessTonemap.usf:454,524-526
float4 OutColor = 0;

#if POST_PROCESS_ALPHA == 2 || \
    (POST_PROCESS_ALPHA == 1 && \
     DIM_OUTPUT_DEVICE == TONEMAPPER_OUTPUT_NoToneCurve) || \
    (POST_PROCESS_ALPHA == 1 && \
     DIM_OUTPUT_DEVICE == TONEMAPPER_OUTPUT_WithToneCurve)
    OutColor.a = SceneColor.a;
#endif
```

注意，Tonemap C++ 因 Stereo View 而启用 RGBA 写掩码，并不等于 Shader 一定保留了输入 Alpha：

```cpp
// Engine/Source/Runtime/Renderer/Private/PostProcess/
// PostProcessTonemap.cpp:550-556
bool ShouldWriteAlphaChannel(
    const FViewInfo& View,
    const FTonemapInputs& Inputs,
    const FRDGTextureRef Output)
{
    const bool bIsStereo =
        IStereoRendering::IsStereoEyeView(View);
    const bool bFormatNeedsAlphaWrite =
        Output->Desc.Format == PF_R9G9B9EXP5;
    return Inputs.bWriteAlphaChannel ||
        bIsStereo || bFormatNeedsAlphaWrite;
}
```

真正决定 `OutColor.a = SceneColor.a` 的仍是前面的 `POST_PROCESS_ALPHA`。它为 0 时，Shader 中 `OutColor` 初始为 0；Stereo 只会让这个 Alpha 分量被写出去，而不会自动把输入 Alpha 复制过来。

因此，对 **中间 Scene Color → Tonemap → Eye Buffer** 路径：

- `r.Mobile.PropagateAlpha = 1`：普通移动 Tonemap 路径可以把上述背景透过率从中间 Scene Color 带到 PICO Eye Buffer。
- 默认 `r.Mobile.PropagateAlpha = 0`：不能假定中间 Scene Color 的材质 Alpha 会原样到达 Eye Buffer。
- 当前 PICOXR 插件源码中没有自动设置 `r.Mobile.PropagateAlpha`；需要由项目配置决定。

对 LDR 1x direct Eye path，不经过 Tonemap，所以不依赖该开关来完成“Scene Color 到 Eye Buffer”的复制；但必须检查第 6.4 节所述的 Eye Buffer clear alpha。LDR MSAA direct-resolve 路径还需要确认 Alpha 随颜色一起正确 Resolve。

### 8.2 Mobile Tonemap Subpass 会显式把最终 Alpha 写成 1

`r.Mobile.TonemapSubpass` 默认是 0。若项目显式开启它，Vulkan inline custom resolve 把 `ViewFamilyTexture` 作为第二个 attachment：

```cpp
// .../MobileShadingRenderer.cpp:1486-1492
if (bTonemapSubpassInline)
{
    BasePassRenderTargets[0].SetResolveTexture(nullptr);
    BasePassRenderTargets[1] = FRenderTargetBinding(
        ViewFamilyTexture, nullptr,
        ERenderTargetLoadAction::EClear);
}
```

Base Pass `OutColor` 仍写 attachment 0，即中间 Scene Color；后续 custom resolve subpass 才写 attachment 1/Eye Buffer。但该 shader 明确覆盖 Alpha：

```hlsl
// Engine/Shaders/Private/PostProcessTonemap.usf:700-710
half4 SceneColor = FetchAndResolveSceneColor(UV, ArrayIndex);
SceneColor.a = 1.0;

half3 LinearColor = SceneColor.rgb * OneOverPreExposure;
half3 DeviceColor = ColorLookupTable(LinearColor);

OutColor.rgb = DeviceColor;
OutColor.a = 1.0;
```

所以需要依赖 Eye Buffer Alpha 做 VST Clip/MR 合成时，不应把这个 subpass 当作“自动保留 Alpha”的路径；应保持其关闭，或修改/验证 custom resolve 的 Alpha 行为。

## 9. Unreal 材质混合与 PICO Runtime Layer 混合是两次不同的混合

### 9.1 第一次：UE 材质在当前颜色 attachment 中混合

这是 `MobileBasePass.cpp` 中的 Blend State：

```text
虚拟半透明材质 Src
        +
当前 UE 颜色 attachment Dst
        ↓
更新后的 attachment RGBA
```

这里解决的是虚拟场景内部的 Opaque/Translucent 排序和颜色合成。该 attachment 在 LDR 1x direct path 中就是 Eye Buffer，在其他路径中可以是 MSAA/中间 Scene Color。

### 9.2 第二次：PICO Runtime 将整张 Eye Layer 与 VST/其他 Layer 合成

主 Eye Layer 提交时，PICOXR 单独设置 Runtime Layer Blend。默认 CVar 是 Clip Mode：

```cpp
// Engine/PICO-Unreal-Integration-SDK/UE_5.4/Plugins/PICOXR/
// Source/PICOXRHMD/Private/PXR_HMD.cpp:90-96
static TAutoConsoleVariable<int32> CVarPICOBlendModeSetting(
    TEXT("r.Mobile.PICO.BlendModeSetting"),
    1,
    TEXT("0: Covering Mode, VST will cover the entire screen\n")
    TEXT("1: Clip Mode,Eye Buffer and VST will clip by Alpha Before add(Default)\n")
    TEXT("2: Additive Mode, Eye Buffer will not clip by Alpha\n"),
    ECVF_Scalability | ECVF_RenderThreadSafe);
```

Clip Mode 的 Runtime blend factors 是：

```cpp
// .../PXR_StereoLayer.cpp:819-848
layerProjection.header.useLayerBlend = 1;
PxrLayerBlend layerBlend = {};

case EPICOXRBlendModeType::ClipMode:
{
    layerBlend.srcColor =
        PxrBlendFactor::PXR_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    layerBlend.dstColor =
        PxrBlendFactor::PXR_BLEND_FACTOR_SRC_ALPHA;
}
break;

layerBlend.srcAlpha = PxrBlendFactor::PXR_BLEND_FACTOR_ONE;
layerBlend.dstAlpha = PxrBlendFactor::PXR_BLEND_FACTOR_ONE;
layerProjection.header.layerBlend = layerBlend;
```

按标准 blend factor 语义，Clip Mode 的颜色可写成：

```text
CompositorColor
  = EyeColor * (1 - EyeAlpha)
  + VST/BackgroundColor * EyeAlpha
```

这正好使用第 6.3 节的“背景透过率”语义：

- Eye Alpha = 0：显示虚拟 Eye Color。
- Eye Alpha = 1：显示 VST/背景。
- 0 到 1：虚拟图像与 VST 按透过率合成。

这里的 blend factors 是提交给 PICO Runtime 的整层混合，和 UE 材质 draw call 的 `BF_SourceAlpha/BF_InverseSourceAlpha` 不是同一个 Blend State，也不发生在同一个 render pass。

## 10. Eye Buffer 如何提交，而不是直接 Present 到物理屏幕

帧开始时 PICO Runtime 选定下一张 swapchain image；帧结束时，PICOXR 提交所有 Layer，最后调用 `EndFrame`：

```cpp
// .../PXR_HMD.cpp:3300-3322
void FPICOXRHMD::OnRHIFrameEnd_RHIThread()
{
    if (GameFrame_RHIThread.IsValid())
    {
        TArray<FPICOLayerPtr> Layers = PXRLayers_RHIThread;
        Layers.Sort(FLayerPtr_CompareByAll());

        for (int32 LayerIndex = 0;
             LayerIndex < Layers.Num();
             LayerIndex++)
        {
            if (Layers[LayerIndex]->IsVisible())
            {
                Layers[LayerIndex]->SubmitLayer_RHIThread(
                    GameSettings_RHIThread.Get(),
                    GameFrame_RHIThread.Get());
            }
        }
        FPICOXRHMDModule::GetPluginWrapper().EndFrame();
    }
}
```

主 Eye Layer 走 `SubmitLayer2`：

```cpp
// .../PXR_StereoLayer.cpp:951-952
FPICOXRHMDModule::GetPluginWrapper().SubmitLayer2(
    (PxrLayerHeader2*)&layerProjection);
```

PICO Custom Present 本身返回 `false`：

```cpp
// .../PXR_HMDRenderBridge.cpp:27-34,57-58
bool FPICOXRRenderBridge::NeedsNativePresent()
{
    return false;
}

bool FPICOXRRenderBridge::Present(int32& InOutSyncInterval)
{
    PICOXRHMD->OnRHIFrameEnd_RHIThread();
    // ...
    InOutSyncInterval = 0;
    return false;
}
```

`FRHICustomPresent` 接口明确规定：返回 `false` 表示不要再执行 native Present。

```cpp
// Engine/Source/Runtime/RHI/Public/RHIResources.h:3507-3512
// Called from RHI thread to perform custom present.
// @return true if native Present should be also be performed;
// false otherwise.
virtual bool Present(int32& InOutSyncInterval) = 0;
```

Android Vulkan RHI 只在 `bNeedNativePresent == true` 时 Present 普通 viewport swapchain：

```cpp
// Engine/Source/Runtime/VulkanRHI/Private/VulkanViewport.cpp:1005-1018
bool bNeedNativePresent = true;
if (bHasCustomPresent)
{
    bNeedNativePresent = CustomPresent->Present(SyncInterval);
}

if (bNeedNativePresent && /* ... */)
{
    // Present the back buffer to the viewport window.
}
```

OpenGL ES 路径也一样：

```cpp
// Engine/Source/Runtime/OpenGLDrv/Private/Android/AndroidOpenGL.cpp:267-277
if (bPresent && Viewport.GetCustomPresent())
{
    bPresent = Viewport.GetCustomPresent()->Present(SyncInterval);
}
if (bPresent)
{
    FAndroidPlatformRHIFramePacer::SwapBuffers(bLockToVsync);
}
```

PICO 返回 `false` 后，普通 Android `SwapBuffers`/Vulkan viewport present 被跳过。这证明 Eye Buffer 不是应用普通窗口的物理屏幕 backbuffer；它由 `SubmitLayer2/EndFrame` 交给 PICO Runtime。

## 11. 最终回答你的两个具体问题

### 问题一：PICO SDK 的 Eye Buffer 和引擎最终输出颜色 RT 是同一个吗？

**如果“引擎最终输出颜色 RT”指 `ViewFamilyTexture`，是同一个底层纹理。**

证据闭环是：

```text
Pxr_GetLayerImage 原生 image
→ RHICreateTexture*FromResource
→ FXRSwapChain 当前 aliased texture
→ FPICOXRHMD::AllocateRenderTargetTexture
→ FSceneViewport::RenderTargetTextureRHI
→ TryCreateViewFamilyTexture
→ ViewFamilyTexture
```

但它不是物理面板 RT。Eye Buffer 最后作为 PICO Projection Layer 被提交给 Runtime，由 Runtime 生成真正的面板输出。

### 问题二：你列出的 Blend State 和 Pixel Shader 直接写 Eye Buffer 吗？

**条件决定；满足下述条件的 PICO Mobile LDR 1x 路径会直接写 Eye Buffer。**

不能只看 Pixel Shader 文件名判断 RT，必须看 `InitRenderTargetBindings_Forward`：

```text
Mobile HDR 关闭 + 1x + 无 Upscale 等条件
    OutColor → ViewFamilyTexture → 直接写当前 PICO Eye Buffer

Mobile HDR 关闭 + MSAA
    OutColor → MSAA Scene Color → render-pass Resolve → PICO Eye Buffer

Mobile HDR/Upscale/其他中间路径
    OutColor → SceneTextures.Color.Target
             → Tonemap/Post Process → PICO Eye Buffer
```

PICO 不会由 `bStereoRenderingAndHMD` 强制走中间路径，因为它返回 `GetHMDDistortionEnabled() == false`，所以该布尔项为 false。Mobile HDR、MSAA、Upscale 和其他条件才决定是否存在中间颜色 RT。

你给出的 Alpha Blend 确实执行：

```text
CurrentColorTarget.a =
    CurrentColorTarget.a * (1 - Opacity)
```

若项目需要 PICO Clip Mode 使用这个 Alpha，还必须按实际分支确认：

1. Direct Eye path：检查 Eye Buffer 的实际 clear alpha。当前插件 OpenGL wrapper 是 1，而 Vulkan wrapper 是 0。
2. MSAA path：确认 Alpha 与颜色一起正确 Resolve 到 Eye Buffer。
3. 中间 Scene Color + Tonemap path：目标平台配置中应开启 `r.Mobile.PropagateAlpha = 1` 并重新编译移动 shader。
4. 确认后续 pass 没有覆盖 Alpha。特别是 `r.Mobile.TonemapSubpass = 1` 的 custom resolve 在当前代码中显式写 `OutColor.a = 1.0`；该 CVar 默认是 0。
5. `r.Mobile.PICO.BlendModeSetting = 1` 的 Runtime Clip blend 是第二次、整层级别的合成，不等于材质 Blend State。

## 12. 建议的运行时验证点

如果要在真机 GPU capture 或断点中验证，最有价值的观察点是：

1. `PXR_StereoLayer.cpp:584`：记录 `Pxr_GetLayerImage` 返回的原生 image handle。
2. `PXR_HMDRenderBridge_Vulkan.cpp:66/69`：确认传给 `RHICreateTexture*FromResource` 的 `VkImage` 是同一 handle。
3. `PXR_HMD.cpp:2599`：确认 `AllocateRenderTargetTexture` 返回当前 `FXRSwapChain` texture。
4. `ScreenPass.cpp:35-40`：确认该 RHI texture 被注册成 `ViewFamilyTexture`。
5. `MobileShadingRenderer.cpp:509-519`：记录本帧 `bRenderToSceneColor`；不要预设它在 PICO 上一定为 true。
6. `MobileShadingRenderer.cpp:1459-1480`：检查 color attachment 0。LDR 1x 时应是 PICO image；MSAA/HDR 等路径时应是 Scene Color。
7. 若 `bRenderToSceneColor == true`，在 `PostProcessing.cpp:2705-2736` 检查最后 Tonemap/后处理输出 attachment 是 PICO image。
8. 检查 Eye Buffer clear：OpenGL wrapper 使用 Alpha 1；当前 Vulkan wrapper 使用 Alpha 0。
9. `PXR_StereoLayer.cpp:951`：该 image 所属的 Eye Layer 随后由 `SubmitLayer2` 提交。

这些点能同时验证 direct/intermediate 分支、Alpha 初值，以及“最终 ViewFamily RT 与 Eye Buffer 同源”。

## 13. `SceneTextures.Color.Target` / “Scene Color” 是否独立于 Eye Buffer

### 13.1 先给结论

需要区分一个**确定的成员名**和一个经常被复用的**概念名/局部变量名**：

- `SceneTextures.Color.Target`：**是 UE 创建的内部 Scene Color 资源；它独立于 PICO Eye Buffer。**
- `ViewFamilyTexture`：在当前 PICO XR 主视图中，是从 PICO swapchain 当前 Eye image 包装、注册进 RDG 的外部纹理，因此它指向 Eye Buffer。
- 名为 `SceneColor` 的局部变量：**不一定是 `SceneTextures.Color.Target`**。在 Mobile LDR、1x MSAA（即无 MSAA）的直写分支中，它被直接赋值为 `ViewFamilyTexture`，所以该局部变量此时就是 Eye Buffer。
- 泛称 “Scene Color”：语义依赖上下文。若明确指 `SceneTextures.Color.Target`，它与 Eye Buffer 独立；若只是指当前 Scene Color color attachment，则它可能就是 Eye Buffer。

因此，不能用变量显示名或 RenderDoc 中类似 “SceneColor” 的标签判断资源身份，必须追踪当帧 RT binding 中实际的 `FRDGTextureRef`/RHI texture。

### 13.2 `SceneTextures.Color.Target` 的创建证明：内部 RDG 纹理

UE 根据 `FSceneTexturesConfig` 构造 Scene Color 描述，然后调用 `CreateTextureMSAA`：

```cpp
// Engine/Source/Runtime/Renderer/Private/SceneTextures.cpp:480-492
// Scene Color
{
    const bool bIsMobilePlatform = Config.ShadingPath == EShadingPath::Mobile;
    const ETextureCreateFlags sRGBFlag =
        (bIsMobilePlatform && IsMobileColorsRGB()) ? TexCreate_SRGB : TexCreate_None;

    // Create the scene color.
    FRDGTextureDesc Desc(Config.bRequireMultiView ?
        FRDGTextureDesc::Create2DArray(
            Config.Extent, Config.ColorFormat, Config.ColorClearValue,
            Config.ColorCreateFlags, 2) :
        FRDGTextureDesc::Create2D(
            Config.Extent, Config.ColorFormat, Config.ColorClearValue,
            Config.ColorCreateFlags));
    Desc.NumSamples = Config.NumSamples;
    SceneTextures.Color = CreateTextureMSAA(
        GraphBuilder, Desc,
        TEXT("SceneColorMS"), TEXT("SceneColor"),
        GFastVRamConfig.SceneColor | sRGBFlag);
}
```

`CreateTextureMSAA` 最终调用的是 `GraphBuilder.CreateTexture`，没有接收或注册 PICO 的外部 Eye image：

```cpp
// Engine/Source/Runtime/RenderCore/Private/RenderGraphUtils.cpp:204-235
RENDERCORE_API FRDGTextureMSAA CreateTextureMSAA(
    FRDGBuilder& GraphBuilder,
    FRDGTextureDesc Desc,
    const TCHAR* NameMultisampled, const TCHAR* NameResolved,
    ETextureCreateFlags ResolveFlagsToAdd)
{
    const bool bForceSeparateTargetAndShaderResource =
        Desc.NumSamples > 1 &&
        RHISupportsSeparateMSAAAndResolveTextures(GMaxRHIShaderPlatform);

    if (LIKELY(bForceSeparateTargetAndShaderResource))
    {
        FRDGTextureMSAA Texture(
            GraphBuilder.CreateTexture(Desc, NameMultisampled));

        Desc.NumSamples = 1;
        // ...设置 Resolve flags...
        Texture.Resolve = GraphBuilder.CreateTexture(Desc, NameResolved);
        return Texture;
    }

    Desc.Flags |= TexCreate_ShaderResource;
    return FRDGTextureMSAA(
        GraphBuilder.CreateTexture(Desc, NameResolved));
}
```

这里有两种内部布局：

1. RHI 需要独立 MSAA/Resolve 纹理时：
   `SceneTextures.Color.Target` 是 `SceneColorMS`，`SceneTextures.Color.Resolve` 是另一个 `SceneColor`。
2. 无需拆分时：`FRDGTextureMSAA` 构造函数让 `Target` 和 `Resolve` 指向同一个内部 RDG 纹理。

```cpp
// Engine/Source/Runtime/RenderCore/Public/RenderGraphUtils.h:294-299
explicit FRDGTextureMSAA(FRDGTextureRef InTexture)
    : Target(InTexture)
    , Resolve(InTexture)
{}
```

第二种情况只表示 **Scene Color 自己的 `Target == Resolve`**，不表示它等于 PICO Eye Buffer。

### 13.3 Eye Buffer 的 RDG 入口证明：外部 `ViewFamilyTexture`

`ViewFamilyTexture` 走的是完全不同的入口。它从 viewport 的现有 RHI RenderTarget 取得纹理，再通过 `RegisterExternalTexture` 注册进 RDG：

```cpp
// Engine/Source/Runtime/Renderer/Private/ScreenPass.cpp:33-42
FRDGTextureRef TryCreateViewFamilyTexture(
    FRDGBuilder& GraphBuilder,
    const FSceneViewFamily& ViewFamily)
{
    FRHITexture* TextureRHI =
        ViewFamily.RenderTarget->GetRenderTargetTexture();
    FRDGTextureRef Texture = nullptr;
    if (TextureRHI)
    {
        Texture = RegisterExternalTexture(
            GraphBuilder, TextureRHI, TEXT("ViewFamilyTexture"));
        GraphBuilder.SetTextureAccessFinal(Texture, ERHIAccess::RTV);
    }
    return Texture;
}
```

结合前文已经给出的 PICO 调用链：

```text
Pxr_GetLayerImage
→ RHICreateTexture*FromResource
→ FXRSwapChain 当前 alias
→ AllocateRenderTargetTexture
→ ViewFamily.RenderTarget
→ RegisterExternalTexture
→ ViewFamilyTexture
```

所以资源身份是：

```text
SceneTextures.Color.Target
    = GraphBuilder.CreateTexture 创建的 UE 内部 RDG Scene Color

ViewFamilyTexture
    = RegisterExternalTexture 注册的 PICO Eye Buffer
```

二者内容可以通过 resolve、copy 或后处理关联，但“内容由 A 写到 B”不意味着 A、B 是同一张纹理。

> RDG 内部纹理可能使用 transient/pool 分配并在不重叠的生命周期之间复用显存；这不改变其逻辑资源身份。只要该分支同时把内部 Scene Color 作为输入/源、把 `ViewFamilyTexture` 作为最终输出，它们就是不同资源。直写分支中未被使用的内部 Scene Color 还可能被 RDG 裁剪而不产生实际分配。

### 13.4 为什么源码里又会看到 `SceneColor = ViewFamilyTexture`

Mobile Renderer 中的 `SceneColor` 只是一个待选择的局部引用：

```cpp
// Engine/Source/Runtime/Renderer/Private/MobileShadingRenderer.cpp:1452-1480
FRDGTextureRef SceneColor = nullptr;
FRDGTextureRef SceneColorResolve = nullptr;

bool bMobileMSAA = NumMSAASamples > 1;

if (!bRenderToSceneColor)
{
    if (bMobileMSAA)
    {
        SceneColor = SceneTextures.Color.Target;
        SceneColorResolve = ViewFamilyTexture;
    }
    else
    {
        SceneColor = ViewFamilyTexture;
    }
}
else
{
    SceneColor = SceneTextures.Color.Target;
    SceneColorResolve = bMobileMSAA
        ? SceneTextures.Color.Resolve
        : nullptr;
}

BasePassRenderTargets[0] = FRenderTargetBinding(
    SceneColor, SceneColorResolve, ERenderTargetLoadAction::EClear);
```

这段代码精确回答了“他们是否是同一个”的问题：

| 移动端分支 | BasePass/普通 Translucency 的 color target | Resolve/最终去向 | 与 Eye Buffer 的关系 |
|---|---|---|---|
| `!bRenderToSceneColor && !bMobileMSAA` | `ViewFamilyTexture` | 无独立 resolve | **直接就是 Eye Buffer**；未使用 `SceneTextures.Color.Target` |
| `!bRenderToSceneColor && bMobileMSAA` | `SceneTextures.Color.Target` | `ViewFamilyTexture` | Target 与 Eye 独立；render-pass resolve 到 Eye |
| `bRenderToSceneColor && !bMobileMSAA` | `SceneTextures.Color.Target` | 后续 Tonemap/Post Process 写 `ViewFamilyTexture` | Scene Color 与 Eye 独立 |
| `bRenderToSceneColor && bMobileMSAA` | `SceneTextures.Color.Target` | 先到 `SceneTextures.Color.Resolve`，再由后处理写 Eye | Target、内部 Resolve、Eye 分属不同逻辑资源 |

`bRenderToSceneColor` 的决定条件仍是前文给出的代码：

```cpp
// Engine/Source/Runtime/Renderer/Private/MobileShadingRenderer.cpp:509-519
bRenderToSceneColor = !bGammaSpace
    || bStereoRenderingAndHMD
    || bRequiresUpscale
    || bShouldCompositeEditorPrimitives
    || Views[0].bIsSceneCapture
    || Views[0].bIsReflectionCapture
    || (NumMSAASamples > 1 &&
        !RHISupportsSeparateMSAAAndResolveTextures(ShaderPlatform))
    || (NumMSAASamples > 1 &&
        (RenderTargetPixelFormat != PF_Unknown &&
         RenderTargetPixelFormat != SceneTexturesConfig.ColorFormat))
    || bIsFullDepthPrepassEnabled;
```

在当前 PICO 实现中 `GetHMDDistortionEnabled() == false`，因此 `bStereoRenderingAndHMD` 不会仅因为开启 Stereo Rendering 就强制为 true；但 Mobile HDR、upscale、MSAA 能力/格式不匹配等条件仍可能迫使渲染先进入独立 Scene Color。

### 13.5 对前述材质 Blend/Pixel Shader 的直接含义

你给出的 Pixel Shader `OutColor = half4(..., Opacity)` 和 `BLEND_Translucent` Blend State 始终作用于**当次 render pass 的 color attachment 0**，而不是固定作用于某个名为 Scene Color 的全局纹理：

```text
直写 LDR 1x：
    OutColor + Blend State
    → ViewFamilyTexture
    → PICO Eye Buffer

直写 LDR + MSAA：
    OutColor + Blend State
    → SceneTextures.Color.Target
    → render-pass resolve
    → PICO Eye Buffer

中间 Scene Color 路径：
    OutColor + Blend State
    → SceneTextures.Color.Target
    → [SceneTextures.Color.Resolve]
    → Tonemap/Post Process
    → ViewFamilyTexture / PICO Eye Buffer
```

所以最准确的一句话是：

> **`SceneTextures.Color.Target` 本身始终是独立于 PICO Eye Buffer 的 UE 内部 Scene Color；但 Mobile Renderer 的局部 `SceneColor`/当前颜色 RT 不一定指它，在 LDR 无 MSAA 直写路径中，当前颜色 RT 会直接指向作为 `ViewFamilyTexture` 注册的 PICO Eye Buffer。**
