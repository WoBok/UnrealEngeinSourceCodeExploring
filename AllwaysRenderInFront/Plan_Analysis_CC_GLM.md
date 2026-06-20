# RenderAfterTranslucency 计划分析

> 针对 `Engine/Docs/Plan.md` 的可行性分析。分析基于 UE5.4 (branch 5.4) 实际源码核对。
> 分析方式：并行子 Agent 核对每个涉及文件 + 直接核对关键代码行。

---

## 0. 结论先行

计划的整体思路是**可行且方向正确**的：复用 `FMobileBasePassMeshProcessor`、新增一个 `EMeshPass`、在 `RenderTranslucency` 之后再做一次绘制，是 UE 移动渲染器里实现“指定物体覆盖在透明物体之上”的合理路径。但**计划以当前形态直接落地无法编译/无法工作**，存在以下几类问题：

1. **致命编译错误**：`EMeshPass` 枚举新值位置错误（放在 `Num` 之后），且 `static_assert` 当前值被误判（实际是 `32`，不是 `33`）。
2. **致命编译错误**：构造函数初始化列表存在括号缺失语法错误。
3. **致命编译错误**：`InProxyDesc.bRenderAfterTranslucency` 引用了 `FPrimitiveSceneProxyDesc` 中不存在的字段。
4. **致命编译错误**：`RenderMobileAfterTranslucencyPass` 未在 `FMobileSceneRenderer` 头文件中声明。
5. **功能失效（最隐蔽）**：新 pass 从未被 `BuildRenderingCommands`，`DispatchDraw` 不会画出任何东西。
6. **功能失效**：`SceneVisibility.cpp` 的可见性门控会使得 `bRenderInMainPass=false` 的物体在所有 pass 都不可见，计划的 `AddMeshBatch` 过滤不足以补救。
7. **概念性误解**：用户对“谁不写深度”的描述与实际相反（实际是透明不写深度，不透明写深度）。
8. **潜在风险**：`RenderForwardMultiPass` 的第二 pass 深度为只读，新 pass 的深度写状态可能无效/校验失败。
9. **遗漏的配套修改**：`DeferredSinglePass/DeferredMultiPass` 路径、统计宏、`bRenderInDepthPass` 协同等。

下面逐条详述，并给出修正建议。

---

## 1. EMeshPass 枚举与 static_assert（计划第 1 步）

### 1.1 枚举新值位置错误 —— 致命

计划把新值放在 `Num` 与 `NumBits` **之后**：

```c++
        Num,
        NumBits = 6,
        MobileAfterTranslucencyPass,//RenderAfterTranslucency Added
    };
```

**问题**：`EMeshPass::Num` 的值由它**之前**的枚举项数量决定。新值放在 `Num` 之后，`Num` 仍然是 `32`，**不会变成 33**。后果：

- `ParallelMeshDrawCommandPasses` 是 `TStaticArray<..., EMeshPass::Num>`（`SceneRendering.h:1362`），下标 `EMeshPass::MobileAfterTranslucencyPass` 其值等于 `Num`（32），**越界访问**。
- 计划把 `static_assert` 改成 `EMeshPass::Num == 33`，而实际 `Num` 仍是 32 → **编译失败**（这反而是好事，能挡住越界）。

**修正**：新值必须放在 `Num` **之前**（主列表内，`WaterInfoTexturePass` 之后、`#if WITH_EDITOR` 之前）：

```c++
        WaterInfoTexturePass,
        MobileAfterTranslucencyPass,   // RenderAfterTranslucency Added
#if WITH_EDITOR
        HitProxy,
        ...
#endif
        Num,
        NumBits = 6,
```

### 1.2 static_assert 当前值误判

计划称“更新底部断言 `EMeshPass::Num == 33 + 4` 与 `== 33`”，暗示当前是 `33`。**实际核对**（`MeshPassProcessor.h:128/130`）：

```c++
static_assert(EMeshPass::Num == 32 + 4, ...);  // WITH_EDITOR
static_assert(EMeshPass::Num == 32, ...);       // 非 editor
```

当前是 **32**，新增一个枚举项后应为 **33**。计划改写成 `33 + 4` / `33` 是正确的目标值，但前提是 1.1 的位置修正成立。修正 1.1 后，把两个 assert 改为 `33 + 4` / `33` 即可。**务必同步更新 GUID 注释**（`{674D7D62-...}`），否则该 GUID 本意是防止自动合并误改。

### 1.3 NumBits

`NumBits = 6` 可表示 0–63，33 个值仍在范围内，**无需改动**。

### 1.4 GetMeshPassName

计划在此函数中新增 `case EMeshPass::MobileAfterTranslucencyPass` 是**必需**的：函数末尾有 `checkf(0, TEXT("Missing case for EMeshPass %u")...)`（`MeshPassProcessor.h:133`），漏 case 会触发致命断言。计划处理正确。

### 1.5 跳转表

`FPassProcessorManager::CreateMeshPassProcessor`（`MeshPassProcessor.h:2194`）是 `JumpTable[ShadingPath][PassType]`，新 pass 只要通过 `REGISTER_MESHPASSPROCESSOR_AND_PSOCOLLECTOR` 注册即可自动可用，无需改跳转表。✅

---

## 2. 构造函数语法错误 —— 致命

计划第 4 步的构造函数初始化列表（`MobileBasePass.cpp` ~810）：

```c++
       , bPassUsesDeferredShading(bDeferredShading && !bTranslucentBasePass
       , bAfterTranslucencyBasePass(IsAfterTranslucencyBasePass))//RenderAfterTranslucency Added
```

**问题**：`bPassUsesDeferredShading(...)` 的右括号被吞掉了，实际当前代码（`MobileBasePass.cpp:824`）是：

```c++
, bPassUsesDeferredShading(bDeferredShading && !bTranslucentBasePass)
```

计划写法会导致 `bPassUsesDeferredShading(` 的括号未闭合 → **编译失败**。

**修正**：

```c++
, bPassUsesDeferredShading(bDeferredShading && !bTranslucentBasePass)
, bAfterTranslucencyBasePass(IsAfterTranslucencyBasePass)
```

---

## 3. PrimitiveComponent / PrimitiveSceneProxy 字段（计划第 2、3 步）

### 3.1 行号与写法核对 —— 基本正确

- `bRenderInMainPass` 声明：`PrimitiveComponent.h:406-408` ✅
- `SetRenderInMainPass` 声明：`PrimitiveComponent.h:1916-1918` ✅
- 构造函数初始化 `bRenderInMainPass = true`：`PrimitiveComponent.cpp:333`（**计划未提及**，需为新 bit 字段补初始化，否则某些编译器/静态分析会告警未初始化）
- `SetRenderInMainPass` 实现：`PrimitiveComponent.cpp:4457-4464` ✅
- Proxy 头 `bRenderInMainPass`：`PrimitiveSceneProxy.h:1200` ✅；`ShouldRenderInMainPass()`：`PrimitiveSceneProxy.h:700` ✅
- `.cpp` 从 `InComponent` 拷贝：`PrimitiveSceneProxy.cpp:277` ✅
- `.cpp` 从 `InProxyDesc` 拷贝：`PrimitiveSceneProxy.cpp:428` ✅

计划新增的 `bRenderAfterTranslucency` / `SetRenderAfterTranslucency` / `ShouldRenderAfterTranslucency` 模仿 `bRenderInMainPass` 的写法是正确的。

### 3.2 遗漏：FPrimitiveSceneProxyDesc 字段 —— 致命

计划在 `PrimitiveSceneProxy.cpp:428` 写了：

```c++
bRenderAfterTranslucency(InProxyDesc.bRenderAfterTranslucency),
```

但 `FPrimitiveSceneProxyDesc`（`PrimitiveSceneProxyDesc.h`）中**只有 `bRenderInMainPass`**（`:93`，默认值 `:25`），**没有 `bRenderAfterTranslucency` 字段** → **编译失败**。

**修正**：在 `PrimitiveSceneProxyDesc.h` 中补：

```c++
uint32 bRenderAfterTranslucency : 1;   // 并在默认构造处 bRenderAfterTranslucency = false;
```

同时确认 `PrimitiveSceneProxyDesc` 的“从 Component 填充”逻辑（通常在 `FPrimitiveSceneProxyDesc` 的 build 处）也拷贝该字段，否则 `InProxyDesc` 路径下值恒为 false。

### 3.3 遗漏：构造函数初始化

新 bit 字段 `bRenderAfterTranslucency` 应在 `UPrimitiveComponent` 构造函数中显式置 `false`（与 `bRenderInMainPass = true` 同处，`PrimitiveComponent.cpp:333` 附近）。计划未提及。

---

## 4. 可见性门控 —— 最隐蔽的功能失效（计划未涉及）

这是计划**最大的概念性遗漏**。即使第 5 步的 pass 注册与 DispatchDraw 全部正确，物体也可能根本进不了绘制。

### 4.1 SceneVisibility 的硬门控

`SceneVisibility.cpp` 中，静态/动态 mesh 元素是否进入任何 draw command 包，受 `bRenderInMainPass` 门控：

- 静态：`:1501` `&& (ViewRelevance.bRenderInMainPass || bRenderCustomDepth || bRenderInDepthPass)`
- 动态：`:2198` `if (ViewRelevance.bDrawRelevance && (bRenderInMainPass || bRenderCustomDepth || bRenderInDepthPass))`
- BasePass 专门：`:2211` `if (bRenderInMainPass || bRenderCustomDepth)`
- 透明计数：`:1860`、静态透明 `:1637`

**含义**：若 `bRenderInMainPass=false` 且 `bRenderCustomDepth=false` 且 `bRenderInDepthPass=false`，物体在**所有 pass**（包括你新建的 after-translucency pass）都不可见，根本到不了 `AddMeshBatch`。计划的 `AddMeshBatch` 过滤发生在更晚的阶段，**无法补救**这一步的剔除。

### 4.2 计划 AddMeshBatch 的隐含约束

计划在 `AddMeshBatch` 中保留了原有 `!ShouldRenderInMainPass()` 早退（在 after-translucency 分支之后）：

```c++
if (bAfterTranslucencyBasePass) { if (!bShouldRenderAfterTranslucency) return; }
else { if (bShouldRenderAfterTranslucency) return; }
if (... || (PrimitiveSceneProxy && !PrimitiveSceneProxy->ShouldRenderInMainPass())) return;
```

这意味着 after-translucency 物体**必须 `bRenderInMainPass=true`** 才能在 after-translucency pass 渲染（否则被这行挡掉），同时也满足了 4.1 的可见性门控。

### 4.3 推论与设计冲突

若采用 `bRenderInMainPass=true` + `bRenderAfterTranslucency=true`：

- ✅ after-translucency pass 会渲染该物体（base pass 的 `AddMeshBatch` 因 `else` 分支早退而跳过它，不会在 base pass 画颜色）。
- ⚠️ 但该物体仍会进入 **DepthPass、Velocity、其它未加过滤的 pass**。用户目标是“在不透明物体渲染阶段不进行渲染”，而 DepthPass 仍会写它的深度，可能与“不渲染”的语义冲突（深度被写入会影响后续透明排序/覆盖判断）。

**建议二选一**：

- **方案 A（最小改动，推荐）**：保持 `bRenderInMainPass=true`、`bRenderInDepthPass=false`（在组件上），仅用 base pass 的 `AddMeshBatch` 过滤跳过 base pass 颜色。这样 DepthPass 也不画它（`ShouldRenderInDepthPass` 依赖 `bRenderInMainPass||bRenderInDepthPass`，MainPass=true 仍会画 DepthPass……需确认）。若要彻底不进 DepthPass，需在 DepthPass processor 也加同样过滤，或单独处理。
- **方案 B（更干净）**：修改 `SceneVisibility.cpp` 的门控，把 `bRenderAfterTranslucency` 作为额外的可见性理由（`bRenderInMainPass || bRenderCustomDepth || bRenderInDepthPass || bRenderAfterTranslucency`），并让 after-translucency 物体设 `bRenderInMainPass=false`。改动面更大、跨 ShadingPath，但语义最清晰，且不会污染 DepthPass/Velocity。

无论哪种，**计划当前都未处理可见性门控，必须补**，否则要么物体不显示，要么在不透明阶段被意外绘制。

---

## 5. 新 pass 从未被 BuildRenderingCommands —— 致命功能失效（计划未涉及）

`View.ParallelMeshDrawCommandPasses[EMeshPass::X]` 必须先调用 `BuildRenderingCommands(...)` 才能在 `DispatchDraw` 时有内容。移动端每帧的构建在 `MobileShadingRenderer.cpp`：

```
:1439  DepthPass
:1441  BasePass
:1442  SkyPass
:1443  StandardTranslucencyMeshPass
:1444  DebugViewMode
```

**新 pass `MobileAfterTranslucencyPass` 不在此列表中**，因此 `DispatchDraw` 画出的是空集 → 功能完全失效。

**修正**：在 `:1443` 附近补：

```c++
View.ParallelMeshDrawCommandPasses[EMeshPass::MobileAfterTranslucencyPass]
    .BuildRenderingCommands(GraphBuilder, Scene->GPUScene, AfterTranslucencyInstanceCullingDrawParams);
```

并需要为其准备 `FInstanceCullingDrawParams`（可复用 `PassParameters->InstanceCullingDrawParams`，或新建一个 `AfterTranslucencyInstanceCullingDrawParams` 并在 `BuildInstanceCullingDrawParams` 中按 `EMeshPass::MobileAfterTranslucencyPass` 构建）。

> 注意：`BuildRenderingCommands` 的调用点在两个上下文都存在（`:824/:885` 与 `:1439-1444`），需确认 VR 双目/多视图路径用的是哪一组，确保两处都补上，或在公共入口补。

---

## 6. RenderMobileAfterTranslucencyPass 声明缺失 —— 致命编译错误（计划未涉及）

计划在 `MobileBasePassRendering.cpp:492` 实现 `FMobileSceneRenderer::RenderMobileAfterTranslucencyPass`，但**未在头文件 `FMobileSceneRenderer` 中声明**。该类声明在 `SceneRendering.h`（`RenderMobileBasePass` 声明于 `:2695`，`RenderTranslucency` 于 `:2734`）。

**修正**：在 `SceneRendering.h` 的 `FMobileSceneRenderer` 内补声明：

```c++
void RenderMobileAfterTranslucencyPass(FRHICommandList& RHICmdList, const FViewInfo& View, const FInstanceCullingDrawParams* InstanceCullingDrawParams);
```

否则 `MobileShadingRenderer.cpp` 中的调用编译失败。

---

## 7. 深度状态与“覆盖透明”语义核对（计划第 4、5 步）

### 7.1 概念纠正：谁不写深度

用户原文：“因为不透明物体不写入深度，所以我指定的物体可以覆盖透明物体”。

**实际源码相反**：

- 不透明 BasePass：`TStaticDepthStencilState<true, CF_DepthNearOrEqual>`（`MobileBasePass.cpp:1157`）→ **写深度**。
- 透明：`TStaticDepthStencilState<false, CF_DepthNearOrEqual>` + `DepthRead_StencilRead`（`MobileBasePass.cpp:1187` 等）→ **不写深度**，只读。

因此“覆盖透明”之所以可行，是因为**透明不写深度**，深度缓冲里仍是不透明深度，after-translucency 物体针对不透明深度做测试即可。计划复制的深度状态（`true, CF_DepthNearOrEqual`，写深度）能达成覆盖，但**用户对原理的表述是反的**，建议在文档/注释中纠正以免误导后续维护。

### 7.2 覆盖语义是否真正达成

after-translucency 物体用 `CF_DepthNearOrEqual`（`z <= 深度缓冲=不透明深度`）+ 写深度：

- 物体在不透明之前（z 更小）：测试通过 → 画出，覆盖透明颜色。✅
- 物体在不透明之后（z 更大）：测试失败 → 不画，被不透明正确遮挡。✅
- 由于透明不写深度，即使透明面在 after-物体更前方，after-物体仍会画上去（透明没留下深度）→ 这正是“覆盖透明”的预期行为。✅

**结论**：覆盖语义在 Forward/SinglePass 路径下成立。但要注意：

- **混合**：计划用 `TStaticBlendStateWriteMask<CW_RGBA>`（不透明直写，不混合）。after-物体会**硬替换**透明像素颜色，不会与透明做 alpha 混合。若用户希望“半透明覆盖”，需改 blend state。当前“覆盖”目标下 OK。
- **深度写副作用**：after-物体写深度。它是场景最后一个绘制 pass（后处理之前），一般无害；但在 `RenderForwardMultiPass` 见第 8 节。

### 7.3 建议改进深度状态

为降低风险（见第 8 节 MultiPass 只读深度）并避免不必要的深度写，建议 after-translucency pass 使用**只读深度**：

```c++
PassDrawRenderState.SetDepthStencilState(TStaticDepthStencilState<false, CF_DepthNearOrEqual>::GetRHI());
PassDrawRenderState.SetDepthStencilAccess(FExclusiveDepthStencil::DepthRead_StencilRead);
```

只读深度不影响覆盖效果（测试仍针对不透明深度），且与 MultiPass pass2 的只读深度附件兼容，避免 RHI 校验错误。

---

## 8. RenderForwardMultiPass 的只读深度风险（计划未涉及）

`RenderForwardMultiPass` 的第二 pass（`"DecalsAndTranslucency"`，`MobileShadingRenderer.cpp:1721`）深度附件为**只读**：

```c++
:1700  FExclusiveDepthStencil::Type ExclusiveDepthStencil = DepthRead_StencilRead;
:1716  SecondPassParameters->RenderTargets.DepthStencil.SetDepthStencilAccess(ExclusiveDepthStencil);
```

计划在 `:1735`（`RenderTranslucency` 之后）插入 `RenderMobileAfterTranslucencyPass`，而该 pass 的 mesh command 深度状态是 `true`(写深度)。在只读深度子 pass 中写深度：

- Vulkan/部分 RHI 后端可能触发**校验错误**或写被静默丢弃。
- 即便写被丢弃，覆盖效果仍成立（深度测试是读），但行为不可移植。

**建议**：采用 7.3 的只读深度状态，即可消除该风险，且在 SinglePass/MultiPass 行为一致。

### 8.1 VR 路径选择

Android VR 移动端通常走 `RenderForwardSinglePass` 或 `RenderForwardMultiPass`（取决于 MSAA/分通道）。计划只在这两个 Forward 路径插入。但移动端还有 **DeferredSinglePass**（`MobileShadingRenderer.cpp:1985`）与 **DeferredMultiPass**（`:2068`）也调用 `RenderTranslucency`。若 VR 项目启用 Mobile Deferred（`IsMobileDeferredShadingEnabled`），计划遗漏了这两条路径 → after-translucency 不会执行。需确认项目实际 ShadingPath，按需补 Deferred 路径的调用。

---

## 9. 其它遗漏与配套项

### 9.1 统计宏

计划使用：

```c++
SCOPE_CYCLE_COUNTER(STAT_AfterTranslucencyDrawTime);
SCOPED_GPU_STAT(RHICmdList, AfterTranslucency);
CSV_SCOPED_TIMING_STAT_EXCLUSIVE(RenderAfterTranslucency);
```

这些统计量默认不存在，需 `DECLARE_CYCLE_STAT`/`DECLARE_GPU_STAT`/CSV 注册，否则**编译失败**。建议先复用现有统计（如 BasePass 的）或补声明。

### 9.2 PSO/Shader 编译

after-translucency pass 复用 `FMobileBasePassMeshProcessor` 且 `InTranslucencyPassType=TPT_MAX` → `bTranslucentBasePass=false` → `Process()` 走 **不透明 base pass 着色器**（`MobileBasePass::GetShaders` 选 `TMobileBasePassPS/VS`，不依赖 `EMeshPass`）。✅ 着色器选择正确，无需为新 pass 单独写 shader。

但 `EMeshPassFlags::CachedMeshCommands` 意味着会为带 `bRenderAfterTranslucency` 的图元缓存静态 mesh command，触发对应材质的不透明 base pass PSO 编译。需确认这些材质在目标 shader platform（Android GLES/Vulkan）上能正常编译 base pass shader（通常没问题）。

### 9.3 ShouldRenderAfterTranslucency 的全局使用面

`bRenderInMainPass` 在引擎中被读取约 30+ 处（`SceneManagement.cpp:340` 的 `FMeshBatchAndRelevance`、`PrimitiveSceneInfo.cpp:305`、各 mesh render 的 `Result.bRenderInMainPass` 等）。`bRenderAfterTranslucency` 目前仅计划在 `MobileBasePass.cpp` 的 `AddMeshBatch` 内使用一处——**对于最小实现是够的**（因为 after-translucency 物体仍 `bRenderInMainPass=true`，走主路径可见性）。无需在所有 `bRenderInMainPass` 使用处镜像添加。仅需注意 4.3 的 DepthPass 污染问题。

### 9.4 序列化/蓝图

`bRenderAfterTranslucency` 作为 `UPROPERTY` 自动序列化、`SetRenderAfterTranslucency` 作为 `UFUNCTION(BlueprintCallable)` 可蓝图调用。✅ 计划处理正确。

### 9.5 单 pass 内 subpass 顺序（SinglePass）

`RenderForwardSinglePass` 中 `RenderTranslucency` 在 `NextSubpass()`（`:1614`）之后的 subpass。计划在其后插入 after-translucency，仍处于同一 RDG raster pass 的后续 subpass/同一场景颜色附着，状态连续。✅ 合理。

---

## 10. 修正后的完整改动清单（核对用）

| # | 文件 | 改动 | 计划是否覆盖 |
|---|------|------|--------------|
| 1 | `MeshPassProcessor.h` | 在 `WaterInfoTexturePass` 后、`#if WITH_EDITOR` 前加 `MobileAfterTranslucencyPass` | ⚠️ 位置错（放在 Num 后），需改 |
| 2 | `MeshPassProcessor.h` | `GetMeshPassName` 加 case | ✅ |
| 3 | `MeshPassProcessor.h` | static_assert `32→33`（+editor `32+4→33+4`），更新 GUID | ⚠️ 当前值误判为 33，实为 32 |
| 4 | `PrimitiveComponent.h` | 加 `bRenderAfterTranslucency` UPROPERTY + `SetRenderAfterTranslucency` | ✅ |
| 5 | `PrimitiveComponent.cpp` | 构造函数初始化 `=false`；实现 Setter | ⚠️ 缺构造函数初始化 |
| 6 | `PrimitiveSceneProxy.h` | 加 bit + `ShouldRenderAfterTranslucency()` | ✅ |
| 7 | `PrimitiveSceneProxy.cpp` | 从 `InComponent`/`InProxyDesc` 拷贝 | ✅（但见 #8） |
| 8 | `PrimitiveSceneProxyDesc.h` | **加 `bRenderAfterTranslucency` 字段 + 默认值** | ❌ 计划遗漏（致编译失败） |
| 9 | `MobileBasePassRendering.h` | 加 `bAfterTranslucencyBasePass` 成员 + 构造参数 | ✅ |
| 10 | `MobileBasePass.cpp` 构造函数 | 初始化列表加 `bAfterTranslucencyBasePass(...)` | ❌ 括号语法错误 |
| 11 | `MobileBasePass.cpp` AddMeshBatch | after-translucency 分流过滤 | ✅（注意 4.3 DepthPass 污染） |
| 12 | `MobileBasePass.cpp` | `CreateMobileAfterTranslucencyPassProcessor` + `REGISTER_MESHPASSPROCESSOR_AND_PSOCOLLECTOR` | ✅（建议改只读深度） |
| 13 | `SceneRendering.h` | **声明 `RenderMobileAfterTranslucencyPass`** | ❌ 计划遗漏（致编译失败） |
| 14 | `MobileBasePassRendering.cpp` | 实现 `RenderMobileAfterTranslucencyPass` | ✅（注意 #16 统计宏） |
| 15 | `MobileShadingRenderer.cpp` | ForwardSinglePass/MultiPass 调用 | ✅（注意 #8 只读深度、Deferred 路径） |
| 16 | `MobileShadingRenderer.cpp` | **`BuildRenderingCommands` 新 pass + InstanceCullingDrawParams** | ❌ 计划遗漏（致 DispatchDraw 空跑） |
| 17 | `SceneVisibility.cpp` | **可见性门控纳入 `bRenderAfterTranslucency`**（方案 B）或确认 MainPass=true 路径（方案 A） | ❌ 计划遗漏（致不显示/不透明阶段误绘） |
| 18 | 统计宏 | `STAT_AfterTranslucencyDrawTime`/`AfterTranslucency`/CSV 声明 | ❌ 计划遗漏（致编译失败） |

---

## 11. 风险等级汇总

| 风险 | 等级 | 类型 |
|------|------|------|
| 枚举新值放在 `Num` 之后 | 🔴 致命 | 编译/越界 |
| 构造函数括号缺失 | 🔴 致命 | 编译 |
| `FPrimitiveSceneProxyDesc` 缺字段 | 🔴 致命 | 编译 |
| 成员函数未在头文件声明 | 🔴 致命 | 编译 |
| 统计宏未声明 | 🔴 致命 | 编译 |
| 未 `BuildRenderingCommands` | 🔴 致命 | 功能空跑 |
| SceneVisibility 可见性门控未处理 | 🔴 致命 | 功能不显示/误绘 |
| static_assert 当前值误判 | 🟡 中 | 编译（修正后消除） |
| MultiPass 只读深度 vs 写深度状态 | 🟡 中 | 运行时校验/行为 |
| Deferred 路径未覆盖 | 🟡 中 | 功能（取决于项目配置） |
| DepthPass/Velocity 污染 | 🟡 中 | 语义偏差 |
| 深度原理表述反转 | 🟢 低 | 文档/理解 |
| 不混合硬覆盖 | 🟢 低 | 设计确认 |

---

## 12. 总体评价

计划抓住了正确的实现骨架（新增 EMeshPass + 复用 MobileBasePassMeshProcessor + 在 RenderTranslucency 后 DispatchDraw），对引擎渲染管线的理解方向正确。但落地细节存在 **5 处致命编译错误** 与 **2 处致命功能失效**（可见性门控、未 BuildRenderingCommands），以及若干中低风险项。这些并非思路错误，而是计划在“注册 → 可见性 → 构建 → 调度 → 绘制”完整链路上的**中段（可见性、构建）缺失**所致。

**建议**：按第 10 节清单补齐 #1/#8/#10/#13/#16/#17/#18 七项后再行编译验证；优先在 `RenderForwardSinglePass` + Android Vulkan 上做最小验证，确认覆盖效果后再补 MultiPass/Deferred 路径与只读深度收尾。整体可行，但不可直接按原计划落地。
