# `r.Mobile.PICO.BlendModeSetting`：Eye Buffer 与 VST 混合公式

## 结论

对每个像素，记：

- $E=(E_r,E_g,E_b)$：Unreal Eye Buffer 的 RGB（PICO 合成器的 source）。
- $A=A_E$：Eye Buffer 的 Alpha。对 PICO VST 合成而言，应把它理解为 **VST 透过率/背景占比**：$A=0$ 偏向完全显示 Eye Buffer，$A=1$ 偏向完全显示 VST，而不是通常直觉中的“Eye Buffer 不透明度”。
- $V=(V_r,V_g,V_b)$：VST 摄像头画面的 RGB（PICO 合成器的 destination）。
- $C_{out}$：最终送显颜色。

在默认 `ColorScale=(1,1,1,1)`、`ColorOffset=(0,0,0,0)` 下，三种模式的逐通道公式为：

| 设置值 | SDK 枚举 | RGB 公式 | 直观含义 |
|---:|---|---|---|
| `0` | `CoveringMode` | $C_{out}=E+V$ | Eye Buffer 和 VST 都不按 $A$ 衰减，直接相加。VST 全屏参与，容易过曝。 |
| `1`（默认） | `ClipMode` | $C_{out}=E(1-A)+VA$ | Eye Buffer 按 $1-A$ 裁剪，VST 按 $A$ 裁剪。适合普通不透明虚拟物体，但会再次衰减已在 Eye Buffer 内做过 Alpha 混合的特效。 |
| `2` | `AdditiveMode` | $C_{out}=E+VA$ | Eye Buffer 不再按 $A$ 二次裁剪；只有 VST 按 $A$ 裁剪。适合粒子、光晕等含透明度的特效。 |

实际显示通常会限制在显示/合成器支持的颜色范围内，因此可以把结果近似写成 `saturate(...)`；SDK 公共代码没有公开合成器内部的色域、线性空间、色调映射及钳位顺序。

> 最重要的区别：模式 `1` 是 $E(1-A)+VA$，模式 `2` 是 $E+VA$。二者只差 Eye Buffer 是否再乘一次 $1-A$。

## 为什么公式是这样

SDK 在提交主投影层（`ID == 0`，即 Eye Buffer）时启用 `PxrLayerBlend`。固定加法混合可写为：

$$
C_{out}=E\cdot F_{srcColor}+V\cdot F_{dstColor}
$$

源码设置的因子如下：

| 模式 | `srcColor` | `dstColor` | 代入后的公式 |
|---|---|---|---|
| `0` | `ONE` | `ONE` | $E+V$ |
| `1` | `ONE_MINUS_SRC_ALPHA` | `SRC_ALPHA` | $E(1-A)+VA$ |
| `2` | `ONE` | `SRC_ALPHA` | $E+VA$ |

`SRC_ALPHA` 就是被提交的 Eye Buffer Alpha。Eye Buffer 是 source、VST 是 destination 这一角色关系，可由两项事实共同确定：

1. `PxrLayerBlend` 被挂在 `ID == 0` 的 `PxrLayerProjection2`（主 Eye Buffer）上。
2. 官方文档对模式 `2` 的描述是“Eye Buffer 叠加到 VST 层，且只有 VST 层会根据 Alpha 值做裁剪”，与 $E+VA$ 完全对应。

PICO 公共头文件只定义了 Blend Factor 枚举和结构体，没有公开底层合成器着色器；所以上述表达式是由公开的标准加法 Blend Equation、SDK 实际因子赋值和官方效果语义共同还原出的等价逐像素公式。

## Alpha 通道的输出

三种模式都固定设置：

```cpp
layerBlend.srcAlpha = PXR_BLEND_FACTOR_ONE;
layerBlend.dstAlpha = PXR_BLEND_FACTOR_ONE;
```

因此，如果继续按固定加法方程描述合成后 Alpha，则三种模式都是：

$$
A_{out}=A_E+A_V
$$

它可能在目标格式中被钳位。不过，这个 `A_out` 不是三种模式产生视觉差异的来源；差异来自 RGB 的 `srcColor` / `dstColor` 因子。最终送显通常主要关心 RGB，SDK 公共代码也没有展示 `A_out` 后续是否还被消费。

## VST 自身的 Alpha 默认值是多少？

### 可以直接确认的结论

**当前公开 Unreal SDK 和官方“视频透视”文档都没有定义或设置 VST 自身的 Alpha 默认值。无法仅依据本仓库断言它恒为 `0`，也无法断言它恒为 `1`。**

更准确地说，在这条合成路径中，VST 由设备侧 PICO 系统/合成服务生成，并不是 Unreal 插件创建、清空或逐像素写入的一张 VST RGBA 纹理。Unreal 插件只提交主 Eye Buffer 及 Blend Factor：

- `PXR_SetSeeThroughBackground(bool)` 最终只把启用/关闭布尔值传给闭源 Native 接口 `Pxr_SetSeeThroughBackground(bool)`，没有 Alpha 参数：`PXR_HMDFunctionLibrary.cpp:345-350`、`PXR_BoundarySystem.cpp:165-170`、`PXR_Plugin.h:79`。
- `PXR_LAYER_VST_MASK` 只在公开头文件中作为 Layer Shape 枚举声明，本插件中没有创建或提交该 Shape 的代码：`PXR_Plugin_Types.h:43-52`。
- PICO Enterprise 的 `PE_AcquireVSTCameraFrame` 是另一条“获取原始相机帧数据”的路径。公开结构只有运行时 `format` 和字节数据，没有独立 Alpha 字段或默认值；它也不是此处设备合成器持有的 VST Layer：`PXR_EnterpriseTypes.h:86-108`。

### 为什么观感不能证明 VST Alpha 为 0

三种模式的 **RGB** 因子没有任何一个读取 VST Alpha：

```text
模式 0：srcColor = ONE,                 dstColor = ONE
模式 1：srcColor = ONE_MINUS_SRC_ALPHA, dstColor = SRC_ALPHA
模式 2：srcColor = ONE,                 dstColor = SRC_ALPHA
```

公开枚举虽然提供 `PXR_BLEND_FACTOR_DST_ALPHA`，但这三种模式从未使用它。因此 RGB 实际上是：

$$
C_{out}=E\cdot f(A_E)+V\cdot g(A_E)
$$

而不是：

$$
C_{out}=E\cdot f(A_E)+V\cdot g(A_V)
$$

所以无论内部 $A_V$ 是 `0`、`1` 还是没有有意义的 Alpha 通道，上一节的三条 RGB 公式都不变。画面中 VST 显示多少，是由 **Eye Buffer Alpha $A_E$** 通过 `SRC_ALPHA` 控制的。

还要特别注意：

```cpp
layerBlend.dstAlpha = PXR_BLEND_FACTOR_ONE;
```

并不表示“把 VST Alpha 设置成 1”。`ONE` 是计算合成后 Alpha 时给已有 destination Alpha 乘的系数；它不修改 destination/VST 原本保存的 Alpha 值。若设备合成器遵循固定加法方程，形式上仍是 $A_{out}=A_E+A_V$，但 $A_V$ 的实际值及 $A_{out}$ 的后续用途都没有通过公开 SDK 暴露。

### 如果必须在设备上实测

公开 API 没有读取系统 VST Alpha 的 Getter。可在独立调试版本中临时增加一组 Blend Factor 来间接验证：

```cpp
// 只输出 VST * VST_Alpha
layerBlend.srcColor = PXR_BLEND_FACTOR_ZERO;
layerBlend.dstColor = PXR_BLEND_FACTOR_DST_ALPHA;
```

再与下面的“只输出完整 VST”对照：

```cpp
layerBlend.srcColor = PXR_BLEND_FACTOR_ZERO;
layerBlend.dstColor = PXR_BLEND_FACTOR_ONE;
```

- 两者相同：运行时采样到的 VST Alpha 接近 `1`。
- 第一种全黑：运行时采样到的 VST Alpha 接近 `0`。
- 第一种局部/渐变：VST Alpha 可能携带逐像素遮罩。

这只能测出特定设备、PICO OS 和 compositor 版本在该接口下的实际行为，不能代替公开 API 契约；设备侧也可能把 VST 当作无 Alpha 的 RGB 图像，并为 `DST_ALPHA` 定义内部常量。

## 场景实例：透明红色物体的材质 Alpha 为 0，为什么仍然看到 VST？

这是符合预期的结果。必须区分两个不同的 Alpha：

- **材质 Alpha / Opacity $α_m$**：当前透明物体对 Eye Buffer 的颜色和遮挡贡献。
- **混合公式中的 $A=A_E$**：整个 Unreal 场景渲染完成后，该像素最终写入 **Eye Buffer** 的 Alpha；在 PICO VST 合成路径中，它相当于 VST 透过率。

它们不是同一个渲染阶段的数值，也不能把“材质 $α_m=0$”直接代入最终合成公式中的 $A_E$。

### 该场景的计算过程

假设场景没有其他物体，背景是供 VST 显示的透明背景。该背景像素通常近似为：

$$
E_{bg}=(0,0,0),\qquad A_{E,bg}=1
$$

红色透明物体的材质 Opacity 为 $α_m=0$ 时，它对 Eye Buffer RGB 的标准透明混合贡献为 0：

$$
E_{new}=C_{red}\alpha_m+E_{bg}(1-\alpha_m)
$$

代入 $α_m=0$：

$$
E_{new}=C_{red}\cdot0+E_{bg}\cdot1=E_{bg}\approx(0,0,0)
$$

由于该物体完全透明，它也不会为这个像素建立虚拟内容遮挡，最终 Eye Buffer 的 VST 透过率仍接近：

$$
A_E\approx1
$$

再代入模式 `1`：

$$
\begin{aligned}
C_{out}
&=E(1-A_E)+VA_E\\
&=0\cdot(1-1)+V\cdot1\\
&=V
\end{aligned}
$$

所以最终看到完整的 VST 画面，红色物体不可见。

### 材质 Opacity 与 Eye Buffer Alpha 的直观对应

在这条 VST 渲染路径的典型情况下，可以近似理解为：

$$
A_E\approx1-\alpha_m
$$

| 材质 Opacity $α_m$ | 最终 Eye Buffer Alpha $A_E$ | 典型观感 |
|---:|---:|---|
| `0`，完全透明 | 接近 `1` | 物体不贡献红色，完整显示 VST |
| `0.5`，半透明 | 接近 `0.5` | 红色与 VST 混合 |
| `1`，完全不透明 | 接近 `0` | 显示红色物体并遮住 VST |

这个对应关系是便于理解的典型近似。实际 Eye Buffer Alpha 还可能受到材质 Blend Mode、是否写 Alpha、预乘/非预乘方式、后处理以及 Unreal Alpha 传播设置影响。

### 为什么半透明物体更适合模式 `2`

当 $0<\alpha_m<1$ 时，Eye Buffer 中的红色 RGB 通常已经由 Unreal 透明混合衰减过一次，例如：

$$
E\approx C_{red}\alpha_m
$$

模式 `1` 又会令它乘 $1-A_E\approx\alpha_m$：

$$
E(1-A_E)\approx C_{red}\alpha_m^2
$$

这会产生二次 Alpha 衰减，使红色物体过暗、边缘异常甚至近似消失。模式 `2` 使用：

$$
C_{out}=E+VA_E
$$

它保留 Eye Buffer 中已经混合好的红色 $E$，只用 $A_E$ 裁剪 VST，通常更适合粒子、光晕和半透明材质。

> 无论使用模式 `1` 还是 `2`，材质 Opacity 为 `0` 都表示红色贡献为 0。若希望看到半透明红色覆盖在 VST 上，应使用大于 0 的 Opacity，例如 `0.2`～`0.5`，并优先测试模式 `2`。

## Alpha 的正确理解与半透明特效

可以用三个边界值快速检查模式 `1`：

| Eye Buffer Alpha $A$ | 模式 `1` 输出 |
|---:|---|
| `0` | $E$ |
| `0.5` | $0.5E+0.5V$ |
| `1` | $V$ |

所以在该 VST 合成路径中，$A$ 更像“让 VST 通过多少”的遮罩。

对于一个不透明虚拟物体，典型像素接近 $A=0$，模式 `1` 输出 Eye Buffer；纯 VST 背景接近 $A=1$，且 Eye Buffer RGB 通常接近 0，模式 `1` 和 `2` 都输出 VST。

对于透明粒子/光晕，Eye Buffer 中的 RGB 往往已经经过 Unreal 内部透明混合，可近似看作预乘后的 $E\approx C_f\alpha_f$，而其透过率 $A\approx1-\alpha_f$：

- 模式 `1` 又对 $E$ 乘 $1-A\approx\alpha_f$，结果近似 $C_f\alpha_f^2$，发生二次 Alpha 衰减，特效可能变暗、边缘异常或消失。
- 模式 `2` 保留 $E\approx C_f\alpha_f$，并加入 $V(1-\alpha_f)$，即常见的预乘 Alpha “over” 形式，因此官方推荐它处理包含透明度的特效。

## 三种模式的实际观感

### `0`：Covering Mode

```text
Eye Buffer: 100%
VST:        100%
结果:       E + V
```

VST 在全屏参与相加，不受 Eye Buffer Alpha 遮挡；高亮虚拟内容与摄像头画面相加后容易饱和、泛白。SDK 的 CVar 帮助文本称“VST will cover the entire screen”，当前官方文档则称“VST 层会叠加到 Eye Buffer，适用于 Passthrough 与 Eye Buffer 同时显示”。两者都指向 VST 不被 Alpha 局部裁掉，源码公式以 $E+V$ 为准。

### `1`：Clip Mode（默认）

```text
Eye Buffer: 权重 1 - A
VST:        权重 A
结果:       E * (1 - A) + V * A
```

这是普通的 Eye/VST 遮罩插值。对不透明虚拟物体和纯 VST 背景最直观，但透明特效的 Eye RGB 可能被二次 Alpha 衰减。

### `2`：Additive Mode

```text
Eye Buffer: 权重 1
VST:        权重 A
结果:       E + V * A
```

Eye Buffer 不再被合成器二次乘 Alpha。只要 Unreal 输出到 Eye Buffer 的透明特效已经正确完成内部混合，这一模式通常能让它自然叠加在 VST 上。

## 源码定位（本仓库）

本结论针对当前仓库 `UE_5.4` 下的 PICO XR 插件 `VersionName: 3.2.3`：

- CVar 定义、默认值 `1` 和三种模式的帮助文本：`UE_5.4/Plugins/PICOXR/Source/PICOXRHMD/Private/PXR_HMD.cpp:90-96`
- `0/1/2` 到 `CoveringMode/ClipMode/AdditiveMode` 的枚举顺序：`UE_5.4/Plugins/PICOXR/Source/PICOXRHMD/Public/PXR_HMDRuntimeSettings.h:57-62`
- 主投影层的三组 RGB Blend Factor，以及统一的 Alpha Blend Factor：`UE_5.4/Plugins/PICOXR/Source/PICOXRHMD/Private/PXR_StereoLayer.cpp:813-848`
- `PxrBlendFactor` 各枚举值：`UE_5.4/Plugins/PICOXR/Source/ThirdParty/PXRPlugin/PXRPlugin/Include/PXR_Plugin_Types.h:545-553`
- `PxrLayerBlend` 的四个字段：`UE_5.4/Plugins/PICOXR/Source/ThirdParty/PXRPlugin/PXRPlugin/Include/PXR_Plugin_Types.h:1082-1087`
- 插件版本：`UE_5.4/Plugins/PICOXR/PICOXR.uplugin:4`

`PxrLayerBlend` 只在 `ID == 0` 分支设置，因此这里讨论的是主 Eye Buffer 与系统 VST 的合成，不是普通 Unreal 材质节点内部的混合，也不是其他 Stereo Layer 自身的材质混合。

## 官方文档核对

PICO 官方文档：[视频透视（Unreal）](https://developer-cn.picoxr.com/document/unreal/seethrough/)

文档明确说明（页面当前内容）：

- 此设置仅适用于 UE 5.4 及以上。
- `0`：VST 层叠加到 Eye Buffer，二者同时显示。
- `1`（默认）：VST 与 Eye Buffer 都根据 Alpha 裁剪；透明度特效可能存在混合问题。
- `2`：Eye Buffer 叠加到 VST，只有 VST 根据 Alpha 裁剪；包含透明度的特效可正常混合。

## 使用建议与注意事项

- 常规 MR 场景、主要是不透明几何体：先使用默认值 `1`。
- 需要透明粒子、光晕、半透明 UI 等效果：通常使用 `2`。
- 希望 Eye Buffer 与 VST 无条件相加或做特殊视觉实验：使用 `0`，但要注意过曝/饱和。
- 不要设置 `0/1/2` 之外的值。当前 `switch` 的 `default` 不赋 RGB 因子，而 `PxrLayerBlend layerBlend = {}` 会令未赋值项保持 0，结果不属于任何受支持模式。
- 这些公式描述的是 PICO 系统合成阶段；Eye Buffer 内部的 Unreal 材质混合、后处理和 Alpha 写入发生在此之前，会直接影响这里的 $E$ 和 $A$。
