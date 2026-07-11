# MobileBasePassPixelShader.usf 中 Default Lit 与 Unlit 的差异分析

> **问题引用：**
> 如果我在材质中设置了 Default Lit 或者 Unlit，MobileBasePassPixelShader.usf 中发生了什么变化？相关的代码都有哪些？主要是移动端 Forward 路径，分析中要包含文件名和行号。

---

## 一、概览：差异如何产生

Unlit / Default Lit 在移动端 Forward 路径下的"差异"**不是运行时**的 if 分支，而是**编译期**由 `MATERIAL_SHADINGMODEL_UNLIT` 和 `MATERIAL_SHADINGMODEL_DEFAULT_LIT` 这两个宏定义驱动的代码裁剪 + 运行时通过 `GBuffer.ShadingModelID`（`SHADINGMODELID_UNLIT`=0 / `SHADINGMODELID_DEFAULT_LIT`=1）分流到不同路径。

| 材质设置 | 关键宏 | 关键运行时 ID |
|---|---|---|
| **Unlit** | `MATERIAL_SHADINGMODEL_UNLIT=1`（此时**不会**定义 `MATERIAL_SHADINGMODEL_DEFAULT_LIT`） | `SHADINGMODELID_UNLIT` = 0（见 `ShadingCommon.ush:20`） |
| **Default Lit** | `MATERIAL_SHADINGMODEL_DEFAULT_LIT=1`（同时还有 `MATERIAL_SINGLE_SHADINGMODEL=1`） | `SHADINGMODELID_DEFAULT_LIT` = 1（见 `ShadingCommon.ush:21`） |

> 在非 Substrate 路径下，`MATERIAL_SHADINGMODEL_*` 宏决定**该 Shader 是否为某个 Shading Model 生成特化代码**；而**真正进入哪一段分支**，由运行时读出来的 `GBuffer.ShadingModelID`（= `GetMaterialShadingModel()` 的返回值）决定。

---

## 二、宏定义从哪里来（CPU 端 → Shader 端）

### 2.1 默认值定义

`Engine/Shaders/Private/Definitions.usf:96-109`

```hlsl
#ifndef MATERIAL_SHADINGMODEL_DEFAULT_LIT
#define MATERIAL_SHADINGMODEL_DEFAULT_LIT				0
#endif
...
#ifndef MATERIAL_SHADINGMODEL_UNLIT
#define	MATERIAL_SHADINGMODEL_UNLIT						0
#endif
```

> 注意：`Definitions.usf` 只是兜底默认值。真正起作用的是 CPU 端编译材质时塞进 Shader 环境（`FShaderCompilerEnvironment`）的宏。

### 2.2 由材质翻译器写入 Shader 环境

`Engine/Source/Runtime/Engine/Private/Materials/HLSLMaterialTranslator.cpp`

- `Line 14361-14363`：当 `ShadingModels.IsLit()` 为 true（也就是**不包含 Unlit**）时，会把 `bShadingModelsIsLit=true`：
  ```cpp
  if (ShadingModels.IsLit())
  {
      EnvironmentDefines->bShadingModelsIsLit = true;
  ```
- `Line 14393-14397`：`else` 分支（即 Unlit-only），不再设 `bShadingModelsIsLit`，并把 `MSM_Unlit` 写到位掩码：
  ```cpp
  else
  {
      EnvironmentDefines->MaterialShadingModelEnabled |= 1 << MSM_Unlit;
  }
  ```
- `Line 2575-2580`：Lit 分支里根据位掩码设置 `MATERIAL_SHADINGMODEL_DEFAULT_LIT=1`：
  ```cpp
  if (EnvironmentDefines->bShadingModelsIsLit)
  {
      if (EnvironmentDefines->HasShadingModel(MSM_DefaultLit))
      {
          OutEnvironment.SetDefine(TEXT("MATERIAL_SHADINGMODEL_DEFAULT_LIT"), TEXT("1"));
      }
  ```
- `Line 2658-2662`：非 Lit（Unlit）分支，直接打开 `MATERIAL_SHADINGMODEL_UNLIT=1`：
  ```cpp
  else
  {
      OutEnvironment.SetDefine(TEXT("MATERIAL_SINGLE_SHADINGMODEL"), TEXT("1"));
      OutEnvironment.SetDefine(TEXT("MATERIAL_SHADINGMODEL_UNLIT"), TEXT("1"));
  }
  ```

> 简而言之：**你在材质编辑器里切换 Shading Model，会让材质编译时产物里出现 / 不出现这两个宏。** GPU 上跑的同一个 `MobileBasePassPixelShader.usf` 源文件，Unlit 材质编译出来的变体会比 Default Lit 变体少很多代码段。

---

## 三、`SHADINGMODELID_*` 常量与运行时取值

### 3.1 数值常量

`Engine/Shaders/Private/ShadingCommon.ush:19-34`

```hlsl
// SHADINGMODELID_* occupy the 4 low bits of an 8bit channel and SKIP_* occupy the 4 high bits
#define SHADINGMODELID_UNLIT				0
#define SHADINGMODELID_DEFAULT_LIT			1
...
```

### 3.2 `GetMaterialShadingModel` 取值来源

`Engine/Shaders/Private/MaterialTemplate.ush:3375-3378`

```hlsl
// Shading Model is an uint and represents a SHADINGMODELID_* in ShadingCommon.ush 
uint GetMaterialShadingModel(FPixelMaterialInputs PixelMaterialInputs)
{
	return PixelMaterialInputs.ShadingModel;
}
```

> `PixelMaterialInputs.ShadingModel` 是由 `FHLSLMaterialTranslator::GetMaterialShaderCode` 在每种材质编译产物里 inline 写死的（`%{shading_model_id};` 之类替换），它直接对应你 Material Editor 里选的 `EMaterialShadingModel`。详见 `HLSLMaterialTranslator::GetMaterialShadingModel` 链路。

---

## 四、`MobileBasePassPixelShader.usf` 中所有相关分支

> 以下行号都基于 `Engine/Shaders/Private/MobileBasePassPixelShader.usf`。被 `MATERIAL_SHADINGMODEL_*` 控制的关键节点共 **8 处**（Unlit 相关 6 处，Default Lit 相关 1 处，间接 1 处）。

### 4.1 AO（环境光遮蔽）开关 — `Line 38-43`

```hlsl
#if (MATERIALBLENDING_MASKED || MATERIALBLENDING_SOLID) 
	#if ENABLE_AMBIENT_OCCLUSION && !MATERIAL_SHADINGMODEL_UNLIT
		#undef APPLY_AO
		#define APPLY_AO 1
	#endif
#endif
```

- **Unlit**：直接走 `APPLY_AO=0`，跳过 `Line 384-389` 的 AO 采样。
- **Default Lit**：若 `ENABLE_AMBIENT_OCCLUSION=1`，打开 `APPLY_AO=1`，会采样 `MobileBasePass.AmbientOcclusionTexture` 并乘到 `MaterialAO` 上。

### 4.2 Translucent SH-Lighting 路径选择 — `Line 106`

```hlsl
#define TRANSLUCENCY_SH_LIGHTING (MATERIAL_SHADINGMODEL_DEFAULT_LIT || MATERIAL_SHADINGMODEL_SUBSURFACE) && (MATERIALBLENDING_TRANSLUCENT || MATERIALBLENDING_ADDITIVE) && !TRANSLUCENCY_LIGHTING_SURFACE_FORWARDSHADING && !MATERIAL_SHADINGMODEL_SINGLELAYERWATER
```

- **Unlit**：为 0，`AccumulateDirectionalLighting`（`MobileLightingCommon.ush:286`）走 `AccumulateDynamicLighting`（实 BRDF）路径——但因为 `Line 853` 直接跳过了 `AccumulateDirectionalLighting` 调用，所以此处不再有意义。
- **Default Lit**（半透 / 加性）：为 1，会走 `GetTranslucencySHLighting` SH 近似路径，见 `MobileLightingCommon.ush:292-296`：
  ```hlsl
  #if TRANSLUCENCY_SH_LIGHTING
      FLightAccumulator NewLighting = GetTranslucencySHLighting(GBuffer, LightData, TranslatedWorldPosition);
  #else
      FLightAccumulator NewLighting = AccumulateDynamicLighting(TranslatedWorldPosition, CameraVector, GBuffer, 1, GBuffer.ShadingModelID, LightData, LightAttenuation, 0, uint2(0, 0), OutDirectionalLightShadow);
  #endif
  ```

### 4.3 GBuffer 编码（仅 GBuffer Forward 路径） — `Line 842-844`

```hlsl
#if MOBILE_USE_GBUBUFFER && !MATERIAL_SHADINGMODEL_UNLIT
	GBuffer.IndirectIrradiance = IndirectIrradiance;
	MobileEncodeGBuffer(GBuffer, OutGBufferA, OutGBufferB, OutGBufferC, OutGBufferD);
#else
    ...
```

- **Unlit**：跳过 `MobileEncodeGBuffer`，`OutGBufferA/B/C/D` 不被写入（前提是 `MOBILE_USE_GBUBUFFER` 打开，启用 Mobile Deferred 时）。这也是为什么 Deferred 路径下 Unlit 不会污染 GBuffer 通道——少一次写。
- **Default Lit**：把 GBuffer 数据填到 4 个 RT 上去。

### 4.4 方向光累加 — `Line 853-855`

```hlsl
// Directional light
#if !MATERIAL_SHADINGMODEL_UNLIT
	AccumulateDirectionalLighting(GBuffer, MaterialParameters.WorldPosition_CamRelative, CameraVector, MaterialParameters.ScreenPosition, SvPosition, DynamicShadowFactors, DirectionalLightShadow, DirectLighting);
#endif
```

- **Unlit**：**完全跳过**方向光计算（`MobileLightingCommon.ush:286 AccumulateDirectionalLighting`），省 CSM 采样 + 主光 BRDF。
- **Default Lit**：正常累加方向光，包括 CSM 阴影（`MobileLightingCommon.ush:131-182`）和动态阴影因子。

### 4.5 直接光作为基础颜色 — `Line 993-997`

```hlsl
#if !MATERIAL_SHADINGMODEL_UNLIT
	half3 Color = DirectLighting.TotalLight;
#else
	half3 Color = 0.0f;
#endif
```

- **Unlit**：`Color` 直接置零，最终输出只由 `Emissive`（`Line 1010`）和 blending mode 决定。
- **Default Lit**：`Color` 取 `DirectLighting.TotalLight`（方向光 + IBL + 局部光 + 水体光照 累加结果）。

### 4.6 Unlit Viewmode（编辑器） — `Line 1014-1016`

```hlsl
#if !MATERIAL_SHADINGMODEL_UNLIT && MOBILE_EMULATION
	Color = lerp(Color, DiffuseColorForIndirect, ResolvedView.UnlitViewmodeMask);
#endif
```

- **Unlit**：跳过（因为本来就没意义）。
- **Default Lit**：在 Editor 视图模式为"Unlit"时，按 `UnlitViewmodeMask` 把 `Color` 渐变到 `DiffuseColorForIndirect`，即用间接漫反射替代真实光照。

### 4.7 安全颜色钳制 — `Line 1090-1093`

```hlsl
#if MATERIAL_IS_SKY || (!MATERIAL_SHADINGMODEL_UNLIT && !MOBILE_USE_GBUBUFFER)
	// clamp lit color to avoid overflows
	OutColor.rgb = SafeGetOutColor(OutColor.rgb);
#endif
```

- **Unlit**：跳过 `SafeGetOutColor`（`MobileLightingCommon.ush:55-63`）——因为它本来就不会触发 high luminance 路径（除非自带 Emissive + 极端值）。
- **Default Lit**（非 GBuffer Forward）：仍会钳制，避免 fp10 上溢出。

### 4.8 `SetGBufferForShadingModel` 与运行时分支（间接差异） — `Line 500-517`

`MobileBasePassPixelShader.usf:500-517`：

```hlsl
FGBufferData GBuffer = (FGBufferData)0;
GBuffer.GBufferAO = MaterialAO;
GBuffer.Depth = MaterialParameters.ScreenPosition.w;

SetGBufferForShadingModel(
	GBuffer,
	MaterialParameters,
	Opacity,
	BaseColor,
	Metallic,
	Specular,
	Roughness,
	Anisotropy,
	SubsurfaceColor,
	SubsurfaceProfile,
	0.0f,
	ShadingModelID
);
```

> 这一段**两边都会执行**（它本身不受 `MATERIAL_SHADINGMODEL_UNLIT` 控制），但其内部逻辑由 `MATERIAL_SHADINGMODEL_*` 宏裁剪。

详见 `Engine/Shaders/Private/ShadingModelsMaterial.ush:12-204`：

- `Line 26-33`：基础字段赋值（**两边都一样**）。
- `Line 41-203`：各 ShadingModel 的 `else if` 分支仅当对应 `MATERIAL_SHADINGMODEL_*=1` 时才参与编译。
- 关键点：**`ShadingModelID == SHADINGMODELID_UNLIT` 时**，这个函数体的 `if/else if` 链都不匹配，于是 `GBuffer.CustomData = 0`、`GBuffer.ShadingModelID = SHADINGMODELID_UNLIT(0)`、`GBuffer.BaseColor` 仍携带材质输出——**默认 GBuffer 数据继续被填充**，但不会被光照计算用到。

---

## 五、`BaseColor / Metallic / Specular / Roughness / Anisotropy` 提取差异

`MobileBasePassPixelShader.usf:393-431`：

```hlsl
#if !SUBSTRATE_ENABLED
	half3 BaseColor = GetMaterialBaseColor(PixelMaterialInputs);
	half Metallic = GetMaterialMetallic(PixelMaterialInputs);
	half Specular = GetMaterialSpecular(PixelMaterialInputs);
	...
	half Roughness = max(0.015625f, GetMaterialRoughness(PixelMaterialInputs));
	half Anisotropy = GetMaterialAnisotropy(PixelMaterialInputs);
	uint ShadingModelID = GetMaterialShadingModel(PixelMaterialInputs);
```

- 这段**两种材质都执行**，但 Unlit 材质中 `Metallic / Specular / Roughness / Anisotropy` 通常未被连接（UE 编辑器里 Unlit 材质这些 pin 是禁用 / 默认值），但代码里仍然**会调用 `GetMaterialXXX` 函数**。
- 真正区别在于：Default Lit 这些值会喂给 `SetGBufferForShadingModel`、后续 `ComputeF0` (`Line 537`)、`EnvBRDFApproxFullyRough` (`Line 542`) 等 BRDF 计算；Unlit 由于跳过了所有光照函数，这些值仅停留在 `GBuffer` 内部，不会被着色。

---

## 六、`F0 / DiffuseColor` 与 BRDF 准备 — `Line 533-551`

```hlsl
#if NONMETAL
	GBuffer.DiffuseColor = GBuffer.BaseColor;
	GBuffer.SpecularColor = 0.04;
#else
	GBuffer.SpecularColor = ComputeF0(GBuffer.Specular, GBuffer.BaseColor, GBuffer.Metallic);
	GBuffer.DiffuseColor = GBuffer.BaseColor - GBuffer.BaseColor * GBuffer.Metallic;
#endif
...
#if FULLY_ROUGH
	EnvBRDFApproxFullyRough(GBuffer.DiffuseColor, GBuffer.SpecularColor);
#endif
```

- **Unlit**：虽仍执行，但因 `Line 842/853/993` 都不走，`GBuffer.DiffuseColor/SpecularColor` 仅写入 GBuffer 通道（若 `MOBILE_USE_GBUBUFFER`）。
- **Default Lit**：正常参与 IBL 与 BRDF。

---

## 七、IBL / 反射 — `Line 860-869`

```hlsl
#if MATERIALBLENDING_MASKED || MATERIALBLENDING_SOLID || TRANSLUCENCY_LIGHTING_SURFACE_FORWARDSHADING || TRANSLUCENCY_LIGHTING_SURFACE_LIGHTINGVOLUME || MATERIAL_SHADINGMODEL_SINGLELAYERWATER
	// Reflection IBL
	AccumulateReflection(GBuffer
		, CameraVector
		, MaterialParameters.WorldPosition_CamRelative
		, MaterialParameters.ReflectionVector
		, IndirectIrradiance
		, GridIndex
		, DirectLighting);
#endif
```

- 这一段**不由 `MATERIAL_SHADINGMODEL_UNLIT` 直接控制**，但前提是 `!MATERIAL_SHADINGMODEL_UNLIT && (MASKED || SOLID || ...)`——意味着 Unlit 不透明材质仍可能**进入**这个分支，只是因为 `Line 853` 上面没 `AccumulateDirectionalLighting`，`DirectLighting` 在此之前仍为 0；`AccumulateReflection`（`MobileLightingCommon.ush:442-521`）依然会被调用，把 IBL 写进 `DirectLighting.Specular`，从而给 `Line 994` 的 `Color = DirectLighting.TotalLight` 提供一个非零输入。
  - **注意**：这是 UE 移动 Forward 路径的行为，**理论上 Unlit 材质在 IBL 配置下也会吸收少量 IBL**。如果要严格"Unlit"，需要保证材质不依赖 SkyLight / Reflection Capture，或者在编辑器里关掉 Lightmap / Reflection。

---

## 八、最终输出与 Blending — `Line 1056-1079`

```hlsl
#elif MATERIALBLENDING_ALPHACOMPOSITE || MATERIAL_SHADINGMODEL_SINGLELAYERWATER
	OutColor = half4(Color * VertexFog.a + VertexFog.rgb * Opacity, Opacity);
#elif MATERIALBLENDING_ALPHAHOLDOUT
	OutColor = half4(Color * VertexFog.a + VertexFog.rgb * Opacity, Opacity);
#elif MATERIALBLENDING_TRANSLUCENT
	OutColor = half4(Color * VertexFog.a + VertexFog.rgb, Opacity);
#elif MATERIALBLENDING_ADDITIVE
	OutColor = half4(Color * (VertexFog.a * Opacity.x), 0.0f);
#elif MATERIALBLENDING_MODULATE
	half3 FoggedColor = lerp(half3(1, 1, 1), Color, VertexFog.aaa * VertexFog.aaa);
	OutColor = half4(FoggedColor, Opacity);
#else
	OutColor.rgb = Color * VertexFog.a + VertexFog.rgb;
	...
#endif
```

- 这块两个材质都执行，只是 `Color` 取值不同：
  - **Unlit**：`Color = 0`（方向光累加 0） + `GetMaterialEmissive(PixelMaterialInputs)`（`Line 1010-1012`），最终 RGB ≈ `Emissive * 雾`。
  - **Default Lit**：`Color = DirectLighting.TotalLight + Emissive`，叠加 fog 后输出。

---

## 九、关键文件清单（按相关性排序）

| # | 文件 | 关键行 | 作用 |
|---|---|---|---|
| 1 | `Engine/Shaders/Private/MobileBasePassPixelShader.usf` | 见上文 8 处 | Forward 主路径主入口 |
| 2 | `Engine/Shaders/Private/Definitions.usf` | 96-109 | `MATERIAL_SHADINGMODEL_*` 默认值兜底 |
| 3 | `Engine/Shaders/Private/MaterialTemplate.ush` | 3375-3378 | `GetMaterialShadingModel()` 返回 `PixelMaterialInputs.ShadingModel` |
| 4 | `Engine/Shaders/Private/ShadingCommon.ush` | 20-21 | `SHADINGMODELID_UNLIT=0` / `SHADINGMODELID_DEFAULT_LIT=1` |
| 5 | `Engine/Shaders/Private/ShadingModelsMaterial.ush` | 12-204 | `SetGBufferForShadingModel`：由宏裁剪的 if/else if 链 |
| 6 | `Engine/Shaders/Private/MobileLightingCommon.ush` | 37-63 / 90-298 / 442-521 / 531+ | `SafeGetOutColor`、`AccumulateDirectionalLighting`、`AccumulateReflection`、`AccumulateLightGridLocalLighting` |
| 7 | `Engine/Shaders/Private/MobileBasePassCommon.ush` | 1-59 | 共享宏（`USE_VERTEX_FOG` 等） |
| 8 | `Engine/Shaders/Private/DeferredShadingCommon.ush` | 645 | Mobile Deferred 路径反序列化 `ShadingModelID` |
| 9 | `Engine/Source/Runtime/Engine/Private/Materials/HLSLMaterialTranslator.cpp` | 14355-14397 / 2575-2662 | CPU 端把材质设置 → Shader `define` |
| 10 | `Engine/Source/Runtime/Engine/Private/Materials/MaterialShader.cpp` | 105 / 247 | 运行时 `MSM_Unlit` 字符串 / 着色模型识别 |
| 11 | `Engine/Source/Runtime/Renderer/Private/MobileBasePass.cpp`（间接相关） | — | 调用 `GetShaders` 时筛 shader permutation，Unlit vs Lit 影响 permutation 维度 |
| 12 | `Engine/Docs/MobileBasePass_KeyCodeNotes.md` | — | 同目录下既有文档，可作交叉参考 |

---

## 十、一张表总结"Unlit vs Default Lit 在 Forward 路径上的差异"

| 行为 / 代码位置 | Default Lit | Unlit |
|---|---|---|
| `MATERIAL_SHADINGMODEL_DEFAULT_LIT` 定义 | ✅ (1) | ❌ (0) |
| `MATERIAL_SHADINGMODEL_UNLIT` 定义 | ❌ (0) | ✅ (1) |
| `MATERIAL_SINGLE_SHADINGMODEL` 定义 | ✅ (1) | ✅ (1) |
| AO 采样 (`Line 38-43`) | 视 `ENABLE_AMBIENT_OCCLUSION` | 跳过 |
| 漫反射 / IBL Precompute (`Line 786-822`) | 执行 | 仍执行（结果丢弃） |
| 方向光累加 (`Line 853`) | ✅ | ❌ 跳过 |
| 局部光（移动 Merged / Clustered） (`Line 872-956`) | 视平台 | 仍执行 IBL 部分（见 §7） |
| `GBuffer.DiffuseColor/SpecularColor` 计算 (`Line 533-543`) | 执行 | 执行但不参与最终输出 |
| `MobileEncodeGBuffer` (`Line 842-844`) | ✅（若 GBuffer Forward） | ❌ 跳过 |
| `Color = DirectLighting.TotalLight` (`Line 993-997`) | `DirectLighting.TotalLight` | `0` |
| `Color += Emissive` (`Line 1010-1012`) | ✅ | ✅ |
| `SafeGetOutColor` 钳制 (`Line 1090-1093`) | ✅ | ❌ 跳过 |
| 最终像素值 | `DirectLight + Emissive + 雾` | `Emissive + 雾` |
| 性能 | 方向光 + IBL + 局部光 + GBuffer 写 | 跳过方向光 / GBuffer 写，最快 |

---

## 十一、调试建议

1. **快速验证 Unlit 关闭方向光**：在 `MobileBasePassPixelShader.usf:853` 处插入 `OutputDebugString` / `InterlockedAdd`（在支持 SM5 的 PC 上调试）。
2. **快速验证 `MATERIAL_SHADINGMODEL_*` 是否生效**：在材质编辑器 Details 面板切换 Shading Model，触发材质重编后查看 `Saved/ShaderDebugInfo/<Platform>/Default.../` 目录里 `.usf` 反编译出的 HLSL（`.shader` 文件夹下 `CompileTimeDefines` 列表里能看到 `MATERIAL_SHADINGMODEL_DEFAULT_LIT=1` 或 `MATERIAL_SHADINGMODEL_UNLIT=1`）。
3. **运行时 ShadingModelId 验证**：在 PC 上开启 `r.ShaderDevelopmentMode 1` + `r.Mobile.UseHWsRGBEncoding 0`，并用 RenderDoc 抓帧，观察 Forward 路径 fragment 的运行时分叉即可。
