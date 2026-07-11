# RenderAfterTranslucency 完整修改方案（UE5.6 版）

功能：标记的 StaticMesh/SkeletalMesh 在不透明阶段只写深度（走深度专用 Shader，MobileAfterTranslucencyDepthPass），半透明渲染完成后再绘制颜色（MobileAfterTranslucencyPass），从而遮挡半透明物体。仅移动端 Forward 路径。

本文档是 `Plan_Final.md`（UE5.4 版）针对 **UE5.6 源码逐文件核验后的移植版**。所有行号均已按本仓库 5.6 源码核实（修改前行号，插入后顺移）。所有新增代码以 `//RenderAfterTranslucency Added` / `Changed` 标注。

---

## 0. 5.6 相对 5.4 的关键差异总览（先读）

核验结论：**方案整体设计在 5.6 完全可行**，但以下 6 点必须按 5.6 新写法调整：

| # | 差异 | 影响的章节 |
|---|------|-----------|
| 1 | `EMeshPass::Num` 由 32 变为 **39**（MeshDecal 拆为 5 个、新增 SecondStageDepthPass / TranslucencyHoldout / SingleLayerWaterDepthPrepass / MaterialCacheProjection 等），static_assert 数字要改为 41 | §1 |
| 2 | `GetViewRelevance` 所在文件变了：StaticMeshRender.cpp → **StaticMeshSceneProxy.cpp**；SkeletalMesh.cpp → **SkeletalMeshSceneProxy.cpp** | §8、§9 |
| 3 | `FPrimitiveSceneProxyDesc::InitializeFrom` 更名为 **`InitializeFromPrimitiveComponent`** | §5 |
| 4 | `View.ParallelMeshDrawCommandPasses[...]` 由对象数组改为 **指针数组（懒创建，可能为 nullptr）**；`DispatchDraw(nullptr, RHICmdList, ...)` 改为 **`Pass->Draw(RHICmdList, ...)`**，必须判空 | §13 |
| 5 | `FMobileSceneRenderer::BuildInstanceCullingDrawParams` 成员函数与 `DepthPassInstanceCullingDrawParams` 等**成员变量已被删除**；改为在 `RenderForwardSinglePass` / `RenderForwardMultiPass` 内部的**局部 `ParameterCollection` 结构体** + 静态函数 `BuildMeshRenderingCommands` | §14.2 作废、§15 重写 |
| 6 | Stat/事件宏改为 `RHI_BREADCRUMB_EVENT_STAT(RHICmdList, StatName, "Text")` 搭配 `SCOPED_GPU_STAT`；`SetCurrentStat(GET_STATID(STAT_CLMM_*))` 已从移动渲染器移除 | §13、§15 |

另外两处 5.6 新逻辑经核验**不影响本方案**：
- **SecondStageDepthPass**（DepthRendering.cpp:1219-1233、SceneVisibility.cpp:1638、2357）：所有分流都带 `ShadingPath != EShadingPath::Mobile` 条件，移动端不启用，与本方案无交集。
- **DDM_AllOpaqueNoVelocity**：5.6 中 `bIsFullDepthPrepassEnabled = (EarlyZPassMode == DDM_AllOpaque || EarlyZPassMode == DDM_AllOpaqueNoVelocity)`（MobileShadingRenderer.cpp:319），本方案沿用该成员做判断即可，语义不变。`FDepthPassMeshProcessor::AddMeshBatch` 内新增的 velocity 跳过块（:1023-1048）仅在 `EarlyZPassMode == DDM_AllOpaqueNoVelocity` 时生效，我们的深度 Pass 固定传 `DDM_AllOpaque`，不受影响。

懒创建带来一个额外好处：场景里没有任何标记物体时，两个新 Pass 的指针为 nullptr，Build/Draw 全部自然 no-op，零开销。

---

## 1. Engine/Source/Runtime/Renderer/Public/MeshPassProcessor.h

### 1.1 枚举添加两个 Pass（:77 之后）

相关代码（:75-88）：
```c++
		WaterInfoTextureDepthPass,
		WaterInfoTexturePass,
		MaterialCacheProjection,

#if WITH_EDITOR
		HitProxy,
		HitProxyOpaqueOnly,
		EditorLevelInstance,
		EditorSelection,
#endif

		Num,
		NumBits = 6,
	};
```

修改为：
```c++
		WaterInfoTextureDepthPass,
		WaterInfoTexturePass,
		MaterialCacheProjection,
		MobileAfterTranslucencyDepthPass,	//RenderAfterTranslucency Added: 不透明阶段之后、半透明之前，只写深度
		MobileAfterTranslucencyPass,		//RenderAfterTranslucency Added: 半透明之后绘制颜色

#if WITH_EDITOR
		HitProxy,
		HitProxyOpaqueOnly,
		EditorLevelInstance,
		EditorSelection,
#endif

		Num,
		NumBits = 6,
	};
```
> 说明：新增后 Num = **41**（非编辑器）/ **45**（编辑器），`NumBits = 6`（上限 64）无需改动，:90 的 `static_assert(EMeshPass::Num <= (1 << EMeshPass::NumBits))` 自动通过。

### 1.2 GetMeshPassName 添加 case 并更新断言（:135、:145-148）

相关代码（:133-148）：
```c++
	case EMeshPass::WaterInfoTextureDepthPass: return TEXT("WaterInfoTextureDepthPass");
	case EMeshPass::WaterInfoTexturePass: return TEXT("WaterInfoTexturePass");
	case EMeshPass::MaterialCacheProjection: return TEXT("ObjectProjection");
#if WITH_EDITOR
	...
#endif
	}

#if WITH_EDITOR
	static_assert(EMeshPass::Num == 39 + 4, "Need to update switch(MeshPass) after changing EMeshPass"); // GUID ... {674D7D62-CFD8-4971-9A8D-CD91E5612CD8}
#else
	static_assert(EMeshPass::Num == 39, "Need to update switch(MeshPass) after changing EMeshPass"); // GUID ...
#endif
```

修改为：
```c++
	case EMeshPass::WaterInfoTextureDepthPass: return TEXT("WaterInfoTextureDepthPass");
	case EMeshPass::WaterInfoTexturePass: return TEXT("WaterInfoTexturePass");
	case EMeshPass::MaterialCacheProjection: return TEXT("ObjectProjection");
	case EMeshPass::MobileAfterTranslucencyDepthPass: return TEXT("MobileAfterTranslucencyDepthPass");	//RenderAfterTranslucency Added
	case EMeshPass::MobileAfterTranslucencyPass: return TEXT("MobileAfterTranslucencyPass");			//RenderAfterTranslucency Added
#if WITH_EDITOR
	...
#endif
	}

#if WITH_EDITOR
	static_assert(EMeshPass::Num == 41 + 4, "Need to update switch(MeshPass) after changing EMeshPass"); //RenderAfterTranslucency Changed
#else
	static_assert(EMeshPass::Num == 41, "Need to update switch(MeshPass) after changing EMeshPass"); //RenderAfterTranslucency Changed
#endif
```

---

## 2. Engine/Source/Runtime/Engine/Classes/Components/PrimitiveComponent.h

与 5.4 相同，仅行号变化。

### 2.1 添加组件属性（:437 之后）

相关代码（:435-441）：
```c++
	/** If true, this component will be rendered in the main pass (z prepass, basepass, transparency) */
	UPROPERTY(EditAnywhere, AdvancedDisplay, BlueprintReadOnly, Category = Rendering)
	uint8 bRenderInMainPass:1;

	/** If true, this component will be rendered in the depth pass even if it's not rendered in the main pass */
	UPROPERTY(EditAnywhere, AdvancedDisplay, BlueprintReadOnly, Category = Rendering, meta = (EditCondition = "!bRenderInMainPass"))
	uint8 bRenderInDepthPass:1;
```

在 `bRenderInMainPass:1;` 与下一条注释之间插入：
```c++
	//RenderAfterTranslucency Added: 移动端Forward下，不透明阶段只写深度，半透明之后再绘制颜色
	UPROPERTY(EditAnywhere, AdvancedDisplay, BlueprintReadOnly, Category = Rendering, meta = (DisplayName = "Render Opaque After Translucency (Mobile)", EditCondition = "bRenderInMainPass"))
	uint8 bRenderAfterTranslucency : 1;
```

### 2.2 添加 Setter 声明（:2018-2020 之后）

相关代码：
```c++
	/** Sets bRenderInMainPass property and marks the render state dirty. */
	UFUNCTION(BlueprintCallable, Category = "Rendering")
	ENGINE_API void SetRenderInMainPass(bool bValue);
```

其后插入：
```c++
	//RenderAfterTranslucency Added
	/** Sets bRenderAfterTranslucency property and marks the render state dirty. */
	UFUNCTION(BlueprintCallable, Category = "Rendering")
	ENGINE_API void SetRenderAfterTranslucency(bool bValue);
```

---

## 3. Engine/Source/Runtime/Engine/Private/Components/PrimitiveComponent.cpp

与 5.4 相同，仅行号变化。

### 3.1 构造函数默认值（:352 之后）

相关代码（:349-354）：
```c++
	bVisibleInReflectionCaptures = true;
	bVisibleInRealTimeSkyCaptures = true;
	bVisibleInRayTracing = true;
	bRenderInMainPass = true;
	bRenderInDepthPass = true;
	VisibilityId = INDEX_NONE;
```

在 `bRenderInMainPass = true;` 后插入：
```c++
	bRenderAfterTranslucency = false;	//RenderAfterTranslucency Added
```

### 3.2 Setter 实现（:4897-4904 `SetRenderInMainPass` 之后）

```c++
//RenderAfterTranslucency Added
void UPrimitiveComponent::SetRenderAfterTranslucency(bool bValue)
{
	if (bRenderAfterTranslucency != bValue)
	{
		bRenderAfterTranslucency = bValue;
		MarkRenderStateDirty();
	}
}
```

---

## 4. Engine/Source/Runtime/Engine/Public/PrimitiveSceneProxy.h

与 5.4 相同，仅行号变化。

### 4.1 Proxy 字段（:1286-1289）

相关代码：
```c++
	uint8 bRenderInDepthPass : 1;

	/** If true this primitive Renders in the mainPass */
	uint8 bRenderInMainPass : 1;

	/** If true this primitive is hidden (used when level is not yet visible) */
	uint8 bForceHidden : 1;
```

在 `bRenderInMainPass : 1;` 后插入：
```c++
	//RenderAfterTranslucency Added（位置紧跟 bRenderInMainPass，与5.2初始化列表顺序保持一致，避免 -Wreorder）
	uint8 bRenderAfterTranslucency : 1;
```

### 4.2 Getter（:744-745）

相关代码：
```c++
	inline bool ShouldRenderInMainPass() const { return bRenderInMainPass; }
	inline bool ShouldRenderInDepthPass() const { return bRenderInMainPass || bRenderInDepthPass; }
```

两行之间插入：
```c++
	inline bool ShouldRenderAfterTranslucency() const { return bRenderAfterTranslucency; }	//RenderAfterTranslucency Added
```

---

## 5. Engine/Source/Runtime/Engine/Private/PrimitiveSceneProxy.cpp

### 5.1 Component → Desc 拷贝（:297，**注意 5.6 中函数已更名为 `FPrimitiveSceneProxyDesc::InitializeFromPrimitiveComponent`**，:285）

相关代码（:293-298）：
```c++
void FPrimitiveSceneProxyDesc::InitializeFromPrimitiveComponent(const UPrimitiveComponent* InComponent)
{
	...
	bRenderInDepthPass = InComponent->bRenderInDepthPass;
	bRenderInMainPass = InComponent->bRenderInMainPass;
	bTreatAsBackgroundForOcclusion = InComponent->bTreatAsBackgroundForOcclusion;
```

在 `bRenderInMainPass = ...;` 后插入：
```c++
	bRenderAfterTranslucency = InComponent->bRenderAfterTranslucency;	//RenderAfterTranslucency Added
```

### 5.2 Desc → Proxy 初始化列表（:458-460）

相关代码：
```c++
,	bRenderInDepthPass(InProxyDesc.bRenderInDepthPass)
,	bRenderInMainPass(InProxyDesc.bRenderInMainPass)
,	bForceHidden(false)
```

`bRenderInMainPass(...)` 后插入：
```c++
,	bRenderAfterTranslucency(InProxyDesc.bRenderAfterTranslucency)	//RenderAfterTranslucency Added
```

---

## 6. Engine/Source/Runtime/Engine/Public/PrimitiveSceneProxyDesc.h

与 5.4 相同，仅行号变化。

### 6.1 默认值（:25 之后）

```c++
		bRenderInDepthPass = true;
		bRenderInMainPass = true;
		bRenderAfterTranslucency = false;	//RenderAfterTranslucency Added
		bTreatAsBackgroundForOcclusion = false;
```

### 6.2 字段（:100 之后）

```c++
	uint32 bRenderInDepthPass : 1;
	uint32 bRenderInMainPass : 1;
	uint32 bRenderAfterTranslucency : 1;	//RenderAfterTranslucency Added
	uint32 bTreatAsBackgroundForOcclusion : 1;
```

> 说明：静态网格 Desc 路径 `FStaticMeshSceneProxyDesc::InitializeFromStaticMeshComponent`（StaticMeshSceneProxy.cpp:2787-2789）首行调用 `InitializeFromPrimitiveComponent(InComponent)`，自动生效，无需额外修改。骨骼网格 Desc 同理。

---

## 7. Engine/Source/Runtime/Engine/Public/PrimitiveViewRelevance.h

### 7.1 字段（:54 之后）

相关代码（:53-56）：
```c++
	/** The primitive should render to the base pass / normal depth / velocity rendering. */
	uint32 bRenderInMainPass : 1;
	/** The primitive is drawn only in the editor and composited onto the scene after post processing */
	uint32 bEditorPrimitiveRelevance : 1;
```

`bRenderInMainPass : 1;` 后插入：
```c++
	/** RenderAfterTranslucency Added: mobile forward, opaque color rendered after translucency */
	uint32 bRenderAfterTranslucency : 1;
```

> 构造函数按字节整体清零，默认即 false，无需显式初始化。

---

## 8. Engine/Source/Runtime/Engine/Private/StaticMeshSceneProxy.cpp（**5.4 时在 StaticMeshRender.cpp，5.6 已拆分到此文件**）

### FStaticMeshSceneProxy::GetViewRelevance（:2234）

相关代码（:2231-2236）：
```c++
	FPrimitiveViewRelevance Result;
	Result.bDrawRelevance = IsShown(View) && View->Family->EngineShowFlags.StaticMeshes;
	Result.bRenderCustomDepth = ShouldRenderCustomDepth();
	Result.bRenderInMainPass = ShouldRenderInMainPass();
	Result.bRenderInDepthPass = ShouldRenderInDepthPass();
	Result.bUsesLightingChannels = GetLightingChannelMask() != GetDefaultLightingChannelMask();
```

在 `Result.bRenderInMainPass = ...;` 后插入：
```c++
	Result.bRenderAfterTranslucency = ShouldRenderAfterTranslucency();	//RenderAfterTranslucency Added
```

> InstancedStaticMesh / HISM 的 Proxy 继承自 FStaticMeshSceneProxy 的 GetViewRelevance，自动生效。

---

## 9. Engine/Source/Runtime/Engine/Private/SkeletalMeshSceneProxy.cpp（**5.4 时在 SkeletalMesh.cpp，5.6 已拆分到此文件**）

### FSkeletalMeshSceneProxy::GetViewRelevance（:1155）

相关代码（:1150-1157）：
```c++
	Result.bStaticRelevance = bRenderStatic && !IsRichView(*View->Family)
		&& MeshObject->GetLOD() >= GetCurrentFirstLODIdx_Internal()
		&& !IsDynamic();
	Result.bDynamicRelevance = ~Result.bStaticRelevance;
	Result.bRenderCustomDepth = ShouldRenderCustomDepth();
	Result.bRenderInMainPass = ShouldRenderInMainPass();
	Result.bRenderInDepthPass = ShouldRenderInDepthPass();
	Result.bUsesLightingChannels = GetLightingChannelMask() != GetDefaultLightingChannelMask();
```

在 `Result.bRenderInMainPass = ...;` 后插入：
```c++
	Result.bRenderAfterTranslucency = ShouldRenderAfterTranslucency();	//RenderAfterTranslucency Added
```

---

## 10. Engine/Source/Runtime/Renderer/Private/SceneVisibility.cpp

结构与 5.4 一致，注意 5.6 中 `Scene` 在 FRelevancePacket 内是**引用**（用 `Scene.GetShaderPlatform()`，参照 :1413 现成写法）。

### 10.1 静态网格路径（:1659-1681）

相关代码：
```c++
							// Mark static mesh as visible for rendering
							if (StaticMeshRelevance.bUseForMaterial && (ViewRelevance.bRenderInMainPass || ViewRelevance.bRenderCustomDepth))
							{
								// Specific logic for mobile packets
								if (ShadingPath == EShadingPath::Mobile)
								{
									// Skydome must not be added to base pass bucket
									if (!StaticMeshRelevance.bUseSkyMaterial)
									{
										DrawCommandPacket.AddCommandsForMesh(PrimitiveIndex, PrimitiveSceneInfo, StaticMeshRelevance, StaticMesh, CullingPayloadFlags, Scene, bCanCache, EMeshPass::BasePass);
										if (!bMobileBasePassAlwaysUsesCSM)
										{
											DrawCommandPacket.AddCommandsForMesh(PrimitiveIndex, PrimitiveSceneInfo, StaticMeshRelevance, StaticMesh, CullingPayloadFlags, Scene, bCanCache, EMeshPass::MobileBasePassCSM);
										}
									}
									else
									{
										DrawCommandPacket.AddCommandsForMesh(PrimitiveIndex, PrimitiveSceneInfo, StaticMeshRelevance, StaticMesh, CullingPayloadFlags, Scene, bCanCache, EMeshPass::SkyPass);
									}
									// bUseSingleLayerWaterMaterial is added to BasePass on Mobile. No need to add it to SingleLayerWaterPass

									MarkMask |= EMarkMaskBits::StaticMeshVisibilityMapMask;
								}
```

`if (!StaticMeshRelevance.bUseSkyMaterial)` 块内修改为：
```c++
									if (!StaticMeshRelevance.bUseSkyMaterial)
									{
										//RenderAfterTranslucency Added: 仅移动端Forward路径分流；Mobile Deferred没有dispatch新Pass，必须回落BasePass
										const bool bMobileRenderAfterTranslucency = ViewRelevance.bRenderAfterTranslucency
											&& !IsMobileDeferredShadingEnabled(Scene.GetShaderPlatform());
										if (bMobileRenderAfterTranslucency)
										{
											DrawCommandPacket.AddCommandsForMesh(PrimitiveIndex, PrimitiveSceneInfo, StaticMeshRelevance, StaticMesh, CullingPayloadFlags, Scene, bCanCache, EMeshPass::MobileAfterTranslucencyDepthPass);
											DrawCommandPacket.AddCommandsForMesh(PrimitiveIndex, PrimitiveSceneInfo, StaticMeshRelevance, StaticMesh, CullingPayloadFlags, Scene, bCanCache, EMeshPass::MobileAfterTranslucencyPass);
										}
										else
										{
											DrawCommandPacket.AddCommandsForMesh(PrimitiveIndex, PrimitiveSceneInfo, StaticMeshRelevance, StaticMesh, CullingPayloadFlags, Scene, bCanCache, EMeshPass::BasePass);
											if (!bMobileBasePassAlwaysUsesCSM)
											{
												//RenderAfterTranslucency Changed: 移入else，标记物体不生成CSM BasePass命令
												DrawCommandPacket.AddCommandsForMesh(PrimitiveIndex, PrimitiveSceneInfo, StaticMeshRelevance, StaticMesh, CullingPayloadFlags, Scene, bCanCache, EMeshPass::MobileBasePassCSM);
											}
										}
									}
```
> `IsMobileDeferredShadingEnabled` 声明于 `RenderUtils.h:275`（RenderCore 公共头）；若编译报未声明，在文件顶部补 `#include "RenderUtils.h"`。
> 深度命令入队处（:1634-1645，`EMeshPass::DepthPass` / `SecondStageDepthPass`）**不做修改**：标记物体保持进入常规 DepthPass（SecondStageDepthPass 分支带 `ShadingPath != EShadingPath::Mobile`，移动端不会走到），这样 Masked-Only Prepass / Full Prepass 配置下其深度仍在正确时机写入。

### 10.2 动态网格路径 ComputeDynamicMeshRelevance（:2342 函数、修改点 :2368-2389）

相关代码：
```c++
		if (ViewRelevance.bRenderInMainPass || ViewRelevance.bRenderCustomDepth)
		{
			PassMask.Set(EMeshPass::BasePass);
			View.NumVisibleDynamicMeshElements[EMeshPass::BasePass] += NumElements;

			if (ViewRelevance.bUsesSkyMaterial)
			{
				PassMask.Set(EMeshPass::SkyPass);
				View.NumVisibleDynamicMeshElements[EMeshPass::SkyPass] += NumElements;
			}

			if (ViewRelevance.bUsesAnisotropy)
			{
				PassMask.Set(EMeshPass::AnisotropyPass);
				View.NumVisibleDynamicMeshElements[EMeshPass::AnisotropyPass] += NumElements;
			}

			if (ShadingPath == EShadingPath::Mobile)
			{
				PassMask.Set(EMeshPass::MobileBasePassCSM);
				View.NumVisibleDynamicMeshElements[EMeshPass::MobileBasePassCSM] += NumElements;
			}
```

修改为：
```c++
		if (ViewRelevance.bRenderInMainPass || ViewRelevance.bRenderCustomDepth)
		{
			//RenderAfterTranslucency Added: 仅移动端Forward路径分流
			const bool bMobileRenderAfterTranslucency = ShadingPath == EShadingPath::Mobile
				&& ViewRelevance.bRenderAfterTranslucency
				&& !IsMobileDeferredShadingEnabled(View.GetShaderPlatform());
			if (bMobileRenderAfterTranslucency)
			{
				PassMask.Set(EMeshPass::MobileAfterTranslucencyDepthPass);
				View.NumVisibleDynamicMeshElements[EMeshPass::MobileAfterTranslucencyDepthPass] += NumElements;
				PassMask.Set(EMeshPass::MobileAfterTranslucencyPass);
				View.NumVisibleDynamicMeshElements[EMeshPass::MobileAfterTranslucencyPass] += NumElements;
			}
			else
			{
				PassMask.Set(EMeshPass::BasePass);
				View.NumVisibleDynamicMeshElements[EMeshPass::BasePass] += NumElements;
			}

			if (ViewRelevance.bUsesSkyMaterial)
			{
				PassMask.Set(EMeshPass::SkyPass);
				View.NumVisibleDynamicMeshElements[EMeshPass::SkyPass] += NumElements;
			}

			if (ViewRelevance.bUsesAnisotropy)
			{
				PassMask.Set(EMeshPass::AnisotropyPass);
				View.NumVisibleDynamicMeshElements[EMeshPass::AnisotropyPass] += NumElements;
			}

			if (ShadingPath == EShadingPath::Mobile && !bMobileRenderAfterTranslucency)	//RenderAfterTranslucency Changed: 标记物体不进CSM Pass
			{
				PassMask.Set(EMeshPass::MobileBasePassCSM);
				View.NumVisibleDynamicMeshElements[EMeshPass::MobileBasePassCSM] += NumElements;
			}
```
> `FSceneView::GetShaderPlatform()` 在 5.6 存在（SceneView.h:1954）。
> 函数开头 :2355-2366 的 DepthPass/SecondStageDepthPass 分流不修改（移动端恒走 DepthPass 分支）。

---

## 11. Engine/Source/Runtime/Renderer/Private/MobileBasePass.cpp（颜色 Pass）

与 5.4 结构一致，行号变化。`bDeferredShading` / `bTranslucentBasePass` 为构造函数成员（:729-730）；`EFlags::ForcePassDrawRenderState` 定义在 MobileBasePassRendering.h:451；`Process()` 的状态覆盖逻辑在 :846-866（`bForcePassDrawRenderState` 已被支持）。

### 11.1 AddMeshBatch 分流（:780-788）

相关代码：
```c++
void FMobileBasePassMeshProcessor::AddMeshBatch(const FMeshBatch& RESTRICT MeshBatch, uint64 BatchElementMask, const FPrimitiveSceneProxy* RESTRICT PrimitiveSceneProxy, int32 StaticMeshId)
{
	if (!MeshBatch.bUseForMaterial || 
		(Flags & FMobileBasePassMeshProcessor::EFlags::DoNotCache) == FMobileBasePassMeshProcessor::EFlags::DoNotCache ||
		(PrimitiveSceneProxy && !PrimitiveSceneProxy->ShouldRenderInMainPass()))
	{
		return;
	}
	
	const FMaterialRenderProxy* MaterialRenderProxy = MeshBatch.MaterialRenderProxy;
```

guard 块之后、`MaterialRenderProxy` 之前插入：
```c++
	//RenderAfterTranslucency Added: Pass分流。
	// bDeferredShading时禁用分流（延迟路径没有dispatch新Pass，标记物体保持走BasePass）。
	// else分支必须限定!bTranslucentBasePass：本Processor同时服务TranslucencyStandard/AfterDOF/All
	//（见本文件:1136-1138注册），不能把标记物体上的半透明材质Section从半透明Pass中剔除。
	const bool bAfterTranslucencyColorPass = (MeshPassType == EMeshPass::MobileAfterTranslucencyPass);
	const bool bShouldRenderAfterTranslucency = !bDeferredShading && PrimitiveSceneProxy && PrimitiveSceneProxy->ShouldRenderAfterTranslucency();
	if (bAfterTranslucencyColorPass)
	{
		if (!bShouldRenderAfterTranslucency)
		{
			return;
		}
	}
	else if (!bTranslucentBasePass)
	{
		if (bShouldRenderAfterTranslucency)
		{
			return;
		}
	}
```

### 11.2 CollectPSOInitializers 跳过新 Pass（:978 函数开头）

5.6 中该函数开头结构有变（TranslucencyAll 检查移入 `if (bTranslucentBasePass)`），但我们的修改仍是**在函数最顶部**加早退，不受影响：

```c++
void FMobileBasePassMeshProcessor::CollectPSOInitializers(const FSceneTexturesConfig& SceneTexturesConfig, const FMaterial& Material, const FPSOPrecacheVertexFactoryData& VertexFactoryData, const FPSOPrecacheParams& PreCacheParams, TArray<FPSOPrecacheData>& PSOInitializers)
{
	//RenderAfterTranslucency Added: 暂不为该Pass做PSO预热（SetupPrecachePSOParams不感知标记，收集到的状态也不匹配）。
	// 代价：标记物体首次绘制时运行时编译PSO，可能有一次卡顿；如需消除，可参照本函数按颜色Pass实际RenderState收集。
	if (MeshPassType == EMeshPass::MobileAfterTranslucencyPass)
	{
		return;
	}
	if (bTranslucentBasePass)
	{
		...（原有代码不动）
```

### 11.3 添加颜色 Pass 的 Create 函数（:1132 `CreateMobileTranslucencyAllPassProcessor` 结束之后、:1134 注册表之前）

```c++
//RenderAfterTranslucency Added: 半透明之后的不透明颜色Pass。
// 深度已由MobileAfterTranslucencyDepthPass(或Full Prepass)写入，此处只读深度、CF_DepthNearOrEqual测试。
// 必须带ForcePassDrawRenderState：否则Process()会走SetOpaqueRenderState/CF_Equal覆盖(本文件:846-866)，
// 把深度写重新打开，与DepthRead访问声明及MultiPass第二RenderPass的只读深度绑定冲突。
// 副作用：不再输出ReceiveDecal的stencil位（本Pass在贴花之后执行，本就无意义）。
FMeshPassProcessor* CreateMobileAfterTranslucencyPassProcessor(ERHIFeatureLevel::Type FeatureLevel, const FScene* Scene, const FSceneView* InViewIfDynamicMeshCommand, FMeshPassDrawListContext* InDrawListContext)
{
	FMeshPassProcessorRenderState PassDrawRenderState;
	PassDrawRenderState.SetBlendState(TStaticBlendStateWriteMask<CW_RGBA>::GetRHI());
	PassDrawRenderState.SetDepthStencilAccess(FExclusiveDepthStencil::DepthRead_StencilRead);
	PassDrawRenderState.SetDepthStencilState(TStaticDepthStencilState<false, CF_DepthNearOrEqual>::GetRHI());

	const FMobileBasePassMeshProcessor::EFlags Flags = FMobileBasePassMeshProcessor::EFlags::CanUseDepthStencil
		| FMobileBasePassMeshProcessor::EFlags::ForcePassDrawRenderState;

	return new FMobileBasePassMeshProcessor(EMeshPass::MobileAfterTranslucencyPass, Scene, InViewIfDynamicMeshCommand, PassDrawRenderState, InDrawListContext, Flags);
}
```

### 11.4 注册（:1138 之后，`// Skipping EMeshPass::TranslucencyAfterDOFModulate ...` 注释之前）

```c++
//RenderAfterTranslucency Added
REGISTER_MESHPASSPROCESSOR_AND_PSOCOLLECTOR(MobileAfterTranslucencyPass,	CreateMobileAfterTranslucencyPassProcessor,	EShadingPath::Mobile, EMeshPass::MobileAfterTranslucencyPass,	EMeshPassFlags::CachedMeshCommands | EMeshPassFlags::MainView);
```

---

## 12. Engine/Source/Runtime/Renderer/Private/DepthRendering.cpp（深度 Pass）

深度 Pass 使用 `FDepthPassMeshProcessor`（不透明、无 WPO、支持 PositionOnly 流的材质自动替换为默认材质 + 仅位置 VS + 空 PS；骨骼网格走完整 VF 的 DepthOnly VS）。5.6 构造函数签名未变（DepthRendering.h:184-197，`bShadowProjection`、`bSecondStageDepthPass` 均有默认值 false）。

### 12.1 AddMeshBatch 分流（:995-997）

```c++
void FDepthPassMeshProcessor::AddMeshBatch(const FMeshBatch& RESTRICT MeshBatch, uint64 BatchElementMask, const FPrimitiveSceneProxy* RESTRICT PrimitiveSceneProxy, int32 StaticMeshId)
{
	//RenderAfterTranslucency Added: 本Pass只绘制标记物体。
	// 注意：常规DepthPass（Masked预Pass/Full Prepass）不剔除标记物体——那些配置下其深度需要在原时机写入。
	if (MeshPassType == EMeshPass::MobileAfterTranslucencyDepthPass)
	{
		if (!PrimitiveSceneProxy || !PrimitiveSceneProxy->ShouldRenderAfterTranslucency())
		{
			return;
		}
	}

	bool bDraw = MeshBatch.bUseForDepthPass;
	...（原有代码不动）
```
> :1023-1048 新增的 `DDM_AllOpaqueNoVelocity` velocity 跳过块：我们的 Pass 固定 `DDM_AllOpaque`，条件不满足，无影响。

### 12.2 CollectPSOInitializers 跳过新 Pass（:1070 函数开头）

```c++
void FDepthPassMeshProcessor::CollectPSOInitializers(const FSceneTexturesConfig& SceneTexturesConfig, const FMaterial& Material, const FPSOPrecacheVertexFactoryData& VertexFactoryData, const FPSOPrecacheParams& PreCacheParams, TArray<FPSOPrecacheData>& PSOInitializers)
{		
	//RenderAfterTranslucency Added: 本Pass在SceneColor RenderPass内执行（绑定颜色RT），
	// 此处收集的深度专用RT布局与实际RenderPass不匹配，预热无效，直接跳过。
	if (MeshPassType == EMeshPass::MobileAfterTranslucencyDepthPass)
	{
		return;
	}
	// Are we currently collecting PSO's for the default material
	if (PreCacheParams.bDefaultMaterial)
	...（原有代码不动）
```

### 12.3 Create 函数与注册（:1217 `MobileDepthPass` 注册行之后）

相关代码（:1216-1217）：
```c++
REGISTER_MESHPASSPROCESSOR_AND_PSOCOLLECTOR(DepthPass, CreateDepthPassProcessor, EShadingPath::Deferred, EMeshPass::DepthPass, EMeshPassFlags::CachedMeshCommands | EMeshPassFlags::MainView);
REGISTER_MESHPASSPROCESSOR_AND_PSOCOLLECTOR(MobileDepthPass, CreateDepthPassProcessor, EShadingPath::Mobile, EMeshPass::DepthPass, EMeshPassFlags::CachedMeshCommands | EMeshPassFlags::MainView);
```

其后追加：
```c++
//RenderAfterTranslucency Added: 标记物体的只写深度Pass（BasePass之后、半透明之前执行）。
// SetupDepthPassState(:471-476) = CW_NONE + <true, CF_DepthNearOrEqual>，正是"只写深度不写颜色"。
// EarlyZPassMode固定传DDM_AllOpaque：保证Masked材质也被绘制（带FDepthOnlyPS做clip），
//   不受项目r.EarlyZPass设置影响。
// bRespectUseAsOccluderFlag传false：否则AddMeshBatch(:1000-1021)会按Occluder/Movable/屏幕尺寸过滤掉动态小物体。
FMeshPassProcessor* CreateMobileAfterTranslucencyDepthPassProcessor(ERHIFeatureLevel::Type FeatureLevel, const FScene* Scene, const FSceneView* InViewIfDynamicMeshCommand, FMeshPassDrawListContext* InDrawListContext)
{
	FMeshPassProcessorRenderState DepthPassState;
	SetupDepthPassState(DepthPassState);
	DepthPassState.SetDepthStencilAccess(FExclusiveDepthStencil::DepthWrite_StencilWrite);

	return new FDepthPassMeshProcessor(
		EMeshPass::MobileAfterTranslucencyDepthPass,
		Scene,
		FeatureLevel,
		InViewIfDynamicMeshCommand,
		DepthPassState,
		/*InbRespectUseAsOccluderFlag*/ false,
		/*InEarlyZPassMode*/ DDM_AllOpaque,
		/*InbEarlyZPassMovable*/ true,
		/*bDitheredLODFadingOutMaskPass*/ false,
		InDrawListContext);
}
REGISTER_MESHPASSPROCESSOR_AND_PSOCOLLECTOR(MobileAfterTranslucencyDepthPass, CreateMobileAfterTranslucencyDepthPassProcessor, EShadingPath::Mobile, EMeshPass::MobileAfterTranslucencyDepthPass, EMeshPassFlags::CachedMeshCommands | EMeshPassFlags::MainView);
```

---

## 13. Engine/Source/Runtime/Renderer/Private/MobileBasePassRendering.cpp【5.6 写法重写】

### 添加两个 Render 函数（:547，`RenderMobileBasePass` 结束之后、`RenderMobileEditorPrimitives` 之前）

**5.6 差异**：`ParallelMeshDrawCommandPasses` 是指针数组必须判空；`DispatchDraw` → `Pass->Draw`；事件宏用 `RHI_BREADCRUMB_EVENT_STAT`；视口用 `SetStereoViewport`（与 `FMobileSceneRenderer::RenderPrePass`（DepthRendering.cpp:660-676）写法一致，兼容 Instanced Stereo；MMV 下与 `SetViewport(View.ViewRect...)` 等价）。

```c++
//RenderAfterTranslucency Added: 只写深度。必须在RHICmdList.NextSubpass()之前调用（Subpass 0深度可写）。
void FMobileSceneRenderer::RenderMobileAfterTranslucencyDepthPass(FRHICommandList& RHICmdList, const FViewInfo& View, const FInstanceCullingDrawParams* InstanceCullingDrawParams)
{
	if (auto* Pass = View.ParallelMeshDrawCommandPasses[EMeshPass::MobileAfterTranslucencyDepthPass])
	{
		CSV_SCOPED_TIMING_STAT_EXCLUSIVE(RenderAfterTranslucencyDepth);
		SCOPE_CYCLE_COUNTER(STAT_AfterTranslucencyDepthDrawTime);
		RHI_BREADCRUMB_EVENT_STAT(RHICmdList, AfterTranslucencyDepth, "MobileAfterTranslucencyDepthPass");
		SCOPED_GPU_STAT(RHICmdList, AfterTranslucencyDepth);

		SetStereoViewport(RHICmdList, View);
		Pass->Draw(RHICmdList, InstanceCullingDrawParams);
	}
}

//RenderAfterTranslucency Added: 半透明之后绘制颜色（只读深度）。
void FMobileSceneRenderer::RenderMobileAfterTranslucencyPass(FRHICommandList& RHICmdList, const FViewInfo& View, const FInstanceCullingDrawParams* InstanceCullingDrawParams)
{
	if (auto* Pass = View.ParallelMeshDrawCommandPasses[EMeshPass::MobileAfterTranslucencyPass])
	{
		CSV_SCOPED_TIMING_STAT_EXCLUSIVE(RenderAfterTranslucency);
		SCOPE_CYCLE_COUNTER(STAT_AfterTranslucencyDrawTime);
		RHI_BREADCRUMB_EVENT_STAT(RHICmdList, AfterTranslucency, "MobileAfterTranslucencyPass");
		SCOPED_GPU_STAT(RHICmdList, AfterTranslucency);

		SetStereoViewport(RHICmdList, View);
		Pass->Draw(RHICmdList, InstanceCullingDrawParams);
	}
}
```
> 本文件已使用 `SCOPED_GPU_STAT(RHICmdList, Basepass)`（:527），说明 BasePassRendering.h 已被包含，§18 新增的 GPU Stat 可直接使用。

---

## 14. Engine/Source/Runtime/Renderer/Private/SceneRendering.h

### 14.1 函数声明（:2869-2872 附近）

相关代码：
```c++
	/** Renders the opaque base pass for mobile. */
	void RenderMobileBasePass(FRHICommandList& RHICmdList, const FViewInfo& View, const FInstanceCullingDrawParams* InstanceCullingDrawParams, const FInstanceCullingDrawParams* SkyPassInstanceCullingDrawParams);

	void PostRenderBasePass(FRHICommandList& RHICmdList, FViewInfo& View);
```

`RenderMobileBasePass` 声明后插入：
```c++
	//RenderAfterTranslucency Added
	void RenderMobileAfterTranslucencyDepthPass(FRHICommandList& RHICmdList, const FViewInfo& View, const FInstanceCullingDrawParams* InstanceCullingDrawParams);
	void RenderMobileAfterTranslucencyPass(FRHICommandList& RHICmdList, const FViewInfo& View, const FInstanceCullingDrawParams* InstanceCullingDrawParams);
```

### 14.2 InstanceCulling 参数成员 —— **5.6 作废，不需要**

5.4 方案中给 `FMobileSceneRenderer` 添加的 `AfterTranslucencyDepthInstanceCullingDrawParams` / `AfterTranslucencyInstanceCullingDrawParams` 成员**在 5.6 不再需要**：5.6 已删除 `DepthPassInstanceCullingDrawParams` 等所有此类成员和 `BuildInstanceCullingDrawParams` 函数，改为 RDG 分配的局部 ParameterCollection 结构体（见 §15）。

---

## 15. Engine/Source/Runtime/Renderer/Private/MobileShadingRenderer.cpp【5.6 写法重写】

5.6 用静态辅助函数（:201）构建渲染命令，内部已判空：
```c++
static void BuildMeshRenderingCommands(FRDGBuilder& GraphBuilder, EMeshPass::Type MeshPass, FViewInfo& View, const FGPUScene& GPUScene, FInstanceCullingManager& InstanceCullingManager, FInstanceCullingDrawParams& OutParams)
```

### 15.1 RenderForwardSinglePass —— 参数结构体与命令构建（:1744-1767）

相关代码：
```c++
	struct FForwardSinglePassParameterCollection
	{
		FInstanceCullingDrawParams DepthPassInstanceCullingDrawParams;
		FInstanceCullingDrawParams SkyPassInstanceCullingDrawParams;
		FInstanceCullingDrawParams DebugViewModeInstanceCullingDrawParams;
		FInstanceCullingDrawParams MeshDecalSceneColorInstanceCullingDrawParams;
		FInstanceCullingDrawParams TranslucencyInstanceCullingDrawParams;
	};
	auto ParameterCollection = GraphBuilder.AllocParameters<FForwardSinglePassParameterCollection>();

	FViewInfo& View = *ViewContext.ViewInfo;

	if (Scene->GPUScene.IsEnabled())
	{
		if (!bIsFullDepthPrepassEnabled)
		{
			BuildMeshRenderingCommands(GraphBuilder, EMeshPass::DepthPass, View, Scene->GPUScene, InstanceCullingManager, ParameterCollection->DepthPassInstanceCullingDrawParams);
		}
		BuildMeshRenderingCommands(GraphBuilder, EMeshPass::BasePass, View, Scene->GPUScene, InstanceCullingManager, PassParameters->InstanceCullingDrawParams);
		BuildMeshRenderingCommands(GraphBuilder, EMeshPass::SkyPass, View, Scene->GPUScene, InstanceCullingManager, ParameterCollection->SkyPassInstanceCullingDrawParams);
		BuildMeshRenderingCommands(GraphBuilder, EMeshPass::DebugViewMode, View, Scene->GPUScene, InstanceCullingManager, ParameterCollection->DebugViewModeInstanceCullingDrawParams);
		BuildMeshRenderingCommands(GraphBuilder, EMeshPass::MeshDecal_SceneColor, View, Scene->GPUScene, InstanceCullingManager, ParameterCollection->MeshDecalSceneColorInstanceCullingDrawParams);
		BuildMeshRenderingCommands(GraphBuilder, StandardTranslucencyMeshPass, View, Scene->GPUScene, InstanceCullingManager, ParameterCollection->TranslucencyInstanceCullingDrawParams);
	}
```

修改为：
```c++
	struct FForwardSinglePassParameterCollection
	{
		FInstanceCullingDrawParams DepthPassInstanceCullingDrawParams;
		FInstanceCullingDrawParams SkyPassInstanceCullingDrawParams;
		FInstanceCullingDrawParams DebugViewModeInstanceCullingDrawParams;
		FInstanceCullingDrawParams MeshDecalSceneColorInstanceCullingDrawParams;
		FInstanceCullingDrawParams TranslucencyInstanceCullingDrawParams;
		//RenderAfterTranslucency Added
		FInstanceCullingDrawParams AfterTranslucencyDepthInstanceCullingDrawParams;
		FInstanceCullingDrawParams AfterTranslucencyInstanceCullingDrawParams;
	};
	...
	if (Scene->GPUScene.IsEnabled())
	{
		...（原有各行不动）
		BuildMeshRenderingCommands(GraphBuilder, StandardTranslucencyMeshPass, View, Scene->GPUScene, InstanceCullingManager, ParameterCollection->TranslucencyInstanceCullingDrawParams);
		//RenderAfterTranslucency Added: Full Prepass时深度已在Prepass写入且SceneColor Pass深度只读，不构建/不派发深度Pass
		if (!bIsFullDepthPrepassEnabled)
		{
			BuildMeshRenderingCommands(GraphBuilder, EMeshPass::MobileAfterTranslucencyDepthPass, View, Scene->GPUScene, InstanceCullingManager, ParameterCollection->AfterTranslucencyDepthInstanceCullingDrawParams);
		}
		BuildMeshRenderingCommands(GraphBuilder, EMeshPass::MobileAfterTranslucencyPass, View, Scene->GPUScene, InstanceCullingManager, ParameterCollection->AfterTranslucencyInstanceCullingDrawParams);
	}
```

### 15.2 RenderForwardSinglePass —— Lambda 内派发（:1796-1812）

**约束：深度 Pass 必须在 `RHICmdList.NextSubpass()`（:1804）之前（Subpass 0 深度可写；切换后为 DepthReadSubpass，见 :1775 `SubpassHint`）。颜色 Pass 必须在 `RenderTranslucency`（:1812）之后、`bTonemapSubpassInline` 的第二次 `NextSubpass()`（:1837）之前。**

相关代码：
```c++
		// Depth pre-pass
		RenderMaskedPrePass(RHICmdList, View, &ParameterCollection->DepthPassInstanceCullingDrawParams);
		// Opaque and masked
		RenderMobileBasePass(RHICmdList, View, &PassParameters->InstanceCullingDrawParams, &ParameterCollection->SkyPassInstanceCullingDrawParams);
		RenderMobileDebugView(RHICmdList, View, &ParameterCollection->DebugViewModeInstanceCullingDrawParams);

		PostRenderBasePass(RHICmdList, View);
		// scene depth is read only and can be fetched
		RHICmdList.NextSubpass();
		RenderDecals(RHICmdList, View, &ParameterCollection->MeshDecalSceneColorInstanceCullingDrawParams);
		RenderModulatedShadowProjections(RHICmdList, ViewContext.ViewIndex, View);
		if (GMaxRHIShaderPlatform != SP_METAL_SIM)
		{
			RenderFog(RHICmdList, View);
		}
		// Draw translucency.
		RenderTranslucency(RHICmdList, View, Views, StandardTranslucencyPass, StandardTranslucencyMeshPass, &ParameterCollection->TranslucencyInstanceCullingDrawParams);
```

修改为：
```c++
		// Depth pre-pass
		RenderMaskedPrePass(RHICmdList, View, &ParameterCollection->DepthPassInstanceCullingDrawParams);
		// Opaque and masked
		RenderMobileBasePass(RHICmdList, View, &PassParameters->InstanceCullingDrawParams, &ParameterCollection->SkyPassInstanceCullingDrawParams);
		//RenderAfterTranslucency Added: 必须位于NextSubpass()之前（Subpass 0深度可写）。
		// Full Prepass时深度只读且标记物体深度已在Prepass写入，跳过。
		if (!bIsFullDepthPrepassEnabled)
		{
			RenderMobileAfterTranslucencyDepthPass(RHICmdList, View, &ParameterCollection->AfterTranslucencyDepthInstanceCullingDrawParams);
		}
		RenderMobileDebugView(RHICmdList, View, &ParameterCollection->DebugViewModeInstanceCullingDrawParams);

		PostRenderBasePass(RHICmdList, View);
		// scene depth is read only and can be fetched
		RHICmdList.NextSubpass();
		RenderDecals(RHICmdList, View, &ParameterCollection->MeshDecalSceneColorInstanceCullingDrawParams);
		RenderModulatedShadowProjections(RHICmdList, ViewContext.ViewIndex, View);
		if (GMaxRHIShaderPlatform != SP_METAL_SIM)
		{
			RenderFog(RHICmdList, View);
		}
		// Draw translucency.
		RenderTranslucency(RHICmdList, View, Views, StandardTranslucencyPass, StandardTranslucencyMeshPass, &ParameterCollection->TranslucencyInstanceCullingDrawParams);
		//RenderAfterTranslucency Added: 半透明之后绘制标记物体颜色（只读深度，遮挡半透明）。
		// 必须位于bTonemapSubpassInline的第二次NextSubpass()之前。
		RenderMobileAfterTranslucencyPass(RHICmdList, View, &ParameterCollection->AfterTranslucencyInstanceCullingDrawParams);
```
> Lambda 已按值捕获 `ParameterCollection`（指针），无需改动捕获列表。

### 15.3 RenderForwardMultiPass —— 第一个 RenderPass（:1875-1916）

结构体（:1875-1880）追加深度参数：
```c++
	struct FForwardFirstPassParameterCollection
	{
		FInstanceCullingDrawParams DepthPassInstanceCullingDrawParams;
		FInstanceCullingDrawParams SkyPassInstanceCullingDrawParams;
		FInstanceCullingDrawParams DebugViewModeInstanceCullingDrawParams;
		//RenderAfterTranslucency Added
		FInstanceCullingDrawParams AfterTranslucencyDepthInstanceCullingDrawParams;
	};
```

命令构建（:1885-1894）追加：
```c++
	if (Scene->GPUScene.IsEnabled())
	{
		...（原有各行不动）
		//RenderAfterTranslucency Added
		if (!bIsFullDepthPrepassEnabled)
		{
			BuildMeshRenderingCommands(GraphBuilder, EMeshPass::MobileAfterTranslucencyDepthPass, View, Scene->GPUScene, InstanceCullingManager, ParameterCollection->AfterTranslucencyDepthInstanceCullingDrawParams);
		}
	}
```

Lambda 内派发（:1909-1915）：
```c++
		// Depth pre-pass
		RenderMaskedPrePass(RHICmdList, View, &ParameterCollection->DepthPassInstanceCullingDrawParams);
		// Opaque and masked
		RenderMobileBasePass(RHICmdList, View, &PassParameters->InstanceCullingDrawParams, &ParameterCollection->SkyPassInstanceCullingDrawParams);
		//RenderAfterTranslucency Added: 第一个RenderPass深度可写(!bIsFullDepthPrepassEnabled时)；
		// 深度必须在本Pass写入——第二个RenderPass深度绑定为只读(:1932 DepthRead)。
		if (!bIsFullDepthPrepassEnabled)
		{
			RenderMobileAfterTranslucencyDepthPass(RHICmdList, View, &ParameterCollection->AfterTranslucencyDepthInstanceCullingDrawParams);
		}
		RenderMobileDebugView(RHICmdList, View, &ParameterCollection->DebugViewModeInstanceCullingDrawParams);

		PostRenderBasePass(RHICmdList, View);
```

### 15.4 RenderForwardMultiPass —— 第二个 RenderPass（:1941-1981）

结构体（:1941-1945）追加颜色参数：
```c++
	struct FForwardSecondPassParameterCollection
	{
		FMobileRenderPassParameters PassParameters;
		FInstanceCullingDrawParams MeshDecalSceneColorInstanceCullingDrawParams;
		//RenderAfterTranslucency Added
		FInstanceCullingDrawParams AfterTranslucencyInstanceCullingDrawParams;
	};
```

命令构建（:1961-1965）追加：
```c++
	if (Scene->GPUScene.IsEnabled())
	{
		BuildMeshRenderingCommands(GraphBuilder, EMeshPass::MeshDecal_SceneColor, View, Scene->GPUScene, InstanceCullingManager, SecondParameterCollection->MeshDecalSceneColorInstanceCullingDrawParams);
		BuildMeshRenderingCommands(GraphBuilder, StandardTranslucencyMeshPass, View, Scene->GPUScene, InstanceCullingManager, SecondPassParameters->InstanceCullingDrawParams);
		//RenderAfterTranslucency Added
		BuildMeshRenderingCommands(GraphBuilder, EMeshPass::MobileAfterTranslucencyPass, View, Scene->GPUScene, InstanceCullingManager, SecondParameterCollection->AfterTranslucencyInstanceCullingDrawParams);
	}
```

Lambda 内派发（:1976-1981，`RenderTranslucency` 之后）：
```c++
		// Draw translucency.
		RenderTranslucency(RHICmdList, View, Views, StandardTranslucencyPass, StandardTranslucencyMeshPass, &SecondPassParameters->InstanceCullingDrawParams);
		//RenderAfterTranslucency Added: 只读深度，与本RenderPass的DepthRead绑定兼容
		RenderMobileAfterTranslucencyPass(RHICmdList, View, &SecondParameterCollection->AfterTranslucencyInstanceCullingDrawParams);
```

> 说明：Android Vulkan 走 SinglePass（`RequiresMultiPass`（:2513-2519）对 Vulkan 直接返回 false）；MultiPass 分支主要覆盖 GLES 等平台，两处都改保证完整性。
> 注意 MultiPass 第二 RenderPass 的深度绑定为 `DepthRead_StencilRead`（modulated shadows 时 StencilWrite，:1932-1937），颜色 Pass 的 `DepthRead_StencilRead` 声明与之兼容。

---

## 16. Engine/Source/Runtime/RenderCore/Public/RenderCore.h

### CPU 周期 Stat 声明（:28 之后）

相关代码（:27-29）：
```c++
DECLARE_CYCLE_STAT_EXTERN(TEXT("Depth drawing"),STAT_DepthDrawTime,STATGROUP_SceneRendering, RENDERCORE_API);
DECLARE_CYCLE_STAT_EXTERN(TEXT("Base pass drawing"),STAT_BasePassDrawTime,STATGROUP_SceneRendering, RENDERCORE_API);
DECLARE_CYCLE_STAT_EXTERN(TEXT("Anisotropy pass drawing"), STAT_AnisotropyPassDrawTime, STATGROUP_SceneRendering, RENDERCORE_API);
```

`STAT_BasePassDrawTime` 行后插入：
```c++
//RenderAfterTranslucency Added
DECLARE_CYCLE_STAT_EXTERN(TEXT("After translucency depth drawing"), STAT_AfterTranslucencyDepthDrawTime, STATGROUP_SceneRendering, RENDERCORE_API);
DECLARE_CYCLE_STAT_EXTERN(TEXT("After translucency drawing"), STAT_AfterTranslucencyDrawTime, STATGROUP_SceneRendering, RENDERCORE_API);
```

---

## 17. Engine/Source/Runtime/RenderCore/Private/RenderCore.cpp

### Stat 定义（:72 之后）

相关代码（:71-74）：
```c++
DEFINE_STAT(STAT_StaticDrawListDrawTime);
DEFINE_STAT(STAT_BasePassDrawTime);
DEFINE_STAT(STAT_AnisotropyPassDrawTime);
DEFINE_STAT(STAT_DepthDrawTime);
```

`STAT_BasePassDrawTime` 行后插入：
```c++
//RenderAfterTranslucency Added
DEFINE_STAT(STAT_AfterTranslucencyDepthDrawTime);
DEFINE_STAT(STAT_AfterTranslucencyDrawTime);
```

---

## 18. Engine/Source/Runtime/Renderer/Private/BasePassRendering.h

### GPU Stat 声明（:145 之后）

相关代码（:145-146）：
```c++
DECLARE_GPU_DRAWCALL_STAT_EXTERN(Basepass);
DECLARE_GPU_STAT_NAMED_EXTERN(NaniteBasePass, TEXT("Nanite BasePass"));
```

`Basepass` 行后插入：
```c++
//RenderAfterTranslucency Added
DECLARE_GPU_DRAWCALL_STAT_EXTERN(AfterTranslucencyDepth);
DECLARE_GPU_DRAWCALL_STAT_EXTERN(AfterTranslucency);
```

---

## 19. Engine/Source/Runtime/Renderer/Private/BasePassRendering.cpp

### GPU Stat 定义（:188 之后）——必须在 .cpp 中，放头文件会重复定义链接错误

相关代码（:188）：
```c++
DEFINE_GPU_DRAWCALL_STAT(Basepass);
```

其后插入：
```c++
//RenderAfterTranslucency Added
DEFINE_GPU_DRAWCALL_STAT(AfterTranslucencyDepth);
DEFINE_GPU_DRAWCALL_STAT(AfterTranslucency);
```

---

## 20. Shader（.usf/.ush）：无需任何修改

与 5.4 结论一致，已按 5.6 复核：

1. **颜色 Pass**：复用 `TMobileBasePassVSPolicyParamType` / `TMobileBasePassPSPolicyParamType`（经 `MobileBasePass::GetShaders` 选取，MobileBasePass.cpp:835-844；5.6 新增了 `EMobileLocalLightSetting` 参数，但 Process() 内部已处理，与 Pass 无关）。Shader 排列 Key 不含 EMeshPass，同一材质在 BasePass 编译的 Shader 直接复用。
2. **深度 Pass**：复用 DepthRendering 现成的 `TDepthOnlyVS<true/false>` / `FDepthOnlyPS`，`MobileDepthPass` 已注册（DepthRendering.cpp:1217）并服务于 Masked 预 Pass / Full Prepass，移动端已编译。
3. 半透明读取 SceneDepth（DepthFetch/DepthAux）机制未改变。

---

## 21. 使用方法与运行前提（同 5.4）

- 组件上勾选 `Render Opaque After Translucency (Mobile)`（或蓝图调用 `SetRenderAfterTranslucency(true)`），仅对不透明/Masked 材质生效；物体上的半透明材质 Section 仍走正常半透明流程。
- 生效条件：移动端 Forward（`r.Mobile.ShadingPath=0`）。Mobile Deferred 下自动回落 BasePass 正常渲染。
- `r.EarlyZPass` 任意配置均可运行：
  - 非 Full Prepass：深度由 MobileAfterTranslucencyDepthPass 在 BasePass 之后写入；
  - Full Prepass（DDM_AllOpaque **或 5.6 新增的 DDM_AllOpaqueNoVelocity**）：深度由 Prepass 写入，本方案自动跳过深度 Pass。

## 22. 已知行为/限制（5.4 版 7 条全部仍然成立，另加 1 条 5.6 新增）

1. 贴花/雾/调制阴影在 Subpass 1 开头执行，颜色 Pass 在其后绘制会覆盖——标记物体不接受屏幕空间贴花与半透明前的雾效。
2. 标记物体身后的不透明物体正常绘制（一次额外 overdraw），最终被颜色 Pass 覆盖，视觉正确。
3. PSO 预热对两个新 Pass 关闭（§11.2 / §12.2），首次绘制可能有一次 PSO 编译卡顿。
4. 阴影投射（CSM）不受影响。
5. VR Instanced Stereo / MultiView 自动支持：新 Pass 注册为 MainView，`FSceneRenderer::SetupMeshPass`（SceneRendering.cpp:4606-4663）对 MainView Pass 统一走 Stereo Instance Culling；渲染函数使用 `SetStereoViewport`。
6. SceneDepthAux 不含标记物体深度：半透明材质靠采样场景深度的效果（粒子 DepthFade、软粒子、水深淡出）在标记物体表面附近不精确。VR 项目粒子大量用 Depth Fade 需留意。
7. Masked 材质 + `r.EarlyZPassOnlyMaterialMasking=1` 组合下镂空像素可能被错误填充（移动端默认关）。
8. **【5.6 新增】Custom Render Pass**（`FCustomRenderPassBase`，SceneCapture 的 DepthPass/DepthAndBasePass 模式，MobileShadingRenderer.cpp:1368-1378、RenderCustomRenderPassBasePass）只派发 DepthPass/BasePass，不派发新 Pass：标记物体在这类捕获输出中**深度正常**（仍在常规 DepthPass 中）但**颜色缺失**（被 BasePass 分流剔除）。常规 SceneCapture（走完整 FMobileSceneRenderer::RenderForward）不受影响。若项目使用 DepthAndBasePass 型捕获且需要标记物体颜色，需在 `RenderCustomRenderPassBasePass` 中额外派发 MobileAfterTranslucencyPass（或对捕获视图禁用分流）。

## 23. 验证清单（同 5.4，追加 2 项）

- [ ] Android Vulkan（SinglePass + MultiView）：标记物体遮挡半透明、被不透明遮挡、MSAA 开/关。
- [ ] `r.EarlyZPass` 各配置（0 / MaskedOnly / AllOpaque / **AllOpaqueNoVelocity**）下均无 RHI 校验报错且效果一致。
- [ ] 标记物体使用 Masked 材质（验证 FDepthOnlyPS clip 路径）。
- [ ] 标记 SkeletalMesh（动态路径 + GPUSkin DepthOnly VS）。
- [ ] 运行时切换 `SetRenderAfterTranslucency`（验证 MarkRenderStateDirty 后缓存命令重建）。
- [ ] 含半透明 Section 的标记物体：半透明部分正常绘制（验证 §11.1 的 `!bTranslucentBasePass` 分支）。
- [ ] `stat scenerendering` 可见 "After translucency drawing"；`stat gpu` 可见 AfterTranslucency/AfterTranslucencyDepth；RenderDoc 中确认 Pass 顺序：MobileBasePass → MobileAfterTranslucencyDepthPass → (Decals/Fog) → Translucency → MobileAfterTranslucencyPass。
- [ ] 编辑器 Mobile Preview（ES3.1）不崩溃、效果一致。
- [ ] **【5.6 新增】场景中无任何标记物体时**：两个新 Pass 指针为 nullptr，无绘制事件、无崩溃（验证懒创建判空路径）。
- [ ] **【5.6 新增】若使用 SceneCapture 的 Custom Render Pass（DepthAndBasePass）**：确认标记物体在捕获结果中的表现符合预期（见 §22.8）。
