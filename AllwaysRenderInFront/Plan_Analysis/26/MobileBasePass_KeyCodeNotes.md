# MobileBasePass.cpp 关键代码解读

本文档针对 `Engine/Source/Runtime/Renderer/Private/MobileBasePass.cpp` 中两处关键代码进行梳理，并说明其与 `Engine/Shaders/Private/MobileBasePassPixelShader.usf` 的对应关系。

---

## 1. `MobileBasePass.cpp:207` 之前的代码都在做什么

### 位置与函数入口

`MobileBasePass.cpp:207` 的入口函数是：

```cpp
bool MobileBasePass::GetShaders(
    ELightMapPolicyType LightMapPolicyType,
    EMobileLocalLightSetting LocalLightSetting,
    const FMaterial& MaterialResource,
    const FVertexFactoryType* VertexFactoryType,
    bool bEnableSkyLight,
    TShaderRef<TMobileBasePassVSPolicyParamType<FUniformLightMapPolicy>>& VertexShader,
    TShaderRef<TMobileBasePassPSPolicyParamType<FUniformLightMapPolicy>>& PixelShader)
```

在调用 `GetShaders` 之前，所做的事情可以概括为一句话：

> **根据当前渲染上下文（材质属性、场景光照配置、CVar 等）筛选出“这一帧里这条网格需要用哪一个 `TMobileBasePassPS` 模板特化”，并把它注册到 ShaderMap 中供后续取用。**

整个分类（特化 / permutation）分三层，每一层都最终体现在 `MobileBasePassPixelShader.usf` 中的 `define` 宏上：

### 三层分类链路

| 层级 | 函数 / 模板参数 | 影响的 `.usf` 宏 |
|---|---|---|
| 第 1 层：LightMapPolicy（光照策略） | `GetMobileBasePassShaders<LMP_*>` 行 169-204；共 10 种 `LMP_*` 特化 | `LQ_TEXTURE_LIGHTMAP`、`CACHED_POINT_INDIRECT_LIGHTING`、`MOBILE_USE_GBUBUFFER`（搭配 `MOBILE_DEFERRED_SHADING`）、`HAS_SCENE_DEPTH_AUX_OUTPUT` 等 |
| 第 2 层：HDR / SkyLight 维度 | `GetUniformMobileBasePassShaders` 行 125-165；调用 `AddMobileBasePassPixelShaderTypes`（行 98-122） | `ENABLE_SKY_LIGHT`、`HDR_LINEAR_64` vs `LDR_GAMMA_32`（影响输出 gamma 处理） |
| 第 3 层：Thin Translucent Fallback 模式 | `switch (ThinTranslucentFallback)` 行 148-155；3 种 `EMobileTranslucentColorTransmittanceMode` | `MOBILE_TRANSLUCENT_COLOR_TRANSMITTANCE_DEFAULT` / `_DUAL_SRC_BLENDING` / `_PROGRAMMABLE_BLENDING` |

### 关键模板类 `TMobileBasePassPS`（`MobileBasePassRendering.h:360`）

```cpp
template<typename LightMapPolicyType,
         EMobileSceneTextureSamplingMode SceneTextureSamplingMode,
         bool bEnableSkyLight,
         EMobileLocalLightSetting LocalLightSetting,
         EMobileTranslucentColorTransmittanceMode ThinTranslucencyFallback>
class TMobileBasePassPS : public TMobileBasePassPSBaseType<LightMapPolicyType>
```

`AddMobileBasePassPixelShaderTypes`（行 98-122）正是把这一堆模板实参固化下来：

```cpp
ShaderTypes.AddShaderType<TMobileBasePassPS<
    TUniformLightMapPolicy<Policy>,
    HDR_LINEAR_64,           // SceneTextureSamplingMode
    true,                    // bEnableSkyLight
    LocalLightSetting,       // LOCAL_LIGHTS_DISABLED/ENABLED/BUFFER
    ThinTranslucencyFallback // DEFAULT/SINGLE_SRC_BLENDING/...
>>();
```

这些模板参数最终作为 `.usf` 中预处理宏的“开关”影响编译：

- `bEnableSkyLight` → `ENABLE_SKY_LIGHT`（控制 `GetSkyLighting()` 体是否被编译，`MobileBasePassPixelShader.usf:198`）
- `LocalLightSetting` → `MERGED_LOCAL_LIGHTS_MOBILE` 取值（0/1/2，分别对应 disabled / buffer / culled，参见 `.usf:872, 896, 937`）
- `ThinTranslucencyFallback` → `MOBILE_TRANSLUCENT_COLOR_TRANSMITTANCE_*`（控制 `OutColor/OutColor1/OutProgrammableBlending` 的输出分支，`MobileBasePassPixelShader.usf:299-319`）
- `Policy` → `LMP_*` → `LQ_TEXTURE_LIGHTMAP` / `CACHED_POINT_INDIRECT_LIGHTING` / `MOBILE_USE_GBUBUFFER` 等
- `bIsMobileHDR`（`HDR_LINEAR_64` vs `LDR_GAMMA_32`）→ `OUTPUT_GAMMA_SPACE`、是否做 sRGB 转换

### 流程图

```
TryAddMeshBatch (行 851)
  └─ SelectMeshLightmapPolicy            ← 决定 LMP_*
  └─ Process → AddMeshBatch
       └─ GetUniformMobileBasePassShaders<Policy, LocalLightSetting>
            ├─ AddShaderType<TMobileBasePassVS<...>>     // VS
            └─ AddMobileBasePassPixelShaderTypes<Policy, LocalLightSetting, ThinTranslucencyFallback>
                 └─ AddShaderType<TMobileBasePassPS<Policy, HDR/LDR, bEnableSkyLight, LocalLightSetting, ThinTranslucencyFallback>>
                      └─ Material.TryGetShaders(...)     // 真正从 ShaderMap 取
```

> **结论**：第 207 行 `MobileBasePass::GetShaders` 之前的代码，本质就是在 C++ 模板层“枚举出要使用的 `TMobileBasePassPS` 特化”，这些特化在编译 `.usf` 时会以宏的形式控制分支代码的取舍，最终体现在 `MobileBasePassPixelShader.usf` 中若干 `#if MATERIALBLENDING_* / #if ENABLE_SKY_LIGHT / #if MERGED_LOCAL_LIGHTS_MOBILE == n` 之类的开关上。

---

## 2. `MobileBasePass.cpp:1154` 的 `SetBlendState(CW_RGBA)` 改成 `CW_NONE` 的链路

### 原始代码（行 1151-1162）

```cpp
FMeshPassProcessor* CreateMobileBasePassProcessor(ERHIFeatureLevel::Type FeatureLevel, const FScene* Scene, const FScene* InViewIfDynamicMeshCommand, FMeshPassDrawListContext* InDrawListContext)
{
    FMeshPassProcessorRenderState PassDrawRenderState;
    PassDrawRenderState.SetBlendState(TStaticBlendStateWriteMask<CW_RGBA>::GetRHI());   // ← 行 1154
    const FExclusiveDepthStencil::Type DefaultBasePassDepthStencilAccess =
        FScene::GetDefaultBasePassDepthStencilAccess(FeatureLevel);
    PassDrawRenderState.SetDepthStencilAccess(DefaultBasePassDepthStencilAccess);
    PassDrawRenderState.SetDepthStencilState(TStaticDepthStencilState<true, CF_DepthNearOrEqual>::GetRHI());
    ...
    return new FMobileBasePassMeshProcessor(EMeshPass::BasePass, Scene, InViewIfDynamicMeshCommand, PassDrawRenderState, InDrawListContext, Flags);
}
```

### 回答：可以改成 `CW_NONE`

把：

```cpp
PassDrawRenderState.SetBlendState(TStaticBlendStateWriteMask<CW_RGBA>::GetRHI());
```

改成：

```cpp
PassDrawRenderState.SetBlendState(TStaticBlendStateWriteMask<CW_NONE>::GetRHI());
```

**确实会让所有 8 个 RenderTarget 的颜色写入被关闭**（`CW_NONE = 0`，见 `Engine/Source/Runtime/RHI/Public/RHIDefinitions.h:278`），即 PSO 在写入颜色附件时不产生任何效果——但 **深度写入仍可正常工作**（深度状态由 `SetDepthStencilState` 单独控制，与混合状态无关）。

### 经过的完整处理链路

#### ① `TStaticBlendStateWriteMask<CW_NONE>` 的构造（`RHIStaticStates.h:382`）

```cpp
template<EColorWriteMask RT0ColorWriteMask = CW_RGBA, ...>
class TStaticBlendStateWriteMask : public TStaticBlendState<
    RT0ColorWriteMask, BO_Add, BF_One, BF_Zero, BO_Add, BF_One, BF_Zero,    // RT0
    RT1ColorWriteMask, BO_Add, BF_One, BF_Zero, BO_Add, BF_One, BF_Zero,    // RT1
    ...
    RT7ColorWriteMask, BO_Add, BF_One, BF_Zero, BO_Add, BF_One, BF_Zero>    // RT7
```

- 模板参数 `RT0..RT7` 都设为 `CW_NONE`
- 混合方程保持 `Src*1 + Dst*0`，即理论上“覆盖”RT——但因 `CW_NONE` 屏蔽所有写入，结果等价于“无操作”。
- 通过 `TStaticStateRHI` 单例缓存（首次调用时 `RHICreateBlendState(...)` 创建，之后 `GetRHI()` 直接返回缓存指针）。

#### ② `FMeshPassProcessorRenderState::SetBlendState(...)` 

把 `FRHIBlendState*` 保存到 `BlendState` 成员变量。

#### ③ 传入 `FMobileBasePassMeshProcessor` 构造函数（行 1162）

```cpp
return new FMobileBasePassMeshProcessor(EMeshPass::BasePass, Scene, InViewIfDynamicMeshCommand,
    PassDrawRenderState, InDrawListContext, Flags);   // 拷贝到成员 PassDrawRenderState
```

#### ④ 在 `Process(...)`（行 886+）构建 `FMeshDrawCommand` 时

```cpp
GraphicsPSOInit.BlendState = DrawRenderState.GetBlendState();   // 取到上面那个 CW_NONE 的 RHI 指针
```

并连同 `DepthStencilState / RasterizerState / RenderTargets / VertexDeclaration` 一起喂给 `PSOPrecacheParams/PSOCollector`。

#### ⑤ 最终：构建 PSO（Pipeline State Object）提交到 RHI 线程

驱动在真正绘制网格时，把这个 PSO 绑到命令缓冲区——GPU 端对所有 SV_Target 输出按 `CW_NONE` 处理，结果**不会写入任何颜色 RT**。

### 对应到 `MobileBasePassPixelShader.usf` 的什么？

> **重要：混合状态（BlendState / ColorWriteMask）是 GPU 管线状态（PSO）层面的设置，不会变成 `.usf` 中的 `#define` 宏；`MobileBasePassPixelShader.usf` 里没有任何一行会被这次改动直接影响。**

但是，shader 中所有输出到颜色附件的语句，因为 PSO 写屏蔽等于关闭，会**等于空操作**。具体来说：

| `.usf` 行 | 输出声明 | `CW_NONE` 后是否到达帧缓冲 |
|---|---|---|
| 298 | `out HALF4_TYPE OutProxy : SV_Target0`（GLES FBF 延迟路径） | 否 |
| 300 | `out HALF4_TYPE OutProgrammableBlending : SV_Target0`（可编程混合路径） | 否 |
| 302 | `out HALF4_TYPE OutColor : SV_Target0`（常规 forward 路径） | 否 |
| 312-313 | `OutColor / OutColor1 DUAL_SOURCE_BLENDING_SLOT(0/1)`（Dual-Src 路径） | 否 |
| 315 | `out HALF4_TYPE OutProgrammableBlending` | 否 |
| 317 | `out HALF4_TYPE OutColor`（普通 forward） | 否 |
| 304-308 | `OutGBufferA/B/C/D : SV_Target1..4`（Mobile GBuffer 路径） | 否 |
| 321 | `out float OutSceneDepthAux : SV_TargetDepthAux`（深度辅助） | 否 |
| 324 | `out float OutDepth : SV_Depth`（像素深度偏移） | **是**（由 `DepthStencilState` 独立控制） |

也就是说：

- `OutColor.rgb = Color * VertexFog.a + VertexFog.rgb;`（`MobileBasePassPixelShader.usf:1069`）等赋值**仍然会被执行**（shader 不知道 PSO 写屏蔽）。
- 只是从 GPU 视角看，所有 `SV_Target0..N` 都不会真正落到帧缓冲。

### 注意 / 副作用

1. **GBuffer 路径会被破坏**：如果走 `MOBILE_USE_GBUBUFFER`（mobile 延迟着色），所有 `OutGBufferA/B/C/D` 都不会写入，后续 pass（如光照合成）会读到未初始化数据，画面将严重出错。
2. **深度仍然有效**：因为深度状态独立设置（`SetDepthStencilState<true, CF_DepthNearOrEqual>`），所以 Z 写入/Z 测试照常工作——这正是“只写深度不写颜色”这种做法的常规用途（例如 occlusion pass、depth prepass）。
3. **`SetTranslucentRenderState`（行 615）内部又重新调用了 `SetBlendState`**，因此 **半透明（Translucent）走的是另一套 blend state，不受 1154 行影响**。
4. `SetOpaqueRenderState`（行 531）只在“Masked + AlphaToCoverage”分支再覆盖一次 blend state，对绝大多数不透明材质基本不会覆盖 1154 行设的默认状态。
5. 同样的 `TStaticBlendStateWriteMask<CW_RGBA>::GetRHI()` 也出现在 `MobileBasePass.cpp:1170`（`CreateMobileBasePassCSMProcessor`），如果需要 CSM pass 也不写颜色，需要一起改。
6. 真正“彻底只写深度不写颜色”的标准用法，可参考 `MobileDeferredShadingPass.cpp:674` / `SceneOcclusion.cpp:1238` 使用的 `TStaticBlendStateWriteMask<CW_NONE, CW_NONE, ..., CW_NONE>`，用法与本次改动语义相同。

---

## 3. 启用 `CW_NONE` 后 `MobileBasePassPixelShader.usf` 为什么不禁用相关颜色计算？

**结论：不会禁用。这是 UE 渲染管线有意为之的设计，核心原因可以归结为三点。**

### 3.1 Shader 编译时根本不知道 PSO 的 blend state

Pixel Shader 的编译产物（DXBC / SPIR-V / MetalLib）和 PSO 的 BlendState 是两条**独立的运行时状态**：

- Shader 在材质 ShaderMap 编译阶段就生成了（`AddShaderType<TMobileBasePassPS<...>>()`），那时候还**没有任何 DrawRenderState 可言**
- PSO 在 `CollectPSOInitializers` 时用 `GraphicsPSOInit.BlendState = DrawRenderState.GetBlendState()` 注入
- 同一份 shader 二进制可以**搭配不同的 BlendState** 使用（例如半透明/不透明同 shader 不同 blend）

HLSL / Shader Model 没有"基于 BlendState 写屏蔽宏化"的机制——UE 也不可能为 `CW_NONE` 单独编译一份 shader permutation，那样会产生 shader 数量爆炸（每种 MRT 写屏蔽组合 × 每种 shader 特化 ≈ N 倍量级）。

> 类似的"`#define` 由运行时状态决定"在 UE 里只发生在 per-material / per-platform / per-feature 的编译期维度，比如 `OUTPUT_PIXEL_DEPTH_OFFSET`、`MOBILE_USE_GBUBUFFER`、`MERGED_LOCAL_LIGHTS_MOBILE`，这些都是 shader 编译时就能决定的。

### 3.2 Pixel Shader 即便 "颜色不写" 也**不能**整体跳过

`MobileBasePassPixelShader.usf` 里除了 `OutColor / OutGBufferA-D`，还有这些**与 blend state 无关、仍然需要执行的输出 / 副作用**：

| 行 | 输出 / 副作用 | 是否仍需执行 |
|---|---|---|
| 324 | `out float OutDepth : SV_Depth`（像素深度偏移） | **必须**（深度由独立的 `DepthStencilState` 控制） |
| 321 | `out float OutSceneDepthAux : SV_TargetDepthAux` | **会受 CW_NONE 影响**（同属颜色附件路径） |
| 1083 | `OutSceneDepthAux = SvPosition.z;` | 仍执行 |
| 1109-1116 | `FinalizeVirtualTextureFeedback(...)` → 写 UAV | **必须**（UAV 不受 BlendState 写屏蔽控制） |
| 380 | `GetMaterialCoverageAndClipping(MaterialParameters, PixelMaterialInputs);` | **必须**（clip 决定哪些像素存活，深度测试依赖此） |
| 504-517 | `SetGBufferForShadingModel(...)` | 仍执行（产生 GBuffer 中间值） |
| 854 | `AccumulateDirectionalLighting(...)` | 仍执行 |

也就是说：**材质覆盖率裁剪、深度偏移、VT feedback、深度输出**，都是与颜色写屏蔽正交的"必须工作"，shader 不能整体跳过。

特别是在 TBDR（Mobile 普遍使用）架构下：

```
Pixel Shader 执行（写入 tile memory）  →  Tile Resolve 时 BlendStage 应用 CW_NONE
```

shader 必须跑完才能算深度、才能决定像素是否存活（early-Z 也只能让一些像素提前淘汰，远不是全部）。

### 3.3 设计哲学：UE 把"算什么"和"怎么写"严格解耦

UE 渲染管线的层次：

```
Material Shader (.usf)  ── 决定"算什么"
       │
       ▼
PSO (BlendState + DepthStencilState + RasterState + RT formats)  ── 决定"怎么写到 RT"
       │
       ▼
MeshDrawCommand / RHI Draw  ── 把上面两者喂给 GPU
```

- 想"不写颜色" → **改 PSO 的 ColorWriteMask**（本次的做法）
- 想"不计算颜色" → **改 Shader 的宏 / 分支**（如 `MOBILE_USE_GBUBUFFER` 切换到 GBuffer 输出路径）
- 想"完全跳过这个 mesh" → **Cull / `ShouldDraw` 返回 false**

这三件事的**责任分得很清**，UE 故意不在 shader 里读 BlendState 写屏蔽来决定分支——那会破坏分层，并且让 shader 与某个具体 PSO 绑定。

### 3.4 实际后果

启用 `CW_NONE` 后：

- **GPU 仍会执行整个像素着色器**（无法跳过）
- 颜色输出的计算（BaseColor、GBuffer、Lighting、PreExposure 等）**都白算了**——只是 tile resolve 时不写出去
- 但深度、clipping、VT feedback 等**仍然有效**

所以 `CW_NONE` 真正适合的场景只有那些**确实只想要深度、不要颜色**的 pass（depth prepass、occlusion pass、shadow depth）。如果只是想"省一点 BW"而颜色输出还有用，就不要这么做。

> 工程上想真正"省 shader 工作"，应该走 shader 端的分支（例如用 `MATERIALBLENDING_*` 切换、关闭 `MOBILE_USE_GBUBUFFER`、关闭 `HAS_SCENE_DEPTH_AUX_OUTPUT` 等），而不是 PSO 端的写屏蔽。

---

## 4. 透明 Pass 的 `SetDepthStencilState<false, ...>` 是否影响 `MobileBasePassPixelShader.usf`？

> **用户提问**：`Engine/Source/Runtime/Renderer/Private/MobileBasePass.cpp:1187` 中，
> ```cpp
> PassDrawRenderState.SetDepthStencilState(TStaticDepthStencilState<false, CF_DepthNearOrEqual>::GetRHI());
> PassDrawRenderState.SetDepthStencilAccess(FExclusiveDepthStencil::DepthRead_StencilRead);
> ```
> 对于透明物体来说，这里设置了不写入深度，或者说设置了写入深度，对 `MobileBasePassPixelShader.usf` 有影响吗？

### 4.1 先把两个模板参数讲清楚

```cpp
TStaticDepthStencilState<false, CF_DepthNearOrEqual>
//                          ↑                  ↑
//                  bEnableDepthWrite    DepthTestFunc
```

- 第一个参数 `false` → **DepthWrite = false**（输出合并阶段不写深度）
- 第二个参数 `CF_DepthNearOrEqual` → **深度测试函数** 仍为 "Less or Equal"（深度测试**仍然开启**，只是不写入）

第 1188 行的 `SetDepthStencilAccess(FExclusiveDepthStencil::DepthRead_StencilRead)` 则是给**渲染器资源调度器**的提示，告诉它："这个 pass 读深度 + 读模板，但不写"。这与 shader / PSO 的实际行为无关，只影响 UE 的 render graph 排程（决定后续 pass 能否与它并发 / 重叠）。

### 4.2 直接回答：`.usf` 中**没有任何一行**因为 DepthWrite=false 而变化

和 `CW_NONE` 同样的道理——`DepthStencilState` 是 PSO 的状态，shader 二进制里没有"PSO 是否写深度"这个概念可读。`MobileBasePassPixelShader.usf` 中：

- 没有 `#define MOBILE_TRANSLUCENT_DEPTH_WRITE_DISABLED` 之类的宏
- 没有 `#if !DEPTH_WRITE` 之类的分支
- `MATERIALBLENDING_TRANSLUCENT` 这个宏只控制 **输出颜色怎么算**（行 1061-1067 走 alpha blend 那条），与深度写无关

`OUTPUT_PIXEL_DEPTH_OFFSET`（行 323-324）控制的是 **shader 是否声明 `out float OutDepth : SV_Depth`**，这是**材质层面的特性**（像素深度偏移，常见于贴花、避免 z-fighting），与"DepthWrite=false"是两件不同的事：
- `OUTPUT_PIXEL_DEPTH_OFFSET` = 0 → shader 没有 SV_Depth 输出声明
- `OUTPUT_PIXEL_DEPTH_OFFSET` = 1 → shader 写了 SV_Depth，但若 PSO DepthWrite=false，GPU 在 OM 阶段会丢弃这次写入

### 4.3 间接影响：哪些 `.usf` 内的行为**事实上**会被这个 PSO 改变

虽然 shader 代码不变，但 PSO 状态会影响 GPU **怎么执行** 这份 shader，具体到 `MobileBasePassPixelShader.usf`：

#### ① `PIXELSHADER_EARLYDEPTHSTENCIL` 宏（行 186-190）—— **不会**被 PSO 影响

```hlsl
#define USES_PIXEL_DISCARD ((MATERIALBLENDING_MASKED || USE_DITHERED_LOD_TRANSITION) && !EARLY_Z_PASS_ONLY_MATERIAL_MASKING)
#if (MATERIAL_VIRTUALTEXTURE_FEEDBACK || LIGHTMAP_VT_ENABLED) && !(OUTPUT_PIXEL_DEPTH_OFFSET || USES_PIXEL_DISCARD)
    #define PIXELSHADER_EARLYDEPTHSTENCIL EARLYDEPTHSTENCIL
#else
    #define PIXELSHADER_EARLYDEPTHSTENCIL
#endif
```

这个宏的判定条件**完全来自材质/permutation 维度**（VT、深度偏移、Masked 裁剪），**不读 PSO 状态**。所以对 `MobileBasePass.cpp:1187` 创建的 TranslucencyStandard pass：
- 透明材质 → `MATERIALBLENDING_TRANSLUCENT` 而非 `MASKED` → `USES_PIXEL_DISCARD = false`
- `OUTPUT_PIXEL_DEPTH_OFFSET` 默认关闭
- 若开了 VT feedback，则 shader 头部插入 `EARLYDEPTHSTENCIL`，强制走 early-Z；否则留空，由 RHI/驱动决定

**Early-Z 的"早晚"是否启用**：取决于 shader 是否写 `SV_Depth` 或 `clip/discard`，与 PSO 的 DepthWrite 无关。所以"DepthWrite=false"既不会强制开 early-Z 也不会强制关 early-Z。

#### ② shader 末尾的 `OutDepth` 赋值（行 374-376）—— **不会执行**

```hlsl
#if OUTPUT_PIXEL_DEPTH_OFFSET
    ApplyPixelDepthOffsetForMobileBasePass(MaterialParameters, PixelMaterialInputs, OutDepth);
#endif
```

透明材质一般不会启用 pixel depth offset，所以这段**根本不进编译产物**。即使启用了，shader 写了 `OutDepth` 但 PSO 不写深度 → 写入被 OM 丢弃，shader 那次计算白做，但**没有可见的副作用**。

#### ③ 颜色输出相关分支（行 1061-1067 等）—— **与深度写完全无关**

`MATERIALBLENDING_TRANSLUCENT` 走的是 alpha-blend 那条 `OutColor` 赋值（行 1061），无论 DepthWrite 是 true 还是 false，shader 都会算 Color + 算 Lighting，然后通过 PSO 的 BlendState 与帧缓冲混合。

### 4.4 真正发生变化的，是 **OM（Output Merger）阶段**，不在 shader 里

对于 `MobileBasePass.cpp:1187` 创建的 transparent PSO：

| OM 行为 | 是否发生 |
|---|---|
| Depth Test（`CF_DepthNearOrEqual` 比较） | **执行**：像素被已写入的不透明深度遮挡时直接 kill |
| Depth Write | **不执行**：PSO `DepthWrite=false`，shader 即便写 `SV_Depth` 也不落到 tile memory |
| Stencil Test | 取决于 PSO Stencil 配置（行 1187 这个 `TStaticDepthStencilState` 只配了 depth，stencil 走默认关闭） |
| Stencil Write | 不执行 |
| Color Blend | **执行**（由 `SetTranslucentRenderState` 设置的 blend state 控制） |

对 TBDR（mobile 普遍使用）来说，深度写关闭 = **tile memory 中的 depth attachment 不会被这个 pass 更新** → 减少 tile memory BW 写流量；后续需要深度的 pass（PostProcess、后续不透明等）仍然能读到这之前的不透明深度。

### 4.5 小结

| 维度 | DepthWrite=false 对其的影响 |
|---|---|
| `.usf` 中具体哪一行代码会改变 | **没有任何一行**（shader 不感知 PSO DepthWrite） |
| `PIXELSHADER_EARLYDEPTHSTENCIL` 宏取值 | 不影响（由材质特性决定） |
| `USES_PIXEL_DISCARD` 宏 | 不影响 |
| `OUTPUT_PIXEL_DEPTH_OFFSET` 宏 | 不影响（材质独立属性） |
| `MATERIALBLENDING_TRANSLUCENT` 路径下颜色计算 | **完全照常**（shader 不知道不写深度） |
| GPU OM 阶段是否写深度 | **不写** |
| 深度测试是否还做 | **做**（`CF_DepthNearOrEqual` 仍生效） |
| Tile memory 流量 | 减少（depth attachment 不写） |
| Render graph 调度 | 受 `SetDepthStencilAccess(DepthRead_StencilRead)` 提示影响（与 shader 无关） |

> 一句话：**`MobileBasePassPixelShader.usf` 完全不感知"是否写深度"这件事；该决策只发生在 PSO 的 `DepthStencilState`，由 GPU 在 OM 阶段执行**。与上一节 `CW_NONE` 的逻辑一致——"算什么"在 shader，"怎么写到 RT / 是否写深度"在 PSO。

---

## 相关源文件索引

| 文件 | 关键内容 |
|---|---|
| `Engine/Source/Runtime/Renderer/Private/MobileBasePass.cpp` | 主题文件（`GetShaders`、`CreateMobileBasePassProcessor`、`SetOpaqueRenderState`、`SetTranslucentRenderState`） |
| `Engine/Source/Runtime/Renderer/Private/MobileBasePassRendering.h` | `TMobileBasePassPS` 模板类定义（L360） |
| `Engine/Source/Runtime/Engine/Public/RHIDefinitions.h:273-281` | `CW_NONE / CW_RGB / CW_RGBA` 等枚举 |
| `Engine/Source/Runtime/RenderCore/Public/RHIStaticStates.h:382` | `TStaticBlendStateWriteMask` 定义 |
| `Engine/Shaders/Private/MobileBasePassPixelShader.usf` | 最终被上述模板参数控制的 shader |
| `Engine/Source/Runtime/Renderer/Private/BasePassRendering.cpp:501,505` | PC 端 `TStaticBlendStateWriteMask` 典型用法参考 |
| `Engine/Source/Runtime/Renderer/Private/MobileDeferredShadingPass.cpp:674` | `CW_NONE` 用法参考（深度+MRT 不写颜色） |
| `Engine/Source/Runtime/Renderer/Private/SceneOcclusion.cpp:1238,1919` | Occlusion 通道 `CW_NONE` 经典用法 |