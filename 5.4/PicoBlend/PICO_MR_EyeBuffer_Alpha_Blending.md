# 移动端前向渲染 · 半透明 Alpha → Eye Buffer → VST 混合 全链路分析

> 适用范围：UE 5.4 Mobile Forward Shading + PICO Unreal Integration SDK（`Engine/PICO-Unreal-Integration-SDK/UE_5.4`）
> 所有结论均来自本仓库源码，文中标注了 `文件:行号` 便于复核。
> 说明：PICO 官方 SeeThrough 文档页为 SPA，抓取不到正文，因此本文以引擎与 SDK 源码为准（源码是运行时真正生效的东西）。

---

## 0. 先回答"我的理解对不对"

你的问题原话：

> 我透明物体的 Alpha 如何影响了最终 RT（Eye Buffer）的 Alpha，进而影响了 Eye Buffer 与 VST 的混合？

**框架完全正确，但有一处关键语义必须纠正。**

| 你的理解 | 判定 | 说明 |
|---|---|---|
| Eye Buffer 就是引擎输出的那张最终 RT | ✅ 正确 | 就是 `ViewFamilyTexture`，即 PICO 的 Projection Layer(ID=0) swapchain，格式 `R8G8B8A8` |
| PICO 文档说的"根据 Alpha 混合 Eye Buffer 与 VST"，这个 Alpha 就是 Eye Buffer 的 Alpha | ✅ 正确 | 合成发生在 PICO Compositor，`src` = Eye Buffer，`dst` = VST |
| 半透明物体的 Alpha 会影响最终 RT 的 Alpha，进而影响 VST 混合 | ✅ 方向正确 | 但**影响方式不是"写入"，是"相乘衰减"** |
| **半透明物体 Opacity=0.3，RT 的 Alpha 就变成 0.3** | ❌ **错误** | 实际变成 `旧Alpha × 0.7`。见下 |

### ⚠️ 必须先建立的语义：UE 的 Alpha 是"透过率"，不是"不透明度"

这是理解全部内容的前提，也是绝大多数人踩坑的地方：

```
Eye Buffer 的 Alpha  ==  "这个像素还有多少比例没有被虚拟内容盖住"
                     ==  背景/VST 的透过率 (transmittance)
```

| Alpha 值 | 含义 | MR 下的表现 |
|---|---|---|
| **1.0** | 该像素**完全没有**虚拟内容 | 显示纯 VST（透视看到真实世界） |
| **0.0** | 该像素被虚拟内容**完全遮挡** | 显示纯虚拟画面 |
| 0.7 | 70% 露出背景 | 70% VST + 30% 虚拟内容 |

所以它和材质面板里的 `Opacity` 恰好是**相反**的量：

```
写入 RT 的 Alpha 增量语义 = 1 - Opacity
```

证据（不透明物体在 Base Pass 里往 Alpha 写 **0**，不是 1）：

`Engine/Shaders/Private/MobileBasePassPixelShader.usf:1071-1080`
```hlsl
#else
    OutColor.rgb = Color * VertexFog.a + VertexFog.rgb;

    #if !MATERIAL_USE_ALPHA_TO_COVERAGE
        // Planar reflections and scene captures use scene color alpha to keep track
        // of where content has been rendered, for compositing into a different scene later
        OutColor.a = 0.0;          // ← 不透明物体：Alpha = 0
    #else
        ...
    #endif
#endif
```

注释写得很明白：**Alpha 用来记录"哪里已经画过东西了"**。画过 = 0，没画过 = 1。

---

## 1. 完整渲染链路

```
┌──────────────────────────────────────────────────────────────────┐
│ 1. SceneColor RT 创建                                             │
│    格式: PF_FloatRGBA (需 r.Mobile.PropagateAlpha=1，否则 R11G11B10 无 A) │
│    ClearValue: FClearValueBinding::Black = (0,0,0,1)   ← Alpha=1  │
├──────────────────────────────────────────────────────────────────┤
│ 2. Base Pass 开始，LoadAction = EClear                            │
│    整张 RT 被清成 Alpha = 1.0（"到处都是背景"）                     │
├──────────────────────────────────────────────────────────────────┤
│ 3. Opaque / Masked 绘制                                           │
│    BlendState = 默认不混合(One, Zero)，PS 输出 OutColor.a = 0      │
│    → 被物体覆盖的像素 Alpha 直接变 0                               │
├──────────────────────────────────────────────────────────────────┤
│ 4. Translucent 绘制（前向：同一个 RenderPass 里直接混进 SceneColor）│
│    Alpha 通道: A_dst = A_dst × (1 - Opacity)     ← 连乘衰减        │
├──────────────────────────────────────────────────────────────────┤
│ 5. PostProcess / Tonemapper                                       │
│    POST_PROCESS_ALPHA==2 时: OutColor.a = SceneColor.a（原样透传）  │
│    否则 OutColor 初值为 0 → Alpha 全 0（MR 直接失效）              │
├──────────────────────────────────────────────────────────────────┤
│ 6. 写入 ViewFamilyTexture = PICO Eye Buffer (R8G8B8A8 swapchain)  │
├──────────────────────────────────────────────────────────────────┤
│ 7. 提交给 PICO Compositor，按 r.Mobile.PICO.BlendModeSetting 与 VST 合成 │
└──────────────────────────────────────────────────────────────────┘
```

关键代码位置：

| 环节 | 位置 |
|---|---|
| SceneColor ClearValue | `Source/Runtime/Engine/Private/SceneTexturesConfig.cpp:268` |
| `FClearValueBinding::Black` 定义 | `Source/Runtime/RHI/Private/RHI.cpp:105` |
| SceneColor 格式选择 | `Source/Runtime/Engine/Private/SceneTexturesConfig.cpp:53-64` |
| 是否需要 Alpha 通道 | `Source/Runtime/Renderer/Private/SceneTextures.cpp:400` |
| Base Pass RT 绑定 + EClear | `Source/Runtime/Renderer/Private/MobileShadingRenderer.cpp:1485` |
| 不透明/半透明 PS 输出 | `Shaders/Private/MobileBasePassPixelShader.usf:1060-1082` |
| 半透明 BlendState | `Source/Runtime/Renderer/Private/MobileBasePass.cpp:615-744` |
| Tonemapper 透传 Alpha | `Shaders/Private/PostProcessTonemap.usf:454, 524-526` |
| Eye Buffer swapchain 格式 | `PICO-Unreal-Integration-SDK/.../PXR_StereoLayer.cpp:453, 656` |
| PICO 合成 BlendMode | `PICO-Unreal-Integration-SDK/.../PXR_StereoLayer.cpp:819-848` |

---

## 2. 问题一：最终 RT 的默认 Alpha 值是多少？

### **答案：1.0**

链路：

`Source/Runtime/Engine/Private/SceneTexturesConfig.cpp:268`
```cpp
ColorClearValue = FClearValueBinding::Black;
```

`Source/Runtime/RHI/Private/RHI.cpp:105`
```cpp
const FClearValueBinding FClearValueBinding::Black(FLinearColor(0.0f, 0.0f, 0.0f, 1.0f));
//                                                                            ^^^^ Alpha = 1
```

`Source/Runtime/Renderer/Private/MobileShadingRenderer.cpp:1485`
```cpp
BasePassRenderTargets[0] = FRenderTargetBinding(SceneColor, SceneColorResolve,
                                                ERenderTargetLoadAction::EClear);
```

注意 `FClearValueBinding::Black` 是 `(0,0,0,**1**)`，**不是** `Transparent (0,0,0,0)`。UE 里 `Transparent` 是另一个常量（`RHI.cpp:108`）。

> **推论**：一帧开始时整张 RT 的 Alpha 是 1.0 —— 也就是"整个视野都是 VST"。之后每画一个虚拟物体，就把对应像素的 Alpha 往 0 拉。这正是 MR 想要的默认行为：**没画东西的地方自动透出真实世界。**

---

## 3. 问题二：半透明物体的 Alpha 如何与 RT 的 Alpha 混合？公式是什么？

### 3.1 半透明的 Blend State

`Source/Runtime/Renderer/Private/MobileBasePass.cpp:666`（`BLEND_Translucent`，未勾选 Write Alpha Only）
```cpp
TStaticBlendState<CW_RGBA,
    BO_Add, BF_SourceAlpha, BF_InverseSourceAlpha,   // RGB
    BO_Add, BF_Zero,        BF_InverseSourceAlpha    // Alpha
>::GetRHI()
```

`TStaticBlendState` 模板参数顺序：
`<ColorWriteMask, ColorBlendOp, ColorSrcFactor, ColorDstFactor, AlphaBlendOp, AlphaSrcFactor, AlphaDstFactor>`

### 3.2 PS 输出

`Shaders/Private/MobileBasePassPixelShader.usf:1064-1065`
```hlsl
#elif MATERIALBLENDING_TRANSLUCENT
    OutColor = half4(Color * VertexFog.a + VertexFog.rgb, Opacity);
    //                                                    ^^^^^^^ SrcAlpha = 材质 Opacity
#endif
```

### 3.3 ⭐ 混合公式

设：
- `A_src` = 材质 Opacity（本例 0.3）
- `A_dst` = RT 中已有的 Alpha
- `C_src` = 物体颜色，`C_dst` = RT 中已有颜色

**RGB 通道**（标准 over 混合）：
```
C_dst' = C_src × A_src + C_dst × (1 − A_src)
```

**Alpha 通道**（重点！）：
```
A_dst' = A_src × 0  +  A_dst × (1 − A_src)

  ⇒   A_dst' = A_dst × (1 − Opacity)
```

因为 `AlphaSrcFactor = BF_Zero`，**源 Alpha 本身完全不进入结果**，它只作为"衰减系数 `(1 - Opacity)`"去乘目标 Alpha。

这是标准的**透过率连乘 (transmittance accumulation)**：每穿过一层半透明介质，背景就再衰减一次。

### 3.4 所有 Blend Mode 的 Alpha 通道行为（移动端前向）

| 材质 Blend Mode | 源码 | Alpha 通道公式 | 对 VST 的效果 |
|---|---|---|---|
| **Opaque / Masked** | `MobileBasePass.cpp:562-574` + PS `a=0.0` | `A_dst' = 0` | 完全遮挡 VST |
| **Masked + Alpha To Coverage** | `MobileBasePass.cpp:565` `CW_RGB` | `A_dst' = A_dst`（**不写**） | ⚠️ **不遮挡 VST**，见 §3.6.1 |
| **Translucent** | `MobileBasePass.cpp:666` | `A_dst' = A_dst × (1−Opacity)` | 按 Opacity 逐层遮挡 |
| **Translucent + Write Alpha Only** | `MobileBasePass.cpp:655` | `A_dst' = Opacity`（**直接覆盖**） | 精确指定 VST 权重 |
| **Additive** | `MobileBasePass.cpp:678` + PS `a=0.0f` | `A_dst' = A_dst`（**完全不变**） | ⚠️ **不遮挡 VST**，见 §3.5 |
| **Modulate** | `MobileBasePass.cpp:689` `CW_RGB` | `A_dst' = A_dst`（**不写**） | ⚠️ **不遮挡 VST**，见 §3.6.2 |
| **AlphaComposite** | `MobileBasePass.cpp:700` | `A_dst' = A_dst × (1−Opacity)` | 同 Translucent（但 RGB 需自行预乘，见 §3.6.3） |
| **AlphaHoldout** | `MobileBasePass.cpp:711` | `A_dst' = A_src + A_dst × (1−A_src)` | **增加** VST（挖洞），且 RGB 被 `(1−A_src)` 削暗 |
| **Thin Translucent**（着色模型） | `MobileBasePass.cpp:646` → `577-613` | 视子路径而定，**主流路径下 `A_dst' = A_dst`** | ⚠️ 多数设备上**不遮挡 VST**，见 §3.6.4 |
| **Single Layer Water**（着色模型） | `MobileBasePass.cpp:724` | `A_dst' = A_dst × (1−Opacity)` | 浅水区 Alpha 残留高，VST 会从水里透出，见 §3.6.5 |

> `AlphaHoldout` 是唯一一个会把 Alpha **往上**推的模式，语义上就是"在虚拟画面上挖一个透视窗口"。它同时会把已有 RGB 乘 `(1−A_src)` 变暗，所以在 ClipMode 下会出现二次衰减（见第 6 节）。想要干净地挖洞，用 **Write Alpha Only** 更可控。
>
> 表中标 ⚠️ 的四类都是"Alpha 通道被绕过"的特例——它们和 Additive 是同一个家族的问题，统一在 §3.6 展开。

### 3.5 ⚠️ Additive 特例：Opacity 在 Shader 里预乘，Alpha 通道完全不动

Additive 的 BlendState（`MobileBasePass.cpp:678`）RGB 部分是 `BF_One, BF_One`：

```
C_dst' = C_src × 1 + C_dst × 1 = C_src + C_dst
```

**混合器层面 Alpha 完全不参与颜色混合。** 但实际使用中 Opacity 明明能控制 Additive 物体的"透明度"，原因在 Pixel Shader：

`Shaders/Private/MobileBasePassPixelShader.usf:1066-1067`
```hlsl
#elif MATERIALBLENDING_ADDITIVE
    OutColor = half4(Color * (VertexFog.a * Opacity.x), 0.0f);
    //               ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^  ^^^^
    //               Opacity 在 Shader 里就乘进 RGB 了   输出 Alpha 硬编码为 0
```

即 **预乘 alpha（premultiplied alpha）**：

```
C_src  = Color × Opacity        ← Shader 内完成
C_dst' = C_src + C_dst          ← 混合器只做加法
```

数学上 `(0.3·C) + C_dst` 与"先按 alpha 缩放再加"完全等价，所以视觉上和 alpha 混合无异，但机制不同。这也是 Additive 材质**不需要排序**的原因：加法可交换。

#### 关键后果：`OutColor.a = 0.0f`

代入 Alpha 混合式 `BO_Add, BF_Zero, BF_InverseSourceAlpha`：

```
A_dst' = 0 × BF_Zero + A_dst × (1 − 0) = A_dst        ← 保持不变
```

**Additive 材质根本不修改 Eye Buffer 的 Alpha，因此完全不遮挡 VST。**

在空背景处（`A = 1.0`）与 VST 合成：

| 模式 | 计算 | 结果 |
|---|---|---|
| **ClipMode（默认）** | `E×(1−1) + V×1` | **`= V`，加法特效被完全抹掉，一点都看不见** ❌ |
| **AdditiveMode** | `E×1 + V×1` | `= 0.3·C + V`，特效正确叠加在透视上 ✅ |
| CoveringMode | `E + V` | 同上，正常 ✅ |

> **实践含义**：火焰、光晕、法术、Bloom 类 Additive 粒子特效，在默认的 ClipMode 下**在透视区域会凭空消失**。这很可能就是 PICO 单独提供 `AdditiveMode` 的原因——模式名里的 "Additive" 指的正是这类内容。
>
> 若项目重度依赖 Additive 特效：
> - 方案一：`r.Mobile.PICO.BlendModeSetting 2` 全局切 AdditiveMode（注意半透明也会跟着换公式，见 §5）
> - 方案二：保持 ClipMode，在特效区域额外叠一个 **Write Alpha Only** 的面片把 Alpha 压低，给特效"腾出"权重

### 3.6 其余 Blend Mode 的特例逐条核对

Additive 不是孤例。把移动端前向所有路径的 **PS Alpha 输出** 和 **BlendState Alpha 段 + 写掩码** 逐一对齐后，共有 **5 类特例**。判据只有一个：

> **Eye Buffer 的 Alpha 最终有没有被压低？没压低 = 不遮挡 VST = 在透视区域被 ClipMode 抹掉。**

#### 3.6.1 ⚠️ Masked + Use Alpha To Coverage —— Alpha 被写掩码挡住（最容易踩）

A2C 的 Masked 材质走的是 **Opaque 路径**，但 `SetOpaqueRenderState` 为它单独设了一个 BlendState：

`Source/Runtime/Renderer/Private/MobileBasePass.cpp:562-573`
```cpp
const bool bIsMasked = IsMaskedBlendMode(Material);
if (bIsMasked && Material.IsUsingAlphaToCoverage())
{
    DrawRenderState.SetBlendState(TStaticBlendState<CW_RGB, BO_Add, BF_One, BF_Zero, BO_Add, BF_One, BF_Zero,
                                  /* ...RT1-7... */ true>::GetRHI());
    //                                              ^^^^^^  RT0 写掩码 = CW_RGB     ^^^^ bUseAlphaToCoverage
}
```

而 PS 在 A2C 分支里**确实往 alpha 写了东西**：

`Shaders/Private/MobileBasePassPixelShader.usf:1074-1081`
```hlsl
#if !MATERIAL_USE_ALPHA_TO_COVERAGE
    OutColor.a = 0.0;                                    // 普通 Opaque/Masked：压到 0，正常遮挡
#else
    half MaterialOpacityClip = GetMaterialOpacityMaskClipValue();
    float Mask = GetMaterialMask(PixelMaterialInputs) / (1.0 - MaterialOpacityClip);
    OutColor.a = uint((Mask + 0.25f) * 4.0f) / 4.0f;     // 量化的覆盖率，喂给 A2C 硬件阶段
#endif
```

**关键点**：A2C 硬件阶段在**写掩码之前**读取 PS 的 alpha 来生成 coverage mask；随后 `CW_RGB` 把 alpha 通道的写入丢弃。于是：

```
A_dst' = A_dst        ← 完全不变，停留在清屏值 1.0
```

> **后果**：勾了 *Use Alpha To Coverage* 的树叶、草、铁丝网、头发，在 MR 下**整片透出 VST**——不是边缘发虚，是整个网格都不遮挡真实世界。
>
> PICO / Quest 上 MSAA 常开（A2C 只在 MSAA 下才有意义），所以这条在 VR 项目里很容易命中。
>
> **排查方法**：材质里搜 `bUseAlphaToCoverage`（材质编辑器 Details → *Use Alpha To Coverage*）。触发条件是 `MaterialShared.cpp:1944`：`bUseAlphaToCoverage && MD_Surface && IsMaskedBlendMode && !WritesEveryPixel()`。
>
> **解法**：MR 场景里关掉 A2C，退回普通 Masked（PS 走 `OutColor.a = 0.0`，正常遮挡）。代价是叶片边缘变硬。

#### 3.6.2 Modulate —— PS 写了 Opacity，但被双重屏蔽

PS 这里是唯一一个"写了却白写"的：

`MobileBasePassPixelShader.usf:1068-1070`
```hlsl
#elif MATERIALBLENDING_MODULATE
    half3 FoggedColor = lerp(half3(1, 1, 1), Color, VertexFog.aaa * VertexFog.aaa);
    OutColor = half4(FoggedColor, Opacity);              // ← alpha = Opacity，看起来会写
```

但 BlendState（`MobileBasePass.cpp:689`）上了**两道锁**：

```cpp
TStaticBlendState<CW_RGB, BO_Add, BF_DestColor, BF_Zero, BO_Add, BF_Zero, BF_One, ...>
//                ^^^^^^ 第一道：写掩码不含 A            ^^^^^^^^^^^^^^^^^^^^^^^^ 第二道：A' = 0×src + 1×dst
```

两道锁指向同一个结果：

```
A_dst' = A_dst        ← 不变
```

源码注释 `// Modulate with the existing scene color, preserve destination alpha.` 明确说了这是**有意为之**（保留 dst alpha 是 Modulate 的设计语义），不是疏漏。

> **后果**：Modulate 材质（贴花式压暗、色彩滤镜）不遮挡 VST。在 ClipMode 下，若它盖在空背景上（`A=1.0`），`E×(1−1) + V×1 = V` —— 压暗效果完全消失。
>
> 附带一条：Modulate 也是**唯一不乘 PreExposure** 的模式（`MobileBasePassPixelShader.usf:1089-1091` 的 `#if !MATERIALBLENDING_MODULATE`）。这只影响 RGB，与 Alpha 无关，但排查颜色不对时值得知道。

#### 3.6.3 AlphaComposite —— Alpha 正常，但 RGB 要求**你自己**预乘

这条和 Additive 恰好相反：Additive 是**引擎替你乘**，AlphaComposite 是**引擎不替你乘**。

`MobileBasePassPixelShader.usf:1059-1060`
```hlsl
#elif MATERIALBLENDING_ALPHACOMPOSITE || MATERIAL_SHADINGMODEL_SINGLELAYERWATER
    OutColor = half4(Color * VertexFog.a + VertexFog.rgb * Opacity, Opacity);
    //               ^^^^^ Color 没有乘 Opacity
```

而 BlendState 的 RGB src factor 是 `BF_One`（不是 `BF_SourceAlpha`）——源码注释写得很直白：`// New color is already pre-multiplied by alpha.`

```
C_dst' = C_src × 1 + C_dst × (1 − Opacity)
A_dst' = 0 + A_dst × (1 − Opacity)              ← Alpha 通道与 Translucent 完全一致，正常
```

> **Alpha 侧没有问题**，§3.4 表里那一行是对的，MR 合成行为等同 Translucent。
>
> **RGB 侧是个坑**：材质里必须自己把 `Emissive`/`BaseColor` 乘上 `Opacity`，否则 `C_src` 没缩放就直接加进去 → 画面过亮。这是 AlphaComposite 的常规用法要求（配合已预乘的粒子贴图），不是 bug，但和 Translucent 换用时极易忘。

#### 3.6.4 ⚠️ Thin Translucent —— 完全绕过 Blend Mode 分支，主流路径 Alpha 不变

`MSM_ThinTranslucent` 着色模型在 `SetTranslucentRenderState` 里**先于** blend mode 的 `switch` 被拦截：

`MobileBasePass.cpp:644-647`
```cpp
else if (ShadingModels.HasShadingModel(MSM_ThinTranslucent))
{
    DrawRenderState.SetBlendState(GetBlendStateForColorTransmittanceBlending(ShaderPlatform));
}
```

`GetBlendStateForColorTransmittanceBlending`（`:577-613`）有三条子路径，选哪条由 `MobileDefaultTranslucentColorTransmittanceMode`（`MobileBasePassRendering.cpp:69-80`）决定：

| 子路径 | 何时选中 | BlendState Alpha 段 | PS 输出 | `A_dst'` |
|---|---|---|---|---|
| **DUAL_SRC_BLENDING** | 平台支持双源混合（**Vulkan 移动端主流，PICO 走这条**） | `BO_Add, BF_One, BF_Source1Alpha` | `OutColor.a=0.0`<br>`OutColor1.a=1.0` | `0 + A×1 = A` ⚠️ **不变** |
| **PROGRAMMABLE_BLENDING** | Metal 移动端 / Android GLES | `BF_One, BF_Zero`（直接覆盖，混合在 shader 里做） | `OutProgrammableBlending = OutColor1 * FramebufferFetchColor0() + OutColor`<br>→ `1.0×A + 0.0` | `= A` ⚠️ **不变** |
| **SINGLE_SRC_BLENDING** | 前两者都不可用时的 fallback | `BO_Add, BF_Zero, BF_InverseSourceAlpha` | `AdjustedAlpha = saturate(1 − dot(Mul, 1/3))` | `A × (1−AdjustedAlpha)` ✅ 正常 |

（PS 三分支见 `MobileBasePassPixelShader.usf:1043-1057`、`:1124-1126`）

> **后果**：薄膜半透明（玻璃、水膜、肥皂泡这类用 Thin Translucent 做的材质）在 PICO 上**大概率走 DUAL_SRC_BLENDING，不遮挡 VST**，ClipMode 下在透视区域消失。
>
> 现象和 Additive 一模一样，但原因不同（这里是"彩色透射率被塞进第二个输出槽，alpha 通道被挪作他用"），所以排查时容易漏掉。
>
> **解法**：MR 场景里的玻璃改用普通 `Translucent` 着色模型，或按 §3.5 方案二叠 Write Alpha Only 面片。

#### 3.6.5 Single Layer Water —— "不透明"材质却不压 Alpha

水面是 Opaque 材质，但移动端强制走半透明预乘路径：

`MobileBasePass.cpp:721-731`
```cpp
if (ShadingModels.HasShadingModel(MSM_SingleLayerWater))
{
    // Single layer water is an opaque marerial rendered as translucent on Mobile.
    // We force pre-multiplied alpha to achieve water depth based transmittance.
    TStaticBlendState<CW_RGBA, BO_Add, BF_One, BF_InverseSourceAlpha, BO_Add, BF_Zero, BF_InverseSourceAlpha, ...>
}
```

PS 走 AlphaComposite 同一分支（`:1059`，条件里就写了 `|| MATERIAL_SHADINGMODEL_SINGLELAYERWATER`），且此前 Opacity 已被水深透射率改写：

`MobileBasePassPixelShader.usf:985`
```hlsl
Opacity = 1.0 - ((1.0 - Opacity) * dot(WaterLighting.WaterToSceneToLightTransmittance, float3(1.0/3.0, ...)));
```

```
A_dst' = A_dst × (1 − Opacity)
```

> **特例在于语义错位**：材质面板上写着 Opaque，直觉是"完全遮挡"，实际 Alpha 只按水深衰减。**浅水区 Opacity 小 → Alpha 残留高 → VST 从水里透出来**，深水区才正常遮挡。
>
> MR 里做水体要留意：这不是 bug，是水深透射率的正常表现，只是在 VST 合成下被放大成"能透过浅水看见真实房间"。

#### 3.6.6 编辑器移动预览：Alpha 被强制为 1.0

`MobileBasePassPixelShader.usf:1098-1105`
```hlsl
#if USE_EDITOR_COMPOSITING && (MOBILE_EMULATION)
    OutColor.a = 1.0;
    #if MATERIALBLENDING_MASKED
        OutColor.a = GetMaterialMaskInputRaw(PixelMaterialInputs);
    #endif
    clip(OutColor.a - GetMaterialOpacityMaskClipValue());
#else
```

> **PC 上的移动预览（Mobile Preview / MOBILE_EMULATION）里，Alpha 通道被整体覆写成 1.0，编辑器视口的 Alpha 完全不能作为 MR 合成的参考。** 所有 Alpha 相关验证必须上真机或用 RenderDoc 抓帧看真实的 Eye Buffer。
>
> 这条与 §6.2 "Write Alpha Only 仅移动端生效" 是两个独立的 PC/真机差异，叠加起来会让 PC 预览与设备表现相差很远。

#### 3.6.7 小结：谁会遮挡 VST

| 会压低 Alpha（遮挡 VST）✅ | 不动 Alpha（不遮挡 VST）⚠️ |
|---|---|
| Opaque / Masked（未开 A2C） | **Masked + Alpha To Coverage** |
| Translucent | **Additive** |
| Translucent + Write Alpha Only | **Modulate** |
| AlphaComposite | **Thin Translucent**（双源 / 可编程混合路径） |
| Single Layer Water（仅深水区） | |
| AlphaHoldout（反向，主动挖洞） | |

> 右列四项在 ClipMode 下于空背景区域（`A = 1.0`）一律退化为 `E×(1−1) + V×1 = V`，即**画面里只剩 VST**。若项目命中其中任意一条，`r.Mobile.PICO.BlendModeSetting 2`（AdditiveMode）是一次性的全局解法，代价是半透明物体的合成公式也跟着改变（见 §5）。

---

## 4. 问题三：`r.Mobile.PICO.BlendModeSetting` 三种模式怎么混合？

### 4.1 CVar 定义

`PICO-Unreal-Integration-SDK/UE_5.4/Plugins/PICOXR/Source/PICOXRHMD/Private/PXR_HMD.cpp:90-96`
```cpp
static TAutoConsoleVariable<int32> CVarPICOBlendModeSetting(
    TEXT("r.Mobile.PICO.BlendModeSetting"),
    1,                                    // ← 默认 1 = ClipMode
    TEXT("0: Covering Mode, VST will cover the entire screen\n")
    TEXT("1: Clip Mode,Eye Buffer and VST will clip by Alpha Before add(Default)\n")
    TEXT("2: Additive Mode, Eye Buffer will not clip by Alpha\n"),
    ECVF_Scalability | ECVF_RenderThreadSafe);
```

对应 UENUM：`PXR_HMDRuntimeSettings.h:57-62` → `CoveringMode / ClipMode / AdditiveMode`

### 4.2 提交给 Compositor 的 Blend Factor

`PICO-Unreal-Integration-SDK/UE_5.4/Plugins/PICOXR/Source/PICOXRHMD/Private/PXR_StereoLayer.cpp:819-848`
```cpp
layerProjection.header.useLayerBlend = 1;
PxrLayerBlend layerBlend = {};
switch (BlendModeType) {
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
```

其中（`PXR_Plugin_Types.h:545-553`, `1082-1087`）：
- `src` = **Eye Buffer**（本层，即你提交的 Projection Layer）
- `dst` = **VST**（Compositor 里已有的透视图像）
- `SRC_ALPHA` = **Eye Buffer 的 Alpha**，也就是全文讨论的那个 A

### 4.3 ⭐ 三种模式的最终公式

令 `E` = Eye Buffer 颜色，`A` = Eye Buffer Alpha，`V` = VST 颜色：

| 模式 | 值 | 公式 | 语义 |
|---|---|---|---|
| **CoveringMode** | 0 | `Out = E·1 + V·1 = E + V` | VST 全屏满强度铺底，虚拟内容纯叠加上去（Alpha 完全不参与裁剪）。画面偏亮、发白 |
| **ClipMode**（默认） | 1 | `Out = E·(1−A) + V·A` | 两边都按 Alpha 裁剪后相加。A=0→纯虚拟，A=1→纯 VST。权重恒为 1，最稳 |
| **AdditiveMode** | 2 | `Out = E·1 + V·A` | Eye Buffer 不被裁剪（全额输出），只裁 VST |

自检一下三个名字与公式是自洽的：
- Covering：`dstColor=ONE` → "VST will cover the entire screen" ✔
- Clip：src、dst 各乘 `(1−A)` 和 `A` → "Eye Buffer and VST will clip by Alpha before add" ✔
- Additive：`srcColor=ONE` → "Eye Buffer will not clip by Alpha" ✔

> Alpha 通道恒为 `srcAlpha=ONE, dstAlpha=ONE`（`PXR_StereoLayer.cpp:845-846`），即合成后 alpha = A + A_vst。这个值只在还要继续被上层合成时才有意义，对最终显示无影响。

---

## 5. 问题四：Opacity = 0.3 的半透明物体，具体怎么走完全程？

### 场景 A：半透明物体前面是空的（无天空盒，Alpha 仍是清屏值 1.0）

**Step 1 — 清屏**
```
C_dst = (0,0,0)      A_dst = 1.0
```

**Step 2 — 画 Opacity=0.3 的半透明物体**

RGB：`C_dst' = C_obj × 0.3 + 0 × 0.7 = 0.3·C_obj`
Alpha：`A_dst' = 0 × 0.3 + 1.0 × (1 − 0.3) = 0.7`

```
Eye Buffer:   E = 0.3·C_obj      A = 0.7
```

> ⚠️ 注意：**RT 的 Alpha 是 0.7，不是 0.3。**

**Step 3 — Compositor 与 VST 合成**

| 模式 | 计算 | 结果 | 总权重 |
|---|---|---|---|
| ClipMode (默认) | `0.3·C_obj × (1−0.7) + V × 0.7` | `0.09·C_obj + 0.7·V` | 0.79（**偏暗**） |
| AdditiveMode | `0.3·C_obj × 1 + V × 0.7` | `0.3·C_obj + 0.7·V` | **1.00 ✔ 物理正确** |
| CoveringMode | `0.3·C_obj + V` | `0.3·C_obj + 1.0·V` | 1.30（**偏亮/发白**） |

**这是本文最重要的一个发现：**

UE 的 SceneColor 在"没画东西"的地方是**黑色 (0,0,0)**，也就是说 SceneColor.rgb 事实上已经是**预乘 (premultiplied)** 形态了 —— 虚拟内容的贡献已经带好了自己的覆盖率权重。
- **AdditiveMode 才是数学上正确的合成公式**（`premultiplied_src + dst × transmittance`）。
- **ClipMode 会对半透明区域二次衰减**（乘了两次 0.3），半透明物体在 MR 里会显得比编辑器里暗。
- ClipMode 的优势是**绝对不会过曝**：全不透明处 (A=0) 精确等于 Eye Buffer，全空处 (A=1) 精确等于 VST，中间是干净的 lerp。所以 PICO 把它设为默认。

如果你的项目里**大量使用半透明**且发现"MR 下半透明物体发灰发暗"，切 `r.Mobile.PICO.BlendModeSetting 2` 是对症的解法。

### 场景 B：半透明物体前面是不透明物体（A_dst 已是 0）

```
A_dst' = 0 × (1 − 0.3) = 0
```
Alpha 保持 0 → 三种模式下 VST 权重都是 0 → 完全看不到透视。**符合预期**：不透明物体后面本来就不该透出真实世界。

### 场景 C：两层 Opacity=0.3 叠在一起（空背景）

```
A = 1.0 × 0.7 × 0.7 = 0.49
```
两层玻璃，VST 透过率 49%。连乘语义正确。

### 场景 D：Opacity = 1.0 的"半透明"材质

```
A_dst' = A_dst × (1 − 1.0) = 0
```
Alpha 归零，等价于不透明 → 完全遮挡 VST。

---

## 6. 问题五：材质里的 "Write Alpha Only" 是怎么做的？会直接覆盖 RT 的 Alpha 吗？

### **答案：会，直接覆盖，且完全不写 RGB。**

### 6.1 材质属性定义

`Source/Runtime/Engine/Classes/Materials/Material.h:621-623`
```cpp
/** Whether the transluency pass should write its alpha, and only the alpha, into the framebuffer */
UPROPERTY(EditAnywhere, Category = Translucency, AdvancedDisplay)
uint8 bWriteOnlyAlpha : 1;
```
> 位于材质面板 **Translucency → 高级 (Advanced)** 展开项里。

访问器：`Source/Runtime/Engine/Private/Materials/MaterialShared.cpp:1720`
```cpp
bool FMaterialResource::ShouldWriteOnlyAlpha() const { return Material->bWriteOnlyAlpha; }
```

### 6.2 生效位置（**仅移动端 + 仅 BLEND_Translucent**）

`Source/Runtime/Renderer/Private/MobileBasePass.cpp:652-663`
```cpp
case BLEND_Translucent:
    if (Material.ShouldWriteOnlyAlpha())
    {
        DrawRenderState.SetBlendState(TStaticBlendState<
            CW_ALPHA,                              // ← 只写 Alpha 通道
            BO_Add, BF_Zero, BF_Zero,              // RGB: 被 CW_ALPHA 屏蔽
            BO_Add, BF_One,  BF_Zero,              // Alpha: src×1 + dst×0
            ...>::GetRHI());
    }
```

全仓库只有这一处使用（`ShouldWriteOnlyAlpha` grep 结果：`MaterialShared.h:1900/2658`、`MaterialShared.cpp:1720`、`MobileBasePass.cpp:653`）。
→ **PC / 延迟渲染路径不支持，只在 Mobile Forward 的 Translucent 材质上生效。**

### 6.3 公式

```
A_dst' = A_src × BF_One + A_dst × BF_Zero
       = A_src
       = 材质的 Opacity          ← 直接覆盖，与旧值无关
RGB    = 不写（CW_ALPHA 屏蔽）
```

### 6.4 与普通 Translucent 的对比

| | 普通 Translucent | Write Alpha Only |
|---|---|---|
| Color Write Mask | `CW_RGBA` | `CW_ALPHA` |
| RGB | `C_src·A + C_dst·(1−A)` | **不写，保持原样** |
| Alpha | `A_dst × (1 − Opacity)`（乘法累积） | `= Opacity`（**直接覆盖**） |
| Opacity=1 的效果 | Alpha→0，完全遮挡 VST | Alpha→1，**纯 VST 透视窗口** |
| Opacity=0 的效果 | Alpha 不变 | Alpha→0，**强制纯虚拟** |

### 6.5 典型用法

这是做 **MR 透视窗口 / 遮罩** 的标准手段：

1. 材质：`Blend Mode = Translucent`，`Shading Model = Unlit`，勾选 **Write Alpha Only**
2. `Opacity` 接你要的 VST 权重（1 = 全透视，0 = 全虚拟，0.5 = 半混）
3. 放一个 Plane / Box 到场景里，就在那块区域"挖"出真实世界

因为它不写 RGB，所以**不会破坏已经渲染好的虚拟画面颜色**，只是改写"这块区域给 VST 多少权重"，非常干净。

**注意事项：**
- 它仍然是半透明物体：**受深度测试影响**（被不透明物体挡住就不绘制），**默认不写深度**，并且**参与半透明排序**。
- 它会被排在它后面绘制的半透明物体继续用 `× (1−Opacity)` 衰减掉。想要它"最终说了算"，需要保证它是最后画的（调 `Translucency Sort Priority`）。
- `Opacity` 是最终写入 8bit UNORM 的值，`0.3` 会量化成 `77/255 ≈ 0.302`，这点误差无关紧要。

---

## 7. ⚠️ 前置条件：`r.Mobile.PropagateAlpha` 必须打开

**这是整套机制的开关。不开，上面全部无效。**

`Source/Runtime/Renderer/Private/MobileBasePassRendering.cpp:52-57`
```cpp
static TAutoConsoleVariable<int32> CVarMobilePropagateAlpha(
    TEXT("r.Mobile.PropagateAlpha"),
    0,                                  // ← 默认关闭！
    TEXT("0: Disabled")
    TEXT("1: Propagate Full Alpha Propagate"),
    ECVF_ReadOnly | ECVF_RenderThreadSafe);   // ReadOnly = 只能在 ini 里设，需重编 shader
```

### 关闭时会发生什么

1. **SceneColor 根本没有 Alpha 通道**
   `SceneTexturesConfig.cpp:63`
   ```cpp
   DefaultColorFormat = bRequiresAlphaChannel ? PF_FloatRGBA : PF_FloatR11G11B10;
   ```
   `PF_FloatR11G11B10` 是 32bit 无 Alpha 格式，前面所有 Alpha 混合都被丢弃。

2. **Tonemapper 不透传 Alpha，输出全 0**
   `Shaders/Private/PostProcessTonemap.usf:454, 524-526`
   ```hlsl
   float4 OutColor = 0;                    // 初值 0
   ...
   #if POST_PROCESS_ALPHA == 2 || ...
       OutColor.a = SceneColor.a;          // 只有开了才透传
   #endif
   ```
   `POST_PROCESS_ALPHA` 由 `r.Mobile.PropagateAlpha` 决定（`Source/Runtime/RenderCore/Private/Shader.cpp:1905-1910`，`>0` 时置为 2）。

3. **Eye Buffer Alpha 全 0** → ClipMode 下 `Out = E·(1−0) + V·0 = E` → **完全看不到 VST**，退化成纯 VR。

### 正确配置

项目 `Config/DefaultEngine.ini`：
```ini
[/Script/Engine.RendererSettings]
r.Mobile.PropagateAlpha=1
```
（`ECVF_ReadOnly` → 控制台改无效，必须改 ini 并重新编译 shader）

读取入口：`Source/Runtime/Core/Private/GenericPlatform/GenericPlatformMisc.cpp:2039-2047`

---

## 8. 常见坑清单

| 现象 | 原因 | 解法 |
|---|---|---|
| **完全看不到透视，纯 VR** | `r.Mobile.PropagateAlpha=0` → Eye Buffer Alpha 全 0 | ini 里设为 1，重编 shader |
| **完全看不到虚拟内容，纯 VST** | Eye Buffer Alpha 全 1；常见于开了 `r.Mobile.TonemapSubpass=1` | 关掉。该路径的 `MobileCustomResolve_MainPS` 硬编码 `OutColor.a = 1.0`（`PostProcessTonemap.usf:709-710`），会彻底破坏 Alpha |
| **整个视野都是虚拟内容，天上地下都不透视** | 场景里有**不透明的天空盒 / SkySphere / SkyAtmosphere** → 全屏 Alpha 被写成 0 | MR 场景删掉天空盒，让背景保持清屏状态 (Alpha=1, RGB=0) |
| **Additive 特效（火焰/光晕/法术）在透视区域完全消失** | Additive 的 PS 输出 `a=0.0f`，不改 Alpha；空背景处 A=1，ClipMode 下 `E×(1−1)=0` 把特效抹掉（见 §3.5） | 切 `r.Mobile.PICO.BlendModeSetting 2`；或在特效区叠 Write Alpha Only 面片压低 Alpha |
| **树叶 / 草 / 铁丝网整片不遮挡真实世界** | 材质勾了 **Use Alpha To Coverage**，Opaque 路径的 BlendState 用 `CW_RGB` 屏蔽 Alpha 写入（`MobileBasePass.cpp:565`，见 §3.6.1） | MR 场景关掉 Use Alpha To Coverage，退回普通 Masked（PS 走 `a=0.0`）。代价是边缘变硬 |
| **玻璃 / 水膜（Thin Translucent）在透视区域消失** | 双源混合与可编程混合路径下 `A' = A`，Alpha 通道被彩色透射率挪用（见 §3.6.4） | 改用普通 Translucent 着色模型；或叠 Write Alpha Only 面片 |
| **Modulate 压暗 / 滤镜在透视区域无效** | `CW_RGB` + `BF_Zero,BF_One` 双重屏蔽，Alpha 不变（源码注释明示为设计语义，见 §3.6.2） | 改用 Translucent 实现压暗；或切 AdditiveMode |
| **AlphaComposite 材质过亮** | 引擎不替你预乘：RGB src factor 是 `BF_One`，PS 里 `Color` 未乘 Opacity（见 §3.6.3） | 材质中手动 `Emissive × Opacity`，或改用 Translucent |
| **浅水区能看见真实房间** | Single Layer Water 按水深透射率写 Alpha，`A' = A×(1−Opacity)`，浅水 Opacity 小（见 §3.6.5） | 属正常行为；需要遮挡就抬高浅水区的 Opacity |
| **半透明物体在 MR 里发暗/发灰** | ClipMode 对半透明区域二次衰减（见 §5 场景 A） | 改用 `r.Mobile.PICO.BlendModeSetting 2` (AdditiveMode) |
| **画面整体发白、对比度低** | 用了 CoveringMode，VST 满强度铺底 | 改回 1 或 2 |
| **半透明区域透视比预期强** | Alpha 是**连乘**的：`A = ∏(1−Opacity_i)`，多层叠加衰减很快 | 减少叠加层数，或用 Write Alpha Only 在最后强制覆盖成想要的值 |
| **UMG / 3D Widget 挡住透视失效** | Widget 材质多为 Translucent，Alpha 只被乘 `(1−Opacity)` | 需要完全不透视的 UI 用 Opaque，或用 Write Alpha Only 设 0 |
| **PC 预览与真机不一致** | 两个独立原因：① `bWriteOnlyAlpha` 只在 Mobile Forward 生效；② Mobile Preview 下 PS 强制 `OutColor.a = 1.0`（`MobileBasePassPixelShader.usf:1098-1100`，见 §3.6.6） | Alpha 相关验证一律上真机或 RenderDoc 抓 Eye Buffer，不看编辑器视口 |

---

## 9. 一页速查

```
════════════════ Alpha 语义 ════════════════
  Eye Buffer 的 A  =  VST 的透过率（不是不透明度！）
  A = 1 → 纯 VST        A = 0 → 纯虚拟

════════════════ 默认值 ════════════════
  SceneColor 清屏 Alpha = 1.0   (FClearValueBinding::Black = 0,0,0,1)

════════════════ 各阶段公式 ════════════════
  ✅ 会遮挡 VST（Alpha 被压低）
  Opaque / Masked          A' = 0
  Translucent              A' = A × (1 − Opacity)
  Translucent+WriteAlphaOnly  A' = Opacity          ← 直接覆盖
  AlphaComposite           A' = A × (1 − Opacity)   ← RGB 需自行预乘
  SingleLayerWater         A' = A × (1 − Opacity)   ← 浅水区几乎不遮挡
  AlphaHoldout             A' = Opacity + A×(1−Opacity)  ← 反向，主动挖洞

  ⚠️ 不遮挡 VST（Alpha 原样不动，ClipMode 下在空背景处被抹掉）
  Additive                 A' = A   PS 输出 a=0.0f
  Modulate                 A' = A   CW_RGB + BF_Zero,BF_One 双重屏蔽
  Masked + AlphaToCoverage A' = A   CW_RGB 挡住写入（A2C 只用于生成 coverage）
  ThinTranslucent          A' = A   双源/可编程混合路径；SINGLE_SRC 路径才正常

════════════════ PICO Compositor (src=Eye, dst=VST) ════════════════
  0 CoveringMode   Out = E + V
  1 ClipMode ★默认 Out = E×(1−A) + V×A
  2 AdditiveMode   Out = E + V×A       ← 对预乘型 SceneColor 数学最准

════════════════ 算例：Opacity=0.3，背景为空 ════════════════
  Eye Buffer:  E = 0.3·C_obj,  A = 1.0 × 0.7 = 0.7
  ClipMode:    Out = 0.09·C_obj + 0.70·V     (总权重 0.79，偏暗)
  Additive:    Out = 0.30·C_obj + 0.70·V     (总权重 1.00 ✔)
  Covering:    Out = 0.30·C_obj + 1.00·V     (总权重 1.30，偏亮)

════════════════ 必开开关 ════════════════
  [/Script/Engine.RendererSettings]
  r.Mobile.PropagateAlpha=1        # ECVF_ReadOnly，必须写 ini
```

---

## 10. 源码索引

### UE 引擎

| 内容 | 路径 | 行 |
|---|---|---|
| `FClearValueBinding::Black = (0,0,0,1)` | `Source/Runtime/RHI/Private/RHI.cpp` | 105 |
| SceneColor ClearValue | `Source/Runtime/Engine/Private/SceneTexturesConfig.cpp` | 268 |
| SceneColor 格式（有无 Alpha） | `Source/Runtime/Engine/Private/SceneTexturesConfig.cpp` | 53-90 |
| `bRequiresAlphaChannel` 判定 | `Source/Runtime/Renderer/Private/SceneTextures.cpp` | 400 |
| `IsMobilePropagateAlphaEnabled` | `Source/Runtime/Engine/Private/SceneUtils.cpp` | 47-50 |
| `r.Mobile.PropagateAlpha` CVar | `Source/Runtime/Renderer/Private/MobileBasePassRendering.cpp` | 52-57 |
| ini 读取 | `Source/Runtime/Core/Private/GenericPlatform/GenericPlatformMisc.cpp` | 2039-2047 |
| `POST_PROCESS_ALPHA` 宏生成 | `Source/Runtime/RenderCore/Private/Shader.cpp` | 1903-1925 |
| Base Pass RT 绑定 / EClear | `Source/Runtime/Renderer/Private/MobileShadingRenderer.cpp` | 1464-1500 |
| `r.Mobile.TonemapSubpass` | `Source/Runtime/Renderer/Private/MobileShadingRenderer.cpp` | 102-113 |
| Opaque Render State | `Source/Runtime/Renderer/Private/MobileBasePass.cpp` | 531-575 |
| **Alpha To Coverage Blend State（`CW_RGB`，Alpha 不写）** | `Source/Runtime/Renderer/Private/MobileBasePass.cpp` | **562-573** |
| `IsUsingAlphaToCoverage()` 触发条件 | `Source/Runtime/Engine/Private/Materials/MaterialShared.cpp` | 1942-1945 |
| `MATERIAL_USE_ALPHA_TO_COVERAGE` 宏生成 | `Source/Runtime/Engine/Private/Materials/MaterialShared.cpp` | 2734 |
| **Translucent Render State（全部 Blend Mode）** | `Source/Runtime/Renderer/Private/MobileBasePass.cpp` | **615-744** |
| Write Alpha Only Blend State | `Source/Runtime/Renderer/Private/MobileBasePass.cpp` | 653-663 |
| Thin Translucent Blend State（三条子路径） | `Source/Runtime/Renderer/Private/MobileBasePass.cpp` | 577-613, 644-647 |
| Thin Translucent 路径选择 | `Source/Runtime/Renderer/Private/MobileBasePassRendering.cpp` | 69-110 |
| `MOBILE_TRANSLUCENT_COLOR_TRANSMITTANCE_*` 宏生成 | `Source/Runtime/Renderer/Private/MobileBasePassRendering.h` | 441-448 |
| Single Layer Water Blend State | `Source/Runtime/Renderer/Private/MobileBasePass.cpp` | 721-731 |
| `bWriteOnlyAlpha` 材质属性 | `Source/Runtime/Engine/Classes/Materials/Material.h` | 621-623 |
| `ShouldWriteOnlyAlpha()` | `Source/Runtime/Engine/Private/Materials/MaterialShared.cpp` | 1720 |
| Base Pass PS 输出 Alpha | `Shaders/Private/MobileBasePassPixelShader.usf` | 1043-1082 |
| A2C 量化 Mask 写进 `OutColor.a` | `Shaders/Private/MobileBasePassPixelShader.usf` | 1074-1081 |
| Thin Translucent 双源 / 可编程混合 PS | `Shaders/Private/MobileBasePassPixelShader.usf` | 1043-1057, 1124-1126 |
| Single Layer Water 的 Opacity 改写 | `Shaders/Private/MobileBasePassPixelShader.usf` | 985 |
| Modulate 不乘 PreExposure | `Shaders/Private/MobileBasePassPixelShader.usf` | 1089-1091 |
| 编辑器移动预览强制 `a=1.0` | `Shaders/Private/MobileBasePassPixelShader.usf` | 1098-1105 |
| Tonemapper Alpha 透传 | `Shaders/Private/PostProcessTonemap.usf` | 454, 524-526 |
| `MobileCustomResolve` 强制 a=1.0 | `Shaders/Private/PostProcessTonemap.usf` | 709-710 |
| `ShouldWriteAlphaChannel` | `Source/Runtime/Renderer/Private/PostProcess/PostProcessTonemap.cpp` | 550-557 |

### PICO SDK（`Engine/PICO-Unreal-Integration-SDK/UE_5.4/Plugins/PICOXR/`）

| 内容 | 路径 | 行 |
|---|---|---|
| `r.Mobile.PICO.BlendModeSetting` CVar | `Source/PICOXRHMD/Private/PXR_HMD.cpp` | 90-96 |
| `EPICOXRBlendModeType` 枚举 | `Source/PICOXRHMD/Public/PXR_HMDRuntimeSettings.h` | 56-62 |
| **Compositor Blend Factor 设置** | `Source/PICOXRHMD/Private/PXR_StereoLayer.cpp` | **819-848** |
| Eye Buffer swapchain 格式 (`R8G8B8A8`) | `Source/PICOXRHMD/Private/PXR_StereoLayer.cpp` | 453, 656 |
| `PxrBlendFactor` 枚举 | `Source/ThirdParty/PXRPlugin/PXRPlugin/Include/PXR_Plugin_Types.h` | 545-553 |
| `PxrLayerBlend` 结构体 | `Source/ThirdParty/PXRPlugin/PXRPlugin/Include/PXR_Plugin_Types.h` | 1082-1087 |
| `PxrLayerHeader2.useLayerBlend` | `Source/ThirdParty/PXRPlugin/PXRPlugin/Include/PXR_Plugin_Types.h` | 1096-1111 |
| swapchain 创建 | `Source/PICOXRHMD/Private/PXR_HMDRenderBridge.cpp` | 61-77 |
