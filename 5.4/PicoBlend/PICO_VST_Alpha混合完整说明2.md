# UE 移动端透明 Alpha 与 PICO VST 合成

> 适用于：UE **5.4.4**、PICOXR **3.2.3**、Mobile Forward。  
> PICO 官方参考：[视频透视](https://developer-cn.picoxr.com/document/unreal/seethrough/)。


## 1. EyeBuffer 与 VST 的三种合成模式

PICO 将 EyeBuffer 作为 source、VST 作为 destination。`r.Mobile.PICO.BlendModeSetting` 默认值为 `1`。

### 1.1 混合公式

| 值 | 模式 | 最终颜色公式 |
|---:|---|---|
| `0` | Covering Mode | $$C_{\mathrm{Screen}}=C_{\mathrm{Eye}}+C_{\mathrm{VST}}$$ |
| `1` | Clip Mode（默认） | $$C_{\mathrm{Screen}}=C_{\mathrm{Eye}}(1-A_{\mathrm{Eye}})+C_{\mathrm{VST}}A_{\mathrm{Eye}}$$ |
| `2` | Additive Mode | $$C_{\mathrm{Screen}}=C_{\mathrm{Eye}}+C_{\mathrm{VST}}A_{\mathrm{Eye}}$$ |

三种模式的 `srcAlpha/dstAlpha` 都是 `ONE/ONE`：

$$
A_{\mathrm{Output}}=A_{\mathrm{Eye}}+A_{\mathrm{VST}}
$$

屏幕颜色由表中的 Color Blend Factor 决定。

相关代码：

```cpp
// .../PICO-Unreal-Integration-SDK/UE_5.4/Plugins/PICOXR/Source/PICOXRHMD/Private/PXR_HMD.cpp:90-96
static TAutoConsoleVariable<int32> CVarPICOBlendModeSetting(
    TEXT("r.Mobile.PICO.BlendModeSetting"),
    1,
    TEXT("0: Covering Mode, VST will cover the entire screen\n")
    TEXT("1: Clip Mode,Eye Buffer and VST will clip by Alpha Before add(Default)\n")
    TEXT("2: Additive Mode, Eye Buffer will not clip by Alpha\n"),
    ECVF_Scalability | ECVF_RenderThreadSafe);
```

```cpp
// .../PICO-Unreal-Integration-SDK/UE_5.4/Plugins/PICOXR/Source/PICOXRHMD/Private/PXR_StereoLayer.cpp:813-848
if (ID == 0)
{
    layerProjection.header.useLayerBlend = 1;

    switch (BlendModeType)
    {
    case EPICOXRBlendModeType::CoveringMode:
        layerBlend.srcColor = PXR_BLEND_FACTOR_ONE;
        layerBlend.dstColor = PXR_BLEND_FACTOR_ONE;
        break;
    case EPICOXRBlendModeType::ClipMode:
        layerBlend.srcColor = PXR_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        layerBlend.dstColor = PXR_BLEND_FACTOR_SRC_ALPHA;
        break;
    case EPICOXRBlendModeType::AdditiveMode:
        layerBlend.srcColor = PXR_BLEND_FACTOR_ONE;
        layerBlend.dstColor = PXR_BLEND_FACTOR_SRC_ALPHA;
        break;
    }

    layerBlend.srcAlpha = PXR_BLEND_FACTOR_ONE;
    layerBlend.dstAlpha = PXR_BLEND_FACTOR_ONE;
    layerProjection.header.layerBlend = layerBlend;
}
```

### 1.2 使用方式

蓝图中使用 **Execute Console Command** 节点，根据需要填写：

```text
r.Mobile.PICO.BlendModeSetting 0
r.Mobile.PICO.BlendModeSetting 1
r.Mobile.PICO.BlendModeSetting 2
```

也可在启动配置中设置默认值：

```ini
[ConsoleVariables]
r.Mobile.PICO.BlendModeSetting=2
```

## 2. UE 材质写入 Base Pass 颜色目标的混合公式

`Before/After` 指 Base Pass 当前绑定的颜色目标；它可能是 UE 内部 SceneColor，也可能直接是 Eye Buffer，见 5.4。`A_Material` 指材质 Opacity。普通 Masked 被裁掉的片元不写入 RT；表中 Masked 公式只表示通过裁剪的片元。

| 材质 Blend Mode | 颜色目标混合公式 |
|---|---|
| Opaque / Masked | $$\begin{aligned}C_{\mathrm{After}}&=C_{\mathrm{Material}}\\A_{\mathrm{After}}&=0\end{aligned}$$ |
| Translucent | $$\begin{aligned}C_{\mathrm{After}}&=C_{\mathrm{Material}}A_{\mathrm{Material}}+C_{\mathrm{Before}}(1-A_{\mathrm{Material}})\\A_{\mathrm{After}}&=A_{\mathrm{Before}}(1-A_{\mathrm{Material}})\end{aligned}$$ |
| Additive | $$\begin{aligned}C_{\mathrm{After}}&=C_{\mathrm{Before}}+C_{\mathrm{Material}}A_{\mathrm{Material}}\\A_{\mathrm{After}}&=A_{\mathrm{Before}}\end{aligned}$$ |

### 2.1 Opaque / Masked 相关代码

默认 Base Pass 不启用混合，直接写入 RGBA：

```cpp
// .../Engine/Source/Runtime/Renderer/Private/MobileBasePass.cpp:1173-1179
PassDrawRenderState.SetBlendState(
    TStaticBlendStateWriteMask<CW_RGBA>::GetRHI());
```

Masked 先根据 Opacity Mask 裁剪片元：

```hlsl
// .../Engine/Shaders/Private/MaterialTemplate.ush:3979-4006
void GetMaterialCoverageAndClipping(...)
{
    ...
#if MATERIALBLENDING_MASKED && !SINGLE_LAYER_WATER_NO_DISCARD
    ...
    clip(GetMaterialMask(PixelMaterialInputs));
#endif
}
```

留存片元输出 `Alpha = 0`：

```hlsl
// .../Engine/Shaders/Private/MobileBasePassPixelShader.usf:1071-1080
#else
    OutColor.rgb = Color * VertexFog.a + VertexFog.rgb;

    #if !MATERIAL_USE_ALPHA_TO_COVERAGE
        OutColor.a = 0.0;
    #else
        // Alpha To Coverage 路径
    #endif
#endif
```

### 2.2 Translucent 相关代码

Blend State 使用源 Alpha 混合 RGB，并用 `InverseSourceAlpha` 衰减目标 Alpha：

```cpp
// .../Engine/Source/Runtime/Renderer/Private/MobileBasePass.cpp:652-673
case BLEND_Translucent:
    // 未启用 Write Alpha Only
    DrawRenderState.SetBlendState(TStaticBlendState<
        CW_RGBA,
        BO_Add, BF_SourceAlpha, BF_InverseSourceAlpha,
        BO_Add, BF_Zero,        BF_InverseSourceAlpha
    >::GetRHI());
```

Pixel Shader 输出材质颜色和 Opacity：

```hlsl
// .../Engine/Shaders/Private/MobileBasePassPixelShader.usf:1064-1065
#elif MATERIALBLENDING_TRANSLUCENT
    OutColor = half4(Color * VertexFog.a + VertexFog.rgb, Opacity);
```

### 2.3 Additive 相关代码

Blend State 对 RGB 执行加法；因为源 Alpha 为 `0`，目标 Alpha 保持不变：

```cpp
// .../Engine/Source/Runtime/Renderer/Private/MobileBasePass.cpp:676-685
case BLEND_Additive:
    DrawRenderState.SetBlendState(TStaticBlendState<
        CW_RGBA,
        BO_Add, BF_One,  BF_One,
        BO_Add, BF_Zero, BF_InverseSourceAlpha
    >::GetRHI());
```
Additive 在 Pixel Shader 内将 Opacity 预乘进 RGB，输出 Alpha 固定为 `0`：

```hlsl
// .../Engine/Shaders/Private/MobileBasePassPixelShader.usf:1066-1067
#elif MATERIALBLENDING_ADDITIVE
    OutColor = half4(Color * (VertexFog.a * Opacity.x), 0.0f);
```


## 3. 特殊处理

以下推导假设 Base Pass 颜色目标的清屏 Alpha 为 `1`，且其 RGBA 未被后续阶段改写并最终写入 Eye Buffer；否则应以 Eye Buffer 的最终值为准。

### 3.1 Translucent 在 Clip Mode 中被二次缩放

当普通 Translucent 直接绘制在清屏区域时：

$$
\begin{aligned}
C_{\mathrm{Eye}}&=C_{\mathrm{Material}}A_{\mathrm{Material}}\\
A_{\mathrm{Eye}}&=1-A_{\mathrm{Material}}
\end{aligned}
$$

代入 Clip Mode：

$$
\begin{aligned}
C_{\mathrm{Screen}}
&=C_{\mathrm{Eye}}(1-A_{\mathrm{Eye}})+C_{\mathrm{VST}}A_{\mathrm{Eye}}\\
&=C_{\mathrm{Material}}A_{\mathrm{Material}}^2
  +C_{\mathrm{VST}}(1-A_{\mathrm{Material}})
\end{aligned}
$$

虚拟颜色在 UE 透明混合和 PICO Clip 合成中各乘一次材质 Opacity，最终系数变为其平方，因此画面会变暗。

若希望最终虚拟颜色系数为 $A_{Target}$，可在材质中使用：

$$
A_{\mathrm{Material}}=\sqrt{A_{\mathrm{Target}}}
$$

此时：

$$
C_{\mathrm{Screen}}
=C_{\mathrm{Material}}A_{\mathrm{Target}}
+C_{\mathrm{VST}}\left(1-\sqrt{A_{\mathrm{Target}}}\right)
$$

Sqrt 能补偿虚拟颜色被平方的问题，但 VST 权重会变为上式中的值，并不等价于标准 Alpha 合成。

### 3.2 Additive 不写 RT Alpha

Additive 直接绘制在清屏区域后：

$$
\begin{aligned}
C_{\mathrm{Eye}}&=C_{\mathrm{Material}}A_{\mathrm{Material}}\\
A_{\mathrm{Eye}}&=1
\end{aligned}
$$

在 Clip Mode 中：

$$
C_{\mathrm{Screen}}
=C_{\mathrm{Eye}}(1-1)+C_{\mathrm{VST}}
=C_{\mathrm{VST}}
$$

因此 Additive 已写入 EyeBuffer 的 RGB 会被完全裁掉，在 VST 中不可见。PICO混合切换为 Additive Mode 后：

$$
C_{\mathrm{Screen}}
=C_{\mathrm{Material}}A_{\mathrm{Material}}+C_{\mathrm{VST}}
$$

### 3.3 Write Alpha Only

Write Alpha Only 只写入材质 Opacity，不写 RGB：

$$
\begin{aligned}
C_{\mathrm{After}}&=C_{\mathrm{Before}}\\
A_{\mathrm{After}}&=A_{\mathrm{Material}}
\end{aligned}
$$

它会直接覆盖目标 Alpha，不执行普通 Translucent 的透过率连乘。

```cpp
// .../Engine/Source/Runtime/Engine/Classes/Materials/Material.h:621-623
/** Whether the transluency pass should write its alpha,
    and only the alpha, into the framebuffer */
uint8 bWriteOnlyAlpha : 1;
```

```cpp
// .../Engine/Source/Runtime/Renderer/Private/MobileBasePass.cpp:652-662
case BLEND_Translucent:
    if (Material.ShouldWriteOnlyAlpha())
    {
        DrawRenderState.SetBlendState(TStaticBlendState<
            CW_ALPHA,
            BO_Add, BF_Zero, BF_Zero,
            BO_Add, BF_One,  BF_Zero
        >::GetRHI());
    }
```
若要精确控制 Base Pass 颜色目标的 Alpha，可以在材质中开启此选项。

## 4. VST 下使用透明物体的方法

| 方法 | 设置 |
|---|---|
| Clip Mode + Sqrt 补偿 | `r.Mobile.PICO.BlendModeSetting 1`；材质 Opacity 使用 $$A_{\mathrm{Material}}=\sqrt{A_{\mathrm{Target}}}$$ |
| Additive Mode | `r.Mobile.PICO.BlendModeSetting 2`；材质继续使用目标 Opacity |

## 5. 说明

### 5.1 公式符号

| 符号 | 含义 |
|---|---|
| `C` | RGB 颜色向量 |
| `A` | Alpha 标量 |
| `Material` | 当前材质的输出；`A_Material` 即材质 Opacity |
| `Before` | 当前材质绘制前的 Base Pass 颜色目标 |
| `After` | 当前材质绘制后的 Base Pass 颜色目标 |
| `Eye` | 最终提交给 PICO 的 EyeBuffer |
| `VST` | 视频透视画面 |
| `Screen` | PICO 合成后的屏幕输出 |
| `Target` | 希望最终得到的虚拟颜色强度 |


### 5.2 EyeBuffer Alpha 的含义

EyeBuffer Alpha 表示**还应透出多少背景/VST**，不是虚拟内容的不透明度：

| EyeBuffer Alpha | 含义 |
|---:|---|
| `1` | 没有虚拟内容遮挡，应显示 VST |
| `0` | 被虚拟不透明内容覆盖，不应显示 VST |
| `0～1` | 按该比例保留 VST |

### 5.3 UE 内部 SceneColor 默认值

UE 内部 `SceneTextures.Color` 的清屏值为：

$$
(R,G,B,A)_{\mathrm{Clear}}=(0,0,0,1)
$$

因此，**UE 内部 SceneColor 的清屏 Alpha 默认值是 `1.0`**。这些代码只证明内部 SceneColor，不能据此断言 Eye Buffer 的清屏 Alpha 也为 `1.0`。

```cpp
// .../Engine/Source/Runtime/Engine/Private/SceneTexturesConfig.cpp:268
ColorClearValue = FClearValueBinding::Black;

// .../Engine/Source/Runtime/RHI/Private/RHI.cpp:105
const FClearValueBinding FClearValueBinding::Black(
    FLinearColor(0.0f, 0.0f, 0.0f, 1.0f));

// .../Engine/Source/Runtime/Renderer/Private/SceneTextures.cpp:487-491
FRDGTextureDesc Desc(Config.bRequireMultiView ?
    FRDGTextureDesc::Create2DArray(
        Config.Extent, Config.ColorFormat, Config.ColorClearValue, Config.ColorCreateFlags, 2) :
    FRDGTextureDesc::Create2D(
        Config.Extent, Config.ColorFormat, Config.ColorClearValue, Config.ColorCreateFlags));
Desc.NumSamples = Config.NumSamples;
SceneTextures.Color = CreateTextureMSAA(
    GraphBuilder, Desc, TEXT("SceneColorMS"), TEXT("SceneColor"), GFastVRamConfig.SceneColor | sRGBFlag);
```

### 5.4 SceneColor 与 Eye Buffer 的关系

不能无条件认为 `SceneColor RT = Eye Buffer`：

| 名称 | 含义 |
|---|---|
| `SceneTextures.Color` | UE 创建的内部 SceneColor |
| `SceneColor` | Mobile Renderer 中 Base Pass 当前颜色目标的局部变量 |
| `ViewFamilyTexture` / Eye Buffer | PICO Eye Layer SwapChain，最终提交目标 |

UE 从当前 XR 设备取得 Render Target Manager，并通过 `RTRHI/SRVRHI` 输出参数接收纹理：

```cpp
// .../Engine/Source/Runtime/Engine/Private/Slate/SceneViewport.cpp:363-367,2148-2161
return (bIsStereoRenderingAllowed &&
        GEngine->StereoRenderingDevice.IsValid() &&
        GEngine->StereoRenderingDevice->IsStereoEnabledOnNextFrame())
    ? GEngine->StereoRenderingDevice->GetRenderTargetManager()
    : nullptr;

// ...
else if (StereoRenderTargetManager == nullptr ||
    !StereoRenderTargetManager->AllocateRenderTargetTexture(
        i, TexSizeX, TexSizeY, SceneTargetFormat, 1,
        TexCreate_None, TexCreate_RenderTargetable, RTRHI, SRVRHI))
{
    // XR 未提供纹理时的默认创建路径
}
BufferedRenderTargetsRHI[i] = RTRHI;
BufferedShaderResourceTexturesRHI[i] = SRVRHI;
```

PICO 的 Render Target Manager 是 `FPICOXRHMD` 自身，其分配函数返回 Eye Layer 的当前 SwapChain 图像：

```cpp
// .../PICO-Unreal-Integration-SDK/UE_5.4/Plugins/PICOXR/Source/PICOXRHMD/Private/PXR_HMD.h:181
virtual IStereoRenderTargetManager* GetRenderTargetManager() override { return this; }

// .../PICO-Unreal-Integration-SDK/UE_5.4/Plugins/PICOXR/Source/PICOXRHMD/Private/PXR_HMD.cpp:2594-2601
const FXRSwapChainPtr& SwapChain = PXREyeLayer_RenderThread->GetSwapChain();
if (SwapChain.IsValid())
{
    OutTargetableTexture = OutShaderResourceTexture =
        SwapChain->GetTexture2DArray() ? SwapChain->GetTexture2DArray() : SwapChain->GetTexture2D();
    return true;
}
```

UE 将最终视口目标注册为 `ViewFamilyTexture`：

```cpp
// .../Engine/Source/Runtime/Renderer/Private/ScreenPass.cpp:33-40
FRHITexture* TextureRHI = ViewFamily.RenderTarget->GetRenderTargetTexture();
Texture = RegisterExternalTexture(GraphBuilder, TextureRHI, TEXT("ViewFamilyTexture"));
```

Mobile Forward 再决定 Base Pass 的实际颜色目标：

```cpp
// .../Engine/Source/Runtime/Renderer/Private/MobileShadingRenderer.cpp:1459-1476
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
    SceneDepth = SceneTextures.Depth.Target;
}
else
{
    SceneColor = SceneTextures.Color.Target;
    SceneColorResolve = bMobileMSAA ? SceneTextures.Color.Resolve : nullptr;
    SceneDepth = SceneTextures.Depth.Target;
}
```

若使用内部 SceneColor，最终输出到 `ViewFamilyTexture`：

```cpp
// .../Engine/Source/Runtime/Renderer/Private/PostProcess/PostProcessing.cpp:2110-2117,2935-2938
const FScreenPassRenderTarget ViewFamilyOutput =
    FScreenPassRenderTarget::CreateViewFamilyOutput(Inputs.ViewFamilyTexture, View);
FScreenPassTexture SceneColor(
    (*Inputs.SceneTextures)->SceneColorTexture, FinalOutputViewRect);

if (SceneColor.Texture != ViewFamilyOutput.Texture)
{
    AddDrawTexturePass(GraphBuilder, View, SceneColor, ViewFamilyOutput);
}
```

因此，XR 只提供最终视口目标，不会禁止 UE 创建内部 SceneColor。本文用“Base Pass 颜色目标”描述材质混合，用“Eye Buffer”描述最终提交纹理；只有直接渲染分支中，两者才是同一纹理。