# RenderAfterTranslucency 完整修改方案（最终版）

功能：标记的 StaticMesh/SkeletalMesh 在不透明阶段只写深度（走深度专用 Shader，MobileAfterTranslucencyDepthPass），半透明渲染完成后再绘制颜色（MobileAfterTranslucencyPass），从而遮挡半透明物体。仅移动端 Forward 路径。

本方案已合并 `Plan_Analysis.md` 中的全部修复：
- 深度 Pass 改用 `FDepthPassMeshProcessor`（DepthOnly VS + 空/极简 PS，解决性能问题，同时天然规避 `Process()` 状态覆盖问题）；
- 颜色 Pass 加 `ForcePassDrawRenderState`；
- AddMeshBatch 分流不误伤半透明 Pass；
- `bIsFullDepthPrepassEnabled`（EarlyZ = DDM_AllOpaque）时跳过深度 Pass（深度已由 Full Prepass 写入，且此时深度附件只读）；
- 移动端延迟渲染路径保护（标记物体回落 BasePass 正常渲染）；
- 静态/动态路径均不再给标记物体生成 MobileBasePassCSM 命令；
- GPU Stat 的 DEFINE 放在 .cpp。

> 行号均为**修改前**的当前源码行号，插入代码后后续行号会顺移。所有新增代码以 `//RenderAfterTranslucency Added` / `Changed` 标注。

---

## 1. Engine/Source/Runtime/Renderer/Public/MeshPassProcessor.h

### 1.1 枚举添加两个 Pass（:66-67 附近）

相关代码（:63-78）：
```c++
		DitheredLODFadingOutMaskPass, /** A mini depth pass used to mark pixels with dithered LOD fading out. Currently only used by ray tracing shadows. */
		NaniteMeshPass,
		MeshDecal,
		WaterInfoTextureDepthPass,
		WaterInfoTexturePass,

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
		DitheredLODFadingOutMaskPass, /** A mini depth pass used to mark pixels with dithered LOD fading out. Currently only used by ray tracing shadows. */
		NaniteMeshPass,
		MeshDecal,
		WaterInfoTextureDepthPass,
		WaterInfoTexturePass,
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
> 说明：新增后 Num = 34（非编辑器）/ 38（编辑器），`NumBits = 6`（上限 64）无需改动，:80 的 `static_assert(EMeshPass::Num <= (1 << EMeshPass::NumBits))` 自动通过。

### 1.2 GetMeshPassName 添加 case 并更新断言（:117-131）

相关代码：
```c++
	case EMeshPass::WaterInfoTextureDepthPass: return TEXT("WaterInfoTextureDepthPass");
	case EMeshPass::WaterInfoTexturePass: return TEXT("WaterInfoTexturePass");
#if WITH_EDITOR
	case EMeshPass::HitProxy: return TEXT("HitProxy");
	case EMeshPass::HitProxyOpaqueOnly: return TEXT("HitProxyOpaqueOnly");
	case EMeshPass::EditorLevelInstance: return TEXT("EditorLevelInstance");
	case EMeshPass::EditorSelection: return TEXT("EditorSelection");
#endif
	}

#if WITH_EDITOR
	static_assert(EMeshPass::Num == 32 + 4, "Need to update switch(MeshPass) after changing EMeshPass"); // GUID to prevent incorrect auto-resolves, please change when changing the expression: {674D7D62-CFD8-4971-9A8D-CD91E5612CD8}
#else
	static_assert(EMeshPass::Num == 32, "Need to update switch(MeshPass) after changing EMeshPass"); // GUID to prevent incorrect auto-resolves, please change when changing the expression: {674D7D62-CFD8-4971-9A8D-CD91E5612CD8}
#endif
```

修改为：
```c++
	case EMeshPass::WaterInfoTextureDepthPass: return TEXT("WaterInfoTextureDepthPass");
	case EMeshPass::WaterInfoTexturePass: return TEXT("WaterInfoTexturePass");
	case EMeshPass::MobileAfterTranslucencyDepthPass: return TEXT("MobileAfterTranslucencyDepthPass");	//RenderAfterTranslucency Added
	case EMeshPass::MobileAfterTranslucencyPass: return TEXT("MobileAfterTranslucencyPass");			//RenderAfterTranslucency Added
#if WITH_EDITOR
	case EMeshPass::HitProxy: return TEXT("HitProxy");
	case EMeshPass::HitProxyOpaqueOnly: return TEXT("HitProxyOpaqueOnly");
	case EMeshPass::EditorLevelInstance: return TEXT("EditorLevelInstance");
	case EMeshPass::EditorSelection: return TEXT("EditorSelection");
#endif
	}

#if WITH_EDITOR
	static_assert(EMeshPass::Num == 34 + 4, "Need to update switch(MeshPass) after changing EMeshPass"); //RenderAfterTranslucency Changed
#else
	static_assert(EMeshPass::Num == 34, "Need to update switch(MeshPass) after changing EMeshPass"); //RenderAfterTranslucency Changed
#endif
```

---

## 2. Engine/Source/Runtime/Engine/Classes/Components/PrimitiveComponent.h

### 2.1 添加组件属性（:404-411）

相关代码：
```c++
	uint8 bVisibleInRayTracing : 1;

	/** If true, this component will be rendered in the main pass (z prepass, basepass, transparency) */
	UPROPERTY(EditAnywhere, AdvancedDisplay, BlueprintReadOnly, Category = Rendering)
	uint8 bRenderInMainPass:1;

	/** If true, this component will be rendered in the depth pass even if it's not rendered in the main pass */
	UPROPERTY(EditAnywhere, AdvancedDisplay, BlueprintReadOnly, Category = Rendering, meta = (EditCondition = "!bRenderInMainPass"))
```

修改为：
```c++
	uint8 bVisibleInRayTracing : 1;

	/** If true, this component will be rendered in the main pass (z prepass, basepass, transparency) */
	UPROPERTY(EditAnywhere, AdvancedDisplay, BlueprintReadOnly, Category = Rendering)
	uint8 bRenderInMainPass:1;

	//RenderAfterTranslucency Added: 移动端Forward下，不透明阶段只写深度，半透明之后再绘制颜色
	UPROPERTY(EditAnywhere, AdvancedDisplay, BlueprintReadOnly, Category = Rendering, meta = (DisplayName = "Render Opaque After Translucency (Mobile)", EditCondition = "bRenderInMainPass"))
	uint8 bRenderAfterTranslucency : 1;

	/** If true, this component will be rendered in the depth pass even if it's not rendered in the main pass */
	UPROPERTY(EditAnywhere, AdvancedDisplay, BlueprintReadOnly, Category = Rendering, meta = (EditCondition = "!bRenderInMainPass"))
```

### 2.2 添加 Setter 声明（:1916-1918 附近）

相关代码：
```c++
	/** Sets bRenderInMainPass property and marks the render state dirty. */
	UFUNCTION(BlueprintCallable, Category = "Rendering")
	ENGINE_API void SetRenderInMainPass(bool bValue);
```

修改为：
```c++
	/** Sets bRenderInMainPass property and marks the render state dirty. */
	UFUNCTION(BlueprintCallable, Category = "Rendering")
	ENGINE_API void SetRenderInMainPass(bool bValue);

	//RenderAfterTranslucency Added
	/** Sets bRenderAfterTranslucency property and marks the render state dirty. */
	UFUNCTION(BlueprintCallable, Category = "Rendering")
	ENGINE_API void SetRenderAfterTranslucency(bool bValue);
```

---

## 3. Engine/Source/Runtime/Engine/Private/Components/PrimitiveComponent.cpp

### 3.1 构造函数默认值（:333）

相关代码（:330-335）：
```c++
	bVisibleInReflectionCaptures = true;
	bVisibleInRealTimeSkyCaptures = true;
	bVisibleInRayTracing = true;
	bRenderInMainPass = true;
	bRenderInDepthPass = true;
	VisibilityId = INDEX_NONE;
```

修改为：
```c++
	bVisibleInReflectionCaptures = true;
	bVisibleInRealTimeSkyCaptures = true;
	bVisibleInRayTracing = true;
	bRenderInMainPass = true;
	bRenderAfterTranslucency = false;	//RenderAfterTranslucency Added
	bRenderInDepthPass = true;
	VisibilityId = INDEX_NONE;
```

### 3.2 Setter 实现（:4457-4464 之后）

相关代码：
```c++
void UPrimitiveComponent::SetRenderInMainPass(bool bValue)
{
	if (bRenderInMainPass != bValue)
	{
		bRenderInMainPass = bValue;
		MarkRenderStateDirty();
	}
}

void UPrimitiveComponent::SetRenderInDepthPass(bool bValue)
```

修改为：
```c++
void UPrimitiveComponent::SetRenderInMainPass(bool bValue)
{
	if (bRenderInMainPass != bValue)
	{
		bRenderInMainPass = bValue;
		MarkRenderStateDirty();
	}
}

//RenderAfterTranslucency Added
void UPrimitiveComponent::SetRenderAfterTranslucency(bool bValue)
{
	if (bRenderAfterTranslucency != bValue)
	{
		bRenderAfterTranslucency = bValue;
		MarkRenderStateDirty();
	}
}

void UPrimitiveComponent::SetRenderInDepthPass(bool bValue)
```

---

## 4. Engine/Source/Runtime/Engine/Public/PrimitiveSceneProxy.h

### 4.1 Proxy 字段（:1197-1200）

相关代码：
```c++
	uint8 bRenderInDepthPass : 1;

	/** If true this primitive Renders in the mainPass */
	uint8 bRenderInMainPass : 1;
```

修改为：
```c++
	uint8 bRenderInDepthPass : 1;

	/** If true this primitive Renders in the mainPass */
	uint8 bRenderInMainPass : 1;

	//RenderAfterTranslucency Added（注意：位置紧跟 bRenderInMainPass，与5.2初始化列表顺序保持一致，避免 -Wreorder）
	uint8 bRenderAfterTranslucency : 1;
```

### 4.2 Getter（:700-701）

相关代码：
```c++
	inline bool ShouldRenderInMainPass() const { return bRenderInMainPass; }
	inline bool ShouldRenderInDepthPass() const { return bRenderInMainPass || bRenderInDepthPass; }
```

修改为：
```c++
	inline bool ShouldRenderInMainPass() const { return bRenderInMainPass; }
	inline bool ShouldRenderAfterTranslucency() const { return bRenderAfterTranslucency; }	//RenderAfterTranslucency Added
	inline bool ShouldRenderInDepthPass() const { return bRenderInMainPass || bRenderInDepthPass; }
```

---

## 5. Engine/Source/Runtime/Engine/Private/PrimitiveSceneProxy.cpp

### 5.1 Component → Desc 拷贝（:277，注意此函数是 `FPrimitiveSceneProxyDesc::InitializeFrom`，Component 构造路径经 :393-396 统一委托到 Desc 路径）

相关代码（:265-278）：
```c++
void FPrimitiveSceneProxyDesc::InitializeFrom(const UPrimitiveComponent* InComponent)
{
	...
	bRenderInDepthPass = InComponent->bRenderInDepthPass;
	bRenderInMainPass = InComponent->bRenderInMainPass;
	bTreatAsBackgroundForOcclusion = InComponent->bTreatAsBackgroundForOcclusion;
```

修改为：
```c++
void FPrimitiveSceneProxyDesc::InitializeFrom(const UPrimitiveComponent* InComponent)
{
	...
	bRenderInDepthPass = InComponent->bRenderInDepthPass;
	bRenderInMainPass = InComponent->bRenderInMainPass;
	bRenderAfterTranslucency = InComponent->bRenderAfterTranslucency;	//RenderAfterTranslucency Added
	bTreatAsBackgroundForOcclusion = InComponent->bTreatAsBackgroundForOcclusion;
```

### 5.2 Desc → Proxy 初始化列表（:427-429）

相关代码：
```c++
,	bRenderInDepthPass(InProxyDesc.bRenderInDepthPass)
,	bRenderInMainPass(InProxyDesc.bRenderInMainPass)
,	bForceHidden(false)
```

修改为：
```c++
,	bRenderInDepthPass(InProxyDesc.bRenderInDepthPass)
,	bRenderInMainPass(InProxyDesc.bRenderInMainPass)
,	bRenderAfterTranslucency(InProxyDesc.bRenderAfterTranslucency)	//RenderAfterTranslucency Added
,	bForceHidden(false)
```

---

## 6. Engine/Source/Runtime/Engine/Public/PrimitiveSceneProxyDesc.h

### 6.1 默认值（:24-25）

相关代码：
```c++
		bRenderInDepthPass = true;
		bRenderInMainPass = true;
		bTreatAsBackgroundForOcclusion = false;
```

修改为：
```c++
		bRenderInDepthPass = true;
		bRenderInMainPass = true;
		bRenderAfterTranslucency = false;	//RenderAfterTranslucency Added
		bTreatAsBackgroundForOcclusion = false;
```

### 6.2 字段（:92-94）

相关代码：
```c++
	uint32 bRenderInDepthPass : 1;
	uint32 bRenderInMainPass : 1;
	uint32 bTreatAsBackgroundForOcclusion : 1;
```

修改为：
```c++
	uint32 bRenderInDepthPass : 1;
	uint32 bRenderInMainPass : 1;
	uint32 bRenderAfterTranslucency : 1;	//RenderAfterTranslucency Added
	uint32 bTreatAsBackgroundForOcclusion : 1;
```

> 说明：`FStaticMeshSceneProxyDesc::InitializeFrom`（StaticMeshRender.cpp:2608-2610）调用基类版本，静态网格 Desc 路径自动生效，无需额外修改。

---

## 7. Engine/Source/Runtime/Engine/Public/PrimitiveViewRelevance.h

### 7.1 字段（:53-54）

相关代码：
```c++
	/** The primitive should render to the base pass / normal depth / velocity rendering. */
	uint32 bRenderInMainPass : 1;
	/** The primitive is drawn only in the editor and composited onto the scene after post processing */
	uint32 bEditorPrimitiveRelevance : 1;
```

修改为：
```c++
	/** The primitive should render to the base pass / normal depth / velocity rendering. */
	uint32 bRenderInMainPass : 1;
	/** RenderAfterTranslucency Added: mobile forward, opaque color rendered after translucency */
	uint32 bRenderAfterTranslucency : 1;
	/** The primitive is drawn only in the editor and composited onto the scene after post processing */
	uint32 bEditorPrimitiveRelevance : 1;
```

> 说明：构造函数（:90-104）已将整个结构体按字节清零（`for(...) *p++ = 0;`），`bRenderAfterTranslucency` 默认即为 false，**不需要**在构造函数中显式赋值（写上也无害）。

---

## 8. Engine/Source/Runtime/Engine/Private/StaticMeshRender.cpp

### GetViewRelevance（:2062）

相关代码（:2059-2064）：
```c++
	FPrimitiveViewRelevance Result;
	Result.bDrawRelevance = IsShown(View) && View->Family->EngineShowFlags.StaticMeshes;
	Result.bRenderCustomDepth = ShouldRenderCustomDepth();
	Result.bRenderInMainPass = ShouldRenderInMainPass();
	Result.bRenderInDepthPass = ShouldRenderInDepthPass();
	Result.bUsesLightingChannels = GetLightingChannelMask() != GetDefaultLightingChannelMask();
```

修改为：
```c++
	FPrimitiveViewRelevance Result;
	Result.bDrawRelevance = IsShown(View) && View->Family->EngineShowFlags.StaticMeshes;
	Result.bRenderCustomDepth = ShouldRenderCustomDepth();
	Result.bRenderInMainPass = ShouldRenderInMainPass();
	Result.bRenderAfterTranslucency = ShouldRenderAfterTranslucency();	//RenderAfterTranslucency Added
	Result.bRenderInDepthPass = ShouldRenderInDepthPass();
	Result.bUsesLightingChannels = GetLightingChannelMask() != GetDefaultLightingChannelMask();
```

---

## 9. Engine/Source/Runtime/Engine/Private/SkeletalMesh.cpp

### GetViewRelevance（:7115）

相关代码（:7109-7117）：
```c++
	FPrimitiveViewRelevance Result;
	Result.bDrawRelevance = IsShown(View) && View->Family->EngineShowFlags.SkeletalMeshes;
	Result.bShadowRelevance = IsShadowCast(View);
	Result.bStaticRelevance = bRenderStatic && !IsRichView(*View->Family);
	Result.bDynamicRelevance = !Result.bStaticRelevance;
	Result.bRenderCustomDepth = ShouldRenderCustomDepth();
	Result.bRenderInMainPass = ShouldRenderInMainPass();
	Result.bRenderInDepthPass = ShouldRenderInDepthPass();
```

修改为：
```c++
	FPrimitiveViewRelevance Result;
	Result.bDrawRelevance = IsShown(View) && View->Family->EngineShowFlags.SkeletalMeshes;
	Result.bShadowRelevance = IsShadowCast(View);
	Result.bStaticRelevance = bRenderStatic && !IsRichView(*View->Family);
	Result.bDynamicRelevance = !Result.bStaticRelevance;
	Result.bRenderCustomDepth = ShouldRenderCustomDepth();
	Result.bRenderInMainPass = ShouldRenderInMainPass();
	Result.bRenderAfterTranslucency = ShouldRenderAfterTranslucency();	//RenderAfterTranslucency Added
	Result.bRenderInDepthPass = ShouldRenderInDepthPass();
```

---

## 10. Engine/Source/Runtime/Renderer/Private/SceneVisibility.cpp

### 10.1 静态网格路径（:1556-1577）

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

修改为：
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
										//RenderAfterTranslucency Added: 仅移动端Forward路径分流；Mobile Deferred没有dispatch新Pass，必须回落BasePass
										const bool bMobileRenderAfterTranslucency = ViewRelevance.bRenderAfterTranslucency
											&& !IsMobileDeferredShadingEnabled(Scene->GetShaderPlatform());
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
									else
									{
										DrawCommandPacket.AddCommandsForMesh(PrimitiveIndex, PrimitiveSceneInfo, StaticMeshRelevance, StaticMesh, CullingPayloadFlags, Scene, bCanCache, EMeshPass::SkyPass);
									}
									// bUseSingleLayerWaterMaterial is added to BasePass on Mobile. No need to add it to SingleLayerWaterPass

									MarkMask |= EMarkMaskBits::StaticMeshVisibilityMapMask;
								}
```
> `IsMobileDeferredShadingEnabled` 声明于 `RenderUtils.h:284`（RenderCore 公共头，Renderer 模块可直接使用；若报未声明则在文件顶部补 `#include "RenderUtils.h"`）。
> 深度命令入队处（:1530-1542，`EMeshPass::DepthPass`）**不做修改**：标记物体保持进入常规 DepthPass，这样 Masked-Only Prepass / Full Prepass 配置下其深度仍在正确时机写入。

### 10.2 动态网格路径 ComputeDynamicMeshRelevance（:2211-2232）

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

---

## 11. Engine/Source/Runtime/Renderer/Private/MobileBasePass.cpp（颜色 Pass）

### 11.1 AddMeshBatch 分流（:867-874）

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

修改为：
```c++
void FMobileBasePassMeshProcessor::AddMeshBatch(const FMeshBatch& RESTRICT MeshBatch, uint64 BatchElementMask, const FPrimitiveSceneProxy* RESTRICT PrimitiveSceneProxy, int32 StaticMeshId)
{
	if (!MeshBatch.bUseForMaterial || 
		(Flags & FMobileBasePassMeshProcessor::EFlags::DoNotCache) == FMobileBasePassMeshProcessor::EFlags::DoNotCache ||
		(PrimitiveSceneProxy && !PrimitiveSceneProxy->ShouldRenderInMainPass()))
	{
		return;
	}

	//RenderAfterTranslucency Added: Pass分流。
	// bDeferredShading时禁用分流（延迟路径没有dispatch新Pass，标记物体保持走BasePass）。
	// else分支必须限定!bTranslucentBasePass：本Processor同时服务TranslucencyStandard/AfterDOF/All
	//（见本文件:1220-1222注册），不能把标记物体上的半透明材质Section从半透明Pass中剔除。
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

	const FMaterialRenderProxy* MaterialRenderProxy = MeshBatch.MaterialRenderProxy;
```
> `bDeferredShading` / `bTranslucentBasePass` 为本类现成成员（构造于 :822-823）。

### 11.2 CollectPSOInitializers 跳过新 Pass（:1056-1062）

相关代码：
```c++
void FMobileBasePassMeshProcessor::CollectPSOInitializers(const FSceneTexturesConfig& SceneTexturesConfig, const FMaterial& Material, const FPSOPrecacheVertexFactoryData& VertexFactoryData, const FPSOPrecacheParams& PreCacheParams, TArray<FPSOPrecacheData>& PSOInitializers)
{
	static IConsoleVariable* PSOPrecacheTranslucencyAllPass = IConsoleManager::Get().FindConsoleVariable(TEXT("r.PSOPrecache.TranslucencyAllPass"));
	// PSO precaching enabled for TranslucencyAll
	if (MeshPassType == EMeshPass::TranslucencyAll && PSOPrecacheTranslucencyAllPass->GetInt() == 0)
	{
		return;
	}
```

修改为：
```c++
void FMobileBasePassMeshProcessor::CollectPSOInitializers(const FSceneTexturesConfig& SceneTexturesConfig, const FMaterial& Material, const FPSOPrecacheVertexFactoryData& VertexFactoryData, const FPSOPrecacheParams& PreCacheParams, TArray<FPSOPrecacheData>& PSOInitializers)
{
	//RenderAfterTranslucency Added: 暂不为该Pass做PSO预热（SetupPrecachePSOParams不感知标记，收集到的状态也不匹配）。
	// 代价：标记物体首次绘制时运行时编译PSO，可能有一次卡顿；如需消除，可参照本函数按颜色Pass实际RenderState收集。
	if (MeshPassType == EMeshPass::MobileAfterTranslucencyPass)
	{
		return;
	}
	static IConsoleVariable* PSOPrecacheTranslucencyAllPass = IConsoleManager::Get().FindConsoleVariable(TEXT("r.PSOPrecache.TranslucencyAllPass"));
	// PSO precaching enabled for TranslucencyAll
	if (MeshPassType == EMeshPass::TranslucencyAll && PSOPrecacheTranslucencyAllPass->GetInt() == 0)
	{
		return;
	}
```

### 11.3 添加颜色 Pass 的 Create 函数（:1216 之后、:1218 注册表之前）

相关代码（:1207-1218）：
```c++
FMeshPassProcessor* CreateMobileTranslucencyAllPassProcessor(ERHIFeatureLevel::Type FeatureLevel, const FScene* Scene, const FSceneView* InViewIfDynamicMeshCommand, FMeshPassDrawListContext* InDrawListContext)
{
	FMeshPassProcessorRenderState PassDrawRenderState;
	PassDrawRenderState.SetDepthStencilState(TStaticDepthStencilState<false, CF_DepthNearOrEqual>::GetRHI());
	PassDrawRenderState.SetDepthStencilAccess(FExclusiveDepthStencil::DepthRead_StencilRead);

	const FMobileBasePassMeshProcessor::EFlags Flags = FMobileBasePassMeshProcessor::EFlags::CanUseDepthStencil;

	return new FMobileBasePassMeshProcessor(EMeshPass::TranslucencyAll, Scene, InViewIfDynamicMeshCommand, PassDrawRenderState, InDrawListContext, Flags, ETranslucencyPass::TPT_AllTranslucency);
}

REGISTER_MESHPASSPROCESSOR_AND_PSOCOLLECTOR(MobileBasePass, 			CreateMobileBasePassProcessor, 			EShadingPath::Mobile, EMeshPass::BasePass, 		EMeshPassFlags::CachedMeshCommands | EMeshPassFlags::MainView);
```

修改为：
```c++
FMeshPassProcessor* CreateMobileTranslucencyAllPassProcessor(ERHIFeatureLevel::Type FeatureLevel, const FScene* Scene, const FSceneView* InViewIfDynamicMeshCommand, FMeshPassDrawListContext* InDrawListContext)
{
	FMeshPassProcessorRenderState PassDrawRenderState;
	PassDrawRenderState.SetDepthStencilState(TStaticDepthStencilState<false, CF_DepthNearOrEqual>::GetRHI());
	PassDrawRenderState.SetDepthStencilAccess(FExclusiveDepthStencil::DepthRead_StencilRead);

	const FMobileBasePassMeshProcessor::EFlags Flags = FMobileBasePassMeshProcessor::EFlags::CanUseDepthStencil;

	return new FMobileBasePassMeshProcessor(EMeshPass::TranslucencyAll, Scene, InViewIfDynamicMeshCommand, PassDrawRenderState, InDrawListContext, Flags, ETranslucencyPass::TPT_AllTranslucency);
}

//RenderAfterTranslucency Added: 半透明之后的不透明颜色Pass。
// 深度已由MobileAfterTranslucencyDepthPass(或Full Prepass)写入，此处只读深度、CF_DepthNearOrEqual测试。
// 必须带ForcePassDrawRenderState：否则Process()会走SetOpaqueRenderState/CF_Equal覆盖(本文件:945-961)，
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

REGISTER_MESHPASSPROCESSOR_AND_PSOCOLLECTOR(MobileBasePass, 			CreateMobileBasePassProcessor, 			EShadingPath::Mobile, EMeshPass::BasePass, 		EMeshPassFlags::CachedMeshCommands | EMeshPassFlags::MainView);
```

### 11.4 注册（:1222 之后）

相关代码（:1222-1224）：
```c++
REGISTER_MESHPASSPROCESSOR_AND_PSOCOLLECTOR(MobileTranslucencyAfterDOFPass,	CreateMobileTranslucencyAfterDOFProcessor,	EShadingPath::Mobile, EMeshPass::TranslucencyAfterDOF, 	EMeshPassFlags::MainView);
// Skipping EMeshPass::TranslucencyAfterDOFModulate because dual blending is not supported on mobile
```

修改为：
```c++
REGISTER_MESHPASSPROCESSOR_AND_PSOCOLLECTOR(MobileTranslucencyAfterDOFPass,	CreateMobileTranslucencyAfterDOFProcessor,	EShadingPath::Mobile, EMeshPass::TranslucencyAfterDOF, 	EMeshPassFlags::MainView);
//RenderAfterTranslucency Added
REGISTER_MESHPASSPROCESSOR_AND_PSOCOLLECTOR(MobileAfterTranslucencyPass,	CreateMobileAfterTranslucencyPassProcessor,	EShadingPath::Mobile, EMeshPass::MobileAfterTranslucencyPass,	EMeshPassFlags::CachedMeshCommands | EMeshPassFlags::MainView);
// Skipping EMeshPass::TranslucencyAfterDOFModulate because dual blending is not supported on mobile
```

---

## 12. Engine/Source/Runtime/Renderer/Private/DepthRendering.cpp（深度 Pass）

深度 Pass 使用 `FDepthPassMeshProcessor` 而非 `FMobileBasePassMeshProcessor`：不透明、无 WPO、支持 PositionOnly 流的材质自动替换为默认材质 + 仅位置 VS + **空 PS**（`ShouldRender`，:939-972；Shader 见 :158-165），骨骼网格走完整 VF 的 DepthOnly VS。这正是"绑定移动端 DepthPass Shader"的诉求，且该 Processor 没有 `SetOpaqueRenderState` 那套状态覆盖逻辑。

### 12.1 AddMeshBatch 分流（:1021-1023）

相关代码：
```c++
void FDepthPassMeshProcessor::AddMeshBatch(const FMeshBatch& RESTRICT MeshBatch, uint64 BatchElementMask, const FPrimitiveSceneProxy* RESTRICT PrimitiveSceneProxy, int32 StaticMeshId)
{
	bool bDraw = MeshBatch.bUseForDepthPass;
	
	// Filter by occluder flags and settings if required.
	if (bDraw && bRespectUseAsOccluderFlag && !MeshBatch.bUseAsOccluder && EarlyZPassMode < DDM_AllOpaque)
```

修改为：
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
	
	// Filter by occluder flags and settings if required.
	if (bDraw && bRespectUseAsOccluderFlag && !MeshBatch.bUseAsOccluder && EarlyZPassMode < DDM_AllOpaque)
```

### 12.2 CollectPSOInitializers 跳过新 Pass（:1096-1103）

相关代码：
```c++
void FDepthPassMeshProcessor::CollectPSOInitializers(const FSceneTexturesConfig& SceneTexturesConfig, const FMaterial& Material, const FPSOPrecacheVertexFactoryData& VertexFactoryData, const FPSOPrecacheParams& PreCacheParams, TArray<FPSOPrecacheData>& PSOInitializers)
{		
	// Are we currently collecting PSO's for the default material
	if (PreCacheParams.bDefaultMaterial)
	{
		CollectDefaultMaterialPSOInitializers(SceneTexturesConfig, Material, VertexFactoryData, PSOInitializers);
		return;
	}
```

修改为：
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
	{
		CollectDefaultMaterialPSOInitializers(SceneTexturesConfig, Material, VertexFactoryData, PSOInitializers);
		return;
	}
```

### 12.3 Create 函数与注册（:1243 之后）

相关代码（:1230-1244）：
```c++
FMeshPassProcessor* CreateDepthPassProcessor(ERHIFeatureLevel::Type FeatureLevel, const FScene* Scene, const FSceneView* InViewIfDynamicMeshCommand, FMeshPassDrawListContext* InDrawListContext)
{
	EDepthDrawingMode EarlyZPassMode;
	bool bEarlyZPassMovable;
	FScene::GetEarlyZPassMode(FeatureLevel, EarlyZPassMode, bEarlyZPassMovable);

	FMeshPassProcessorRenderState DepthPassState;
	SetupDepthPassState(DepthPassState);
		
	return new FDepthPassMeshProcessor(EMeshPass::DepthPass, Scene, FeatureLevel, InViewIfDynamicMeshCommand, DepthPassState, true, EarlyZPassMode, bEarlyZPassMovable, false, InDrawListContext);
}

REGISTER_MESHPASSPROCESSOR_AND_PSOCOLLECTOR(DepthPass, CreateDepthPassProcessor, EShadingPath::Deferred, EMeshPass::DepthPass, EMeshPassFlags::CachedMeshCommands | EMeshPassFlags::MainView);
REGISTER_MESHPASSPROCESSOR_AND_PSOCOLLECTOR(MobileDepthPass, CreateDepthPassProcessor, EShadingPath::Mobile, EMeshPass::DepthPass, EMeshPassFlags::CachedMeshCommands | EMeshPassFlags::MainView);
```

修改为（在 :1243 的 MobileDepthPass 注册行之后追加）：
```c++
REGISTER_MESHPASSPROCESSOR_AND_PSOCOLLECTOR(DepthPass, CreateDepthPassProcessor, EShadingPath::Deferred, EMeshPass::DepthPass, EMeshPassFlags::CachedMeshCommands | EMeshPassFlags::MainView);
REGISTER_MESHPASSPROCESSOR_AND_PSOCOLLECTOR(MobileDepthPass, CreateDepthPassProcessor, EShadingPath::Mobile, EMeshPass::DepthPass, EMeshPassFlags::CachedMeshCommands | EMeshPassFlags::MainView);

//RenderAfterTranslucency Added: 标记物体的只写深度Pass（BasePass之后、半透明之前执行）。
// SetupDepthPassState(:486-491) = CW_NONE + <true, CF_DepthNearOrEqual>，正是"只写深度不写颜色"。
// EarlyZPassMode固定传DDM_AllOpaque：保证Masked材质也被绘制（带FDepthOnlyPS做clip），
//   不受项目r.EarlyZPass设置影响（ShouldRender中DDM_MaskedOnly/DDM_NonMaskedOnly会漏画，见:939-972）。
// bRespectUseAsOccluderFlag传false：否则AddMeshBatch(:1026-1046)会按Occluder/Movable/屏幕尺寸过滤掉动态小物体。
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
> 构造函数签名见 `DepthRendering.h:180-193`（`bShadowProjection`、`bSecondStageDepthPass` 均有默认值 false，不必传）。

---

## 13. Engine/Source/Runtime/Renderer/Private/MobileBasePassRendering.cpp

### 添加两个 Render 函数（:491，`RenderMobileBasePass` 之后）

相关代码（:470-493）：
```c++
void FMobileSceneRenderer::RenderMobileBasePass(FRHICommandList& RHICmdList, const FViewInfo& View, const FInstanceCullingDrawParams* InstanceCullingDrawParams)
{
	CSV_SCOPED_TIMING_STAT_EXCLUSIVE(RenderBasePass);
	SCOPED_DRAW_EVENT(RHICmdList, MobileBasePass);
	SCOPE_CYCLE_COUNTER(STAT_BasePassDrawTime);
	SCOPED_GPU_STAT(RHICmdList, Basepass);

	RHICmdList.SetViewport(View.ViewRect.Min.X, View.ViewRect.Min.Y, 0, View.ViewRect.Max.X, View.ViewRect.Max.Y, 1);
	View.ParallelMeshDrawCommandPasses[EMeshPass::BasePass].DispatchDraw(nullptr, RHICmdList, InstanceCullingDrawParams);
		
	if (View.Family->EngineShowFlags.Atmosphere)
	{
		View.ParallelMeshDrawCommandPasses[EMeshPass::SkyPass].DispatchDraw(nullptr, RHICmdList, &SkyPassInstanceCullingDrawParams);
	}

	// editor primitives
	FMeshPassProcessorRenderState DrawRenderState;
	DrawRenderState.SetBlendState(TStaticBlendStateWriteMask<CW_RGBA>::GetRHI());
	DrawRenderState.SetDepthStencilAccess(Scene->DefaultBasePassDepthStencilAccess);
	DrawRenderState.SetDepthStencilState(TStaticDepthStencilState<true, CF_DepthNearOrEqual>::GetRHI());
	RenderMobileEditorPrimitives(RHICmdList, View, DrawRenderState, InstanceCullingDrawParams);
}

void FMobileSceneRenderer::RenderMobileEditorPrimitives(FRHICommandList& RHICmdList, const FViewInfo& View, const FMeshPassProcessorRenderState& DrawRenderState, const FInstanceCullingDrawParams* InstanceCullingDrawParams)
```

修改为（两函数体之间插入）：
```c++
	RenderMobileEditorPrimitives(RHICmdList, View, DrawRenderState, InstanceCullingDrawParams);
}

//RenderAfterTranslucency Added: 只写深度。必须在RHICmdList.NextSubpass()之前调用（Subpass 0深度可写）。
void FMobileSceneRenderer::RenderMobileAfterTranslucencyDepthPass(FRHICommandList& RHICmdList, const FViewInfo& View, const FInstanceCullingDrawParams* InstanceCullingDrawParams)
{
	CSV_SCOPED_TIMING_STAT_EXCLUSIVE(RenderAfterTranslucencyDepth);
	SCOPED_DRAW_EVENT(RHICmdList, MobileAfterTranslucencyDepthPass);
	SCOPE_CYCLE_COUNTER(STAT_AfterTranslucencyDepthDrawTime);
	SCOPED_GPU_STAT(RHICmdList, AfterTranslucencyDepth);

	RHICmdList.SetViewport(View.ViewRect.Min.X, View.ViewRect.Min.Y, 0, View.ViewRect.Max.X, View.ViewRect.Max.Y, 1);
	View.ParallelMeshDrawCommandPasses[EMeshPass::MobileAfterTranslucencyDepthPass].DispatchDraw(nullptr, RHICmdList, InstanceCullingDrawParams);
}

//RenderAfterTranslucency Added: 半透明之后绘制颜色（只读深度）。
void FMobileSceneRenderer::RenderMobileAfterTranslucencyPass(FRHICommandList& RHICmdList, const FViewInfo& View, const FInstanceCullingDrawParams* InstanceCullingDrawParams)
{
	CSV_SCOPED_TIMING_STAT_EXCLUSIVE(RenderAfterTranslucency);
	SCOPED_DRAW_EVENT(RHICmdList, MobileAfterTranslucencyPass);
	SCOPE_CYCLE_COUNTER(STAT_AfterTranslucencyDrawTime);
	SCOPED_GPU_STAT(RHICmdList, AfterTranslucency);

	RHICmdList.SetViewport(View.ViewRect.Min.X, View.ViewRect.Min.Y, 0, View.ViewRect.Max.X, View.ViewRect.Max.Y, 1);
	View.ParallelMeshDrawCommandPasses[EMeshPass::MobileAfterTranslucencyPass].DispatchDraw(nullptr, RHICmdList, InstanceCullingDrawParams);
}

void FMobileSceneRenderer::RenderMobileEditorPrimitives(FRHICommandList& RHICmdList, const FViewInfo& View, const FMeshPassProcessorRenderState& DrawRenderState, const FInstanceCullingDrawParams* InstanceCullingDrawParams)
```

---

## 14. Engine/Source/Runtime/Renderer/Private/SceneRendering.h

### 14.1 函数声明（:2694-2697）

相关代码：
```c++
	/** Renders the opaque base pass for mobile. */
	void RenderMobileBasePass(FRHICommandList& RHICmdList, const FViewInfo& View, const FInstanceCullingDrawParams* InstanceCullingDrawParams);

	void PostRenderBasePass(FRHICommandList& RHICmdList, FViewInfo& View);
```

修改为：
```c++
	/** Renders the opaque base pass for mobile. */
	void RenderMobileBasePass(FRHICommandList& RHICmdList, const FViewInfo& View, const FInstanceCullingDrawParams* InstanceCullingDrawParams);

	//RenderAfterTranslucency Added
	void RenderMobileAfterTranslucencyDepthPass(FRHICommandList& RHICmdList, const FViewInfo& View, const FInstanceCullingDrawParams* InstanceCullingDrawParams);
	void RenderMobileAfterTranslucencyPass(FRHICommandList& RHICmdList, const FViewInfo& View, const FInstanceCullingDrawParams* InstanceCullingDrawParams);

	void PostRenderBasePass(FRHICommandList& RHICmdList, FViewInfo& View);
```

### 14.2 InstanceCulling 参数成员（:2791-2797）

相关代码：
```c++
	// All mesh passes that can be fused into single render-pass
	// Base mesh pass gets its culling parameters from a render-pass struct
	FInstanceCullingDrawParams DepthPassInstanceCullingDrawParams;
	FInstanceCullingDrawParams SkyPassInstanceCullingDrawParams;
	FInstanceCullingDrawParams DebugViewModeInstanceCullingDrawParams;
	FInstanceCullingDrawParams TranslucencyInstanceCullingDrawParams;

	const FViewInfo* CachedView = nullptr;
```

修改为：
```c++
	// All mesh passes that can be fused into single render-pass
	// Base mesh pass gets its culling parameters from a render-pass struct
	FInstanceCullingDrawParams DepthPassInstanceCullingDrawParams;
	FInstanceCullingDrawParams SkyPassInstanceCullingDrawParams;
	FInstanceCullingDrawParams DebugViewModeInstanceCullingDrawParams;
	FInstanceCullingDrawParams TranslucencyInstanceCullingDrawParams;
	//RenderAfterTranslucency Added
	FInstanceCullingDrawParams AfterTranslucencyDepthInstanceCullingDrawParams;
	FInstanceCullingDrawParams AfterTranslucencyInstanceCullingDrawParams;

	const FViewInfo* CachedView = nullptr;
```

---

## 15. Engine/Source/Runtime/Renderer/Private/MobileShadingRenderer.cpp

### 15.1 BuildInstanceCullingDrawParams（:1433-1446）

相关代码：
```c++
void FMobileSceneRenderer::BuildInstanceCullingDrawParams(FRDGBuilder& GraphBuilder, FViewInfo& View, FMobileRenderPassParameters* PassParameters)
{
	if (Scene->GPUScene.IsEnabled())
	{
		if (!bIsFullDepthPrepassEnabled)
		{
			View.ParallelMeshDrawCommandPasses[EMeshPass::DepthPass].BuildRenderingCommands(GraphBuilder, Scene->GPUScene, DepthPassInstanceCullingDrawParams);
		}
		View.ParallelMeshDrawCommandPasses[EMeshPass::BasePass].BuildRenderingCommands(GraphBuilder, Scene->GPUScene, PassParameters->InstanceCullingDrawParams);
		View.ParallelMeshDrawCommandPasses[EMeshPass::SkyPass].BuildRenderingCommands(GraphBuilder, Scene->GPUScene, SkyPassInstanceCullingDrawParams);
		View.ParallelMeshDrawCommandPasses[StandardTranslucencyMeshPass].BuildRenderingCommands(GraphBuilder, Scene->GPUScene, TranslucencyInstanceCullingDrawParams);
		View.ParallelMeshDrawCommandPasses[EMeshPass::DebugViewMode].BuildRenderingCommands(GraphBuilder, Scene->GPUScene, DebugViewModeInstanceCullingDrawParams);
	}
}
```

修改为：
```c++
void FMobileSceneRenderer::BuildInstanceCullingDrawParams(FRDGBuilder& GraphBuilder, FViewInfo& View, FMobileRenderPassParameters* PassParameters)
{
	if (Scene->GPUScene.IsEnabled())
	{
		if (!bIsFullDepthPrepassEnabled)
		{
			View.ParallelMeshDrawCommandPasses[EMeshPass::DepthPass].BuildRenderingCommands(GraphBuilder, Scene->GPUScene, DepthPassInstanceCullingDrawParams);
		}
		View.ParallelMeshDrawCommandPasses[EMeshPass::BasePass].BuildRenderingCommands(GraphBuilder, Scene->GPUScene, PassParameters->InstanceCullingDrawParams);
		View.ParallelMeshDrawCommandPasses[EMeshPass::SkyPass].BuildRenderingCommands(GraphBuilder, Scene->GPUScene, SkyPassInstanceCullingDrawParams);
		View.ParallelMeshDrawCommandPasses[StandardTranslucencyMeshPass].BuildRenderingCommands(GraphBuilder, Scene->GPUScene, TranslucencyInstanceCullingDrawParams);
		View.ParallelMeshDrawCommandPasses[EMeshPass::DebugViewMode].BuildRenderingCommands(GraphBuilder, Scene->GPUScene, DebugViewModeInstanceCullingDrawParams);
		//RenderAfterTranslucency Added: Full Prepass时深度已在Prepass写入且SceneColor Pass深度只读，不构建/不派发深度Pass
		if (!bIsFullDepthPrepassEnabled)
		{
			View.ParallelMeshDrawCommandPasses[EMeshPass::MobileAfterTranslucencyDepthPass].BuildRenderingCommands(GraphBuilder, Scene->GPUScene, AfterTranslucencyDepthInstanceCullingDrawParams);
		}
		View.ParallelMeshDrawCommandPasses[EMeshPass::MobileAfterTranslucencyPass].BuildRenderingCommands(GraphBuilder, Scene->GPUScene, AfterTranslucencyInstanceCullingDrawParams);
	}
}
```

### 15.2 RenderForwardSinglePass（:1604-1624）

**约束：深度 Pass 必须在 `RHICmdList.NextSubpass()`（:1614）之前（Subpass 0 深度可写；切换后为 DepthRead Subpass，见 :1586 `SubpassHint = ESubpassHint::DepthReadSubpass`）。颜色 Pass 必须在 `RenderTranslucency` 之后、`bTonemapSubpassInline` 的第二次 `NextSubpass()`（:1650）之前。**

相关代码：
```c++
		// Depth pre-pass
		RHICmdList.SetCurrentStat(GET_STATID(STAT_CLM_MobilePrePass));
		RenderMaskedPrePass(RHICmdList, View);
		// Opaque and masked
		RHICmdList.SetCurrentStat(GET_STATID(STAT_CLMM_Opaque));
		RenderMobileBasePass(RHICmdList, View, &PassParameters->InstanceCullingDrawParams);
		RenderMobileDebugView(RHICmdList, View);
		RHICmdList.PollOcclusionQueries();
		PostRenderBasePass(RHICmdList, View);
		// scene depth is read only and can be fetched
		RHICmdList.NextSubpass();
		RHICmdList.SetCurrentStat(GET_STATID(STAT_CLMM_Translucency));
		RenderDecals(RHICmdList, View, &PassParameters->InstanceCullingDrawParams);
		RenderModulatedShadowProjections(RHICmdList, ViewContext.ViewIndex, View);
		if (GMaxRHIShaderPlatform != SP_METAL_SIM)
		{
			RenderFog(RHICmdList, View);
		}
		// Draw translucency.
		RenderTranslucency(RHICmdList, View);
```

修改为：
```c++
		// Depth pre-pass
		RHICmdList.SetCurrentStat(GET_STATID(STAT_CLM_MobilePrePass));
		RenderMaskedPrePass(RHICmdList, View);
		// Opaque and masked
		RHICmdList.SetCurrentStat(GET_STATID(STAT_CLMM_Opaque));
		RenderMobileBasePass(RHICmdList, View, &PassParameters->InstanceCullingDrawParams);
		//RenderAfterTranslucency Added: 必须位于NextSubpass()之前（Subpass 0深度可写）。
		// Full Prepass时深度只读且标记物体深度已在Prepass写入，跳过。
		if (!bIsFullDepthPrepassEnabled)
		{
			RenderMobileAfterTranslucencyDepthPass(RHICmdList, View, &AfterTranslucencyDepthInstanceCullingDrawParams);
		}
		RenderMobileDebugView(RHICmdList, View);
		RHICmdList.PollOcclusionQueries();
		PostRenderBasePass(RHICmdList, View);
		// scene depth is read only and can be fetched
		RHICmdList.NextSubpass();
		RHICmdList.SetCurrentStat(GET_STATID(STAT_CLMM_Translucency));
		RenderDecals(RHICmdList, View, &PassParameters->InstanceCullingDrawParams);
		RenderModulatedShadowProjections(RHICmdList, ViewContext.ViewIndex, View);
		if (GMaxRHIShaderPlatform != SP_METAL_SIM)
		{
			RenderFog(RHICmdList, View);
		}
		// Draw translucency.
		RenderTranslucency(RHICmdList, View);
		//RenderAfterTranslucency Added: 半透明之后绘制标记物体颜色（只读深度，遮挡半透明）。
		// 必须位于bTonemapSubpassInline的第二次NextSubpass()之前。
		RenderMobileAfterTranslucencyPass(RHICmdList, View, &AfterTranslucencyInstanceCullingDrawParams);
```

### 15.3 RenderForwardMultiPass 第一个 RenderPass（:1680-1685）

相关代码：
```c++
		// Opaque and masked
		RHICmdList.SetCurrentStat(GET_STATID(STAT_CLMM_Opaque));
		RenderMobileBasePass(RHICmdList, View, &PassParameters->InstanceCullingDrawParams);
		RenderMobileDebugView(RHICmdList, View);
		RHICmdList.PollOcclusionQueries();
		PostRenderBasePass(RHICmdList, View);
	});
```

修改为：
```c++
		// Opaque and masked
		RHICmdList.SetCurrentStat(GET_STATID(STAT_CLMM_Opaque));
		RenderMobileBasePass(RHICmdList, View, &PassParameters->InstanceCullingDrawParams);
		//RenderAfterTranslucency Added: 第一个RenderPass深度可写(!bIsFullDepthPrepassEnabled时)；
		// 深度必须在本Pass写入——第二个RenderPass深度绑定为只读(:1700 DepthRead_StencilRead)。
		if (!bIsFullDepthPrepassEnabled)
		{
			RenderMobileAfterTranslucencyDepthPass(RHICmdList, View, &AfterTranslucencyDepthInstanceCullingDrawParams);
		}
		RenderMobileDebugView(RHICmdList, View);
		RHICmdList.PollOcclusionQueries();
		PostRenderBasePass(RHICmdList, View);
	});
```

### 15.4 RenderForwardMultiPass 第二个 RenderPass（:1729-1736）

相关代码：
```c++
		// scene depth is read only and can be fetched
		RHICmdList.SetCurrentStat(GET_STATID(STAT_CLMM_Translucency));
		RenderDecals(RHICmdList, View, &SecondPassParameters->InstanceCullingDrawParams);
		RenderModulatedShadowProjections(RHICmdList, ViewContext.ViewIndex, View);
		RenderFog(RHICmdList, View);
		// Draw translucency.
		RenderTranslucency(RHICmdList, View);
```

修改为：
```c++
		// scene depth is read only and can be fetched
		RHICmdList.SetCurrentStat(GET_STATID(STAT_CLMM_Translucency));
		RenderDecals(RHICmdList, View, &SecondPassParameters->InstanceCullingDrawParams);
		RenderModulatedShadowProjections(RHICmdList, ViewContext.ViewIndex, View);
		RenderFog(RHICmdList, View);
		// Draw translucency.
		RenderTranslucency(RHICmdList, View);
		//RenderAfterTranslucency Added: 只读深度，与本RenderPass的DepthRead绑定兼容
		RenderMobileAfterTranslucencyPass(RHICmdList, View, &AfterTranslucencyInstanceCullingDrawParams);
```

> 说明：Android Vulkan 走 SinglePass（`RequiresMultiPass` 对 Vulkan 直接返回 false，:2131-2137）；MultiPass 分支主要覆盖 GLES 等平台，两处都改保证完整性。

---

## 16. Engine/Source/Runtime/RenderCore/Public/RenderCore.h

### CPU 周期 Stat 声明（:44）

相关代码（:43-45）：
```c++
DECLARE_CYCLE_STAT_EXTERN(TEXT("Depth drawing"),STAT_DepthDrawTime,STATGROUP_SceneRendering, RENDERCORE_API);
DECLARE_CYCLE_STAT_EXTERN(TEXT("Base pass drawing"),STAT_BasePassDrawTime,STATGROUP_SceneRendering, RENDERCORE_API);
DECLARE_CYCLE_STAT_EXTERN(TEXT("Anisotropy pass drawing"), STAT_AnisotropyPassDrawTime, STATGROUP_SceneRendering, RENDERCORE_API);
```

修改为：
```c++
DECLARE_CYCLE_STAT_EXTERN(TEXT("Depth drawing"),STAT_DepthDrawTime,STATGROUP_SceneRendering, RENDERCORE_API);
DECLARE_CYCLE_STAT_EXTERN(TEXT("Base pass drawing"),STAT_BasePassDrawTime,STATGROUP_SceneRendering, RENDERCORE_API);
//RenderAfterTranslucency Added
DECLARE_CYCLE_STAT_EXTERN(TEXT("After translucency depth drawing"), STAT_AfterTranslucencyDepthDrawTime, STATGROUP_SceneRendering, RENDERCORE_API);
DECLARE_CYCLE_STAT_EXTERN(TEXT("After translucency drawing"), STAT_AfterTranslucencyDrawTime, STATGROUP_SceneRendering, RENDERCORE_API);
DECLARE_CYCLE_STAT_EXTERN(TEXT("Anisotropy pass drawing"), STAT_AnisotropyPassDrawTime, STATGROUP_SceneRendering, RENDERCORE_API);
```

---

## 17. Engine/Source/Runtime/RenderCore/Private/RenderCore.cpp

### Stat 定义（:65）

相关代码（:64-67）：
```c++
DEFINE_STAT(STAT_StaticDrawListDrawTime);
DEFINE_STAT(STAT_BasePassDrawTime);
DEFINE_STAT(STAT_AnisotropyPassDrawTime);
DEFINE_STAT(STAT_DepthDrawTime);
```

修改为：
```c++
DEFINE_STAT(STAT_StaticDrawListDrawTime);
DEFINE_STAT(STAT_BasePassDrawTime);
//RenderAfterTranslucency Added
DEFINE_STAT(STAT_AfterTranslucencyDepthDrawTime);
DEFINE_STAT(STAT_AfterTranslucencyDrawTime);
DEFINE_STAT(STAT_AnisotropyPassDrawTime);
DEFINE_STAT(STAT_DepthDrawTime);
```

---

## 18. Engine/Source/Runtime/Renderer/Private/BasePassRendering.h

### GPU Stat 声明（:144）

相关代码（:144-145）：
```c++
DECLARE_GPU_DRAWCALL_STAT_EXTERN(Basepass);
DECLARE_GPU_STAT_NAMED_EXTERN(NaniteBasePass, TEXT("Nanite BasePass"));
```

修改为：
```c++
DECLARE_GPU_DRAWCALL_STAT_EXTERN(Basepass);
//RenderAfterTranslucency Added
DECLARE_GPU_DRAWCALL_STAT_EXTERN(AfterTranslucencyDepth);
DECLARE_GPU_DRAWCALL_STAT_EXTERN(AfterTranslucency);
DECLARE_GPU_STAT_NAMED_EXTERN(NaniteBasePass, TEXT("Nanite BasePass"));
```

---

## 19. Engine/Source/Runtime/Renderer/Private/BasePassRendering.cpp

### GPU Stat 定义（:184）——**必须在 .cpp 中，放头文件会重复定义链接错误**

相关代码（:182-186）：
```c++
IMPLEMENT_MATERIAL_SHADER_TYPE(, F128BitRTBasePassPS, TEXT("/Engine/Private/BasePassPixelShader.usf"), TEXT("MainPS"), SF_Pixel);

DEFINE_GPU_DRAWCALL_STAT(Basepass);

DECLARE_CYCLE_STAT(TEXT("DeferredShadingSceneRenderer ClearGBufferAtMaxZ"), STAT_FDeferredShadingSceneRenderer_ClearGBufferAtMaxZ, STATGROUP_SceneRendering);
```

修改为：
```c++
IMPLEMENT_MATERIAL_SHADER_TYPE(, F128BitRTBasePassPS, TEXT("/Engine/Private/BasePassPixelShader.usf"), TEXT("MainPS"), SF_Pixel);

DEFINE_GPU_DRAWCALL_STAT(Basepass);
//RenderAfterTranslucency Added
DEFINE_GPU_DRAWCALL_STAT(AfterTranslucencyDepth);
DEFINE_GPU_DRAWCALL_STAT(AfterTranslucency);

DECLARE_CYCLE_STAT(TEXT("DeferredShadingSceneRenderer ClearGBufferAtMaxZ"), STAT_FDeferredShadingSceneRenderer_ClearGBufferAtMaxZ, STATGROUP_SceneRendering);
```
> `MobileBasePassRendering.cpp` 已使用 `SCOPED_GPU_STAT(RHICmdList, Basepass)`（:475），说明 BasePassRendering.h 已被包含，新 Stat 可直接使用。

---

## 20. Shader（.usf/.ush）：**无需任何修改**，理由如下

1. **颜色 Pass**：复用 `TMobileBasePassVSPolicyParamType` / `TMobileBasePassPSPolicyParamType`（经 `MobileBasePass::GetShaders` 选取，MobileBasePass.cpp:906-940）。Shader 排列组合的 Key 是"材质 × VertexFactory × LightMapPolicy × 局部光/天光排列"，**不包含 EMeshPass**——同一材质在 BasePass 已编译过的 Shader 在新 Pass 直接复用，不产生新排列，也不需要新的编译宏。
2. **深度 Pass**：复用 DepthRendering 的现成 Shader（DepthRendering.cpp:158-165）：
```c++
IMPLEMENT_MATERIAL_SHADER_TYPE(template<>,TDepthOnlyVS<true>,TEXT("/Engine/Private/PositionOnlyDepthVertexShader.usf"),TEXT("Main"),SF_Vertex);
IMPLEMENT_MATERIAL_SHADER_TYPE(template<>,TDepthOnlyVS<false>,TEXT("/Engine/Private/DepthOnlyVertexShader.usf"),TEXT("Main"),SF_Vertex);
IMPLEMENT_MATERIAL_SHADER_TYPE(,FDepthOnlyPS,TEXT("/Engine/Private/DepthOnlyPixelShader.usf"),TEXT("Main"),SF_Pixel);
IMPLEMENT_SHADERPIPELINE_TYPE_VS(DepthNoPixelPipeline, TDepthOnlyVS<false>, true);
IMPLEMENT_SHADERPIPELINE_TYPE_VS(DepthPosOnlyNoPixelPipeline, TDepthOnlyVS<true>, true);
```
   这些 Shader 在移动端已经编译且在用——`MobileDepthPass` 早已注册（DepthRendering.cpp:1243）并服务于 Masked 预 Pass（`RenderMaskedPrePass`）与 Full Depth Prepass。静态网格（不透明/无 WPO）走 PositionOnly VS + 空 PS；Masked 材质走 DepthOnly VS + `FDepthOnlyPS`（内部做 clip）；骨骼网格（GPUSkin 不支持 PositionOnly 流）走完整 VF 的 DepthOnly VS。
3. 半透明 Shader 读取的 SceneDepth（DepthFetch/DepthAux）机制未被改变——标记物体深度在半透明绘制前已写入深度附件，半透明的固定管线深度测试自然剔除被遮挡像素，不涉及 Shader 改动。

---

## 21. 使用方法与运行前提

- 组件上勾选 `Render Opaque After Translucency (Mobile)`（或蓝图调用 `SetRenderAfterTranslucency(true)`），仅对不透明/Masked 材质生效；物体上的半透明材质 Section 仍走正常半透明流程。
- 生效条件：移动端 Forward（`r.Mobile.ShadingPath=0`）。Mobile Deferred 下自动回落 BasePass 正常渲染（不会消失，但也没有"盖住半透明"的效果）。
- `r.EarlyZPass`（Scene->EarlyZPassMode）任意配置均可运行：
  - 非 DDM_AllOpaque：深度由 MobileAfterTranslucencyDepthPass 在 BasePass 之后写入；
  - DDM_AllOpaque（Full Prepass）：深度由 Prepass 写入，本方案自动跳过深度 Pass。

## 22. 已知行为/限制（非 Bug，需知晓）

1. **贴花/雾/调制阴影**在 Subpass 1 开头执行（MobileShadingRenderer.cpp:1616-1621），此时深度缓冲已含标记物体：贴花会按其深度投影、逐像素雾按其深度计算，但颜色 Pass 在这些之后绘制，会覆盖这些效果——标记物体不接受屏幕空间贴花与半透明前的雾效（材质端如需雾，需自行计算）。
2. 标记物体的颜色在 BasePass 阶段不写入，其身后的不透明物体会正常绘制（一次额外 overdraw），最终被颜色 Pass 覆盖，视觉正确。
3. PSO 预热对两个新 Pass 关闭（见 11.2 / 12.2），首次绘制可能有一次 PSO 编译；如成为问题再按注释补收集逻辑。
4. 阴影投射（CSM）不受影响：Shadow Depth Pass 与本分流无关，标记物体照常投影。
5. VR Instanced Stereo / MultiView 自动支持：新 Pass 注册为 `MainView`，`FSceneRenderer::SetupMeshPass`（SceneRendering.cpp:4202-4252）对 MainView Pass 统一走 Stereo Instance Culling。
6. SceneDepthAux 不含标记物体深度(半透明阶段):深度 Pass只写硬件深度附件,不写 SceneDepthAux(无 PS 或 FDepthOnlyPS 无 MRT1输出)。半透明材质里靠采样场景深度实现的效果——粒子 DepthFade、软粒子、水深淡出——在标记物体表面附近会"看不到"它(不淡出)。硬件深度测试仍然正确,遮挡本身没问题,只是这类着色器效果在交界处不精确。VR项目粒子如果大量用 Depth Fade,需要留意。                               
7. Masked 材质 + r.EarlyZPassOnlyMaterialMasking=1 的组合:该 CVar开启时 base pass PS 不做 clip、依赖 CF_Equal 剔除;而你的颜色 Pass 是CF_DepthNearOrEqual,被 clip 掉的镂空像素可能被错误填充。若项目没开这个CVar(移动端默认关)则无此问题,验证清单里的"Masked材质"测试项建议把这个组合也测一下。

## 23. 验证清单

- [ ] Android Vulkan（SinglePass + MultiView）：标记物体遮挡半透明、被不透明遮挡、MSAA 开/关。
- [ ] `r.EarlyZPass` 三种配置（0 / MaskedOnly / AllOpaque）下均无 RHI 校验报错且效果一致。
- [ ] 标记物体使用 Masked 材质（验证 FDepthOnlyPS clip 路径）。
- [ ] 标记 SkeletalMesh（动态路径 + GPUSkin DepthOnly VS）。
- [ ] 运行时切换 `SetRenderAfterTranslucency`（验证 MarkRenderStateDirty 后缓存命令重建）。
- [ ] 含半透明 Section 的标记物体：半透明部分正常绘制（验证 11.1 的 `!bTranslucentBasePass` 分支）。
- [ ] `stat scenerendering` 可见 "After translucency drawing"；`stat gpu` 可见 AfterTranslucency/AfterTranslucencyDepth；RenderDoc 中确认 Pass 顺序：MobileBasePass → MobileAfterTranslucencyDepthPass → (Decals/Fog) → Translucency → MobileAfterTranslucencyPass。
- [ ] 编辑器 Mobile Preview（ES3.1）不崩溃、效果一致。
