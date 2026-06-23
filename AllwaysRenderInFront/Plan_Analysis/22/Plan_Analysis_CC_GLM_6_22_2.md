# RenderAfterTranslucency（移动端 Forward 透明后渲染）方案分析

分析对象：`Engine/Docs/Plan.md`
分析依据：本仓库 UE 5.4 分支（branch 5.4）当前源码
分析日期：2026-06-22

---

## 0. 总体结论

方案的整体思路是正确且可行的：新增一个 `EMeshPass::MobileAfterTranslucencyPass`，复用 `FMobileBasePassMeshProcessor` 的不透明渲染逻辑，在透明物体渲染完成后再渲染被标记的物体，利用"透明不写深度、被标记物体后渲染并做深度测试"来遮挡透明物体。注册方式（`REGISTER_MESHPASSPROCESSOR_AND_PSOCOLLECTOR`）与已有的 `MobileBasePassCSM` 完全一致，`SetupMeshPass` 的通用循环会自动接管该 Pass 的构建（含动态网格元素），因此可见性分流、缓存命令、实例剔除、DispatchDraw 的主干链路是通的。

但方案存在 **3 处必定编译失败的错误**、**1 处会影响正确性/驱动稳定性的深度状态错误**、**若干遗漏修改与行为决策点**。下文按严重程度分类列出。所有行号均为本仓库当前源码实际行号（与方案中给出的行号有少量偏差，见第 5 节）。

---

## 1. 必定编译失败的错误（编译阻断，必须修复）

### 1.1 `RenderMobileAfterTranslucencyPass` 缺少头文件声明

方案在 `MobileBasePassRendering.cpp` 中实现了 `FMobileSceneRenderer::RenderMobileAfterTranslucencyPass(...)`，但**没有在任何头文件中声明该成员函数**。

- `RenderMobileBasePass` 的声明位于 `SceneRendering.h:2695`（`FMobileSceneRenderer` 内）。
- 方案未在 `SceneRendering.h` 的 `FMobileSceneRenderer` 中添加 `RenderMobileAfterTranslucencyPass` 的声明。
- 结果：`MobileShadingRenderer.cpp` 与 `MobileBasePassRendering.cpp` 中调用该函数时找不到声明 → 编译失败。

**修复**：在 `SceneRendering.h` 的 `FMobileSceneRenderer` 中（`RenderMobileBasePass` 声明附近，约 2695 行）添加：
```cpp
void RenderMobileAfterTranslucencyPass(FRHICommandList& RHICmdList, const FViewInfo& View, const FInstanceCullingDrawParams* InstanceCullingDrawParams);
```

### 1.2 `STAT_AfterTranslucencyDrawTime` 缺少 DECLARE

方案在 `RenderCore.cpp:65` 附近添加了 `DEFINE_STAT(STAT_AfterTranslucencyDrawTime);`，并在 `RenderMobileAfterTranslucencyPass` 中使用 `SCOPE_CYCLE_COUNTER(STAT_AfterTranslucencyDrawTime)`。

- 现有 `STAT_BasePassDrawTime` 的声明在 `RenderCore.h:44`：`DECLARE_CYCLE_STAT_EXTERN(TEXT("Base pass drawing"), STAT_BasePassDrawTime, STATGROUP_SceneRendering, RENDERCORE_API);`，定义在 `RenderCore.cpp:65`。
- 方案**只加了 `DEFINE_STAT`，没有加对应的 `DECLARE_CYCLE_STAT_EXTERN`** → `STAT_AfterTranslucencyDrawTime` 未声明 → 编译失败。

**修复**：在 `RenderCore.h` 中（`STAT_BasePassDrawTime` 声明附近，约 44 行）添加：
```cpp
DECLARE_CYCLE_STAT_EXTERN(TEXT("After translucency drawing"), STAT_AfterTranslucencyDrawTime, STATGROUP_SceneRendering, RENDERCORE_API);
```

### 1.3 `SCOPED_GPU_STAT(RHICmdList, AfterTranslucency)` 缺少 GPU 统计声明

方案在 `RenderMobileAfterTranslucencyPass` 中使用 `SCOPED_GPU_STAT(RHICmdList, AfterTranslucency)`。

- 现有 `RenderMobileBasePass` 使用 `SCOPED_GPU_STAT(RHICmdList, Basepass)`（`MobileBasePassRendering.cpp:475`），其对应的 `Basepass` GPU 统计声明为 `BasePassRendering.h:144` 的 `DECLARE_GPU_DRAWCALL_STAT_EXTERN(Basepass);`。
- 方案**未声明 `AfterTranslucency` 这个 GPU 统计** → 未定义符号 → 编译失败。

**修复**：在 `BasePassRendering.h`（`DECLARE_GPU_DRAWCALL_STAT_EXTERN(Basepass);` 附近，约 144 行）添加声明，并在对应 `.cpp`（GPU 统计定义集中处，通常 `BasePassRendering.cpp` 或 `RenderCore.cpp` 的 GPU 统计定义区）添加定义：
```cpp
// BasePassRendering.h
DECLARE_GPU_DRAWCALL_STAT_EXTERN(AfterTranslucency);
// 对应 .cpp
// 按 Basepass 的定义方式补一条 DEFINE（GPU drawcall stat 的 DEFINE 宏）
```
（具体 DEFINE 宏名请参照 `Basepass` 的定义写法保持一致。）

> 附带：`CSV_SCOPED_TIMING_STAT_EXCLUSIVE(RenderAfterTranslucency)` 中的 CSV 统计名一般按字符串自动注册，通常无需额外声明，但建议确认 `RenderBasePass` 这类 CSV 名在本工程的注册方式，保持一致即可。

---

## 2. 影响正确性 / 驱动稳定性的问题（强烈建议修复）

### 2.1 深度访问状态与实际 RenderPass 绑定不匹配（核心隐患）

方案在 `CreateMobileAfterTranslucencyPassProcessor` 中为该 Pass 设置了与 BasePass 相同的深度状态：
```cpp
PassDrawRenderState.SetDepthStencilAccess(DefaultBasePassDepthStencilAccess); // = 深度可写
PassDrawRenderState.SetDepthStencilState(TStaticDepthStencilState<true, CF_DepthNearOrEqual>::GetRHI()); // 深度写入开启
```

但 `RenderMobileAfterTranslucencyPass` 的 `DispatchDraw` 实际发生在**透明渲染 Pass / 子通道内**，此时深度附件是以**只读**方式绑定的：

- `RenderForwardSinglePass`（`MobileShadingRenderer.cpp:1578`）：在 `RHICmdList.NextSubpass()`（约 1614，进入透明子通道，深度只读）之后，于 `RenderTranslucency`（1623）之后插入本 Pass。该子通道深度为 `DepthRead`。
- `RenderForwardMultiPass`（`MobileShadingRenderer.cpp:1662`）：第二个 RDG Pass `"DecalsAndTranslucency"` 中，深度访问被显式设为 `FExclusiveDepthStencil::DepthRead_StencilRead`（约 1700、1716）。方案在 1735 `RenderTranslucency` 之后插入本 Pass，同样处于只读深度上下文。

后果：
- 在 Tile-Based 移动 GPU（Vulkan/Metal/GLES 子通道）上，**深度写入会被静默丢弃**（子通道声明的 attachment 用法不含写入），并触发 RHI 校验层警告；部分驱动/校验配置下可能更严格。
- 深度**测试** `CF_DepthNearOrEqual` 仍然生效，颜色写入（`TStaticBlendStateWriteMask<CW_RGBA>`，不透明覆盖）也生效，因此"遮挡透明物体"的核心目标在大多数 RHI 上**仍能达成**（见 2.2 的可达性分析）。但"深度写入开启"这一状态是错误且具有误导性的。

**修复建议**：将该 Pass 的深度状态改为与透明 Pass 上下文一致的**只读深度 + 关闭深度写入**：
```cpp
PassDrawRenderState.SetDepthStencilAccess(FExclusiveDepthStencil::DepthRead_StencilRead);
PassDrawRenderState.SetDepthStencilState(TStaticDepthStencilState<false, CF_DepthNearOrEqual>::GetRHI());
```
- 关闭深度写入后，`CF_DepthNearOrEqual` 仅做测试（见 2.2，仍可正确遮挡），且与只读深度附件一致，避免校验错误。
- 若确实需要该被标记物体写入深度（例如希望后续 Pass 读取其深度），则**不能**在透明 Pass 内联绘制，而需要新建一个深度可写的独立 RDG Pass（代价是 Tile-Based GPU 上的 tile 重载，移动端不推荐）。对于本需求（仅遮挡透明），只读深度即可。

### 2.2 深度测试 `CF_DepthNearOrEqual` 在只读深度下的可达性分析（结论：可行）

需确认关闭深度写入后，被标记物体是否仍能正确绘制并遮挡透明：

- 被标记物体被从 BasePass 排除（`AddMeshBatch` 中 `bAfterTranslucencyBasePass==false` 且 `bShouldRenderAfterTranslucency==true` 时 return）。
- 透明物体不写深度。
- 因此在本 Pass 绘制时，深度缓冲中：被标记物体自身像素处 = 远裁剪面清除值（1.0）（若无其他不透明遮挡），或更近的不透明深度（若有前置不透明）。
  - 自身像素：片元深度 d < 1.0，`CF_DepthNearOrEqual`（d <= stored）通过 → 绘制颜色，覆盖透明 → **达成遮挡**。✓
  - 前方存在更近不透明：stored 更近，d 更远，`NearOrEqual` 失败 → 不绘制 → 正确被前方不透明遮挡。✓
- 结论：**只读深度 + `CF_DepthNearOrEqual` 可满足核心需求**，前提是被标记物体确实未被 BasePass 写入颜色、且透明未写深度（均成立）。

> 注意：若开启了完整深度预pass（`bIsFullDepthPrepassEnabled`），被标记物体会先在预pass 写入自身深度（见 3.1），此时 `CF_DepthNearOrEqual` 在自身像素处精确相等通过，行为同样正确。两种情形下都可达。

---

## 3. 遗漏修改 / 行为决策点（建议处理）

### 3.1 被标记物体仍会进入"不透明阶段"的深度预pass（与需求表述存在张力）

需求原文："在渲染不透明物体的阶段，我标记的物体不进行渲染"。但当前方案下，被标记物体仍会进入移动端深度预pass：
- `ShouldRenderInDepthPass() = bRenderInMainPass || bRenderInDepthPass`（`PrimitiveSceneProxy.h:701`）。
- 方案要求 `bRenderInMainPass` 保持 `true`（否则 `SceneVisibility` 的 `(bRenderInMainPass || bRenderCustomDepth)` 门控不通过，物体根本不会进入 Mobile 基础 Pass 桶，也就不会被加入 `MobileAfterTranslucencyPass`）。
- 因此 `ShouldRenderInDepthPass()` 为 true → 被标记物体在深度预pass 中**写入深度**（仅深度，无颜色）。

影响：
- 深度预pass 写入被标记物体深度后，透明 Pass 中位于其后的透明物体会被深度剔除（不绘制），随后被标记物体绘制颜色覆盖 —— 这其实也达成"遮挡透明"的效果，但与"不透明阶段完全不渲染该物体"的字面表述有出入。
- 其它读取场景深度的效果（软粒子、深度淡化的透明、DOF 等）会看到该被标记物体的深度，可能符合/不符合预期。

**决策点**：是否需要将被标记物体也排除出深度预pass？
- 若接受"深度预pass 写入深度、仅不写颜色"——当前方案即可，无需改动（且有助于遮挡）。
- 若严格要求"不透明阶段完全不出现"——需额外在深度预pass 处理器（`FDepthOnlyMeshProcessor` / 移动端 prepass）中跳过 `ShouldRenderAfterTranslucency()` 的图元，或引入独立开关。**方案当前未处理此项**，请按需求取舍。

### 3.2 `ComputeDynamicMeshRelevance` 的改动未限定 `ShadingPath == Mobile`

方案在 `SceneVisibility.cpp` 的静态路径（约 1564）改动位于 `if (ShadingPath == EShadingPath::Mobile)` 分支内；但动态路径（`ComputeDynamicMeshRelevance`，约 2211）的改动**没有**限定 ShadingPath：
```cpp
if (ViewRelevance.bRenderInMainPass || ViewRelevance.bRenderCustomDepth)
{
    if (ViewRelevance.bRenderAfterTranslucency)
    {
        PassMask.Set(EMeshPass::MobileAfterTranslucencyPass);
        View.NumVisibleDynamicMeshElements[EMeshPass::MobileAfterTranslucencyPass] += NumElements;
    }
    else { ... BasePass ... }
}
```
- 在 Desktop/Deferred 路径下，若某个图元 `bRenderAfterTranslucency=true`，其动态元素会被路由到 `MobileAfterTranslucencyPass`；而该 Pass 仅在 `EShadingPath::Mobile` 注册（`SetupMeshPass` 中 `GetPassFlags(Deferred, MobileAfterTranslucencyPass)` 为 None → 不构建），导致这些动态元素**不会被渲染**（仅浪费计数）。
- 由于标志默认 `false` 且用户仅用移动端，实际风险低，但与静态路径的写法不一致，属于潜在隐患。

**修复建议**：用 `ShadingPath == EShadingPath::Mobile` 包裹该分支（或至少确保仅在移动端走 `MobileAfterTranslucencyPass`，否则回退 `BasePass`）。

### 3.3 移动端 Deferred 路径未接入（与"仅 Forward"需求一致，但需明确）

`MobileShadingRenderer.cpp` 中 `RenderTranslucency(RHICmdList, View);` 共有 4 处：1623（SinglePass）、1735（MultiPass）、1985（DeferredSinglePass）、2068（DeferredMultiPass）。方案只改了 1623 与 1735，未改 1985/2068（移动端延迟着色路径）。
- 这与用户"只需要 Forward 渲染路径"的需求一致，**可接受**。
- 但需注意：`FMobileBasePassMeshProcessor` 在延迟模式下 `bPassUsesDeferredShading=true`，会生成写 GBuffer 的绘制命令；若误在延迟路径中调度本 Pass，会在透明之后错误地写 GBuffer。建议在 `CreateMobileAfterTranslucencyPassProcessor` 或调度点处加 `ensure(!bPassUsesDeferredShading)` 或仅 Forward 生效的保护，并写注释说明本 Pass 仅支持 Forward。

### 3.4 被标记物体在非 AlwaysCSM 模式下丢失 CSM 接收

- `SceneVisibility`（方案改动后）对被标记物体仍调用 `AddCommandsForMesh(... EMeshPass::MobileBasePassCSM)`（约 1567）。
- 但 `CreateMobileBasePassCSMProcessor` 构造的处理器 `bAfterTranslucencyBasePass=false`（默认），其 `AddMeshBatch` 会因 `bShouldRenderAfterTranslucency==true` 而 return → **被标记物体被排除出 MobileBasePassCSM**。
- 同时 `CreateMobileAfterTranslucencyPassProcessor` 在 `!MobileBasePassAlwaysUsesCSM` 时 Flags 不含 `CanReceiveCSM` → 本 Pass 也不接收 CSM。
- 结果：非 AlwaysCSM 模式下，被标记物体**完全不接收 CSM 阴影**（仅受天光/环境/局部光）。这是相对原 BasePass 行为的光照差异。
- AlwaysCSM 模式下，方案处理器镜像了 `CreateMobileBasePassProcessor`（含 `CanReceiveCSM`），CSM 正常。

**建议**：若 VR 场景需要被标记物体也接收方向光阴影，需在 `CreateMobileAfterTranslucencyPassProcessor` 中与 `SceneVisibility` 配合，让本 Pass 在非 AlwaysCSM 模式下也承担 CSM（或保留 MobileBasePassCSM 对该物体的绘制，但那样会在不透明阶段绘制，违背需求）。否则明确接受"被标记物体无 CSM"的限制。

### 3.5 `MobileBasePassCSM` 仍对被标记物体调用 `AddCommandsForMesh`（冗余）

如 3.4 所述，`SceneVisibility` 仍对被标记物体调用 `AddCommandsForMesh(... MobileBasePassCSM)`，最终被处理器 `AddMeshBatch` 排除。功能上无害，但属冗余缓存尝试。可在 `SceneVisibility` 改动中一并跳过（与 BasePass 同样按 `bRenderAfterTranslucency` 分流），保持清晰。次要项。

---

## 4. 其它潜在问题 / 限制（供验证与知会）

1. **MobileBasePassUniformBuffer 绑定**：本 Pass 复用不透明 base pass shader，其缓存绘制命令绑定的 MobileBasePass UB 是不透明（非 Translucent）那套。在透明 Pass 内调度时，该 UB 在本帧仍有效（Forward 模式自包含），应能正确取到方向光/天光/反射/雾等参数。建议实测确认光照正确（尤其方向光、CSM 开关、雾）。

2. **GPUScene 依赖**：`BuildInstanceCullingDrawParams` 中为本 Pass 调用 `BuildRenderingCommands` 位于 `if (Scene->GPUScene.IsEnabled())` 内。若目标平台关闭 GPUScene，本 Pass 不会构建命令 → 被标记物体不可见。与其它 Pass 行为一致，但需确保 Android VR 开启了 GPUScene。

3. **多个被标记物体相互遮挡**：因深度写入被丢弃（见 2.1 修复后为只读深度），多个被标记物体之间不会按深度正确排序，可能出现绘制顺序错误/穿插。若场景中存在多个被标记物体且互相重叠，需评估。

4. **VR 立体渲染路径覆盖**：方案改动了 `RenderForwardSinglePass` 与 `RenderForwardMultiPass`。若该 Android VR 项目使用其它路径（如 instanced stereo / multiview 的专门入口），需确认是否也经过这两个函数；若存在独立的 instanced 渲染入口，需同步加入 `RenderMobileAfterTranslucencyPass` 调用。建议在目标工程实测确认走的是哪个路径。

5. **`bRenderAfterTranslucency` 的序列化/复制**：作为 `UPROPERTY`，序列化与蓝图读写没问题（与 `bRenderInMainPass` 一致）；未做网络复制（渲染标志，与 `bRenderInMainPass` 一致，合理）。

6. **`EMeshPass::NumBits = 6`**：新增后 Num=33（非编辑器）/ 37（编辑器），远小于 64，无溢出风险。✓

---

## 5. 行号与锚点核对

方案中多数行号为近似值，按"锚点字符串"定位均可找到；以下是实际行号与方案行号的差异，便于实施时定位：

| 文件 | 方案行号 | 实际行号 | 锚点 |
|---|---|---|---|
| `MeshPassProcessor.h` | 32 / 83 | 32 / 83 | `namespace EMeshPass` / `GetMeshPassName` ✓ |
| `MeshPassProcessor.h` static_assert | 改为 33 / 33+4 | 当前为 32 / 32+4 | **方案目标值正确**（新增 1 项后应为 33 / 33+4）✓ |
| `PrimitiveComponent.h` | 407 / 1917 | 408 / 1918 | `uint8 bRenderInMainPass:1;` / `SetRenderInMainPass` |
| `PrimitiveComponent.cpp` | 333 / 4457 | 333 / 4457 | `bRenderInMainPass = true;` / `SetRenderInMainPass` ✓ |
| `PrimitiveSceneProxy.h` | 700 / 1200 | 700 / 1200 | `ShouldRenderInMainPass` / `uint8 bRenderInMainPass : 1;` ✓ |
| `PrimitiveSceneProxy.cpp` | 277 / 428 | 277 / 428 | `bRenderInMainPass = InComponent->...` / `bRenderInMainPass(InProxyDesc...)` ✓ |
| `PrimitiveSceneProxyDesc.h` | 25 / 93 | 25 / ~93 | `bRenderInMainPass = true;` ✓（成员位约 93，建议按 `bRenderInMainPass` 字符串定位） |
| `MobileBasePassRendering.h` | 480 / 533 | 480 / 533 | 构造函数 / `const bool bPassUsesDeferredShading;` ✓ |
| `MobileBasePass.cpp` | 810 / 867 / 1151 / 1223 | 810 / 867 / 1151 / 1218 | 构造函数 / `AddMeshBatch` / `CreateMobileBasePassProcessor` / `REGISTER_MESHPASSPROCESSOR_AND_PSOCOLLECTOR` |
| `MobileBasePassRendering.cpp` | 492 | 492（`RenderMobileBasePass` 结束于 491） | ✓ |
| `RenderCore.cpp` | 65 | 65 | `DEFINE_STAT(STAT_BasePassDrawTime);` ✓ |
| `SceneRendering.h` | 2796 | 2796 | `FInstanceCullingDrawParams TranslucencyInstanceCullingDrawParams;` ✓ |
| `MobileShadingRenderer.cpp` | 1433 / 1624 / 1736 | 1433 / 1623 / 1735 | `BuildInstanceCullingDrawParams` / SinglePass `RenderTranslucency` / MultiPass `RenderTranslucency` |
| `PrimitiveViewRelevance.h` | 54 / 103 | 54 / 103 | `uint32 bRenderInMainPass : 1;` / `bRenderInMainPass = true;` ✓ |
| `StaticMeshRender.cpp` | 2055 | 2062 | `Result.bRenderInMainPass = ShouldRenderInMainPass();` |
| `SkeletalMesh.cpp` | 7107 | 7115 | `Result.bRenderInMainPass = ShouldRenderInMainPass();` |
| `SceneVisibility.cpp` | 1564 / 2186~2211 | 1564 / 2186(`ComputeDynamicMeshRelevance`) ~ 2211 | ✓ |

---

## 6. 已确认正确的部分（无需改动）

1. **`EMeshPass` 新增 + `GetMeshPassName` case + static_assert（33 / 33+4）**：与现有 32/32+4 一致，目标值正确。✓
2. **`REGISTER_MESHPASSPROCESSOR_AND_PSOCOLLECTOR(MobileAfterTranslucencyPass, ..., EShadingPath::Mobile, EMeshPass::MobileAfterTranslucencyPass, CachedMeshCommands | MainView)`**：与 `MobileBasePassCSM` 注册模式一致。✓
3. **`SetupMeshPass`（`SceneRendering.cpp:4202`）通用循环**：会按 `MainView` 标志自动构建 `ParallelMeshDrawCommandPasses[MobileAfterTranslucencyPass]`（含静态缓存命令与动态网格元素），不需要像 BasePass/CSM 那样特殊合并（4209 的 `continue` 仅针对 BasePass 与 MobileBasePassCSM）。✓
4. **`AddMeshBatch` 互斥分流**（`bAfterTranslucencyBasePass` 与 `ShouldRenderAfterTranslucency` 互斥）逻辑正确：BasePass/CSM Pass 排除被标记物体，本 Pass 仅收被标记物体。✓
5. **复用 `FMobileBasePassMeshProcessor`**：本 Pass 不传 `ETranslucencyPass` → `bTranslucentBasePass=false` → 走不透明分支（`ShouldDraw` 返回 `!bIsTranslucent`），符合"只让 Mesh/Skeletal Mesh 不透明渲染生效"。✓
6. **构造函数新增 `bool bAfterTranslucencyBasePass=false`**：声明（头）与定义（cpp）均修改，`const bool` 成员在初始化列表初始化，正确。✓
7. **字段/Setter/初始化链路**（`PrimitiveComponent` → `PrimitiveSceneProxyDesc` → `PrimitiveSceneProxy` → `PrimitiveViewRelevance` → `StaticMeshRender`/`SkeletalMesh` 的 `GetViewRelevance`）：传播链完整，`SetRenderAfterTranslucency` 调用 `MarkRenderStateDirty` 触发重缓存，正确。✓
8. **可见性分流（静态 1564 + 动态 2211）+ `BuildInstanceCullingDrawParams` 构建 + `DispatchDraw` 调度**：主链路完整。✓
9. **Forward SinglePass/MultiPass 插入点**：在 `RenderTranslucency` 之后调用，时序正确。✓

---

## 7. 修复清单（优先级排序）

**P0（编译阻断，必做）**
- [ ] 1.1 在 `SceneRendering.h` 的 `FMobileSceneRenderer` 中声明 `RenderMobileAfterTranslucencyPass`。
- [ ] 1.2 在 `RenderCore.h` 添加 `DECLARE_CYCLE_STAT_EXTERN(... STAT_AfterTranslucencyDrawTime ...)`。
- [ ] 1.3 在 `BasePassRendering.h` 添加 `DECLARE_GPU_DRAWCALL_STAT_EXTERN(AfterTranslucency);` 并在对应 .cpp 补定义。

**P1（正确性/稳定性，强烈建议）**
- [ ] 2.1 将 `CreateMobileAfterTranslucencyPassProcessor` 的深度状态改为只读深度 + 关闭深度写入（`DepthRead_StencilRead` + `TStaticDepthStencilState<false, CF_DepthNearOrEqual>`）。
- [ ] 3.2 用 `ShadingPath == EShadingPath::Mobile` 包裹 `ComputeDynamicMeshRelevance` 中的 `MobileAfterTranslucencyPass` 分支。
- [ ] 3.3 在延迟路径或处理器中对 `bPassUsesDeferredShading` 加保护/`ensure`，注明本 Pass 仅 Forward。

**P2（需求取舍 / 加固）**
- [ ] 3.1 决定是否将被标记物体排除出深度预pass（严格符合"不透明阶段不渲染"则需改；接受深度预pass则无需改）。
- [ ] 3.4 决定被标记物体在非 AlwaysCSM 模式下是否需要 CSM（影响光照）。
- [ ] 3.5（可选）在 `SceneVisibility` 中按 `bRenderAfterTranslucency` 一并跳过 `MobileBasePassCSM` 的 `AddCommandsForMesh`。
- [ ] 4.4 确认 Android VR 实际走 SinglePass/MultiPass（或 instanced 专门入口），必要时补充调用点。

---

## 8. 结论

方案主干正确、与现有 `MobileBasePassCSM` 模式一致，可见性/缓存/调度链路完整，核心遮挡需求在修复 P0 后即可编译运行。**但必须先修复 3 处编译阻断（P0）**，并**强烈建议修复深度状态不匹配（P1-2.1）**以避免移动端驱动/校验问题。P2 项为需求层面的取舍（深度预pass 参与、CSM 接收、VR 路径覆盖），请结合实际 VR 场景确认。
