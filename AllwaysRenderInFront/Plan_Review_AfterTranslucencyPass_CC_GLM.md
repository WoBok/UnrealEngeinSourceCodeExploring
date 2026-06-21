# Plan 审查：MobileAfterTranslucencyPass（透明物体后再渲染指定物体）

> 源文件：`Engine/Docs/Plan.md`（用户计划）
> 审查对象：UE 5.4 引擎源码，Android VR，Forward（ES3.1）渲染路径
> 审查范围：`SceneVisibility.cpp`、`MeshPassProcessor.h`、`PrimitiveSceneInfo.cpp`、`MobileBasePass.cpp`
> 后缀说明：`CC_GLM` 为本次 Claude Code（GLM 模型）审查标记

---

## 一、直接结论

**方案整体可行**，思路（复用 `FMobileBasePassMeshProcessor` + 新增 `EMeshPass` + 半透明后 `DispatchDraw`）是 UE 标准扩展方式，深度共享也合理。

**但计划不能直接跑通**，存在 3 个致命问题，其中最关键的是：

> 🔴 **必须在 `ComputeRelevance` 中修改逻辑** —— 这是计划中完全遗漏、最关键的一步。不加则 `RenderMobileAfterTranslucencyPass` 的 `DispatchDraw` 永远什么都不画。

---

## 二、为什么必须改 `ComputeRelevance`

### 2.1 机制链路

新增一个 `EMeshPass` 并通过 `REGISTER_MESHPASSPROCESSOR_AND_PSOCOLLECTOR(..., CachedMeshCommands | MainView)` 注册，只完成了两件事：

1. **Pass 处理器注册**到 `FPassProcessorManager`。
2. **缓存命令自动构建**：`PrimitiveSceneInfo.cpp:476` 的 `CacheMeshDrawCommands` 中有循环
   ```cpp
   for (int32 PassIndex = 0; PassIndex < EMeshPass::Num; PassIndex++)
   {
       if ((FPassProcessorManager::GetPassFlags(ShadingPath, PassType) & EMeshPassFlags::CachedMeshCommands) != EMeshPassFlags::None)
       {
           // 自动调用对应处理器的 AddMeshBatch 建缓存
       }
   }
   ```
   所以 Plan 中的 `AddMeshBatch` 过滤逻辑（基于 `ShouldRenderAfterTranslucency`）**在注册时就会生效**，缓存里确实会有/没有对应命令。

**但是**，缓存建好 ≠ 会被画。`ComputeRelevance` 才是“把缓存命令引用进可见列表”的地方。

### 2.2 决定性证据

`SceneVisibility.cpp:1074` `AddCommandsForMesh` → 写入 `VisibleCachedDrawCommands[PassType]`：
```cpp
VisibleCachedDrawCommands[(uint32)PassType].AddUninitialized();
```

而 `ComputeRelevance`（1299–1958）对 Pass 是**显式逐个枚举**的——1516–1693 行每一个 `AddCommandsForMesh(..., EMeshPass::XXX)` 都是手写一行，**不存在“自动遍历所有已注册 Pass”的逻辑**。

Plan 中没有任何一处为 `MobileAfterTranslucencyPass` 调用 `AddCommandsForMesh`，因此：
- `View.ParallelMeshDrawCommandPasses[EMeshPass::MobileAfterTranslucencyPass]` 的可见命令列表**永远为空**；
- `RenderMobileAfterTranslucencyPass` 里 `DispatchDraw` → **什么都不画**。

### 2.3 需要在 `ComputeRelevance` 里加什么

在 Mobile 分支（约 1559 行 `if (ShadingPath == EShadingPath::Mobile)` 块内，`bUseForMaterial && (bRenderInMainPass || bRenderCustomDepth)` 条件成立处），对标记物体追加：

```cpp
DrawCommandPacket.AddCommandsForMesh(PrimitiveIndex, PrimitiveSceneInfo, StaticMeshRelevance, StaticMesh,
    CullingPayloadFlags, Scene, bCanCache, EMeshPass::MobileAfterTranslucencyPass);
```

并用 `PrimitiveSceneProxy->ShouldRenderAfterTranslucency()` 做 gate（只给标记物体加）。

> **原理**：对正常 BasePass，虽然 `ComputeRelevance` 仍会调 `AddCommandsForMesh(BasePass)`，但因为 `AddMeshBatch` 过滤使标记物体**没有** BasePass 缓存命令，`GetStaticMeshCommandInfoIndex(BasePass)` 返回 -1，什么也不会加。所以“不透明阶段不画”靠缓存过滤，“透明后画”靠新加的这一行——两者配合才完整。

> **前提**：标记物体的 `bRenderInMainPass` 必须保持 `true`（见 §4.3），否则外层 `bRenderInMainPass || bRenderCustomDepth` 条件不成立，根本进不到这个块。

---

## 三、致命问题

### 3.1 枚举放置位置错误（编译/逻辑 bug）

Plan 把新 Pass 放在 `Num` **之后**：
```cpp
        Num,
        NumBits = 6,
        MobileAfterTranslucencyPass,   // ← 错！
```

`Num` 是计数哨兵，全引擎所有 `for (PassIndex < EMeshPass::Num)` 循环都依赖它。放在 `Num` 之后，该 Pass 索引 == `Num`，**永远不被缓存循环遍历到**，`ComputeRelevance` 即使加了调用也引用不到正确缓存。

**正确做法**：放在 `Num` **之前**（非编辑器区段，例如 `WaterInfoTexturePass,` 之后）：
```cpp
        WaterInfoTexturePass,
        MobileAfterTranslucencyPass,   // ← 放这里
#if WITH_EDITOR
        ...
#endif
        Num,
        NumBits = 6,
```

### 3.2 `static_assert` 计数与 GUID 未正确更新

仓库真实值（`MeshPassProcessor.h:128-131`）：
```cpp
#if WITH_EDITOR
    static_assert(EMeshPass::Num == 32 + 4, ...);  // 非编辑器 32
#else
    static_assert(EMeshPass::Num == 32, ...);
#endif
```

加一个非编辑器 Pass 后应为 **33 / 33 + 4**。Plan 注释写了 33，但枚举位置错会导致 `Num` 仍为 32 → 断言失败。

**修复**：按 §3.1 修正位置后，把两处断言改成 `33` 与 `33 + 4`，并按注释要求**改掉那个 GUID**（`{674D7D62-...}`），否则 auto-resolve 检测会失败。

---

## 四、重要问题

### 4.1 Mobile Deferred Shading 路径不适用

`FMobileBasePassMeshProcessor` 有 `bDeferredShading / bPassUsesDeferredShading` 分支。若目标平台开了 Mobile Deferred（GBuffer），BasePass 写的是 GBuffer，而半透明是在 GBuffer 合成到 SceneColor **之后**做的。在半透明后再跑一次“类 BasePass”会写 GBuffer 但不会被正确合成 → 结果错乱。

Android VR 大概率是 Forward（ES3.1），但**务必**加守卫：
- 在 `RenderMobileAfterTranslucencyPass` 或处理器创建处，`IsMobileDeferredShadingEnabled(...)` 为 true 时直接跳过。

### 4.2 Plan 中构造函数代码有括号语法错误（潜在）

`MobileBasePass.cpp` 构造函数初始化列表 Plan 片段：
```cpp
    , bPassUsesDeferredShading(bDeferredShading && !bTranslucentBasePass
    , bAfterTranslucencyBasePass(IsAfterTranslucencyBasePass))
```
`bPassUsesDeferredShading` 的右括号缺失，`bAfterTranslucencyBasePass` 被吞进了它的参数。应改为：
```cpp
    , bPassUsesDeferredShading(bDeferredShading && !bTranslucentBasePass)
    , bAfterTranslucencyBasePass(IsAfterTranslucencyBasePass)
```
实际实现时核对原文件，确保括号配对。

### 4.3 `bRenderInMainPass` 与可见性条件的耦合

设计选了“保留 `bRenderInMainPass=true`，靠 `AddMeshBatch` 过滤掉 BasePass 缓存”。注意：
- `ComputeRelevance` 的 Mobile BasePass 块外层条件是
  `StaticMeshRelevance.bUseForMaterial && (ViewRelevance.bRenderInMainPass || ViewRelevance.bRenderCustomDepth)`。
- `MobileAfterTranslucencyPass` 的 `AddCommandsForMesh` 必须放在此条件**成立**的路径内，所以 `bRenderInMainPass` 必须为 true。Plan 未说明，易踩坑。
- 若有人把 `bRenderInMainPass` 设 false 试图“不画在不透明阶段”，会导致标记物体连 AfterTranslucency 也不画。建议在 setter/文档注明：标记物体应保持 `bRenderInMainPass=true`。

### 4.4 深度/混合状态——基本正确，VR 下需评估写入策略

Plan 用：
- `TStaticBlendStateWriteMask<CW_RGBA>`（不透明、无混合）
- `TStaticDepthStencilState<true, CF_DepthNearOrEqual>`（深度测试+写入）

逻辑上：标记物体与不透明物体共享深度缓冲，半透明通常不写深度，所以标记物体会通过深度测试覆盖在半透明之上——**符合目的**。

需确认：
- 半透明 pass 之后、本 pass 之前，深度缓冲仍是 BasePass 写入的不透明深度（未被半透明清/改）。Mobile forward 一般满足。
- VR 双目：若担心标记物体写深度影响后续（后期、下一只眼），可考虑 `TStaticDepthStencilState<true, CF_DepthNearOrEqual, /*bWriteDepth=*/false>`（只测不写）。值得评估。

---

## 五、一般问题

### 5.1 缺少统计量声明

Plan 用了 `STAT_AfterTranslucencyDrawTime`、`SCOPED_GPU_STAT(RHICmdList, AfterTranslucency)`、`CSV_SCOPED_TIMING_STAT_EXCLUSIVE(RenderAfterTranslucency)`。这些宏引用的 stat 需在某处 `DECLARE_STATS`/CSV 定义，否则编译失败。参照 `STAT_BasePassDrawTime` 声明位置补一套。

### 5.2 SkyMaterial / CSM 子分支

`CreateMobileAfterTranslucencyPassProcessor` 复用了 `CreateMobileBasePassProcessor` 的 Flags（含 `CanReceiveCSM`）。标记物体若是普通不透明材质没问题；若误标 sky material，会进入 BasePass 的 sky 特殊分支。建议标记物体限定为普通不透明 mesh，或在 `AddMeshBatch` 里对 sky material 早退。

---

## 六、已正确 / 无需改动之处

- ✅ **动态（非缓存）路径同样生效**：`AddCommandsForMesh` 在 `bCanCache=false` 时走 `DynamicBuildRequests[PassType]`（`SceneVisibility.cpp:1124-1129`），最终也调用同一处理器的 `AddMeshBatch`，过滤对动态物体（dither/距离淡出）有效，无需额外处理。
- ✅ **VR 双目两个入口都加了**：`RenderForwardSinglePass` 和 `RenderForwardMultiPass` 均加了 `RenderMobileAfterTranslucencyPass` 调用。需确认 `PassParameters->InstanceCullingDrawParams` 在 `RenderTranslucency` 之后仍有效（同一 view 内通常有效）。
- ✅ **Component/Proxy/Setter 字段链路**（Plan 第 2、3 步）方向正确，`MarkRenderStateDirty()` 会在下次缓存重建时让 `AddMeshBatch` 过滤生效。

---

## 七、修改清单总览

| 优先级 | 遗漏/问题 | 位置 |
|---|---|---|
| 🔴 致命 | `ComputeRelevance` 未加 `AddCommandsForMesh(MobileAfterTranslucencyPass)` | `SceneVisibility.cpp` Mobile BasePass 块内 |
| 🔴 致命 | 枚举放在 `Num` 之后 → 必须移到 `Num` 之前 | `MeshPassProcessor.h:32` |
| 🔴 致命 | `static_assert` 计数与 GUID 未正确更新（33 / 33+4） | `MeshPassProcessor.h:128-131` |
| 🟠 重要 | Mobile Deferred 路径需加守卫跳过 | `RenderMobileAfterTranslucencyPass` / 处理器创建 |
| 🟠 重要 | 构造函数括号语法错误 | `MobileBasePass.cpp` |
| 🟠 重要 | `bRenderInMainPass` 必须保持 true，AfterTranslucency 调用要在其成立分支内 | `ComputeRelevance` + setter 文档 |
| 🟡 一般 | `STAT_*` / CSV 统计量未声明 | stat 声明文件 |
| 🟡 一般 | 深度写策略在 VR 双目下需评估 | `CreateMobileAfterTranslucencyPassProcessor` |
| 🟡 一般 | SkyMaterial / CSM 子分支需排除 sky | `AddMeshBatch` 或使用约束 |

---

## 八、建议的 `ComputeRelevance` 改动示意

在 Mobile 分支 `bUseForMaterial && (bRenderInMainPass || bRenderCustomDepth)` 块内（参照 1556–1628 行附近的 BasePass 注册区），追加：

```cpp
// RenderAfterTranslucency Added: 在半透明后再绘制标记物体
if (PrimitiveSceneProxy->ShouldRenderAfterTranslucency()
    && StaticMeshRelevance.bUseForMaterial
    && ViewRelevance.bRenderInMainPass
    && !StaticMeshRelevance.bUseSkyMaterial)
{
    DrawCommandPacket.AddCommandsForMesh(
        PrimitiveIndex, PrimitiveSceneInfo,
        StaticMeshRelevance, StaticMesh,
        CullingPayloadFlags, Scene, bCanCache,
        EMeshPass::MobileAfterTranslucencyPass);
}
```

> 同时确保 `RenderMobileAfterTranslucencyPass` 在 `IsMobileDeferredShadingEnabled(...)` 时提前 return。
