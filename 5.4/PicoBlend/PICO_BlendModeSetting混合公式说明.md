# r.Mobile.PICO.BlendModeSetting — Eye Buffer 与 VST 混合公式说明

> 依据:本仓库(PICO Unreal Integration SDK 3.2.0,UE_5.4)源码分析
> 官方文档:<https://developer-cn.picoxr.com/document/unreal/seethrough/>

## 1. CVar 定义

定义位置:`UE_5.4/Plugins/PICOXR/Source/PICOXRHMD/Private/PXR_HMD.cpp:90`

```cpp
static TAutoConsoleVariable<int32> CVarPICOBlendModeSetting(
    TEXT("r.Mobile.PICO.BlendModeSetting"),
    1,
    TEXT("0: Covering Mode, VST will cover the entire screen\n")
    TEXT("1: Clip Mode, Eye Buffer and VST will clip by Alpha Before add(Default)\n")
    TEXT("2: Additive Mode, Eye Buffer will not clip by Alpha\n"),
    ECVF_Scalability | ECVF_RenderThreadSafe);
```

取值与枚举的对应关系(`PXR_HMDRuntimeSettings.h:57`):

| 值 | 枚举 `EPICOXRBlendModeType` | 名称 |
|---|---|---|
| 0 | `CoveringMode` | 覆盖模式 |
| 1 | `ClipMode`(默认) | 裁剪模式 |
| 2 | `AdditiveMode` | 叠加模式 |

## 2. 混合发生的位置与方向

实现位置:`UE_5.4/Plugins/PICOXR/Source/PICOXRHMD/Private/PXR_StereoLayer.cpp:808`(`FPICOXRStereoLayer::SubmitLayer_RHIThread`)。

该混合配置**只作用于 ID == 0 的 Projection Layer,即 Eye Buffer(应用渲染的场景层)**。提交时设置 `layerProjection.header.useLayerBlend = 1`,并把 `PxrLayerBlend` 传给系统合成器(Compositor)。

透视开启后,VST(视频透视相机画面)由系统合成器作为**底层背景(Destination)**先行绘制,Eye Buffer 作为**源(Source)**在其上合成。混合方程为标准的加法混合:

```
C_out = SrcColorFactor × C_eye + DstColorFactor × C_vst
A_out = SrcAlphaFactor × A_eye + DstAlphaFactor × A_vst
```

其中:
- `C_eye` / `A_eye`:Eye Buffer 像素的颜色 / Alpha
- `C_vst`:VST 透视画面像素的颜色
- 三种模式仅改变颜色通道因子;Alpha 通道因子固定为 `srcAlpha = ONE`、`dstAlpha = ONE`(`PXR_StereoLayer.cpp:845-846`),即 `A_out = A_eye + A_vst`(饱和到 1)

`PxrBlendFactor` 枚举定义见 `PICOXR/Source/ThirdParty/PXRPlugin/PXRPlugin/Include/PXR_Plugin_Types.h:545`。

## 3. 三种模式的混合因子与计算公式

源码(`PXR_StereoLayer.cpp:823-843`):

```cpp
switch (BlendModeType) {
case EPICOXRBlendModeType::CoveringMode:   // 0
    layerBlend.srcColor = PXR_BLEND_FACTOR_ONE;
    layerBlend.dstColor = PXR_BLEND_FACTOR_ONE;
    break;
case EPICOXRBlendModeType::ClipMode:       // 1 (默认)
    layerBlend.srcColor = PXR_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    layerBlend.dstColor = PXR_BLEND_FACTOR_SRC_ALPHA;
    break;
case EPICOXRBlendModeType::AdditiveMode:   // 2
    layerBlend.srcColor = PXR_BLEND_FACTOR_ONE;
    layerBlend.dstColor = PXR_BLEND_FACTOR_SRC_ALPHA;
    break;
}
layerBlend.srcAlpha = PXR_BLEND_FACTOR_ONE;
layerBlend.dstAlpha = PXR_BLEND_FACTOR_ONE;
```

### 模式 0:Covering Mode(覆盖模式)

| 因子 | 值 |
|---|---|
| srcColor | ONE |
| dstColor | ONE |

```
C_out = 1 × C_eye + 1 × C_vst = C_eye + C_vst
```

Eye Buffer 与 VST **全屏直接相加**,不参考任何 Alpha。VST 画面以全强度覆盖整个屏幕(对应描述 "VST will cover the entire screen"),场景内容叠加在透视画面之上,黑色像素处只能看到 VST,亮色像素处两者叠加提亮。

### 模式 1:Clip Mode(裁剪模式,默认)

| 因子 | 值 |
|---|---|
| srcColor | ONE_MINUS_SRC_ALPHA |
| dstColor | SRC_ALPHA |

```
C_out = (1 − A_eye) × C_eye + A_eye × C_vst
```

**以 Eye Buffer 的 Alpha 作为遮罩,对两者互补裁剪后再相加。** 注意这里使用的是 UE 移动端场景颜色的 Alpha 约定(反转 Alpha,`A_eye = 1 − 不透明度`):

- 场景不透明像素:`A_eye = 0` → `C_out = C_eye`,只显示场景
- 场景空白(未绘制)像素:`A_eye = 1` → `C_out = C_vst`,只显示透视
- 半透明像素(不透明度 o,`A_eye = 1 − o`):`C_out = o × C_eye + (1 − o) × C_vst`,按不透明度线性插值

即等价于常规的 "场景 over 透视" Alpha 混合,这也是它作为默认 MR 模式的原因。使用此模式需要工程正确输出 Alpha 通道(启用 Alpha 通道支持 / `r.Mobile.PropagateAlpha`,详见官方透视文档)。

### 模式 2:Additive Mode(叠加模式)

| 因子 | 值 |
|---|---|
| srcColor | ONE |
| dstColor | SRC_ALPHA |

```
C_out = C_eye + A_eye × C_vst
```

**Eye Buffer 颜色不做 Alpha 裁剪、始终全量输出**(对应描述 "Eye Buffer will not clip by Alpha"),只有 VST 按 Eye Buffer 的 Alpha 参与:

- 场景不透明像素:`A_eye = 0` → `C_out = C_eye`,只显示场景
- 场景空白像素:`A_eye = 1` → `C_out = C_eye + C_vst`,透视全量透出并与场景颜色相加
- 半透明像素:`C_out = C_eye + (1 − o) × C_vst`,场景颜色不衰减,呈自发光/叠加效果

与 Clip Mode 的区别:Clip Mode 中半透明处场景颜色会按不透明度衰减,Additive Mode 中场景颜色永远保持全亮,适合 HUD、发光特效等希望在透视背景上"加亮"的内容。

## 4. 三种模式对比一览

设 `o = 场景像素不透明度`,`A_eye = 1 − o`(UE 反转 Alpha 约定):

| 模式 | 公式 | 不透明像素 (o=1) | 空白像素 (o=0) | 半透明像素 |
|---|---|---|---|---|
| 0 Covering | `C_eye + C_vst` | 场景 + 透视相加 | 纯透视 | 场景 + 透视相加 |
| 1 Clip(默认) | `(1−A_eye)·C_eye + A_eye·C_vst` | 纯场景 | 纯透视 | `o·C_eye + (1−o)·C_vst` |
| 2 Additive | `C_eye + A_eye·C_vst` | 纯场景 | 场景 + 透视相加 | `C_eye + (1−o)·C_vst` |

Alpha 通道(三种模式相同):`A_out = A_eye + A_vst`(饱和)。

## 5. 相关说明

- 设置方式:控制台/配置执行 `r.Mobile.PICO.BlendModeSetting 0|1|2`,CVar 为 `ECVF_RenderThreadSafe`,可运行时切换。
- 该混合仅在开启透视(VST)且合成器绘制透视背景时可见;透视开启相关接口见 `UPICOXRHMDFunctionLibrary::PXR_SetSeeThroughBackground`(`PXR_HMDFunctionLibrary.h:629`)及项目设置中的 `bEnableVST`(`PXR_HMDRuntimeSettings.h:157`)。
- 其它 Stereo Layer(ID != 0)不走此混合配置,只有 Eye Buffer 投影层受本 CVar 控制。

## 6. 常见疑问:材质 Opacity 与公式中 A_eye 的关系

**现象**:场景中只放一个半透明红色物体,材质 Opacity(Alpha)= 0,在 Clip 模式下看到的是 VST 画面而不是红色。按公式 `C_out = (1 − A_eye) × C_eye + A_eye × C_vst`,`A_eye = 0` 不是应该显示物体吗?

**解释**:公式里的 `A_eye` 不是材质里设置的 Opacity,而是该像素在 Eye Buffer(帧缓冲)中最终存储的 Alpha 值,且遵循 UE 的反转约定(1 = 透明/未绘制,0 = 不透明)。整个过程是两级混合:

### 第一级:UE 引擎内部把半透明物体混入 Eye Buffer

半透明材质按标准 Alpha 混合写入帧缓冲,设材质不透明度为 `o`:

```
颜色:C_fb = o × C_材质 + (1 − o) × C_背景
Alpha:A_fb = (1 − o) × A_背景
```

场景清屏后空白像素的初始状态为 `C = 黑,A = 1`(反转约定下 1 表示"什么都没画")。代入 `o = 0`:

- `C_fb = 0 × 红 + 1 × 黑 = 黑` —— 红色被乘以 0,根本没有写进 Eye Buffer
- `A_fb = 1 × 1 = 1` —— 像素 Alpha 保持 1,即仍是"空白"状态

### 第二级:合成器按 Clip 公式混合 Eye Buffer 与 VST

此时该像素的 `A_eye = A_fb = 1`(而不是材质设置的 0),代入公式:

```
C_out = (1 − 1) × C_eye + 1 × C_vst = C_vst
```

所以显示的是纯 VST 画面,与公式完全一致。

### 结论

在空白背景上,材质 Opacity 与公式中 `A_eye` 的关系是:

```
A_eye = 1 − o
```

两者方向正好相反。Opacity = 0 意味着物体完全透明、对 Eye Buffer 零贡献,像素等价于"未绘制"(`A_eye = 1`),Clip 模式对此类像素显示 VST。验证方法:

| 材质 Opacity `o` | Eye Buffer 状态 | Clip 模式结果 |
|---|---|---|
| 0 | 黑色,`A_eye = 1` | 纯 VST 画面 |
| 0.5 | 半强度红,`A_eye = 0.5` | `0.5 × 红 + 0.5 × VST`,红色与透视各半 |
| 1 | 全红,`A_eye = 0` | 实心红色 |
