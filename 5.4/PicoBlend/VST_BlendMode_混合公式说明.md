# r.Mobile.PICO.BlendModeSetting 混合模式说明

> 适用范围：UE 5.4 及以上（仅 PICO 定制引擎）
> 官方文档：https://developer-cn.picoxr.com/document/unreal/seethrough/
> 源码位置：`UE_5.4/Plugins/PICOXR/Source/PICOXRHMD/Private/PXR_StereoLayer.cpp`（`SubmitLayer_RHIThread`，ID == 0 的 Projection 层，即 Eye Buffer 层）

## 混合由谁执行

混合**不是在 UE 引擎内完成的**。SDK 只是通过 `PxrLayerBlend` 给提交给 PICO 系统合成器（合成服务）的 Eye Buffer 投影层设置 4 个混合因子（`srcColor` / `dstColor` / `srcAlpha` / `dstAlpha`），真正的逐像素混合由 PICO 系统合成器在上屏时执行。

合成器采用标准混合方程：

```
C_out = srcColorFactor * C_src + dstColorFactor * C_dst
```

其中：

- `C_src` = 当前提交的层，即 **Eye Buffer**（应用渲染的虚拟画面）
- `C_dst` = 位于其下的层，即 **VST 透视画面**
- `A` = **Eye Buffer 的 Alpha 通道**（alpha = 1 表示该像素应显示透视，例如 UnderlayMaterial 区域；alpha = 0 表示不透明虚拟内容）

三种模式下 `srcAlpha` / `dstAlpha` 恒为 `ONE / ONE`（Alpha 通道不做区分，始终 `A_out = A_src + A_dst`），区别只在颜色通道。

## 三种模式的计算公式

### 0 — Covering Mode（覆盖模式）

```cpp
layerBlend.srcColor = PXR_BLEND_FACTOR_ONE;
layerBlend.dstColor = PXR_BLEND_FACTOR_ONE;
```

```
C_out = C_EyeBuffer + C_VST
```

VST 画面以全强度叠加到整个 Eye Buffer 上（VST 覆盖全屏），虚拟画面直接相加在其上，**完全忽略 Alpha**。
适用场景：Passthrough 与 Eye Buffer 需要同时全屏显示的场景。

### 1 — Clip Mode（裁剪模式，默认）

```cpp
layerBlend.srcColor = PXR_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
layerBlend.dstColor = PXR_BLEND_FACTOR_SRC_ALPHA;
```

```
C_out = C_EyeBuffer * (1 - A) + C_VST * A
```

Eye Buffer 和 VST 层**都按 Alpha 裁剪**后相加：

- `A = 0`（不透明虚拟物体）→ 只显示 Eye Buffer
- `A = 1`（透视区域，如 UnderlayMaterial）→ 只显示 VST
- `0 < A < 1` → 两者按 alpha 加权混合

注意：该模式下半透明（透明度混合）特效的 RGB 会被 `(1 - A)` 整体缩放，因此半透明特效可能存在混合问题。

### 2 — Additive Mode（叠加模式）

```cpp
layerBlend.srcColor = PXR_BLEND_FACTOR_ONE;
layerBlend.dstColor = PXR_BLEND_FACTOR_SRC_ALPHA;
```

```
C_out = C_EyeBuffer + C_VST * A
```

Eye Buffer 不按 Alpha 裁剪（因子为 1，直接整体叠加到 VST 上），**只有 VST 层按 Alpha 裁剪**：

- `A = 0`（不透明区域）→ 只显示 Eye Buffer，VST 被完全裁掉
- `A = 1`（透视区域）→ `C_EyeBuffer + C_VST`，包含半透明度的特效以加法方式与 VST 正常混合

适用场景：需要半透明特效与透视画面正确混合的场景（官方推荐用于解决半透明特效在模式 1 下的混合问题）。

## 对照表

| 模式 | srcColor (Eye Buffer 因子) | dstColor (VST 因子) | 公式 | 特点 |
|---|---|---|---|---|
| 0 Covering | `ONE` | `ONE` | `C_Eye + C_VST` | 忽略 Alpha，VST 全屏覆盖 |
| 1 Clip（默认） | `ONE_MINUS_SRC_ALPHA` | `SRC_ALPHA` | `C_Eye*(1-A) + C_VST*A` | 双方都按 Alpha 裁剪，半透明特效可能有问题 |
| 2 Additive | `ONE` | `SRC_ALPHA` | `C_Eye + C_VST*A` | Eye Buffer 直接叠加，仅 VST 按 Alpha 裁剪，半透明特效可正常混合 |

## 设置方式

控制台命令（仅运行时生效）：

```
r.Mobile.PICO.BlendModeSetting 0   // 覆盖模式
r.Mobile.PICO.BlendModeSetting 1   // 裁剪模式（默认）
r.Mobile.PICO.BlendModeSetting 2   // 叠加模式
```

或在 `DefaultEngine.ini` 中写入：

```ini
[ConsoleVariables]
r.Mobile.PICO.BlendModeSetting=2
```

## 相关源码

- CVar 定义：`UE_5.4/Plugins/PICOXR/Source/PICOXRHMD/Private/PXR_HMD.cpp:90`
- 混合因子设置：`UE_5.4/Plugins/PICOXR/Source/PICOXRHMD/Private/PXR_StereoLayer.cpp:808`（`FPICOXRStereoLayer::SubmitLayer_RHIThread`）
- 枚举定义：`UE_5.4/Plugins/PICOXR/Source/PICOXRHMD/Public/PXR_HMDRuntimeSettings.h:57`（`EPICOXRBlendModeType`：CoveringMode / ClipMode / AdditiveMode）
- API 结构体：`UE_5.4/Plugins/PICOXR/Source/ThirdParty/PXRPlugin/PXRPlugin/Include/PXR_Plugin_Types.h:1082`（`PxrLayerBlend`）
