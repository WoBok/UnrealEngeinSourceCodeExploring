# 5.4 修复方案：移动端 VR MultiView 下 Custom Depth 只渲染单眼深度

> 仅做计划，不执行。本文件给出完整的修改清单（文件、行号、改动前后代码）。
>
> 参考来源：本地 `UnrealEngine5.6` 源码对照 `UnrealEngine5.4`。联网检索未找到公开的 Epic 官方说明（属于引擎内部实现），以源码 diff 为权威依据。

---

## 1. 背景与根因

`CustomDepthRendering.cpp:287` 处的 `SetStereoViewport(RHICmdList, View, 1.0f)` 本身没问题——它在移动端 VR（Mobile MultiView）下会把视口按 stereo 设置好，让一次 draw 同时写入双眼。

真正的 Bug 在于：**Custom Depth 使用的深度纹理不是纹理数组（Texture2DArray），而是一张普通 2D 纹理（Texture2D）**。

- 在移动端 VR MultiView 下，`SceneColor` / `SceneDepth` 都被创建成 `Texture2DArray`（2 个 slice，左右眼各一），渲染时通过 `MultiViewCount=2` 让一次 draw 同时写入两个 slice。
- 但 `FCustomDepthTextures::Create` 在 5.4 里**无视 `bRequireMultiView`，恒用 `FRDGTextureDesc::Create2D(...)`** 创建单张 2D 深度纹理。
- 于是即便 `SetStereoViewport` 设置了 stereo 视口、即便 draw 提交了，深度也只会写到一个 slice / 一只眼（另一只眼读不到 Custom Depth）。

这就是用户描述的「5.4 只渲染单眼深度，原因是只创建了一张深度纹理而不是纹理数组」。

5.6 已修复：`Create` 新增 `bRequireMultiView` 参数，并改用 `CreateRenderTargetTextureDesc(..., bRequireMultiView, MobileMultiViewRenderTargetNumLayers)`（在 multiView 时内部创建数组）；同时在渲染 pass 上设置 `PassParameters->RenderTargets.MultiViewCount`。

> 注意：5.6 的 `CreateRenderTargetTextureDesc(...)`、`MobileMultiViewRenderTargetNumLayers` 在 **5.4 中并不存在**。因此 5.4 的修复不能照抄 5.6 的 API，而要采用 5.4 自己已有的同款写法（见下）。

---

## 2. 5.4 vs 5.6 关键对比

| 关注点 | 5.4（有 Bug） | 5.6（已修复） |
|---|---|---|
| `FCustomDepthTextures::Create` 签名 | `(GraphBuilder, Extent, ShaderPlatform)` | `(GraphBuilder, Extent, ShaderPlatform, bool bRequireMultiView, uint16 MobileMultiViewRenderTargetNumLayers)` |
| 深度纹理创建 | `Create2D(...)`（单张 2D） | `CreateRenderTargetTextureDesc(..., bRequireMultiView, NumLayers)`（multiView 时为数组） |
| 渲染 pass `MultiViewCount` | **未设置（=0）** | `PassParameters->RenderTargets.MultiViewCount = (View.bIsMobileMultiViewEnabled) ? 2 : (View.Aspects.IsMobileMultiViewEnabled() ? 1 : 0);` |
| 调用处 `SceneTextures.cpp` | `Create(GraphBuilder, Config.Extent, Config.ShaderPlatform)` | `Create(GraphBuilder, Config.Extent, Config.ShaderPlatform, Config.bRequireMultiView, Config.MobileMultiViewRenderTargetNumLayers)` |
| Mobile Uniform Buffer | 仅有 `CustomDepthTexture`(Texture2D) | 新增 `CustomDepthTextureArray`(Texture2DArray) 与 `CustomStencilTextureArray`(Texture2DArray\<uint2\> SRV)，便于 shader 按眼采样 |

### 5.4 已有的同款「multiview 纹理数组」写法（修复时直接复用，保证可编译）

`SceneTextures.cpp`（5.4）创建 SceneDepth/SceneColor/DepthAux 时已经在用：
```cpp
// SceneTextures.cpp:455-457（SceneDepth）
FRDGTextureDesc Desc(Config.bRequireMultiView ?
                     FRDGTextureDesc::Create2DArray(Extent, PF_DepthStencil, Clear, Flags, 2) :
                     FRDGTextureDesc::Create2D(Extent, PF_DepthStencil, Clear, Flags));
// SceneTextures.cpp:604-606（DepthAux）同款三目
```
且 5.4 已把同一个数组资源同时绑定到 `Texture2D` 与 `Texture2DArray` 两个 shader 参数（`SceneTextures.cpp:984-985`）：
```cpp
SceneTextureParameters.SceneDepthTexture      = SceneTextures->Depth.Resolve; // Texture2D
SceneTextureParameters.SceneDepthTextureArray = SceneTextures->Depth.Resolve; // Texture2DArray
```
**这证明 5.4 的 RDG 完全支持「把 Texture2DArray 绑定到 Texture2D 参数」（自动取 slice 0 的单 slice SRV）**。所以把 Custom Depth 改成数组是安全的，不会破坏现有 `CustomDepthTexture`(Texture2D) 绑定。

---

## 3. 修复方案总览

- **Part A（核心、必需）**：让 Custom Depth 在 multiView 时创建为 `Texture2DArray`，并在渲染 pass 设置 `MultiViewCount`。**这一步即可修复「只渲染单眼深度」。**
- **Part B（可选、与 5.6 采样端对齐）**：给 Mobile Uniform Buffer 增加 `CustomDepthTextureArray` / `CustomStencilTextureArray`，让移动端 shader 能按眼采样 Custom Depth。仅当你的移动端 shader（如后处理/材质）确实需要按眼读取 Custom Depth 时才必须；不做也不会导致崩溃（现有 `CustomDepthTexture`(Texture2D) 仍可读到 slice 0）。

> 建议优先实施 Part A；Part B 视下游采样需求决定。

---

## 4. 详细修改清单

### ✅ Part A：核心修复（必需）

#### A1. `UnrealEngine5.4/Engine/Source/Runtime/Renderer/Internal/CustomDepthRendering.h`

**第 24 行**：给 `Create` 声明增加 `bool bRequireMultiView` 参数。

改动前：
```cpp
static FCustomDepthTextures Create(FRDGBuilder& GraphBuilder, FIntPoint CustomDepthExtent, EShaderPlatform ShaderPlatform);
```
改动后：
```cpp
static FCustomDepthTextures Create(FRDGBuilder& GraphBuilder, FIntPoint CustomDepthExtent, EShaderPlatform ShaderPlatform, bool bRequireMultiView);
```

> 说明：5.4 没有 `MobileMultiViewRenderTargetNumLayers`（5.6 新增字段），且移动端 VR 固定双眼，故数组大小沿用 5.4 既有写法的常量 `2`（与 SceneDepth/SceneColor/DepthAux 一致），不需要该参数。

---

#### A2. `UnrealEngine5.4/Engine/Source/Runtime/Renderer/Private/CustomDepthRendering.cpp`

**(a) 第 63 行**：函数签名增加 `bool bRequireMultiView` 参数。

改动前：
```cpp
FCustomDepthTextures FCustomDepthTextures::Create(FRDGBuilder& GraphBuilder, FIntPoint CustomDepthExtent, EShaderPlatform ShaderPlatform)
```
改动后：
```cpp
FCustomDepthTextures FCustomDepthTextures::Create(FRDGBuilder& GraphBuilder, FIntPoint CustomDepthExtent, EShaderPlatform ShaderPlatform, bool bRequireMultiView)
```

**(b) 第 86 行**：把单张 `Create2D` 改为「multiView 时创建数组」的三目（与 5.4 `SceneTextures.cpp:455-457` 完全同款）。

改动前：
```cpp
	const FRDGTextureDesc CustomDepthDesc = FRDGTextureDesc::Create2D(CustomDepthExtent, PF_DepthStencil, FClearValueBinding::DepthFar, CreateFlags);
```
改动后：
```cpp
	const FRDGTextureDesc CustomDepthDesc = bRequireMultiView ?
		FRDGTextureDesc::Create2DArray(CustomDepthExtent, PF_DepthStencil, FClearValueBinding::DepthFar, CreateFlags, 2) :
		FRDGTextureDesc::Create2D(CustomDepthExtent, PF_DepthStencil, FClearValueBinding::DepthFar, CreateFlags);
```

---

#### A3. `UnrealEngine5.4/Engine/Source/Runtime/Renderer/Private/CustomDepthRendering.cpp`

**第 277~279 行之间（在 `FDepthStencilBinding(...)` 闭合之后、`BuildRenderingCommands(...)` 之前）插入一行** 设置 `MultiViewCount`。

改动前（第 273~279 行）：
```cpp
			PassParameters->RenderTargets.DepthStencil = FDepthStencilBinding(
				CustomDepthTextures.Depth,
				DepthLoadAction,
				StencilLoadAction,
				FExclusiveDepthStencil::DepthWrite_StencilWrite);

			View.ParallelMeshDrawCommandPasses[EMeshPass::CustomDepth].BuildRenderingCommands(GraphBuilder, Scene->GPUScene, PassParameters->InstanceCullingDrawParams);
```
改动后：
```cpp
			PassParameters->RenderTargets.DepthStencil = FDepthStencilBinding(
				CustomDepthTextures.Depth,
				DepthLoadAction,
				StencilLoadAction,
				FExclusiveDepthStencil::DepthWrite_StencilWrite);
			PassParameters->RenderTargets.MultiViewCount = (View.bIsMobileMultiViewEnabled) ? 2 : (View.Aspects.IsMobileMultiViewEnabled() ? 1 : 0);

			View.ParallelMeshDrawCommandPasses[EMeshPass::CustomDepth].BuildRenderingCommands(GraphBuilder, Scene->GPUScene, PassParameters->InstanceCullingDrawParams);
```

> 表达式与 5.6 `CustomDepthRendering.cpp:278` 完全一致。`View.bIsMobileMultiViewEnabled` 与 `View.Aspects.IsMobileMultiViewEnabled()` 在 5.4 均已存在（参见 5.4 `MobileShadingRenderer.cpp:1517`、`PostProcessTonemap.cpp:1006`、`ShadowRendering.cpp:1755`）。
>
> 若想用 5.4 更简短的既有写法，可改为 `PassParameters->RenderTargets.MultiViewCount = View.bIsMobileMultiViewEnabled ? 2 : 0;`（见 `PostProcessTonemap.cpp:1006`），但这样会丢掉 `Aspects` 那条「非完整 MMV 但属于 multiView 应用」(=1) 的分支，建议照搬 5.6 表达式以保持一致。

---

#### A4. `UnrealEngine5.4/Engine/Source/Runtime/Renderer/Private/SceneTextures.cpp`

**第 495 行**：调用处传入 `Config.bRequireMultiView`。

改动前：
```cpp
	SceneTextures.CustomDepth = FCustomDepthTextures::Create(GraphBuilder, Config.Extent, Config.ShaderPlatform);
```
改动后：
```cpp
	SceneTextures.CustomDepth = FCustomDepthTextures::Create(GraphBuilder, Config.Extent, Config.ShaderPlatform, Config.bRequireMultiView);
```

> `Config.bRequireMultiView` 在 5.4 的 `FSceneTexturesConfig` 中已存在（`Engine/Public/SceneTexturesConfig.h:188`，位域 `uint32 bRequireMultiView : 1;`），并在 `SceneTextures.cpp:426` 由 `ViewFamily.bRequireMultiView && bAllViewsHaveMultiviewEnabled` 正确赋值。无需改动配置结构。
>
> 全仓检索确认：`FCustomDepthTextures::Create` 在 5.4 中**仅此一处调用**，改签名不会影响其他地方。

---

### 🟡 Part B：Mobile Uniform Buffer 采样端对齐（可选；与 5.6 完全对齐）

> 仅当移动端 shader 需要按眼（按 ViewID/slice）采样 Custom Depth 时才必须。否则 Part A 已让双眼都写入了深度，现有 `CustomDepthTexture`(Texture2D) 仍可读到 slice 0。

#### B1. `UnrealEngine5.4/Engine/Source/Runtime/Engine/Public/SceneTexturesConfig.h`

在 `FMobileSceneTextureUniformParameters` 中，**第 48 行之后** 与 **第 50 行之后** 各加一个数组参数（与 5.6 `SceneTexturesConfig.h:49`、`52` 对齐）。

改动前（第 48~51 行）：
```cpp
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D, CustomDepthTexture)
	SHADER_PARAMETER_SAMPLER(SamplerState, CustomDepthTextureSampler)
	SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<uint2>, CustomStencilTexture)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SceneVelocityTexture)
```
改动后：
```cpp
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D, CustomDepthTexture)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2DArray, CustomDepthTextureArray)
	SHADER_PARAMETER_SAMPLER(SamplerState, CustomDepthTextureSampler)
	SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<uint2>, CustomStencilTexture)
	SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2DArray<uint2>, CustomStencilTextureArray)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SceneVelocityTexture)
```

> 注意：Deferred（桌面）侧的 `FSceneTextureUniformParameters`（第 33~34 行）**不要改**——5.6 也没改，桌面 VR 用 instanced stereo，不走 mobile 多视图纹理数组。

---

#### B2. `UnrealEngine5.4/Engine/Source/Runtime/Renderer/Private/SceneTextures.cpp`

**Mobile 默认值（第 953~955 行区域）**：在 `CustomDepthTexture` 默认值后加数组默认值，在 `CustomStencilTexture` 默认值后加数组默认值。

改动前（第 952~955 行）：
```cpp
	// CustomDepthTexture is a color texture on mobile, with DeviceZ values
	SceneTextureParameters.CustomDepthTexture = SystemTextures.Black;
	SceneTextureParameters.CustomDepthTextureSampler = TStaticSamplerState<>::GetRHI();
	SceneTextureParameters.CustomStencilTexture = SystemTextures.StencilDummySRV;
```
改动后：
```cpp
	// CustomDepthTexture is a color texture on mobile, with DeviceZ values
	SceneTextureParameters.CustomDepthTexture = SystemTextures.Black;
	SceneTextureParameters.CustomDepthTextureArray = GSystemTextures.GetDefaultTexture(GraphBuilder, ETextureDimension::Texture2DArray, PF_DepthStencil, FClearValueBinding::Black);
	SceneTextureParameters.CustomDepthTextureSampler = TStaticSamplerState<>::GetRHI();
	SceneTextureParameters.CustomStencilTexture = SystemTextures.StencilDummySRV;
	SceneTextureParameters.CustomStencilTextureArray = SystemTextures.StencilDummySRV;
```

> ⚠️ 验证点：5.6 用的是 `PF_DepthStencil` 默认数组（5.6 `SceneTextures.cpp:1156`）。5.4 现有的同类默认数组（`SceneDepthTextureArray`、`SceneDepthAuxTextureArray`，见 `SceneTextures.cpp:948`、`964`）都用的是 `PF_B8G8R8A8`。若 5.4 的 `GSystemTextures.GetDefaultTexture(..., Texture2DArray, PF_DepthStencil, ...)` 在目标 RHI 上不支持/会 assert，可退而用与现有默认数组一致的 `PF_B8G8R8A8`（这只是「未产出 CustomDepth 时的 fallback」，不影响实际渲染写入）。
>
> ```cpp
> // 备选（更保守、与 5.4 现有 SceneDepthTextureArray 默认值一致）：
> SceneTextureParameters.CustomDepthTextureArray = GSystemTextures.GetDefaultTexture(GraphBuilder, ETextureDimension::Texture2DArray, EPixelFormat::PF_B8G8R8A8, FClearValueBinding::Black);
> ```

---

#### B3. `UnrealEngine5.4/Engine/Source/Runtime/Renderer/Private/SceneTextures.cpp`

**Mobile 绑定（第 1042~1043 行区域）**：产出 Custom Depth 时，把同一个数组资源同时绑定到 Texture2D 与 Texture2DArray 参数（与 5.6 `SceneTextures.cpp:1251-1254`、以及 5.4 自己对 SceneDepth 的 `984-985` 写法一致）。

改动前（第 1041~1043 行）：
```cpp
			bool bCustomDepthProduced = HasBeenProduced(CustomDepthTextures.Depth);
			SceneTextureParameters.CustomDepthTexture = bCustomDepthProduced ? CustomDepthTextures.Depth : SystemTextures.DepthDummy;
			SceneTextureParameters.CustomStencilTexture = bCustomDepthProduced ? CustomDepthTextures.Stencil : SystemTextures.StencilDummySRV;
```
改动后：
```cpp
			bool bCustomDepthProduced = HasBeenProduced(CustomDepthTextures.Depth);
			SceneTextureParameters.CustomDepthTexture = bCustomDepthProduced ? CustomDepthTextures.Depth : SystemTextures.DepthDummy;
			SceneTextureParameters.CustomDepthTextureArray = bCustomDepthProduced ? CustomDepthTextures.Depth : SystemTextures.DepthDummy;
			SceneTextureParameters.CustomStencilTexture = bCustomDepthProduced ? CustomDepthTextures.Stencil : SystemTextures.StencilDummySRV;
			SceneTextureParameters.CustomStencilTextureArray = bCustomDepthProduced ? CustomDepthTextures.Stencil : SystemTextures.StencilDummySRV;
```

---

## 5. 改动文件与行号一览（汇总）

| # | 文件（5.4 路径） | 行号 | 改动 | 必需 |
|---|---|---|---|---|
| A1 | `Engine/Source/Runtime/Renderer/Internal/CustomDepthRendering.h` | 24 | `Create` 声明加 `bool bRequireMultiView` | ✅ |
| A2 | `Engine/Source/Runtime/Renderer/Private/CustomDepthRendering.cpp` | 63 | `Create` 定义签名加 `bool bRequireMultiView` | ✅ |
| A2 | 同上 | 86 | `Create2D` → `bRequireMultiView ? Create2DArray(...,2) : Create2D(...)` | ✅ |
| A3 | 同上 | 277 后插入 | `PassParameters->RenderTargets.MultiViewCount = ...` | ✅ |
| A4 | `Engine/Source/Runtime/Renderer/Private/SceneTextures.cpp` | 495 | 调用传入 `Config.bRequireMultiView` | ✅ |
| B1 | `Engine/Source/Runtime/Engine/Public/SceneTexturesConfig.h` | 48 后 / 50 后 | 新增 `CustomDepthTextureArray` / `CustomStencilTextureArray` | 🟡 |
| B2 | `Engine/Source/Runtime/Renderer/Private/SceneTextures.cpp` | 953 后 / 955 后 | Mobile 默认值补数组 | 🟡 |
| B3 | 同上 | 1042 后 / 1043 后 | Mobile 绑定补数组 | 🟡 |

---

## 6. 验证与注意事项

1. **编译**：仅 Part A 即可编译通过（`Config.bRequireMultiView`、`View.bIsMobileMultiViewEnabled`、`View.Aspects.IsMobileMultiViewEnabled()`、`FRDGTextureDesc::Create2DArray`、`RenderTargets.MultiViewCount` 在 5.4 均已存在）。
2. **功能验证（移动端 VR MultiView）**：
   - 开启移动端 MultiView（`r.Mobile.MultiView` 与平台支持）。
   - 场景中放入启用 Custom Depth 的 mesh（`RenderCustomDepth`）。
   - 用单眼分别截图 / 在 shader 里分别采样左右眼 Custom Depth，确认**双眼都有 Custom Depth 写入**（修复前只有一只眼有）。
3. **回归**：
   - 桌面（Deferred）侧：`bRequireMultiView` 在桌面恒为 false（`GameViewportClient.cpp:1419` 仅 mobile renderer 才置 true），Custom Depth 仍为单张 2D 纹理，行为不变。
   - 非 VR 移动端：`bRequireMultiView=false`，走 `Create2D` 分支，行为不变。
   - Nanite Custom Depth 路径（`TotalNaniteInstances > 0`）：5.6 在该路径上**没有**新增 multiview 相关改动，本方案也不动它；其纹理同样来自 `Create`，会随之变为数组，由 `Nanite::EmitCustomDepthStencilTargets` 处理，与 5.6 行为一致。
4. **Part B 风险提示**：修改 `FMobileSceneTextureUniformParameters` 会改变 uniform buffer 布局，触发全局 shader 重编译；并需确认默认数组纹理的像素格式支持（见 B2 的验证点）。若下游 shader 不需要按眼采样，可只做 Part A。

---

## 7. 明确**不要**混入的 5.6 改动（与本 Bug 无关）

5.4→5.6 之间 `CustomDepthRendering.cpp` 还有若干**与本 multiview 修复无关**的重构，移植反而引入风险，**不要**包含：

- `if (auto* Pass = ...; Pass && ...)` 局部变量 + 空指针检查（5.6 第 260 行）。
- lambda 由 `[this, &View, PassParameters](FRHICommandList&)` 改为 `[&View, Pass, PassParameters](FRHIAsyncTask, FRHICommandList&)`、`DispatchDraw` 改为 `Pass->Draw(...)`（5.6 第 285~288 行）——属于异步渲染/`FParallelMeshDrawCommandPass` API 演进。
- `InitRasterContext` 新增 `bAsyncCompute` 实参（5.6 第 322 行）。
- `FPackedViewArray::Create` 去掉第二参数 `1`（5.6 第 339 行）——API 演进。
- 模板函数 `CollectPSOInitializers` 重命名为 `CollectPSOInitializersInternal`（5.6 第 456、712、730~741、746 行）。
- `RDG_EVENT_SCOPE_STAT` 新增（5.6 第 252 行）。
- `DoMaterialAndPrimitiveModifyMeshPosition(...)` 取代内联 WPO 判断（5.6 第 575 行）。

这些都与「Custom Depth 单眼/双眼」无关，本方案一律不动。

---

## 8. 最小改动版（只做 Part A 的 patch 摘要）

若只想最小修复，仅 4 处：

1. `Internal/CustomDepthRendering.h:24` — 声明加 `bool bRequireMultiView`
2. `Private/CustomDepthRendering.cpp:63` — 定义加 `bool bRequireMultiView`
3. `Private/CustomDepthRendering.cpp:86` — `Create2D` → 三目 `Create2DArray(...,2) : Create2D(...)`
4. `Private/CustomDepthRendering.cpp:277 后` — 插入 `MultiViewCount = ...`
5. `Private/SceneTextures.cpp:495` — 调用传 `Config.bRequireMultiView`

> （上表 1/2/3/4 对应 A1/A2/A3，5 对应 A4；其中 2、3 同在 `CustomDepthRendering.cpp`，故实际改动文件为 3 个。）
