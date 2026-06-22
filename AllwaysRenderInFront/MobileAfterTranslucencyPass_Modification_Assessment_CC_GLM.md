# `MobileAfterTranslucencyPass` 修改方案评估

> 目标位置：`Engine/Source/Runtime/Renderer/Private/SceneVisibility.cpp:1556`
> 评估对象：在 mobile shading path 的静态网格可见性收集处，于 `BasePass` 之后追加一行 `AddCommandsForMesh(..., EMeshPass::MobileAfterTranslucencyPass)`
> 评估结论：**当前写法不可行**，且即便补齐缺失部分，**设计上也很可能不是预期效果**

---

## 1. 待评估的代码改动

### 原代码（`SceneVisibility.cpp:1556` 区域）

```cpp
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

### 修改后

```cpp
if (StaticMeshRelevance.bUseForMaterial && (ViewRelevance.bRenderInMainPass || ViewRelevance.bRenderCustomDepth))
{
    if (ShadingPath == EShadingPath::Mobile)
    {
        if (!StaticMeshRelevance.bUseSkyMaterial)
        {
            DrawCommandPacket.AddCommandsForMesh(..., EMeshPass::BasePass);
            DrawCommandPacket.AddCommandsForMesh(..., EMeshPass::MobileAfterTranslucencyPass);  // ← 新增
            if (!bMobileBasePassAlwaysUsesCSM)
            {
                DrawCommandPacket.AddCommandsForMesh(..., EMeshPass::MobileBasePassCSM);
            }
        }
        ...
    }
```

---

## 2. 问题分级

按 **"编译都过不去" → "能编但跑不起来" → "跑得起来但语义错"** 的顺序列出。

---

## 3. 阻塞性问题：`EMeshPass::MobileAfterTranslucencyPass` 在引擎中**不存在**

### 3.1 枚举定义证据

来自 `Engine/Source/Runtime/Renderer/Public/MeshPassProcessor.h:32-78`：

```cpp
namespace EMeshPass
{
    enum Type : uint8
    {
        DepthPass,
        SecondStageDepthPass,
        BasePass,
        AnisotropyPass,
        SkyPass,
        SingleLayerWaterPass,
        SingleLayerWaterDepthPrepass,
        CSMShadowDepth,
        VSMShadowDepth,
        Distortion,
        Velocity,
        TranslucentVelocity,
        TranslucencyStandard,
        TranslucencyStandardModulate,
        TranslucencyAfterDOF,
        TranslucencyAfterDOFModulate,
        TranslucencyAfterMotionBlur,
        TranslucencyAll,
        LightmapDensity,
        DebugViewMode,
        CustomDepth,
        MobileBasePassCSM,            // ← Mobile 专属仅此一个
        VirtualTexture,
        LumenCardCapture,
        LumenCardNanite,
        LumenTranslucencyRadianceCacheMark,
        LumenFrontLayerTranslucencyGBuffer,
        DitheredLODFadingOutMaskPass,
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
}
```

### 3.2 硬断言保护

`MeshPassProcessor.h:127-131`：

```cpp
#if WITH_EDITOR
    static_assert(EMeshPass::Num == 32 + 4, "Need to update switch(MeshPass) after changing EMeshPass");
#else
    static_assert(EMeshPass::Num == 32, "Need to update switch(MeshPass) after changing EMeshPass");
#endif
```

### 3.3 结论

- 全工程搜索 `MobileAfterTranslucency` —— **无任何命中**。
- 这段修改 **连编译都过不去**。

---

## 4. 即使把枚举加上，下列环节必须配套，否则只是"占了个坑位"

`EMeshPass` 不是单纯的标签，它是一组**注册表索引**。新增一个枚举值至少需要：

### 4.1 `MeshPassProcessor.h`

- 在枚举里追加 `MobileAfterTranslucencyPass`（`Num` 由 32 → 33；`NumBits = 6` 还够用）。
- 同步修改 `static_assert(EMeshPass::Num == 32, ...)` 和 GUID 注释。
- `GetMeshPassName()` 的 switch 加 `case`，否则末尾 `checkf(0, TEXT("Missing case ..."))` 触发。

### 4.2 PSO 注册路径

`MeshPassProcessor.h:2194-2207` `FPassProcessorManager::CreateMeshPassProcessor`：

```cpp
checkf(JumpTable[ShadingPathIdx][PassType] || DeprecatedJumpTable[ShadingPathIdx][PassType],
       TEXT("Pass type %u create function was never registered ..."));
```

- 必须通过 `FRegisterPassProcessorCreateFunction(... EShadingPath::Mobile, EMeshPass::MobileAfterTranslucencyPass, ...)` 静态注册。
- 否则一旦渲染该 pass，`checkf` 立即 crash。
- 现实做法：**新增一个 `FMobileAfterTranslucencyMeshPassProcessor`**（继承 `FMeshPassProcessor`），实现 shader 选择、blend/depth state、`TryAddMeshBatch` 等。最直接的起点是复制 `FMobileBasePassMeshProcessor` 并改 render state（关闭深度写、改 blend mode、绑定到透明之后的 RT）。

### 4.3 `FScene::ParallelMeshDrawCommandPasses[]`

该数组按 `EMeshPass::Num` 大小开。新增枚举后需检查 `FScene` 构造以及所有遍历 `EMeshPass::Num` 的位置（数量较多，全文搜索修整）。

### 4.4 真正的渲染调度

- 必须在 `MobileShadingRenderer.cpp` 的透明阶段之后插入：
  ```cpp
  View.ParallelMeshDrawCommandPasses[EMeshPass::MobileAfterTranslucencyPass].DispatchDraw(...);
  ```
- 否则 commands 被收集，但没有任何位置去绘制——`AddCommandsForMesh` 完全白调用，仅徒增 CPU 开销。

### 4.5 Relevance 标志

- 如果希望"只有部分 mesh"进入这个 pass（见 §5），必须新增 relevance 位，例如 `FStaticMeshBatchRelevance::bUseMobileAfterTranslucencyMaterial` 和 `FMaterialRelevance` 中对应位。
- 同时给 `FMaterial`/Material editor 暴露开关，否则只能"全打或全不打"。

---

## 5. 最关键的语义问题：当前写法会让**所有不透明物体**重复进入新 pass

当前位置（`SceneVisibility.cpp:1556`）的外层判断是：

```cpp
if (StaticMeshRelevance.bUseForMaterial && (ViewRelevance.bRenderInMainPass || ViewRelevance.bRenderCustomDepth))
```

`bUseForMaterial && bRenderInMainPass` 在 mobile 上**等同于"所有进 BasePass 的不透明 / Masked 几何"**。修改后的实际效果：

| 现象 | 影响 |
|------|------|
| 每个不透明 mesh 同时注册到 `BasePass` 与 `MobileAfterTranslucencyPass` | Draw call 数翻倍 |
| Mobile 是 TBDR 架构 | Vertex 吞吐与带宽几乎翻倍，明显掉帧 |
| 不透明几何在透明之后再画一遍 | 透明粒子被覆盖，alpha blend 失效，高光被"擦掉"，画面错乱 |
| 没有任何"何时只进新 pass、何时只进 BasePass"的区分逻辑 | 失去引入该 pass 的全部价值 |

也就是说——**目前的判断条件没有把"该不该进 AfterTranslucency"和"该不该进 BasePass"区分开**。

---

## 6. 合理的做法（按推测的需求）

如果目标是 **"让部分材质在 mobile 的透明阶段之后再渲染"**：

### 6.1 优先看现成机制能否满足

- Material 的 `BlendMode = Translucent` + `Translucency Pass = AfterDOF / AfterMotionBlur`。
- 引擎在 mobile 上已有 `EMeshPass::TranslucencyAfterDOF`、`EMeshPass::TranslucencyAfterMotionBlur`。
- 多数"透明之后渲染"需求其实可以通过现有 pass 实现，**无需扩展枚举**。

### 6.2 如果确实要新增"不透明 AfterTranslucency"

最小改动清单：

1. **Material 层**：在 `FMaterial` 加 `bMobileAfterTranslucency`（或类似）开关。
2. **Relevance 层**：在 `FStaticMeshBatchRelevance` / `FMaterialRelevance` 新增 `bUseMobileAfterTranslucencyMaterial` 位。
3. **枚举层**：在 `MeshPassProcessor.h` 添加 `EMeshPass::MobileAfterTranslucencyPass`，同步 `static_assert` 与 `GetMeshPassName`。
4. **Processor 层**：新建 `MobileAfterTranslucencyMeshPassProcessor.cpp`，实现 processor，并 `FRegisterPassProcessorCreateFunction` 注册到 `EShadingPath::Mobile`。
5. **SceneVisibility 层**：把改动写成 **互斥分支**，而非无条件追加：

   ```cpp
   if (StaticMeshRelevance.bUseMobileAfterTranslucencyMaterial)
   {
       DrawCommandPacket.AddCommandsForMesh(..., EMeshPass::MobileAfterTranslucencyPass);
       // 注意：该材质通常就不该再进 BasePass，否则会画两遍
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

6. **Renderer 层**：在 `MobileShadingRenderer` 的透明阶段之后 dispatch 该 pass。

---

## 7. 总结

| 项 | 状态 |
|----|------|
| 当前两行新代码是否能编译 | ❌ `EMeshPass::MobileAfterTranslucencyPass` 不存在 |
| 是否能仅靠新增枚举值跑起来 | ❌ 还需 processor 注册、`FScene` 适配、renderer dispatch |
| 业务逻辑是否正确 | ❌ 当前会把所有不透明物体重复注册到新 pass，性能与画面双崩 |
| 推荐路径 | 先看 `TranslucencyAfterDOF/AfterMotionBlur` 能否覆盖需求；否则按 §6.2 完整改动链落地，并用 **材质开关 + relevance 位** 让新 pass 与 BasePass **互斥** |

---

## 8. 参考文件路径

| 文件 | 关键内容 |
|------|----------|
| `Engine/Source/Runtime/Renderer/Public/MeshPassProcessor.h` | `EMeshPass::Type` 枚举、`GetMeshPassName`、`FPassProcessorManager` |
| `Engine/Source/Runtime/Renderer/Private/SceneVisibility.cpp` | `AddCommandsForMesh` 调用点（本次修改位置） |
| `Engine/Source/Runtime/Renderer/Private/MobileShadingRenderer.cpp` | Mobile 渲染管线调度，新 pass 的 dispatch 应在此处插入 |
| `Engine/Source/Runtime/Renderer/Private/MobileBasePassRendering.{h,cpp}` | `FMobileBasePassMeshProcessor`，可作为新 processor 的参考起点 |
