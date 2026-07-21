# UE 材质 `Write Only Alpha` 原理与源码链路

> 分析基线：当前仓库 `Build/Build.version` 为 Unreal Engine **5.4.4**。本文结论以本仓库源码为准。

## 结论先行

`Write Only Alpha`（字段名 `bWriteOnlyAlpha`）在它真正生效的路径上，确实会把这次像素着色器输出的 Alpha **覆盖**到当前 Draw Call 的第 0 个颜色渲染目标（RT0）的 Alpha 通道：

```text
RT0.rgb  = 保持原值
RT0.a    = PixelShaderOutput.a
         = Material Opacity
```

对应的 Alpha 混合公式是：

```text
FinalAlpha = SourceAlpha * 1 + DestinationAlpha * 0
           = SourceAlpha
```

所以，它不是 `SrcAlpha + DstAlpha * (1 - SrcAlpha)` 之类的累计 Alpha 混合，而是逐像素覆盖；多个满足深度/裁剪等测试的对象重叠时，后绘制对象的 Alpha 会覆盖先绘制对象的 Alpha。

但是，必须同时满足下面的限制：

1. 走 **Mobile Shading Path / Mobile Base Pass**。
2. 材质是传统的 `BLEND_Translucent`。
3. 未进入全局 Substrate 分支。
4. Shading Model 不是 `MSM_ThinTranslucent`。
5. RT0 的实际像素格式带 Alpha 通道。

在当前源码中，桌面 Deferred/Forward 渲染器没有消费 `ShouldWriteOnlyAlpha()`；其他 Blend Mode 也不会进入这个判断。因此，不能把该选项理解为对 UE 所有渲染路径都有效的通用“只写 Alpha”。

另外，这里所说的“FrameBuffer”应理解为 **当前 Render Pass 绑定的 RT0**。它通常是移动端 `SceneColor`，某些配置下也可能直接是 `ViewFamilyTexture`；不保证就是最终显示的 Swapchain BackBuffer，更不保证后续 Resolve、Tonemap 或 Post Process 仍保留该 Alpha。

## 1. 材质属性本身的定义

源码直接把它描述为“透明通道 Pass 只向 framebuffer 写 Alpha”：[Material.h](../Source/Runtime/Engine/Classes/Materials/Material.h#L621)

```cpp
/** Whether the transluency pass should write its alpha, and only the alpha, into the framebuffer */
UPROPERTY(EditAnywhere, Category = Translucency, AdvancedDisplay)
uint8 bWriteOnlyAlpha : 1;
```

材质资源通过 `ShouldWriteOnlyAlpha()` 把该 UPROPERTY 交给渲染器：[MaterialShared.cpp](../Source/Runtime/Engine/Private/Materials/MaterialShared.cpp#L1720)

```cpp
bool FMaterialResource::ShouldWriteOnlyAlpha() const
{
    return Material->bWriteOnlyAlpha;
}
```

接口的默认实现返回 `false`，`FMaterialResource` 才覆盖该接口：[MaterialShared.h](../Source/Runtime/Engine/Public/MaterialShared.h#L1900)、[MaterialShared.h](../Source/Runtime/Engine/Public/MaterialShared.h#L2658)。

## 2. 真正改变行为的是 BlendState，不是特殊 Shader 输出

当前仓库对 `ShouldWriteOnlyAlpha()` 的唯一渲染代码调用位于移动端 Base Pass：[MobileBasePass.cpp](../Source/Runtime/Renderer/Private/MobileBasePass.cpp#L615)。核心代码在 `BLEND_Translucent` case 中：[MobileBasePass.cpp](../Source/Runtime/Renderer/Private/MobileBasePass.cpp#L650)

```cpp
case BLEND_Translucent:
    if (Material.ShouldWriteOnlyAlpha())
    {
        DrawRenderState.SetBlendState(
            TStaticBlendState<
                CW_ALPHA,
                BO_Add, BF_Zero, BF_Zero,
                BO_Add, BF_One,  BF_Zero,
                CW_NONE, /* RT1 ... */
                // RT2 ~ RT7 也都是 CW_NONE
            >::GetRHI());
    }
```

这里没有为 `Write Only Alpha` 编译一套特殊的像素着色器；它是在组装 Draw State 时换用一个特殊 BlendState。

### 2.1 `CW_ALPHA`：RT0 只允许写 Alpha

`EColorWriteMask` 的定义表明 `CW_ALPHA` 只开放 Alpha bit，而 `CW_RGBA` 才开放全部四个通道：[RHIDefinitions.h](../Source/Runtime/RHI/Public/RHIDefinitions.h#L271)

```cpp
enum EColorWriteMask
{
    CW_RED   = 0x01,
    CW_GREEN = 0x02,
    CW_BLUE  = 0x04,
    CW_ALPHA = 0x08,

    CW_NONE  = 0,
    CW_RGB   = CW_RED | CW_GREEN | CW_BLUE,
    CW_RGBA  = CW_RED | CW_GREEN | CW_BLUE | CW_ALPHA,
};
```

因此 RT0 的 RGB 不会被本次 Draw 修改；RT1～RT7 的写掩码都是 `CW_NONE`，也不会接收输出。

### 2.2 Alpha 混合参数：源 Alpha 覆盖目标 Alpha

`TStaticBlendState` 的 RT0 模板参数顺序为：[RHIStaticStates.h](../Source/Runtime/RenderCore/Public/RHIStaticStates.h#L275)

```cpp
RT0ColorWriteMask,
RT0ColorBlendOp, RT0ColorSrcBlend, RT0ColorDestBlend,
RT0AlphaBlendOp, RT0AlphaSrcBlend, RT0AlphaDestBlend
```

该文件也明确给出通用公式：`source` 是像素着色器输出，`target` 是渲染目标原值：[RHIStaticStates.h](../Source/Runtime/RenderCore/Public/RHIStaticStates.h#L265)

```cpp
FinalColor.a = SourceAlpha * AlphaSrcBlend
             (AlphaBlendOp)
               DestAlpha * AlphaDestBlend;
```

将 `Write Only Alpha` 的参数代入：

```text
AlphaBlendOp   = BO_Add
AlphaSrcBlend  = BF_One
AlphaDestBlend = BF_Zero

FinalAlpha = SourceAlpha * 1 + DestAlpha * 0
           = SourceAlpha
```

`BO_Add`、`BF_One`、`BF_Zero` 的底层枚举定义见 [RHIDefinitions.h](../Source/Runtime/RHI/Public/RHIDefinitions.h#L340)。这些参数最终被填入 `FBlendStateInitializerRHI::FRenderTarget`，其中 Alpha 和 Color 拥有独立的 BlendOp/SrcBlend/DestBlend 与统一的 ColorWriteMask：[RHI.h](../Source/Runtime/RHI/Public/RHI.h#L449)、[RHIStaticStates.h](../Source/Runtime/RenderCore/Public/RHIStaticStates.h#L351)。

### 2.3 与未勾选时的状态对比

同一个 `BLEND_Translucent` 分支中，未勾选时使用的是：[MobileBasePass.cpp](../Source/Runtime/Renderer/Private/MobileBasePass.cpp#L664)

```cpp
TStaticBlendState<
    CW_RGBA,
    BO_Add, BF_SourceAlpha, BF_InverseSourceAlpha,
    BO_Add, BF_Zero,        BF_InverseSourceAlpha,
    ...
>
```

两者对比如下：

| 状态 | RT0 RGB | RT0 Alpha |
|---|---|---|
| 未勾选 | `SrcRGB * SrcA + DstRGB * (1-SrcA)` | `DstA * (1-SrcA)` |
| 勾选 `Write Only Alpha` | 不写，保持 `DstRGB` | `SrcA`，直接覆盖 |

## 3. `SourceAlpha` 来自哪里

移动端 Base Pass 像素着色器把主颜色输出声明为 `SV_Target0`：[MobileBasePassPixelShader.usf](../Shaders/Private/MobileBasePassPixelShader.usf#L290)

```hlsl
out HALF4_TYPE OutColor : SV_Target0
```

Shader 先从材质像素输入中取得 `Opacity`：[MobileBasePassPixelShader.usf](../Shaders/Private/MobileBasePassPixelShader.usf#L394)

```hlsl
half Opacity = GetMaterialOpacity(PixelMaterialInputs);
```

传统 Translucent 分支再把它放入 `OutColor.a`：[MobileBasePassPixelShader.usf](../Shaders/Private/MobileBasePassPixelShader.usf#L1064)

```hlsl
#elif MATERIALBLENDING_TRANSLUCENT
    OutColor = half4(Color * VertexFog.a + VertexFog.rgb, Opacity);
```

所以完整数据流是：

```text
材质 Opacity 输入
  -> GetMaterialOpacity(PixelMaterialInputs)
  -> OutColor.a / SV_Target0.a
  -> BlendState: SrcA * 1 + DstA * 0
  -> 当前 RT0.a
```

这里的“材质 Alpha”准确地说是材质的 **Opacity 输出**。纹理采样的 A 通道只有在被连接/计算进 Opacity 时，才会成为最终写入值；`Base Color` 的 A 或某个未连接纹理的 A 并不会自动写入。

## 4. RT0 是哪个目标：SceneColor，不一定是最终 BackBuffer

移动端 Forward Render Pass 把 `SceneColor` 绑定到第 0 个颜色目标：[MobileShadingRenderer.cpp](../Source/Runtime/Renderer/Private/MobileShadingRenderer.cpp#L1455)

```cpp
if (!bRenderToSceneColor)
{
    // 无 MSAA 时可以直接使用 ViewFamilyTexture
    SceneColor = ViewFamilyTexture;
}
else
{
    SceneColor = SceneTextures.Color.Target;
}

BasePassRenderTargets[0] = FRenderTargetBinding(
    SceneColor,
    SceneColorResolve,
    ERenderTargetLoadAction::EClear);
```

因此，“覆盖 FrameBuffer Alpha”更精确的说法是：

> 覆盖这次移动端透明 Base Pass Draw Call 所绑定的 RT0 的 Alpha；该 RT0 是当时的 SceneColor/ViewFamilyTexture。

它是不是最终 Swapchain BackBuffer，取决于移动端渲染配置。即使此处成功写入，后续 Resolve、Tonemap、Post Process、格式转换或平台合成也可能修改或丢弃 Alpha。

## 5. 目标必须真的有 Alpha 通道

`Write Only Alpha` 本身不会让 SceneColor 自动改成带 Alpha 的格式。全局搜索 `bWriteOnlyAlpha`/`ShouldWriteOnlyAlpha` 后，除属性、访问器和 `MobileBasePass.cpp` 的 BlendState 分支外，没有发现它参与 SceneColor 创建或格式选择。

移动端 SceneColor 格式选择代码显示：在 Mobile HDR 且支持 `PF_FloatRGBA` 时，如果渲染器不要求 Alpha，默认可以选择不含 Alpha 的 `PF_FloatR11G11B10`；要求 Alpha 时才默认选择 `PF_FloatRGBA`：[SceneTexturesConfig.cpp](../Source/Runtime/Engine/Private/SceneTexturesConfig.cpp#L53)

```cpp
const bool bUseLowPrecisionFormat =
    !IsMobileHDR() || !GSupportsRenderTargetFormat_PF_FloatRGBA;

if (bUseLowPrecisionFormat)
{
    DefaultColorFormat = GetDefaultMobileSceneColorLowPrecisionFormat();
}
else
{
    DefaultColorFormat = bRequiresAlphaChannel
        ? PF_FloatRGBA
        : PF_FloatR11G11B10;
}
```

而且 `r.Mobile.SceneColorFormat=2` 会显式选择 `PF_FloatR11G11B10`：[SceneTexturesConfig.cpp](../Source/Runtime/Engine/Private/SceneTexturesConfig.cpp#L69)。这种目标没有 Alpha，Alpha-only write 没有可保存的通道。

`bRequiresAlphaChannel` 在移动路径中主要由移动端 Alpha 传播决定；Scene Capture 或 Planar Reflection 也会强制它为 `true`：[SceneTextures.cpp](../Source/Runtime/Renderer/Private/SceneTextures.cpp#L395)

```cpp
bool bRequiresAlphaChannel =
    ShadingPath == EShadingPath::Mobile
        ? IsMobilePropagateAlphaEnabled(ViewFamily.GetShaderPlatform())
        : false;

if (View->bIsPlanarReflection || View->bIsSceneCapture)
{
    bRequiresAlphaChannel = true;
}
```

移动端传播开关是只读 CVar `r.Mobile.PropagateAlpha`，定义见 [MobileBasePassRendering.cpp](../Source/Runtime/Renderer/Private/MobileBasePassRendering.cpp#L52)；平台设置读取路径见 [SceneUtils.cpp](../Source/Runtime/Engine/Private/SceneUtils.cpp#L47) 与 [GenericPlatformMisc.cpp](../Source/Runtime/Core/Private/GenericPlatform/GenericPlatformMisc.cpp#L2039)。桌面/通用后处理链的 Alpha 支持则由 `r.PostProcessing.PropagateAlpha` 管理，定义见 [PostProcessing.cpp](../Source/Runtime/Renderer/Private/PostProcess/PostProcessing.cpp#L104)。

## 6. 生效范围与容易误解的点

### 6.1 只在特定移动端传统 Translucent 分支生效

`SetTranslucentRenderState()` 的控制流顺序是：[MobileBasePass.cpp](../Source/Runtime/Renderer/Private/MobileBasePass.cpp#L622)

```text
Substrate enabled?             -> 使用 Substrate 的 BlendState，不检查该选项
否则是 Thin Translucent?       -> 使用 Color Transmittance BlendState，不检查该选项
否则按 BlendMode switch
    BLEND_Translucent          -> 才检查 ShouldWriteOnlyAlpha()
    Additive/Modulate/...      -> 各用自己的 BlendState，不检查该选项
```

因此即使 UI 中能设置该字段，在以下情况也不会触发上述 Alpha-only 状态：

- Desktop Deferred 或 Desktop Forward；
- Substrate 路径；
- Thin Translucent；
- Additive、Modulate、AlphaComposite、AlphaHoldout 等其他 Blend Mode。

### 6.2 它不是对 UAV/任意 RenderTarget 的材质级写操作

这个选项不让材质任意访问 FrameBuffer，也不执行 Read-Modify-Write Shader 逻辑。它只为 Raster Draw 设置固定功能 BlendState，作用对象就是当时绑定的 RT0。

### 6.3 覆盖仍受正常像素测试和绘制顺序约束

只有实际产生 fragment 且通过相关测试的像素才会写入，包括但不限于：

- 几何覆盖范围与裁剪/Discard；
- 深度测试；
- Scissor、Stencil、Sample Mask/MSAA 覆盖；
- 透明对象排序和 Draw Call 顺序。

该选项只替换 BlendState，并不自动关闭深度测试、不改变透明排序，也不保证透明材质写深度。

### 6.4 后绘制者覆盖先绘制者

因为 `DstAlpha` 的系数是 0，而不是 `1-SrcAlpha`，重叠区域不会累计：

```text
初始 DstA = 0.2
先画 A，Opacity = 0.4  -> RT0.a = 0.4
再画 B，Opacity = 0.7  -> RT0.a = 0.7
```

最终值取决于最后一个通过测试的 Draw，而不是 `0.4 + 0.7 * (1-0.4)`。

## 7. 对问题的精确回答

> 它会直接将当前材质的 Alpha 覆盖到 FrameBuffer 中的 Alpha 吗？

**会，但只在上述生效路径和目标带 Alpha 的前提下。** 更精确地说：UE 将传统 Translucent 材质的 `Opacity` 写进像素着色器 `SV_Target0.a`，然后用 `CW_ALPHA + (BO_Add, BF_One, BF_Zero)` 令 `RT0.a = SrcA`，同时禁止写 RT0.rgb。这就是硬件固定功能混合阶段的直接覆盖。

它不是全平台通用功能，也不保证最终 BackBuffer/导出结果仍保留该值。若你的目标是最终输出 Alpha，还必须同时确认：实际使用 Mobile Shading Path、SceneColor/目标纹理格式含 Alpha，以及后续 Alpha propagation/后处理链没有丢弃 Alpha。

## 8. 建议的运行时验证

可用 RenderDoc 或平台 GPU Capture 做一次最直接的确认：

1. 使用 `BLEND_Translucent` 材质，把常量 `0.25` 接到 `Opacity`，勾选 `Write Only Alpha`。
2. 确认实际运行的是 Mobile Shading Path，且未启用会绕过该分支的 Substrate/Thin Translucent。
3. 检查该 Draw Call：RT0 Color Write Mask 应只有 `A`；Alpha Blend 应为 `Add / One / Zero`。
4. 检查 RT0 格式是否带 Alpha，例如 `PF_FloatRGBA`、`PF_R8G8B8A8` 或 `PF_B8G8R8A8`，而不是 `PF_FloatR11G11B10`。
5. 查看 Draw 前后像素：RGB 不变，Alpha 变为约 `0.25`（会受目标格式量化影响）。

这能同时验证源码推导、当前平台 RHI 映射、目标格式和后处理链是否符合预期。
