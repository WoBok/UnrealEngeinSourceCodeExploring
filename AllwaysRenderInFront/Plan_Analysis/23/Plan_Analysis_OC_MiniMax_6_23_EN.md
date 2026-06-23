# Plan Analysis Report: `bRenderAfterTranslucency` Mesh Pass for UE 5.4 Mobile Forward

## 0. Executive Summary

| Category | Verdict |
|---|---|
| **Plan works for the core goal?** | Partially — depth-stencil state is **wrong** for the stated occlusion intent |
| **All 9 line-number claims in step 2-3 (Primitive/SceneProxy)** | **ALL CORRECT** |
| **EMeshPass enum and `static_assert` numbers** | Plan's `33 + 4` / `33` is **CORRECT** (current source is 32 / 32+4) |
| **Critical omissions** | Dynamic-mesh path in `SceneVisibility.cpp:2228-2232` not updated; `SceneRendering.cpp:4209` skip-list not updated; `RenderMobileEditorPrimitives` not handled; `SetupPrecachePSOParams` not updated |
| **Design issues** | `bAfterTranslucencyBasePass` member is **redundant**; depth-write state is **inverted** for the user's goal |
| **Other proxy types that would silently fail** | `Niagara`, `ParticleSystem`, `Landscape`, `GeometryCollection`, `HairStrands/Groom`, `Widget`, `TextRender`, `MRMesh`, etc. — none will use the new pass |

---

## 1. Step 1 — `EMeshPass` Enum and `GetMeshPassName` (Plan Section 1)

### 1.1 Plan claim verification

| Plan Claim | Actual | Verdict |
|---|---|---|
| `MeshPassProcessor.h:32` — `namespace EMeshPass` | Line 32 | ✓ |
| Enum has 33 values before editor block | **32 values**, not 33 (lines 36–67) | ✗ count off by one |
| `NumBits = 6` | Line 77 | ✓ |
| Current `static_assert` = `32 + 4` / `32` | **Actual**: `32 + 4` (line 128) / `32` (line 130) | ✓ plan correctly says current is `32` |
| Plan updates to `33 + 4` / `33` | Math: 32 + 1 = 33 | ✓ CORRECT |
| The 32 enum values listed | Listed at lines 36–67 | ✓ all present |

### 1.2 No other static_asserts are sensitive to the new count

The only hard-coded count assertions are at `MeshPassProcessor.h:128, 130` (which the plan updates). All other references (`MeshPassProcessor.cpp:2258`, etc.) are dynamic and adapt automatically:

- `MeshPassProcessor.h:80` — `static_assert(EMeshPass::Num <= (1 << EMeshPass::NumBits))` — 33 ≤ 64 ✓
- `MeshPassProcessor.h:519` — `sizeof(FMeshPassMask::Data) * 8 >= EMeshPass::Num` — Data is `uint64` ✓
- `MeshPassProcessor.cpp:2258` — `static_assert(EMeshPass::Num <= FPSOCollectorCreateManager::MaxPSOCollectorCount)` — must be ≥ 33; in UE 5.4 `MaxPSOCollectorCount` is well above 64, so safe.

### 1.3 Step 1 verdict: **OK** — insertion positions and `static_assert` numbers are correct.

---

## 2. Step 2-3 — `UPrimitiveComponent`, `FPrimitiveSceneProxy`, `FPrimitiveSceneProxyDesc`

### 2.1 Plan claim verification (ALL CORRECT)

| Plan claim | Actual location | Notes |
|---|---|---|
| `PrimitiveComponent.h:407` UPROPERTY block for `bRenderInMainPass` | Lines 407–408 | UPROPERTY macro on 407, field on 408 |
| `PrimitiveComponent.h:1917` `SetRenderInMainPass` UFUNCTION | Lines 1917–1918 | |
| `PrimitiveComponent.cpp:4457` implementation | Lines 4457–4464 | |
| `PrimitiveComponent.cpp:333` constructor init | Line 333 | |
| `PrimitiveSceneProxy.h:1200` field | Line 1200 | |
| `PrimitiveSceneProxy.h:700` `ShouldRenderInMainPass()` | Line 700 | |
| `PrimitiveSceneProxy.cpp:277` `InitializeFrom` | Line 277 | |
| `PrimitiveSceneProxy.cpp:428` constructor | Line 428 | |
| `PrimitiveSceneProxyDesc.h:93` field | Line 93 | |
| `PrimitiveSceneProxyDesc.h:25` init | Line 25 | |

The plan's mirroring pattern for adding `bRenderAfterTranslucency` is **structurally correct** and these 5 files are sufficient for the basic propagation.

### 2.2 Step 2-3 verdict: **OK** for basic field propagation.

### 2.3 Additional places the plan does NOT update (potential issues)

#### 2.3.1 `SetupPrecachePSOParams` — `PrimitiveComponent.cpp:4620-4632`

```cpp
// PrimitiveComponent.cpp:4620
void UPrimitiveComponent::SetupPrecachePSOParams(FPSOPrecacheParams& Params)
{
    Params.bRenderInMainPass = bRenderInMainPass;       // <-- mirrored for new field?
    Params.bRenderInDepthPass = bRenderInDepthPass;
    ...
}
```

If PSO precache should know about the new pass, this needs `Params.bRenderAfterTranslucency = bRenderAfterTranslucency;`. **Not updated by the plan.** The new `FPSOPrecacheParams` bitfield (`Engine/Public/PSOPrecache.h:110`) is a `uint64 : 1`, so adding one bit is safe.

#### 2.3.2 `bShouldRenderInMainPass` cached in `FPrimitiveSceneInfo`

`Engine/Source/Runtime/Renderer/Public/PrimitiveSceneInfo.h:675` and `Private/PrimitiveSceneInfo.cpp:305` cache `bShouldRenderInMainPass` from the proxy. If the new field is consumed at scene-info level, this needs mirroring. **Not updated by the plan.**

#### 2.3.3 Construction chain

The plan correctly handles the two-arg constructor (line 428) and `InitializeFrom` (line 277) in `PrimitiveSceneProxy.cpp`. The plan also covers the default constructor of `FPrimitiveSceneProxyDesc`. **No issue.**

#### 2.3.4 Serialization

`UPROPERTY` handles save/load automatically. **No additional code needed.** ✓

---

## 3. Step 4 — `FMobileBasePassMeshProcessor` and Pass Registration

### 3.1 Plan claim verification (ALL CORRECT for line numbers)

| Plan claim | Actual |
|---|---|
| `MobileBasePassRendering.h:480` constructor declaration | Line 480–487 |
| `MobileBasePassRendering.h:533` `bPassUsesDeferredShading` field | Line 533 |
| `MobileBasePass.cpp:810` constructor impl | Line 810–826 |
| `MobileBasePass.cpp:867` `AddMeshBatch` | Line 867–890 |
| `MobileBasePass.cpp:1151` `CreateMobileBasePassProcessor` | Line 1151–1163 |
| `MobileBasePass.cpp:1223` REGISTER macro | Line 1223 is the comment `// Skipping EMeshPass::TranslucencyAfterDOFModulate...`; the registration block ends at line 1222. Insertion is on the line just after the `MobileTranslucencyAfterDOFPass` registration. |

### 3.2 ⚠ CRITICAL DESIGN ISSUE — Depth Stencil State Inverted

The plan writes:

```c++
PassDrawRenderState.SetDepthStencilAccess(FExclusiveDepthStencil::DepthRead_StencilRead);
PassDrawRenderState.SetDepthStencilState(TStaticDepthStencilState<false, CF_DepthNearOrEqual>::GetRHI());
```

The first template parameter of `TStaticDepthStencilState` is `bEnableDepthWrite`:
- `true` → depth **write ENABLED**
- `false` → depth **write DISABLED**

The user's stated goal: *"透明物体不写入深度，所以我可以把透明物体遮挡住"* — overwrite the translucent pixels that are in front.

For the after-translucency opaque pass to correctly occlude things behind it AND overwrite the translucent color in front of it, it MUST:
1. **Test depth** against the existing opaque depth (so it doesn't pop through walls) — the `CF_DepthNearOrEqual` part is correct.
2. **Write depth** so the new opaque objects are properly recorded in the depth buffer for any subsequent pass.
3. **Write color** (set via `SetBlendState(TStaticBlendStateWriteMask<CW_RGBA>::GetRHI())` — the plan includes this; good).

**The plan's `TStaticDepthStencilState<false, ...>` is WRONG** for the stated goal. It should be:

```c++
PassDrawRenderState.SetDepthStencilAccess(FExclusiveDepthStencil::DepthWrite_StencilNop);
PassDrawRenderState.SetDepthStencilState(TStaticDepthStencilState<true, CF_DepthNearOrEqual>::GetRHI());
```

If the user truly does NOT want depth writes, they should understand this means subsequent passes (and the `CustomDepth` pass, occlusion, etc.) will not see the after-translucency objects' depth — which likely contradicts the user's goal.

**Answer to the user's question "SetDepthStencilState and TStaticDepthStencilState 的作用是什么?":**
- `SetDepthStencilState` configures the GPU depth-stencil state for the draw.
- `TStaticDepthStencilState<bDepthWrite, DepthTestFunc>` provides a statically-typed GPU state: `<true, CF_DepthNearOrEqual>` = standard opaque (test + write, LessOrEqual), `<false, CF_DepthNearOrEqual>` = translucency-style (test, no write). For after-translucency opaque, the first is correct.

### 3.3 Design Issue — `bAfterTranslucencyBasePass` member is redundant

The plan adds a `bool bAfterTranslucencyBasePass` member, but the same information is already available via `InMeshPassType == EMeshPass::MobileAfterTranslucencyPass`. Inside `AddMeshBatch` this can be simplified:

```c++
// In AddMeshBatch, replace the bAfterTranslucencyBasePass branch with:
const bool bAfterTranslucencyPass = (MeshPassType == EMeshPass::MobileAfterTranslucencyPass);
if (bAfterTranslucencyPass != PrimitiveSceneProxy->ShouldRenderAfterTranslucency())
{
    return;
}
```

Or just compare `MeshPassType` directly to keep the member out of the class entirely.

### 3.4 Design Issue — `EFlags` selection

The plan uses only `FMobileBasePassMeshProcessor::EFlags::CanUseDepthStencil` for the new pass. This means:
- `CanReceiveCSM` is **disabled** — the new pass will not pick up cascade shadow maps. This is consistent with the user's goal (after-translucency objects will be in shadow only if they re-evaluate CSM themselves; for a VR mobile game, this is probably acceptable, but the user should confirm).
- `ForcePassDrawRenderState` is not needed.
- `DoNotCache` is not needed.

This is acceptable. No change required.

### 3.5 Step 4 verdict: **Mostly OK**, but **depth stencil state must be fixed** and the `bAfterTranslucencyBasePass` member is **redundant**.

---

## 4. Step 5 — `RenderMobileAfterTranslucencyPass` and Mobile Scene Renderer Wiring

### 4.1 Plan claim verification (ALL CORRECT for line numbers)

| Plan claim | Actual |
|---|---|
| `SceneRendering.h:2695` `RenderMobileBasePass` decl | Line 2695 |
| `SceneRendering.h:2796` `TranslucencyInstanceCullingDrawParams` | Line 2796 |
| `MobileShadingRenderer.cpp:1433` `BuildInstanceCullingDrawParams` | Lines 1433–1446 |
| `MobileShadingRenderer.cpp:1624` `RenderForwardSinglePass` | Actually begins at 1578; `RenderTranslucency` is at line 1623 (off by 1) |
| `MobileShadingRenderer.cpp:1736` `RenderForwardMultiPass` | Actually begins at 1662; `RenderTranslucency` is at line 1735 (off by 1) |
| `MobileBasePassRendering.cpp:492` `RenderMobileBasePass` | Lines 470–491 (plan says "492附近" — within range) |

### 4.2 ⚠ CRITICAL OMISSION — `FSceneRenderer::SetupMeshPass` mobile skip list

`SceneRendering.cpp:4208-4212` explicitly skips `EMeshPass::BasePass` and `EMeshPass::MobileBasePassCSM` on mobile (because they are merged and sorted later in `SetupMobileBasePassAfterShadowInit`). If the new pass follows the same pattern (built in `BuildInstanceCullingDrawParams`), it must be added here:

```c++
// SceneRendering.cpp:4209 — currently:
if (ShadingPath == EShadingPath::Mobile && (PassType == EMeshPass::BasePass || PassType == EMeshPass::MobileBasePassCSM))
// Should become:
if (ShadingPath == EShadingPath::Mobile && (PassType == EMeshPass::BasePass || PassType == EMeshPass::MobileBasePassCSM || PassType == EMeshPass::MobileAfterTranslucencyPass))
```

Otherwise the new pass will be set up twice: once by `FSceneRenderer::SetupMeshPass` (line 4233 in the same file) and once by `BuildInstanceCullingDrawParams`. This causes double mesh-draw-command creation and incorrect rendering.

### 4.3 ⚠ CRITICAL OMISSION — `SetupMobileBasePassAfterShadowInit` pluming

`MobileShadingRenderer.cpp:377-427` (`SetupMobileBasePassAfterShadowInit`) is what dispatches `EMeshPass::BasePass` and `EMeshPass::MobileBasePassCSM` to a single combined `FParallelMeshDrawCommandPass::DispatchPassSetup` call (lines 410-425). The new pass is **not** registered here, so:

- The new pass won't receive the special "merged with CSM" treatment.
- The new pass uses the **default** `FSceneRenderer::SetupMeshPass` path (per the skip-list above), which means the new pass is set up as a regular cached mesh command pass at the standard time.

This is OK **if** the new pass is not meant to be combined with CSM. For a typical "after-translucency opaque" use case, this is fine. **But** the user must remove the new pass from the `FSceneRenderer::SetupMeshPass` skip list only if they want it to bypass mobile setup, OR keep the skip and accept the standard mobile setup. The plan as written needs the skip list addition.

### 4.4 ⚠ CRITICAL OMISSION — Dynamic-mesh path in `ComputeDynamicMeshRelevance`

`SceneVisibility.cpp:2228-2232` — the dynamic-mesh path has the **same** mobile special-case that adds `EMeshPass::MobileBasePassCSM`. The plan's update is structural and the user might or might not have realized the dynamic-mesh side mirrors the static-mesh side. **This must also be updated** with the same `if (ViewRelevance.bRenderAfterTranslucency) { EMeshPass::MobileAfterTranslucencyPass } else { EMeshPass::BasePass }` logic.

### 4.5 ⚠ CRITICAL STRUCTURAL BUG in SceneVisibility.cpp modification

The plan's static-mesh branch as written (line 1564-1568) puts the `MobileBasePassCSM` add **OUTSIDE** the `if/else`:

```c++
if(ViewRelevance.bRenderAfterTranslucency)
{
    DrawCommandPacket.AddCommandsForMesh(..., EMeshPass::MobileAfterTranslucencyPass);
}else
{
    DrawCommandPacket.AddCommandsForMesh(..., EMeshPass::BasePass);
}
if (!bMobileBasePassAlwaysUsesCSM)
{
    DrawCommandPacket.AddCommandsForMesh(..., EMeshPass::MobileBasePassCSM);  // <-- runs in BOTH branches!
}
```

This means when `bRenderAfterTranslucency=true`, the primitive is added to **both** the new pass and `MobileBasePassCSM`, which would cause it to be drawn twice (once in the base pass with CSM, once in the after-translucency pass). **This is a bug.** The `MobileBasePassCSM` add must be inside the `else` branch, or the entire `else` branch should also be wrapped.

The correct code should be:

```c++
if (ViewRelevance.bRenderAfterTranslucency)
{
    DrawCommandPacket.AddCommandsForMesh(..., EMeshPass::MobileAfterTranslucencyPass);
    // Note: MobileBasePassCSM is intentionally NOT added; the new pass is meant to be a
    // standalone re-draw after translucency, not a CSM-augmented pass.
}
else
{
    DrawCommandPacket.AddCommandsForMesh(..., EMeshPass::BasePass);
    if (!bMobileBasePassAlwaysUsesCSM)
    {
        DrawCommandPacket.AddCommandsForMesh(..., EMeshPass::MobileBasePassCSM);
    }
}
```

**The same bug exists in the dynamic-mesh path** (line 2211-2232) — must be fixed there too.

### 4.6 ⚠ CRITICAL OMISSION — `RenderMobileEditorPrimitives`

`MobileBasePassRendering.cpp:490` calls `RenderMobileEditorPrimitives` from inside `RenderMobileBasePass`. This is for editor viewmesh/topmesh, simple elements, etc. The plan's new `RenderMobileAfterTranslucencyPass` does **not** call this. **Decision required**: should editor primitives also be drawn in the new pass? For a mobile VR game, the answer is likely NO (editor is irrelevant for shipping), but for editor-time iteration, it should be considered.

### 4.7 ⚠ Subpass constraint in `RenderForwardSinglePass` (MobileShadingRenderer.cpp:1578-1660)

The single-pass path uses two RDG subpasses separated by `RHICmdList.NextSubpass()` at line 1614. After this subpass switch, the depth state is read-only. The plan inserts `RenderMobileAfterTranslucencyPass` **after** `RenderTranslucency` (line 1623), which is **after** the subpass switch (line 1614). This means:

- If the new pass uses `TStaticDepthStencilState<true, ...>` (depth write), the **subpass state's read-only depth attachment will cause a validation error** or a forced render-pass boundary.
- The proper insertion is therefore **inside** the second subpass's lambda, but with a depth-write state — this is the conflict.

**Three options** for the user:
1. **Keep the plan as written and use depth-read-only** (`false, CF_DepthNearOrEqual`) — works in the second subpass, but does NOT write depth. (This is what the plan has, but conflicts with the goal of "occluding translucency" because occlusion requires depth write for any subsequent pass to see the new pass's depth.)
2. **Insert the new pass BEFORE the subpass switch** (after `RenderMobileBasePass` on line 1609 and before `PostRenderBasePass` on line 1612) — would be "between base pass and decals/translucency", NOT "after translucency". Wrong semantic.
3. **Use a new RDG AddPass scheduled after the existing pass** (not inside the lambda) — breaks subpass merging but allows any depth state. This is the cleanest if subpass is not critical for perf.

The plan does not address this. **The user must decide the subpass strategy.** For a mobile VR game where subpass is a meaningful perf optimization, option 1 is the safe choice but the depth-stencil state must be revised to match the actual goal.

### 4.8 Decision required — Which paths use the new pass?

The plan mentions only `RenderForwardSinglePass` (1623) and `RenderForwardMultiPass` (1735). Other paths to consider:
- `RenderCustomRenderPassBasePass` (MobileShadingRenderer.cpp:857-908) — scene capture. **Probably skip** for shipping.
- `RenderDeferredSinglePass` (line 1947) and `RenderDeferredMultiPass` (line 1996) — mobile deferred shading. **Probably skip** unless the user wants this on mobile-deferred too.
- `RenderHitProxies` (SceneHitProxyRendering.cpp) — editor-only.

The plan does not mention these. **Decision required.**

### 4.9 EMobileBasePass uniform buffer mode

The mobile scene renderer uses `EMobileBasePass::Opaque` for the first subpass/first pass and `EMobileBasePass::Translucent` for the second subpass/second pass. The new pass runs in the second subpass/second pass context, so the `MobileBasePass` UB is set to `Translucent` mode. The new pass may want `Opaque` mode instead. This is a shader-UB concern, and the user should verify whether the `Opaque` uniform buffer state is what the new pass wants. Likely OK because the new pass uses the same shader as the regular base pass.

### 4.10 Step 5 verdict: **Several critical omissions and one critical structural bug.**

---

## 5. Step 6 — `FPrimitiveViewRelevance` and `GetViewRelevance` Overrides

### 5.1 Plan claim verification (ALL CORRECT)

| Plan claim | Actual |
|---|---|
| `PrimitiveViewRelevance.h:54` `bRenderInMainPass` | Line 54 |
| `PrimitiveViewRelevance.h:103` constructor | Lines 89–104; line 103 = `bRenderInMainPass = true;` |
| `StaticMeshRender.cpp:2055` `GetViewRelevance` | Line 2055 |
| `SkeletalMesh.cpp:7107` `GetViewRelevance` | Line 7107 |

The constructor's `bRenderAfterTranslucency = false` is **redundant** (struct is already zero-initialized by lines 93–97) but harmless.

### 5.2 ⚠ CRITICAL OMISSION — Many other proxy classes also set `bRenderInMainPass`

The plan only updates StaticMesh and SkeletalMesh, but the following proxies also set `bRenderInMainPass` and would also need to set `bRenderAfterTranslucency` if the user wants them to use the new pass:

**High priority (major primitive types used in shipping):**

| File | Line | Class |
|---|---|---|
| `Source/Runtime/Engine/Private/Particles/ParticleSystemRender.cpp` | 6856 | `FParticleSystemSceneProxy` |
| `Plugins/FX/Niagara/Source/Niagara/Private/NiagaraComponent.cpp` | 286 | `FNiagaraSceneProxy` |
| `Source/Runtime/Experimental/GeometryCollectionEngine/Private/GeometryCollection/GeometryCollectionSceneProxy.cpp` | 936 | `FGeometryCollectionSceneProxy` |
| `Source/Runtime/Experimental/GeometryCollectionEngine/Private/GeometryCollection/GeometryCollectionSceneProxy.cpp` | 1171 | `FNaniteGeometryCollectionSceneProxy` |
| `Source/Runtime/Landscape/Private/LandscapeRender.cpp` | 1987 | `FLandscapeComponentSceneProxy` |
| `Plugins/Runtime/HairStrands/Source/HairStrandsCore/Private/GroomComponent.cpp` | 1091 | `FHairStrandsSceneProxy` (Groom) |
| `Source/Runtime/UMG/Private/Components/WidgetComponent.cpp` | 559 | `FWidgetSceneProxy` |
| `Source/Runtime/Engine/Private/Components/HeterogeneousVolumeComponent.cpp` | 171 | `FHeterogeneousVolumeSceneProxy` |
| `Source/Runtime/Engine/Private/Components/TextRenderComponent.cpp` | 857 | `FTextRenderSceneProxy` |
| `Source/Runtime/MRMesh/Private/MRMeshComponent.cpp` | 492 | `FMRMeshSceneProxy` |
| `Source/Runtime/Engine/Private/Rendering/NaniteResources.cpp` | 1031 | `Nanite::FSceneProxy` (hardcoded to true) |

**Medium priority:** ProceduralMesh, CustomMesh, Cable, DynamicMesh, VirtualHeightfieldMesh, WaterMesh, Dataflow, PaperRender, SparseVolumeTexture, USD, ImagePlate, GeometryCache, LidarPointCloud.

**Note:** Several proxies delegate to `FStaticMeshSceneProxy::GetViewRelevance` (HISM, ISM, SplineMesh, Nanite-SplineMesh, WaterBodyInfoMesh, StereoStaticMesh) — these automatically inherit the new field when `FStaticMeshSceneProxy` is updated.

**Decision required:** Will the user use the new pass on any of these proxy types? If only StaticMesh and SkeletalMesh, the plan is OK. If more, additional updates are needed.

### 5.3 Step 6 verdict: **OK if scope is StaticMesh+SkeletalMesh only**; **incomplete otherwise.**

---

## 6. Step 7 — `RenderCore` Stats (Plan Section after Step 5)

### 6.1 Plan claim verification (ALL CORRECT)

| Plan claim | Actual |
|---|---|
| `RenderCore.cpp:65` `DEFINE_STAT(STAT_BasePassDrawTime)` | Line 65 |
| `RenderCore.h:44` `DECLARE_CYCLE_STAT_EXTERN(...)` | Line 44 |
| `BasePassRendering.h:144` `DECLARE_GPU_DRAWCALL_STAT_EXTERN(Basepass)` | Line 144 |

### 6.2 Step 7 verdict: **OK** — all line numbers correct.

---

## 7. Cross-Cutting Issues Not Addressed by the Plan

### 7.1 No `bAfterTranslucencyBasePass` needed
The same information is available via `MeshPassType == EMeshPass::MobileAfterTranslucencyPass`. Suggest removing the redundant member.

### 7.2 NumBits constraint
`EMeshPass::NumBits = 6` → max 64 passes. Currently 32; after addition 33. Safe.

### 7.3 `FPrimitiveSceneInfo` cache
If the new field needs to be queryable at the scene-info level, mirror it in `FPrimitiveSceneInfo::bShouldRenderAfterTranslucency` (PrimitiveSceneInfo.h:675, .cpp:305). Otherwise not strictly needed because `SceneVisibility.cpp` checks the proxy through the view-relevance chain.

### 7.4 No special "dithered LOD" or "Lumen" pass integration needed
Mobile does not use Nanite base pass, and Lumen is deferred-only. No integration needed there.

### 7.5 `EMarkMaskBits::StaticMeshVisibilityMapMask` does not need updating
The visibility map bit is for the static mesh visibility tracking. The new pass uses the same bit.

### 7.6 Subpass constraint summary
For the after-translucency pass to write depth inside the second subpass of `RenderForwardSinglePass`, either:
- The pass is re-scheduled in a new RDG add-pass (loses subpass merging), or
- Depth-write is disabled (the plan's current choice) — but this doesn't match the user's occlusion goal.

The user must decide this trade-off.

---

## 8. Final Verdict and Recommendations

### 8.1 What works as-is in the plan
1. **Step 1** (EMeshPass enum) — correct line numbers, correct static_assert math, correct insertion position.
2. **Step 2-3** (PrimitiveComponent + SceneProxy + Desc) — correct, complete for the proxy-level field.
3. **Step 6** (PrimitiveViewRelevance) — correct for the constructor, but needs extension if more proxy types use the feature.
4. **Step 7** (Stats) — correct line numbers.

### 8.2 What needs to be fixed
1. **Depth-stencil state** (Step 4): change `<false, ...>` to `<true, ...>` and `DepthRead_StencilRead` to `DepthWrite_StencilNop` if the user's occlusion goal is correct.
2. **`bAfterTranslucencyBasePass` member** (Step 4): redundant; use `MeshPassType` instead.
3. **SceneVisibility.cpp static-mesh branch** (Step 6): `MobileBasePassCSM` add must move into the `else` branch.
4. **SceneVisibility.cpp dynamic-mesh path**: not addressed at all; mirror the static-mesh fix.
5. **`FSceneRenderer::SetupMeshPass` skip-list** (SceneRendering.cpp:4209): must add `EMeshPass::MobileAfterTranslucencyPass` to the mobile skip list.
6. **Subpass strategy** for `RenderForwardSinglePass`: must decide between depth-write (loses subpass) or depth-read (matches plan but doesn't occlude).
7. **Decision required** for `RenderCustomRenderPassBasePass`, `RenderDeferredSinglePass`, `RenderDeferredMultiPass`.
8. **Decision required** for `RenderMobileEditorPrimitives`, `RenderMobileDebugView` calls in the new pass.
9. **`SetupPrecachePSOParams`** (PrimitiveComponent.cpp:4622): add `Params.bRenderAfterTranslucency = bRenderAfterTranslucency;`.
10. **`FPrimitiveSceneInfo` cache** (PrimitiveSceneInfo.h:675, .cpp:305): optional, depending on whether scene-info-level queries are needed.
11. **Other proxy classes** (Step 6): if any of Niagara, Particle, Landscape, Groom, etc. need the new pass, their `GetViewRelevance` must also be updated.

### 8.3 Risk assessment
- **Compilation risk**: Low. All line numbers and field placements are correct. The static_asserts will trigger if missed, but the plan handles them.
- **Functional risk**: High. The plan has 4 critical issues (depth state, dynamic-mesh path, skip-list, structural bug in scene-vis) that will cause the feature to either not work or work incorrectly.
- **Maintainability risk**: Low. The field-propagation pattern is well-established.

### 8.4 Recommended changes summary
1. **Fix depth-stencil state** in `CreateMobileAfterTranslucencyPassProcessor` (Step 4).
2. **Remove `bAfterTranslucencyBasePass` member**; use `InMeshPassType == EMeshPass::MobileAfterTranslucencyPass` instead (Step 4).
3. **Fix the SceneVisibility.cpp static-mesh if/else** so `MobileBasePassCSM` is NOT added when `bRenderAfterTranslucency` is true (Step 6).
4. **Add the dynamic-mesh path update** in `ComputeDynamicMeshRelevance` (Step 6 — MISSING).
5. **Add `EMeshPass::MobileAfterTranslucencyPass` to the mobile skip list** in `FSceneRenderer::SetupMeshPass` (SceneRendering.cpp:4209 — MISSING).
6. **Make a subpass strategy decision** and document it.
7. **Decide on the deferred/custom-render paths** and document.
8. **Update `SetupPrecachePSOParams`** at PrimitiveComponent.cpp:4622.
9. **Decide which other proxy classes** (if any) need `GetViewRelevance` updates.
