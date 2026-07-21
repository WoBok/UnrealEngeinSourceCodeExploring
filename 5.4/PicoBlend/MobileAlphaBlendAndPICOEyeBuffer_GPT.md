# UE 移动端前向透明 Alpha 与 PICO Eye Buffer / VST 合成

> 调查基线：本仓库 UE **5.4.4**、`Engine/PICO-Unreal-Integration-SDK/UE_5.4` 中的 PICOXR **3.2.3**，以及 2026-07-21 可见的 [PICO 官方「视频透视」文档](https://developer-cn.picoxr.com/document/unreal/seethrough/)。
>
> 本文主要讨论非 Substrate、`BLEND_Translucent` 的移动端前向路径；特殊路径会单独指出。

## 结论

你的理解**基本正确**，但要修正三点：

1. PICO 的 **Eye Buffer** 是应用最终提交给 PICO Runtime 的双眼投影层纹理，可以理解成 XR 的最终 RT；但 UE 通常先渲染中间 `SceneColor`，再 Copy/Tonemap 到 Eye Buffer，所以它们并非每个阶段都是同一张纹理。
2. PICO 合成使用的 Alpha 确实是最终 **Eye Buffer Alpha**。不过普通半透明材质的 `Opacity=0.3` 不会直接把它写成 `0.3`；若绘制前 RT Alpha 为 1，绘制后会变成 **0.7**。
3. 这里的 RT Alpha 更接近“**还应透出多少背景/VST**”，不是常见的虚拟内容不透明度。因此 `Opacity=0.3` 对应 0.7 的 VST 权重。

只要 Alpha 被完整传到 Eye Buffer，且透明物体后方没有虚拟不透明物体，PICO 模式 2 最终得到：

```text
Screen.rgb = VirtualObject.rgb * 0.3 + VST.rgb * 0.7
```

## 1. 两级混合不能混为一谈

```text
材质 Src.rgb / Src.a
        │
        ▼
UE 移动端透明 Blend：写 SceneColor
        │ SceneColor.a = 剩余背景透过率
        ▼
Copy / Tonemap / 后处理：写 PICO Eye Buffer
        │ 必须保住 Alpha
        ▼
PICO Runtime：Eye Buffer 与 VST 合成
        ▼
头显显示
```

第一级由 UE RHI Blend State 决定；第二级由 PICO SDK 提交的 `PxrLayerBlend` 决定。

## 2. 普通透明物体如何改变 RT Alpha

### 2.1 Shader 输出

移动 Base Pass 将材质 `Opacity` 饱和到 `[0,1]`，再作为源 Alpha 输出：

```hlsl
half Opacity = GetMaterialOpacity(PixelMaterialInputs);
OutColor = half4(Color * VertexFog.a + VertexFog.rgb, Opacity);
```

代码：

- `Engine/Shaders/Private/MaterialTemplate.ush:3516-3521`
- `Engine/Shaders/Private/MobileBasePassPixelShader.usf:1061-1062`

### 2.2 `BLEND_Translucent` 的公式

`MobileBasePass::SetTranslucentRenderState()` 设置：

```cpp
TStaticBlendState<
    CW_RGBA,
    BO_Add, BF_SourceAlpha, BF_InverseSourceAlpha,
    BO_Add, BF_Zero,        BF_InverseSourceAlpha>
```

代码：`Engine/Source/Runtime/Renderer/Private/MobileBasePass.cpp:615-675`。

令 `Cs, As` 为透明物体输出，`Cd, Ad` 为绘制前 RT，结果为 `Co, Ao`：

```text
Co = Cs * As + Cd * (1 - As)
Ao = As * 0  + Ad * (1 - As)
   = Ad * (1 - As)
```

多个透明层的 Alpha 为：

```text
Afinal = Ainitial * Π(1 - Ai)
```

若 `Ainitial=1`，则 `1-Afinal` 才是透明层合起来的覆盖率。

### 2.3 `Opacity=0.3` 示例

假设绘制前该像素没有虚拟颜色，且允许完整透出 VST：

```text
Cd = 0
Ad = 1
```

绘制 `As=0.3` 后：

```text
Co = Cs * 0.3
Ao = 1 * (1 - 0.3) = 0.7
```

所以普通透明材质的 0.3 最终产生的是 **RT Alpha=0.7**。

如果后方已有虚拟不透明物体，它会先写 `Ad=0`，之后 `Ao=0*0.7=0`。普通透明物体不会在不透明物体上重新“挖开”VST。

## 3. 最终 RT 的默认 Alpha 到底是多少

不能脱离渲染阶段只给一个数字。

### 3.1 中间 SceneColor 的清屏 Alpha 是 1

SceneColor 默认清屏绑定是 `FClearValueBinding::Black`：

- `Engine/Source/Runtime/Engine/Public/SceneTexturesConfig.h:180`
- `Engine/Source/Runtime/Engine/Private/SceneTexturesConfig.cpp:259-270`

`Black` 的定义是 `(0,0,0,1)`，不是透明黑：

```cpp
const FClearValueBinding FClearValueBinding::Black(
    FLinearColor(0.0f, 0.0f, 0.0f, 1.0f));
```

代码：`Engine/Source/Runtime/RHI/Private/RHI.cpp:102-108`。

因此逻辑初值是 `SceneColor clear=(0,0,0,1)`。

### 3.2 不透明像素写 Alpha=0

移动 Base Pass 的不透明分支明确写 `OutColor.a=0.0`：

- `Engine/Shaders/Private/MobileBasePassPixelShader.usf:1068-1078`

| 像素状态 | SceneColor Alpha |
|---|---:|
| 只有清屏、没有虚拟内容 | 1 |
| 虚拟不透明物体 | 0 |
| 0.3 透明物体直接画在清屏上 | 0.7 |
| 0.3 透明物体画在不透明物体上 | 0 |

覆盖全屏的 Sky/不透明背景同样会写 0，从而挡住 VST。

### 3.3 Eye Buffer 是最终 RT，但透明混合通常先写中间 SceneColor

PICO 创建 ID 0 投影层，并在 Vulkan 下使用 RGBA8 swapchain：

- `Engine/PICO-Unreal-Integration-SDK/UE_5.4/Plugins/PICOXR/Source/PICOXRHMD/Private/PXR_StereoLayer.cpp:1225-1239`
- `Engine/PICO-Unreal-Integration-SDK/UE_5.4/Plugins/PICOXR/Source/PICOXRHMD/Private/PXR_HMD.cpp:2585-2602`

但 UE 移动渲染器在 Stereo + HMD 时设置 `bRenderToSceneColor=true`：

- `Engine/Source/Runtime/Renderer/Private/MobileShadingRenderer.cpp:505-519`
- `Engine/Source/Runtime/Renderer/Private/MobileShadingRenderer.cpp:1448-1500`

因此准确说法是：Eye Buffer 是 PICO Runtime 消费的最终 RT；普通透明混合常发生在中间 SceneColor。最终写入必须保留 Alpha，透明材质才会影响 VST。

### 3.4 Copy/Tonemap 可能丢掉 Alpha

`r.Mobile.PropagateAlpha` 默认值为 **0**：

- `Engine/Source/Runtime/Renderer/Private/MobileBasePassRendering.cpp:52-57`

移动 Shader 编译用它决定 `POST_PROCESS_ALPHA`：

- `Engine/Source/Runtime/Engine/Private/ShaderCompiler/ShaderCompiler.cpp:8585-8600`

| 最终写入路径 | Eye Buffer Alpha |
|---|---|
| 无 Tonemap，普通 RGBA Copy | 保留 SceneColor Alpha |
| 标准 Tonemapper，`r.Mobile.PropagateAlpha=1` | 复制并保留 `SceneColor.a` |
| 标准 Tonemapper，默认 `r.Mobile.PropagateAlpha=0` | 不复制输入 Alpha；立体视图仍写 Alpha，源码路径通常得到 0 |
| `r.Mobile.TonemapSubpass=1` | `MobileCustomResolve_MainPS` 强制 Alpha=1 |
| HDR 且未要求 Alpha | SceneColor 可能采用没有 Alpha 的 `PF_FloatR11G11B10` |

证据：

- `Engine/Shaders/Private/PostProcessTonemap.usf:454-526, 677-710`
- `Engine/Source/Runtime/Renderer/Private/PostProcess/PostProcessTonemap.cpp:550-556, 1013-1017`
- `Engine/Source/Runtime/Renderer/Private/MobileShadingRenderer.cpp:102-113`
- `Engine/Source/Runtime/Engine/Private/SceneTexturesConfig.cpp:53-64`

依赖 Eye Buffer Alpha 时，建议在启动配置中启用：

```ini
[/Script/Engine.RendererSettings]
r.Mobile.PropagateAlpha=1
```

该设置影响 Shader permutation 和 SceneColor 格式，应重启并重编相关 Shader。同时确认没有启用会强制 Alpha=1 的 `r.Mobile.TonemapSubpass`，并检查自定义后处理是否覆盖 Alpha。

### 3.5 PICO swapchain ClearValue 不是业务 Alpha 初值

本地插件两条 RHI 路径有差异：Vulkan 外部纹理使用 `(0,0,0,0)` 的默认 `FClearValueBinding`，OpenGL 显式使用 `(0,0,0,1)` 的 `Black`：

- `Engine/PICO-Unreal-Integration-SDK/UE_5.4/Plugins/PICOXR/Source/PICOXRHMD/Private/PXR_HMDRenderBridge_Vulkan.cpp:48-72`
- `Engine/PICO-Unreal-Integration-SDK/UE_5.4/Plugins/PICOXR/Source/PICOXRHMD/Private/PXR_HMDRenderBridge_OpenGL.cpp:16-28`

正常帧会由最终 Copy/Tonemap 覆盖 Eye Buffer，不应把 swapchain 包装时的 ClearValue 当作合成语义初值；应检查最终 Eye Buffer 实值。

## 4. `r.Mobile.PICO.BlendModeSetting` 三种模式

PICO 官方文档说明：

- `0`：VST 叠加到 Eye Buffer；
- `1`（默认）：VST 与 Eye Buffer 均根据 Alpha 裁剪，透明特效可能有混合问题；
- `2`：Eye Buffer 叠加到 VST，只有 VST 根据 Alpha 裁剪，含透明度特效可正常混合。

源码：

- CVar：`Engine/PICO-Unreal-Integration-SDK/UE_5.4/Plugins/PICOXR/Source/PICOXRHMD/Private/PXR_HMD.cpp:90-96`
- Blend Factor：`Engine/PICO-Unreal-Integration-SDK/UE_5.4/Plugins/PICOXR/Source/PICOXRHMD/Private/PXR_StereoLayer.cpp:813-848`

令 `E=EyeBuffer.rgb`、`V=VST.rgb`、`A=EyeBuffer.a`。按标准 Blend Factor 展开，并用官方描述确认 source 为 Eye Buffer、destination 为 VST：

| 值 | SDK 名称 | `srcColor` | `dstColor` | 屏幕 RGB 公式 |
|---:|---|---|---|---|
| 0 | CoveringMode | `ONE` | `ONE` | `E + V` |
| 1 | ClipMode（默认） | `ONE_MINUS_SRC_ALPHA` | `SRC_ALPHA` | `E*(1-A) + V*A` |
| 2 | AdditiveMode | `ONE` | `SRC_ALPHA` | `E + V*A` |

三种模式均设置 `srcAlpha=ONE, dstAlpha=ONE`。当前 Eye/VST RGB 权重由表中的 Color 因子决定。模式 0、2 是加法形式，亮部可能饱和，不是普通 `lerp`。

## 5. `Opacity=0.3` 在三种模式中的结果

假设透明物体后方没有虚拟不透明背景，UE 混合后 `E=Cs*0.3, A=0.7`。

### 模式 0

```text
Screen = E + V = Cs*0.3 + V
```

### 模式 1（默认）

```text
Screen = E*(1-A) + V*A
       = (Cs*0.3)*0.3 + V*0.7
       = Cs*0.09 + V*0.7
```

透明颜色在 UE 内已乘一次 0.3，PICO 又用 `1-A=0.3` 乘一次，产生二次衰减。这对应官方所说的透明特效混合问题。

### 模式 2

```text
Screen = E + V*A
       = Cs*0.3 + V*0.7
```

这是正常的 30% 虚拟物体 + 70% VST。普通透明特效应采用这一模式的合成行为。

## 6. `Write Alpha Only` 如何工作

属性定义：

- `Engine/Source/Runtime/Engine/Classes/Materials/Material.h:617-623`
- `Engine/Source/Runtime/Engine/Private/Materials/MaterialShared.cpp:1718-1721`

移动端分支使用：

```cpp
TStaticBlendState<
    CW_ALPHA,
    BO_Add, BF_Zero, BF_Zero,
    BO_Add, BF_One,  BF_Zero>
```

代码：`Engine/Source/Runtime/Renderer/Private/MobileBasePass.cpp:652-663`。

```text
RGBout = RGBdst                 // 不写 RGB
Aout   = As * 1 + Ad * 0 = As // 直接替换 Alpha
```

所以它**会直接覆盖 RT Alpha**，但只覆盖通过光栅化、深度/模板测试且未被 `clip/discard` 的像素。

| 方式 | 目标原为 1、Opacity=0.3 后的 Alpha |
|---|---:|
| 普通 `BLEND_Translucent` | 0.7 |
| `Write Alpha Only` | 0.3 |

注意：

- 特殊分支只在非 Substrate 的移动 `BLEND_Translucent` 路径中；Substrate 分支不读取 `ShouldWriteOnlyAlpha()`。
- 它是最后写入者覆盖，不是层间累积；排序、重叠和深度测试会影响最终值。
- 它不写 RGB。若既要画虚拟颜色又要独立控制 VST mask，通常应拆分 draw/material。
- 最终 Copy/Tonemap 仍须保留 Alpha。

## 7. 逐项答案

- 透明物体与 RT：`RGBout=RGBsrc*As+RGBdst*(1-As)`，`Aout=Adst*(1-As)`。
- 默认 Alpha：SceneColor 清屏为 1；不透明像素为 0；0.3 透明物体直接画在清屏上为 0.7；最终 Eye Buffer 还受格式和 Copy/Tonemap 影响。
- PICO 模式：`0: Eye+VST`；`1: Eye*(1-A)+VST*A`；`2: Eye+VST*A`。
- 0.3 透明物体：若初始 Eye Alpha 为 1，则 Eye Alpha 变 0.7；模式 2 用 0.7 衰减 VST。
- `Write Alpha Only`：`Aout=As`，直接覆盖 Alpha，RGB 不变。

## 8. 建议验证

用 RenderDoc for PICO 或设备抓帧检查同一像素：

1. SceneColor 清屏：`A=1`；
2. 不透明物体：`A=0`；
3. 仅一个 0.3 普通透明物体、后方无虚拟不透明物体：`A≈0.7`；
4. 同材质启用 `Write Alpha Only`：`A≈0.3`，RGB 不变；
5. 最终 Eye Buffer 应与第 3/4 步一致。若变全 0 或全 1，检查 `r.Mobile.PropagateAlpha`、SceneColor 格式、Tonemapper、自定义后处理和 `r.Mobile.TonemapSubpass`。

RGBA8 中 0.3/0.7 可能量化为约 `77/255`、`179/255`。
