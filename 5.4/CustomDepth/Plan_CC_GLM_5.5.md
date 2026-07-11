# 5.4 移动端 VR MultiView CustomDepth 单眼渲染 Bug 修复方案

> 目标：在 `UnrealEngine5.4` 上修复移动端 VR（Mobile MultiView）下 CustomDepth 只渲染单眼深度的问题。
> 方法：对比 `UnrealEngine5.5` 的修复，逐处分析代码与 Shader，给出完整的 5.4 移植方案（只做计划，不执行）。

---

## 0. 速览（TL;DR）

根因有两层，缺一不可：

1. **C++ 资源层**：5.4 的 `FCustomDepthTextures::Create` 永远用 `Create2D` 创建**单张**深度纹理；而同文件的 SceneDepth / SceneColor 都已根据 `Config.bRequireMultiView` 用 `Create2DArray(..., 2)` 创建纹理数组。CustomDepth 漏了这一步 → MultiView 渲染时只有 slice 0 可写，第二只眼被丢弃。
2. **Shader 层**：5.4 的 DepthOnly 顶点着色器在 **原生 Mobile MultiView**（`INSTANCED_STEREO=0 && MOBILE_MULTI_VIEW=1`，即 Vulkan 移动 VR 的常见路径）下缺少 `SV_ViewID` 分支，`ResolvedView` 恒为 eye0 → 即使有纹理数组，两只眼写的是同一只眼的深度。

5.5 的修复涉及 **5 个文件**（2 个 Shader + 3 个 C++）。5.4 移植必须**全部**做，仅改 C++ 不够（第二只眼会得到与第一只眼完全相同的深度，表现为"双眼重叠/无立体"）。

| # | 文件（5.4 路径） | 改动 | 必需 |
|---|---|---|---|
| 1 | `Engine/Shaders/Private/DepthOnlyVertexShader.usf` | 增加 `#elif MOBILE_MULTI_VIEW` 入参 + 三路 `ResolvedView` | 是 |
| 2 | `Engine/Shaders/Private/PositionOnlyDepthVertexShader.usf` | 增加 multiview 入参分支 + 三路 `ResolvedView`（含 fallback） | 是 |
| 3 | `Engine/Source/Runtime/Renderer/Internal/CustomDepthRendering.h` | `Create` 增加 `bool bRequireMultiView` 形参 | 是 |
| 4 | `Engine/Source/Runtime/Renderer/Private/CustomDepthRendering.cpp` | `Create` 用 `Create2DArray`；渲染 Pass 设置 `MultiViewCount` | 是 |
| 5 | `Engine/Source/Runtime/Renderer/Private/SceneTextures.cpp` | 调用处传 `Config.bRequireMultiView` | 是 |

> 其余路径（Nanite CustomDepth、Stencil SRV 提取、DepthOnlyPixelShader、`SetStereoViewport`）**无需改动**，见第 5 节分析。

---

## 1. 背景与两种 Mobile MultiView 模式

移动端 MultiView 有两种实现（见 `RenderCore/Private/StereoRenderUtils.cpp`），Shader 用宏区分：

| 模式 | 宏 | 机制 | Shader 分支 |
|---|---|---|---|
| **原生 Native**（Vulkan `VK_KHR_multiview`，最常见） | `INSTANCED_STEREO=0`，`MOBILE_MULTI_VIEW=1` | RHI 按 `SV_ViewID`/`gl_ViewID_OVR` 自动派发到两个 slice | `#elif MOBILE_MULTI_VIEW`（**5.4 缺失**） |
| **回退 Fallback** | `INSTANCED_STEREO=1`，`MOBILE_MULTI_VIEW=1` | 顶点工厂实例化，`SV_RenderTargetArrayIndex` 选层 | `#if INSTANCED_STEREO && MOBILE_MULTI_VIEW`（5.4 的 `DepthOnlyVertexShader.usf` 已有，`PositionOnly` 缺失） |

`MOBILE_MULTI_VIEW` 是 **per-platform 全局 Shader 宏**，在 `Engine/Source/Runtime/Engine/Private/ShaderCompiler/ShaderCompiler.cpp` 按平台 `Aspects.IsMobileMultiViewEnabled()` 设置。因此 DepthOnly 着色器在 multiview 平台上**会被编译为 `MOBILE_MULTI_VIEW=1`**，5.4 缺失的分支会导致落到 `#else`（eye0 only）。

支持设施 5.4 已具备（无需新增）：
- `InstancedStereo.ush:50-53`：`ViewState ResolveView(uint ViewIndex)` → `GetInstancedView(ViewIndex)`，`#if (INSTANCED_STEREO || MOBILE_MULTI_VIEW)`。
- `InstancedStereo.ush:89`：`#define USE_MULTI_VIEW_ID_SV (MOBILE_MULTI_VIEW && (COMPILER_VULKAN && !INSTANCED_STEREO))`。
- `MobileBasePassVertexShader.usf`（5.4 行 43-57）已用相同 `SV_ViewID`/`ResolveView(ViewId)` 模式 → 证明该模式在 5.4 可编译、可工作。
- `InstancedView` UniformBuffer 在 mobile multiview 时已绑定（`SceneView.cpp`）。

→ 结论：**只需把 DepthOnly 顶点着色器补成和 MobileBasePass 一样的 multiview 分支即可**，底层基础设施已就绪。

---

## 2. 根因定位（5.4 当前代码）

### 2.1 资源层：深度纹理不是数组

`CustomDepthRendering.cpp:63-94`（5.4）：

```cpp
// 5.4 / CustomDepthRendering.cpp:63
FCustomDepthTextures FCustomDepthTextures::Create(FRDGBuilder& GraphBuilder, FIntPoint CustomDepthExtent, EShaderPlatform ShaderPlatform)
{
    ...
    // 5.4 / CustomDepthRendering.cpp:86  —— 永远 Create2D
    const FRDGTextureDesc CustomDepthDesc = FRDGTextureDesc::Create2D(CustomDepthExtent, PF_DepthStencil, FClearValueBinding::DepthFar, CreateFlags);
    CustomDepthTextures.Depth = GraphBuilder.CreateTexture(CustomDepthDesc, TEXT("CustomDepth"));
    ...
}
```

对比同文件 SceneDepth / SceneColor 的正确做法（`SceneTextures.cpp`）：

```cpp
// 5.4 / SceneTextures.cpp:455-457  SceneDepth（正确，用数组）
FRDGTextureDesc Desc(Config.bRequireMultiView ?
                     FRDGTextureDesc::Create2DArray(SceneTextures.Config.Extent, PF_DepthStencil, Config.DepthClearValue, Config.DepthCreateFlags, 2) :
                     FRDGTextureDesc::Create2D(SceneTextures.Config.Extent, PF_DepthStencil, Config.DepthClearValue, Config.DepthCreateFlags));

// 5.4 / SceneTextures.cpp:487-489  SceneColor（正确，用数组）
FRDGTextureDesc Desc(Config.bRequireMultiView ?
                     FRDGTextureDesc::Create2DArray(Config.Extent, Config.ColorFormat, Config.ColorClearValue, Config.ColorCreateFlags, 2) :
                     FRDGTextureDesc::Create2D(Config.Extent, Config.ColorFormat, Config.ColorClearValue, Config.ColorCreateFlags));

// 5.4 / SceneTextures.cpp:495  CustomDepth（BUG：没传 bRequireMultiView）
SceneTextures.CustomDepth = FCustomDepthTextures::Create(GraphBuilder, Config.Extent, Config.ShaderPlatform);
```

`Config.bRequireMultiView` 在 5.4 已存在（`Engine/Public/SceneTexturesConfig.h:99` `bool bRequireMultiView = false;`，于 `SceneTextures.cpp:426` 由 `ViewFamily.bRequireMultiView && bAllViewsHaveMultiviewEnabled` 设置）。**CustomDepth 是唯一漏用它的地方。**

### 2.2 渲染 Pass：未设置 MultiViewCount

`CustomDepthRendering.cpp:273-289`（5.4）：

```cpp
// 5.4 / CustomDepthRendering.cpp:273-289
PassParameters->RenderTargets.DepthStencil = FDepthStencilBinding(
    CustomDepthTextures.Depth,
    DepthLoadAction,
    StencilLoadAction,
    FExclusiveDepthStencil::DepthWrite_StencilWrite);

View.ParallelMeshDrawCommandPasses[EMeshPass::CustomDepth].BuildRenderingCommands(GraphBuilder, Scene->GPUScene, PassParameters->InstanceCullingDrawParams);

GraphBuilder.AddPass(
    RDG_EVENT_NAME("CustomDepth"),
    PassParameters,
    ERDGPassFlags::Raster,
    [this, &View, PassParameters](FRHICommandList& RHICmdList)
{
    SetStereoViewport(RHICmdList, View, 1.0f);                              // <- 用户指向的 287 行
    View.ParallelMeshDrawCommandPasses[EMeshPass::CustomDepth].DispatchDraw(nullptr, RHICmdList, &PassParameters->InstanceCullingDrawParams);
});
```

**缺少** `PassParameters->RenderTargets.MultiViewCount = ...`。没有它，RHI 不会按 multiview 派发到两个 slice。

对比 5.4 已有的正确用法：
- `MobileShadingRenderer.cpp:1517`：`BasePassRenderTargets.MultiViewCount = MainView.bIsMobileMultiViewEnabled ? 2 : (bIsMultiViewApplication ? 1 : 0);`
- `PostProcess/PostProcessTonemap.cpp:1006, 1162`：`PassParameters->RenderTargets.MultiViewCount = View.bIsMobileMultiViewEnabled ? 2 : 0;`

### 2.3 Shader 层：DepthOnly VS 缺原生 multiview 分支

`DepthOnlyVertexShader.usf:38-53`（5.4）：

```c
// 5.4 / DepthOnlyVertexShader.usf:38-42  入参（无 #elif MOBILE_MULTI_VIEW）
#if INSTANCED_STEREO && MOBILE_MULTI_VIEW
    , out uint LayerIndex : SV_RenderTargetArrayIndex
#elif INSTANCED_STEREO
    , out uint ViewportIndex : SV_ViewPortArrayIndex
#endif
    )
{
#if INSTANCED_STEREO
    uint EyeIndex = GetEyeIndexFromVF(Input);
    #if MOBILE_MULTI_VIEW
        LayerIndex = EyeIndex;
    #else
        ViewportIndex = EyeIndex;
    #endif
#endif
    ResolvedView = ResolveViewFromVF(Input);   // <- 5.4 line 53：无条件；原生 multiview 时 == ResolveView() == eye0
    ...
    Output.Position = INVARIANT(mul(RasterizedWorldPosition, ResolvedView.TranslatedWorldToClip));  // 永远用 eye0 矩阵
```

`PositionOnlyDepthVertexShader.usf:11-37`（5.4）更糟：连 fallback 的 `INSTANCED_STEREO && MOBILE_MULTI_VIEW → SV_RenderTargetArrayIndex` 分支都没有，只有 `#if INSTANCED_STEREO → SV_ViewPortArrayIndex`。

→ 原生 multiview（`INSTANCED_STEREO=0`）下，5.4 两个 VS 都落到 eye0，第二只眼深度 = 第一只眼深度。

---

## 3. 5.5 是如何修复的（对比结论）

逐文件 diff 后，5.5 针对该 Bug 的**全部**改动：

### 3.1 Shader（2 文件）

**`DepthOnlyVertexShader.usf`** —— 入参增加 `#elif MOBILE_MULTI_VIEW`；body 把无条件 `ResolvedView = ResolveViewFromVF(Input);` 换成三路：

```c
// 5.5 / DepthOnlyVertexShader.usf:38-44
#if INSTANCED_STEREO && MOBILE_MULTI_VIEW
    , out uint LayerIndex : SV_RenderTargetArrayIndex
#elif MOBILE_MULTI_VIEW
    , in nointerpolation uint ViewId : SV_ViewID            // 新增
#elif INSTANCED_STEREO
    , out uint ViewportIndex : SV_ViewPortArrayIndex
#endif
    )
{
#if INSTANCED_STEREO
    uint EyeIndex = GetEyeIndexFromVF(Input);
    #if MOBILE_MULTI_VIEW
        LayerIndex = EyeIndex;
    #else
        ViewportIndex = EyeIndex;
    #endif
#endif
// 5.5 / DepthOnlyVertexShader.usf:55-61  三路
#if INSTANCED_STEREO
    ResolvedView = ResolveViewFromVF(Input);
#elif MOBILE_MULTI_VIEW
    ResolvedView = ResolveView(ViewId);                    // 新增：按 SV_ViewID 选每眼矩阵
#else
    ResolvedView = ResolveView();                           // 非 instanced 时与原 ResolveViewFromVF 等价
#endif
```

**`PositionOnlyDepthVertexShader.usf`** —— 同样补齐（5.5 完整版见第 4.2 节，建议整段替换）。

**`DepthOnlyPixelShader.usf`** —— `diff` 结果 5.4 与 5.5 **完全一致**，无需改动（深度由 VS 写，PS 仅做 alpha-clip / pixel-depth-offset）。

### 3.2 C++（3 文件）

**`CustomDepthRendering.h:24`**：`Create` 加 `bool bRequireMultiView`。

**`CustomDepthRendering.cpp`**：
- `Create`（行 63）加形参；
- 纹理描述（行 85-87）按 `bRequireMultiView` 选 `Create2DArray(..., 2)` / `Create2D`；
- 渲染 Pass（行 280）新增 `PassParameters->RenderTargets.MultiViewCount = (View.bIsMobileMultiViewEnabled) ? 2 : (View.Aspects.IsMobileMultiViewEnabled() ? 1 : 0);`

**`SceneTextures.cpp:495`**：调用传 `Config.bRequireMultiView`。

> 注意：5.5 的 `View.Aspects.IsMobileMultiViewEnabled()` 是 5.5 新引入的 aspect 系统，**5.4 不存在**，移植时需替换为 5.4 等价物（见第 4.4 节）。

---

## 4. 完整移植方案（5.4，逐文件 + 行号 + before/after）

### 4.1 Shader 文件 1：`Engine/Shaders/Private/DepthOnlyVertexShader.usf`

**改动 A — 入参（5.4 第 38-42 行）**

before：
```c
#if INSTANCED_STEREO && MOBILE_MULTI_VIEW
	, out uint LayerIndex : SV_RenderTargetArrayIndex
#elif INSTANCED_STEREO
	, out uint ViewportIndex : SV_ViewPortArrayIndex
#endif
	)
```

after（在两分支之间插入 `#elif MOBILE_MULTI_VIEW`）：
```c
#if INSTANCED_STEREO && MOBILE_MULTI_VIEW
	, out uint LayerIndex : SV_RenderTargetArrayIndex
#elif MOBILE_MULTI_VIEW
	, in nointerpolation uint ViewId : SV_ViewID
#elif INSTANCED_STEREO
	, out uint ViewportIndex : SV_ViewPortArrayIndex
#endif
	)
```

**改动 B — `ResolvedView` 解析（5.4 第 53 行，整个 `#if INSTANCED_STEREO ... #endif` 块之后的那一行）**

before（5.4 第 45-53 行）：
```c
#if INSTANCED_STEREO
	uint EyeIndex = GetEyeIndexFromVF(Input);
	#if MOBILE_MULTI_VIEW
		LayerIndex = EyeIndex;
	#else
		ViewportIndex = EyeIndex;
	#endif
#endif
	ResolvedView = ResolveViewFromVF(Input);
```

after：
```c
#if INSTANCED_STEREO
	uint EyeIndex = GetEyeIndexFromVF(Input);
	#if MOBILE_MULTI_VIEW
		LayerIndex = EyeIndex;
	#else
		ViewportIndex = EyeIndex;
	#endif
#endif
#if INSTANCED_STEREO
	ResolvedView = ResolveViewFromVF(Input);
#elif MOBILE_MULTI_VIEW
	ResolvedView = ResolveView(ViewId);
#else
	ResolvedView = ResolveView();
#endif
```

> 其余行（54-105）保持不变。`ResolveView()` 在 `!INSTANCED_STEREO` 时等价于原 `ResolveViewFromVF(Input)`（见 `VertexFactoryCommon.ush`），故非 multiview 平台行为不变。

### 4.2 Shader 文件 2：`Engine/Shaders/Private/PositionOnlyDepthVertexShader.usf`

整段替换 `Main`（5.4 第 11-37 行）为 5.5 版本：

before（5.4 第 11-37 行）：
```c
void Main(
	FPositionOnlyVertexFactoryInput Input,
	out INVARIANT_OUTPUT float4 OutPosition : SV_POSITION
#if USE_GLOBAL_CLIP_PLANE
	, out float OutGlobalClipPlaneDistance : SV_ClipDistance
#endif
#if INSTANCED_STEREO
	, out uint ViewportIndex : SV_ViewPortArrayIndex
#endif
	)
{
#if INSTANCED_STEREO
	const uint EyeIndex = GetEyeIndexFromVF(Input);
	ViewportIndex = EyeIndex;
#endif
	ResolvedView = ResolveViewFromVF(Input);

	float4 WorldPos = VertexFactoryGetWorldPosition(Input);

	{
		OutPosition = INVARIANT(mul(WorldPos, ResolvedView.TranslatedWorldToClip));
	}

#if USE_GLOBAL_CLIP_PLANE
	OutGlobalClipPlaneDistance = dot(ResolvedView.GlobalClippingPlane, float4(WorldPos.xyz, 1));
#endif
}
```

after（= 5.5 第 11-49 行，逐字移植）：
```c
void Main(
	FPositionOnlyVertexFactoryInput Input,
	out INVARIANT_OUTPUT float4 OutPosition : SV_POSITION
#if USE_GLOBAL_CLIP_PLANE
	, out float OutGlobalClipPlaneDistance : SV_ClipDistance
#endif
#if INSTANCED_STEREO && MOBILE_MULTI_VIEW // Mobile multi view fallback path
	, out uint LayerIndex : SV_RenderTargetArrayIndex
#elif MOBILE_MULTI_VIEW
	, in nointerpolation uint ViewId : SV_ViewID
#elif INSTANCED_STEREO
	, out uint ViewportIndex : SV_ViewPortArrayIndex
#endif
	)
{
#if INSTANCED_STEREO
	uint EyeIndex = GetEyeIndexFromVF(Input);
	ResolvedView = ResolveViewFromVF(Input);
	#if MOBILE_MULTI_VIEW
		LayerIndex = EyeIndex;
	#else
		ViewportIndex = EyeIndex;
	#endif
#elif MOBILE_MULTI_VIEW
	ResolvedView = ResolveView(ViewId);
#else
	ResolvedView = ResolveViewFromVF(Input);
#endif

	float4 WorldPos = VertexFactoryGetWorldPosition(Input);

	{
		OutPosition = INVARIANT(mul(WorldPos, ResolvedView.TranslatedWorldToClip));
	}

#if USE_GLOBAL_CLIP_PLANE
	OutGlobalClipPlaneDistance = dot(ResolvedView.GlobalClippingPlane, float4(WorldPos.xyz, 1));
#endif
}
```

> 注意 5.5 把 `ResolvedView = ResolveViewFromVF(Input);` 移进了 `#if INSTANCED_STEREO` 块内，并新增 `#elif MOBILE_MULTI_VIEW` / `#else`。务必整段替换，不要只加一行。

### 4.3 C++ 文件 3：`Engine/Source/Runtime/Renderer/Internal/CustomDepthRendering.h`

**第 24 行**：

before：
```cpp
	static FCustomDepthTextures Create(FRDGBuilder& GraphBuilder, FIntPoint CustomDepthExtent, EShaderPlatform ShaderPlatform);
```

after：
```cpp
	static FCustomDepthTextures Create(FRDGBuilder& GraphBuilder, FIntPoint CustomDepthExtent, EShaderPlatform ShaderPlatform, bool bRequireMultiView);
```

### 4.4 C++ 文件 4：`Engine/Source/Runtime/Renderer/Private/CustomDepthRendering.cpp`

**改动 A — `Create` 签名（第 63 行）**

before：
```cpp
FCustomDepthTextures FCustomDepthTextures::Create(FRDGBuilder& GraphBuilder, FIntPoint CustomDepthExtent, EShaderPlatform ShaderPlatform)
```

after：
```cpp
FCustomDepthTextures FCustomDepthTextures::Create(FRDGBuilder& GraphBuilder, FIntPoint CustomDepthExtent, EShaderPlatform ShaderPlatform, bool bRequireMultiView)
```

**改动 B — 纹理描述（第 86 行）**

before：
```cpp
	const FRDGTextureDesc CustomDepthDesc = FRDGTextureDesc::Create2D(CustomDepthExtent, PF_DepthStencil, FClearValueBinding::DepthFar, CreateFlags);
```

after：
```cpp
	const FRDGTextureDesc CustomDepthDesc(bRequireMultiView ?
		FRDGTextureDesc::Create2DArray(CustomDepthExtent, PF_DepthStencil, FClearValueBinding::DepthFar, CreateFlags, 2) :
		FRDGTextureDesc::Create2D(CustomDepthExtent, PF_DepthStencil, FClearValueBinding::DepthFar, CreateFlags));
```

**改动 C — 渲染 Pass 设置 `MultiViewCount`（在第 277 行 `FExclusiveDepthStencil::DepthWrite_StencilWrite);` 之后、第 279 行 `BuildRenderingCommands` 之前插入一行）**

before（5.4 第 273-279 行）：
```cpp
			PassParameters->RenderTargets.DepthStencil = FDepthStencilBinding(
				CustomDepthTextures.Depth,
				DepthLoadAction,
				StencilLoadAction,
				FExclusiveDepthStencil::DepthWrite_StencilWrite);

			View.ParallelMeshDrawCommandPasses[EMeshPass::CustomDepth].BuildRenderingCommands(GraphBuilder, Scene->GPUScene, PassParameters->InstanceCullingDrawParams);
```

after（新增第 278 行后的一行）：
```cpp
			PassParameters->RenderTargets.DepthStencil = FDepthStencilBinding(
				CustomDepthTextures.Depth,
				DepthLoadAction,
				StencilLoadAction,
				FExclusiveDepthStencil::DepthWrite_StencilWrite);
			PassParameters->RenderTargets.MultiViewCount = View.bIsMobileMultiViewEnabled ? 2 : 0;

			View.ParallelMeshDrawCommandPasses[EMeshPass::CustomDepth].BuildRenderingCommands(GraphBuilder, Scene->GPUScene, PassParameters->InstanceCullingDrawParams);
```

> **关于 `MultiViewCount` 取值（推荐方案 B）**：用 `View.bIsMobileMultiViewEnabled ? 2 : 0`，与 5.4 已有的 `PostProcess/PostProcessTonemap.cpp:1006,1162` 完全一致。
>
> **为何省略 5.5 的 `? 1` 分支是安全的**：5.4 引擎有不变量（`SceneTextures.cpp:413-414` 的 `ensureMsgf`：要么全部 view 开 multiview，要么都不开）。在该不变量下，"纹理是数组（`Config.bRequireMultiView`）" ⟺ "所有 view 都 `bIsMobileMultiViewEnabled`" ⟺ "每个 view `MultiViewCount=2`"。因此：
> - `View.bIsMobileMultiViewEnabled==true` ⇒ 纹理必然是数组 ⇒ `MultiViewCount=2` 合法；
> - `View.bIsMobileMultiViewEnabled==false` ⇒ 纹理必然是 2D ⇒ `MultiViewCount=0` 合法。
>
> 两者永远自洽，不会出现"2D 纹理 + MultiViewCount≠0"的非法组合。5.5 的 `? 1`（单 view multiview app）在 5.4 的 `Config.bRequireMultiView` 耦合下对 CustomDepth 不可达，且若强加可能把 `MultiViewCount=1` 设到 2D 纹理上，反而有风险。
>
> **备选方案 A（与 base pass 完全对齐）**：若希望与 `MobileShadingRenderer.cpp:1517` base pass 行为逐字一致，可在 `for` 循环之前（第 256 行之前）计算一次 `bIsMultiViewApplication`，再用：
> ```cpp
> PassParameters->RenderTargets.MultiViewCount = View.bIsMobileMultiViewEnabled ? 2 : (bIsMultiViewApplication ? 1 : 0);
> ```
> 其中 `bIsMultiViewApplication` 由 `static const auto CVar = IConsoleManager::Get().FindTConsoleVariableDataInt(TEXT("vr.MobileMultiView")); const bool bIsMultiViewApplication = (CVar && CVar->GetValueOnAnyThread() != 0);` 得到（抄自 `MobileShadingRenderer.cpp:1513-1514`）。**推荐方案 B**，更简洁且与同模块 PostProcessTonemap 一致。

### 4.5 C++ 文件 5：`Engine/Source/Runtime/Renderer/Private/SceneTextures.cpp`

**第 495 行**（位于 `FMinimalSceneTextures::InitializeViewFamily`，`Config` 在第 436 行已取到）：

before：
```cpp
	// Custom Depth
	SceneTextures.CustomDepth = FCustomDepthTextures::Create(GraphBuilder, Config.Extent, Config.ShaderPlatform);
```

after：
```cpp
	// Custom Depth
	SceneTextures.CustomDepth = FCustomDepthTextures::Create(GraphBuilder, Config.Extent, Config.ShaderPlatform, Config.bRequireMultiView);
```

> 这是 5.4 全引擎 `FCustomDepthTextures::Create` 的**唯一**调用点（已全仓 grep 确认，无其他调用处），改完即可编译。

---

## 5. 为何这些路径无需改动（避免遗漏 / 防止误改）

### 5.1 `SetStereoViewport`（`SceneRendering.cpp:5690-5717`）
仅设置 viewport 矩形（instanced stereo 时用 `ViewRectWithSecondaryViews` 合并矩形，否则单 view 矩形）。multiview 下整个 extent 作为单视口，per-eye 分流由 RHI 经 `SV_ViewID`/纹理数组完成，**与 viewport 无关，无需改**。

### 5.2 DepthOnly 像素着色器 `DepthOnlyPixelShader.usf`
`diff` 确认 5.4/5.5 **完全相同**。深度由 VS 写入 `SV_POSITION`，PS 仅做 masked 的 alpha-clip 与 pixel-depth-offset，用 `ResolveView()`（主视）即可。5.5 也未改它 → **无需改**。

### 5.3 Stencil SRV 提取（`CustomDepthRendering.cpp:394-413`，`TotalNaniteInstances==0` 的 else 分支）
```cpp
CustomDepthTextures.Stencil = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::CreateWithPixelFormat(CustomDepthTextures.Depth, PF_X24_G8));
```
深度变为 2DArray 后，该 SRV 覆盖全部 slice —— 这与 **SceneDepth** 在 multiview 下的处理**完全一致**（`SceneTextures.cpp:477` 对 SceneDepth 用同样写法）。SceneDepth 在 5.4 multiview 下工作正常，故 CustomDepth 同理 → **无需改**。GLES 路径的 `AddCopyTexturePass` 对 2DArray↔2DArray 同样成立。

### 5.4 Nanite CustomDepth 路径（`CustomDepthRendering.cpp:293-393`）
5.5 相对 5.4 在此处的差异（`InitRasterContext` 增加 `bAsyncCompute` 形参、`DispatchDraw`→`Draw`、lambda 增加 `FRHIAsyncTask` 形参）均为 5.4→5.5 的**无关 API 演进**，**非本 Bug 修复**。Nanite 经 compute UAV 写深度，需 SM6；移动 VR multiview（Vulkan ES/GLES mobile）通常不启用 Nanite，本 Bug 场景不触发该路径。5.5 也未为其加 multiview 专属改动 → **无需改**。
> 备注：若确实存在"Nanite + 移动 multiview + CustomDepth"的极端组合，那是 5.5 亦未覆盖的独立问题，不在本次修复范围。

### 5.5 其他 `Create2D` 资源
SceneDepth / SceneColor / Velocity / Shadow 等在 5.4 已正确使用 `Config.bRequireMultiView`（见第 1 节 grep），**无需改**。本次只补 CustomDepth 这一处遗漏。

### 5.6 `FCustomDepthTextures::Create` 其他调用点
全仓 grep 确认 5.4 仅 `SceneTextures.cpp:495` 一处调用，**无遗漏**。

---

## 6. 验证步骤（改完后如何确认生效）

1. **编译**：重编 Renderer 模块 + 重编受影响平台 Shader（`DepthOnlyVertexShader.usf`、`PositionOnlyDepthVertexShader.usf` 会因新增 `SV_ViewID` 输入而重新编译 PSO）。
2. **场景**：开启 CustomDepth（`r.CustomDepth=1` 或 `=2` 带 stencil）+ 移动端 multiview（`vr.MobileMultiView=1`，Vulkan ES/移动 Vulkan XR）。
3. **验证双眼**：
   - 用 CustomDepth 可视化（`ViewMode > Buffer Visualization > Custom Depth` 或自定义材质采样 `CustomDepth`/`SceneTexture:CustomDepth`）。
   - 遮挡单眼，确认两只眼的深度**不一致**（修复前两眼完全相同）。
   - 在 RenderDoc/Pix 抓帧：CustomDepth 纹理应为 **2DArray（2 slices）**，slice 0/1 分别为左右眼；`SV_ViewID` 在 VS 中取到 0/1。
4. **回归**：
   - 非 multiview（桌面 / `vr.MobileMultiView=0`）：纹理退化为 2D，`MultiViewCount=0`，行为与改前一致（`#else ResolvedView = ResolveView()` 路径）。
   - Instanced Stereo（桌面分屏 instanced）：`INSTANCED_STEREO=1` 分支未受影响。
   - CustomStencil：开启 stencil 写入，确认两眼 stencil 正确分写。
5. **PSO 预缓存**：`r.PSOPrecache.CustomDepth=1` 时确认新 VS 变体能被预缓存（`ShouldCompilePermutation` 未变，仅入参/宏分支变化）。

---

## 7. 风险与注意

- **Shader 重新编译**：两个 VS 新增 `SV_ViewID` 输入与 `MOBILE_MULTI_VIEW` 分支，会改变 multiview 平台的 PSO 哈希 → 首次需重编该平台全部使用 DepthOnly VS 的 PSO（含普通 DepthPrepass，因共用同一 VS —— 这同时也修复了 5.4 普通深度预pass 在原生 multiview 下的同类问题，属正向副作用）。
- **`? 1` 单 view multiview**：5.4 耦合下对 CustomDepth 不可达（见 4.4 论证）。若将来 `Config.bRequireMultiView` 的设置逻辑被改（解除 `bAllViewsHaveMultiviewEnabled` 耦合），需重新评估是否补 `? 1`。
- **GLES**：`PositionOnly`/`DepthOnly` VS 在 GLES multiview（`OVR_multiview2`）下也走 `MOBILE_MULTI_VIEW` 分支，`SV_ViewID` 对应 `gl_ViewID_OVR`，由 UE shader compiler 处理，无需额外改动。
- **不影响 Nanite**：Nanite 路径未动（见 5.4）。

---

## 8. 改动清单（落地用）

- [ ] `UnrealEngine5.4/Engine/Shaders/Private/DepthOnlyVertexShader.usf`（第 38-42 行入参、第 53 行 ResolvedView）
- [ ] `UnrealEngine5.4/Engine/Shaders/Private/PositionOnlyDepthVertexShader.usf`（第 11-37 行整段替换为 5.5 版）
- [ ] `UnrealEngine5.4/Engine/Source/Runtime/Renderer/Internal/CustomDepthRendering.h`（第 24 行加 `bool bRequireMultiView`）
- [ ] `UnrealEngine5.4/Engine/Source/Runtime/Renderer/Private/CustomDepthRendering.cpp`（第 63 行签名、第 86 行纹理描述、第 277 行后插入 `MultiViewCount`）
- [ ] `UnrealEngine5.4/Engine/Source/Runtime/Renderer/Private/SceneTextures.cpp`（第 495 行传 `Config.bRequireMultiView`）

> 全部为对 5.5 已验证修复的忠实移植；5.4 已具备所有依赖设施（`Config.bRequireMultiView`、`ResolveView(uint)`、`MOBILE_MULTI_VIEW` 宏、`MultiViewCount` 字段、`InstancedView` UB），无需新增基础设施。
