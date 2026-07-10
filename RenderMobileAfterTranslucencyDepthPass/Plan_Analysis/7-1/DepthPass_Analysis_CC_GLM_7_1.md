# Plan1.md 源码修改方案分析（基于 UE 5.4.4 源码核对）

> 核对基线：`E:\Unreal Engine Work Projects\MR01_DaNaoTianGong_Main\Engine`（UE 5.4.4，Changelist 33043543）
> 分析对象：`Docs\Plan1.md`（新增 `MobileAfterTranslucencyDepthPass` + `MobileAfterTranslucencyPass` 两个移动前向渲染 Pass）
> 核对方式：直接读取 5.4.4 源码逐行验证 Plan1.md 中所声称的文件、行号、代码结构

---

## 0. 总体结论（先看这条）

Plan1.md 的**核心思路是成立的**，并且与 5.4.4 移动前向渲染管线的实际结构（subpass + 双 Pass）**兼容**：

- 移动前向渲染在 `RenderForwardSinglePass` 中用**单个 RDG Pass + 子 Pass（subpass）**完成"不透明→半透明"，深度在 subpass 0 写、subpass 1 读（tile memory 内传递，不清除、不丢失）。
- `RenderForwardMultiPass` 则拆成两个 RDG Pass（Pass1 写深度、Pass2 `DecalsAndTranslucency` 读深度）。
- Plan 提出的"在 `RenderMobileBasePass` 之后插深度写 Pass、在 `RenderTranslucency` 之后插颜色读 Pass"恰好分别落在 subpass0/Pass1 与 subpass1/Pass2，**深度生命周期天然成立**。

但存在 **4 个必须处理的正确性/一致性问题**（按严重度排序）：

| # | 问题 | 严重度 | 后果 |
|---|------|--------|------|
| 1 | 未处理 `bIsFullDepthPrepassEnabled`（`EarlyZPassMode==DDM_AllOpaque`）分支 | **高** | 该模式下深度 Pass 变成空操作 + 与 PrePass 深度来源冲突 |
| 2 | `Process()` 会覆盖处理器构造里设置的 DepthStencilState | 中 | Plan 里写明的 `<true/false, CF_DepthNearOrEqual>` 实际不生效（靠 subpass 只读"歪打正着"成立） |
| 3 | `MobileBasePassCSM` 路由矛盾（可见性侧加、处理器侧拒） | 中 | 标记物体拿不到 CSM，且存在死代码 |
| 4 | `CollectPSOInitializers` 早退导致 PSO 不预缓存 | 中低 | 运行时首次绘制 PSO 编译卡顿 |

另外有 1 个**澄清项**（不是 bug）：Plan1.md 第 6–9 行担心的"复用 `FMobileBasePassMeshProcessor` 仍会绑定颜色 VS/PS"——这是**对的，但反而是优点**（见 §4 深度一致性分析），不建议改用 `RenderPrePass`/`FDepthPassMeshProcessor`。

行号方面：步骤 2、3、9、统计、`SceneRendering.h` 全部准确；步骤 1 枚举计数正确；步骤 4 `REGISTER` 行号偏小约 5；步骤 5/12 插入点行号基本准确（含 SinglePass 与 MultiPass 共 4 处）；`SceneRendering.h`、`BasePassRendering.h` 实际在 `Private` 而非 Plan 所写的 `Public`。

---

## 1. 逐项行号核对

### 步骤 1 — `MeshPassProcessor.h`（EMeshPass 枚举）

| Plan 声称 | 5.4.4 实际 | 结论 |
|-----------|-----------|------|
| 枚举位于 `enum Type : uint8` | `Source/Runtime/Renderer/Public/MeshPassProcessor.h:34` | ✅ |
| 新增 2 项插在 `#if WITH_EDITOR` 之前 | 当前非编辑项 32 个，`MobileBasePassCSM:57`/`VirtualTexture:58`/`NaniteMeshPass:64`/`MeshDecal:65`/`WaterInfoTextureDepthPass:66` 之后即是编辑块 | ✅ 插入位置正确 |
| `static_assert(Num == 34+4)` / `== 34` | 当前 `:128 Num == 32+4`、`:130 Num == 32` | ✅ 加 2 项后改 34+4 / 34 正确 |
| `NumBits` | `:77 NumBits = 6`（上限 64），`:80 assert Num<=64` | ✅ 34 远小于 64，无溢出 |

**结论**：步骤 1 完全正确，`NumBits=6` 足够，无溢出风险。

### 步骤 2 — `PrimitiveComponent.h/.cpp`

| Plan 声称 | 实际 | 结论 |
|-----------|------|------|
| `bRenderInMainPass:1` ~407 | `Classes/Components/PrimitiveComponent.h:408` | ✅ |
| `SetRenderInMainPass` 声明 ~1917 | `:1918` | ✅ |
| `bRenderInMainPass = true` ~333 | `Private/Components/PrimitiveComponent.cpp:333` | ✅ |
| `SetRenderInMainPass` 实现 ~4457 | `:4457` | ✅ |

**结论**：步骤 2 行号全部准确。

### 步骤 3 — `PrimitiveSceneProxy` + `PrimitiveSceneProxyDesc`

| Plan 声称 | 实际 | 结论 |
|-----------|------|------|
| `PrimitiveSceneProxy.h` 路径 | `Source/Runtime/Engine/Public/PrimitiveSceneProxy.h` | ✅（注意：在 `Engine/Public`，不在 `Renderer` 下） |
| `ShouldRenderInMainPass` ~700 | `:700` | ✅ |
| `bRenderInMainPass : 1` ~1200 | `:1200` | ✅ |
| `PrimitiveSceneProxy.cpp` `InitializeFrom` ~265、`bRenderInMainPass = InComponent->bRenderInMainPass` ~277 | `:265`、`:277` | ✅ |
| 构造函数 `bRenderInMainPass(InProxyDesc.bRenderInMainPass)` ~428 | `:428` | ✅ |
| `PrimitiveSceneProxyDesc.h` `bRenderInMainPass=true` ~25、`bRenderInMainPass : 1` ~93 | `:25`、`:93` | ✅ |

**结论**：步骤 3 行号全部准确。

### 步骤 4 — `MobileBasePass.cpp`（MeshProcessor 与注册）

| Plan 声称 | 实际 | 结论 |
|-----------|------|------|
| `AddMeshBatch` ~867 | `:867`（早退 `:869-874`） | ✅ |
| `CreateMobileBasePassProcessor` ~1151 | `:1151` | ✅ |
| `REGISTER_MESHPASSPROCESSOR_AND_PSOCOLLECTOR` ~1223 | 实际在 `:1218-1222` | ⚠️ 行号偏小约 5（`MobileBasePass` 在 1218，不是 1223） |
| `CollectPSOInitializers` 早退 | `:1056` | ✅ 位置正确 |

**结论**：`REGISTER` 行号 1223 实际为 1218，需对齐；其余准确。

### 步骤 5 — `MobileBasePassRendering.cpp` / `SceneRendering.h` / 统计

| Plan 声称 | 实际 | 结论 |
|-----------|------|------|
| `RenderMobileBasePass` ~324/492 | `MobileBasePassRendering.cpp:470` | ⚠️ 行号不符（5.4.4 中是 470） |
| `SceneRendering.h` 在 `Public` | 实际在 `Source/Runtime/Renderer/Private/SceneRendering.h` | ⚠️ 路径应为 Private |
| `RenderMobileBasePass` 声明 ~2695 | `SceneRendering.h:2695` | ✅ |
| 新增 `InstanceCullingDrawParams` 成员 ~2796 | `:2793-2796`（DepthPass/SkyPass/DebugViewMode/Translucency） | ✅ 紧邻 2796 插入正确 |
| `RenderCore.h` `STAT_BasePassDrawTime` EXTERN ~44 | `Source/Runtime/RenderCore/Public/RenderCore.h:44` | ✅ |
| `RenderCore.cpp` `DEFINE_STAT(STAT_BasePassDrawTime)` ~65 | `Source/Runtime/RenderCore/Private/RenderCore.cpp:65` | ✅ |
| `BasePassRendering.h` 在 `Public` ~144/184 | 实际在 `Source/Runtime/Renderer/Private/BasePassRendering.h:144` `DECLARE_GPU_DRAWCALL_STAT_EXTERN(Basepass)` | ⚠️ 路径应为 Private；行号 144 ✅ |

**结论**：行号基本对，但 `SceneRendering.h`、`BasePassRendering.h` 路径应从 `Public` 改为 `Private`。

### 步骤 12 — `MobileShadingRenderer.cpp`（4 处插入点）

| 用途 | Plan 行号 | 实际 | 结论 |
|------|-----------|------|------|
| SinglePass 深度写（RenderMobileBasePass 后） | 1609 | `:1609` | ✅ |
| SinglePass 颜色读（RenderTranslucency 后） | 1624 | `:1623` | ✅（差 1） |
| MultiPass 深度写（Pass1 RenderMobileBasePass 后） | 1682 | `:1682` | ✅ |
| MultiPass 颜色读（Pass2 RenderTranslucency 后） | 1735 | `:1735` | ✅ |

**结论**：4 个插入点行号准确，且 SinglePass 与 MultiPass **两条路径都已覆盖**（这点 Plan 做对了）。

---

## 2. 渲染管线契合度分析（核心）

### 2.1 移动前向的真实结构

`FMobileSceneRenderer::RenderForward`（`MobileShadingRenderer.cpp:1503`）按 `bRequiresMultiPass` 分流：

- **`RenderForwardSinglePass`（1578）**：单个 RDG Pass `SceneColorRendering`（1590），用 **subpass**：
  - `SubpassHint = DepthReadSubpass`（1586）
  - subpass 0：`RenderMaskedPrePass`(1606) → `RenderMobileBasePass`(1609) → `RenderMobileDebugView`(1610) → `PostRenderBasePass`(1612)
  - `RHICmdList.NextSubpass()`（1614，注释"scene depth is read only and can be fetched"）
  - subpass 1：`RenderDecals` → 阴影 → 雾 → **`RenderTranslucency`(1623)** → debug → 遮挡查询 → tonemap/resolve

- **`RenderForwardMultiPass`（1662）**：两个独立 RDG Pass：
  - Pass1 `SceneColorRendering`（1664）：`RenderMaskedPrePass`(1679) → `RenderMobileBasePass`(1682) → `PostRenderBasePass`(1685)
  - Pass2 `DecalsAndTranslucency`（1721）：`RenderTargets[0].SetLoadAction(ELoad)`(1712)、`DepthStencil = DepthRead_StencilRead`(1700) → `RenderTranslucency`(1735)

### 2.2 深度生命周期（关键论据）

- **深度不会被清**：`InitRenderTargetBindings_Forward`（1448）里，深度 LoadAction 在 `!bIsFullDepthPrepassEnabled` 时是 `EClear`（首次 View 清深度，后续 View `ELoad`）。整个不透明阶段写一次深度，subpass1/Pass2 只读不清。✅
- **subpass0→subpass1 深度通过 tile memory 传递**（Vulkan Mobile），无需 resolve，颜色 Pass 在 subpass1 能读到 subpass0 写的深度。✅
- **`AddResolveSceneDepthPass`（1656/1691）在 `!bIsFullDepthPrepassEnabled` 时于 Pass 结束后才执行**，不影响 Pass 内深度可用性。✅
- **颜色（SceneColor）在半透明后仍可写**：SinglePass subpass1 颜色连续可写；MultiPass Pass2 `RenderTargets[0]` 用 `ELoad` 保留半透明像素。✅

### 2.3 Plan 的两个 Pass 落点

- **深度写 Pass**：插在 `RenderMobileBasePass`(1609/1682) 之后、`NextSubpass`(1614)/Pass1 结束(1686) 之前 → 仍在 subpass0/Pass1，**深度可写**（`!bIsFullDepthPrepassEnabled` 时 `DepthWrite_StencilWrite`）。✅
- **颜色读 Pass**：插在 `RenderTranslucency`(1623/1735) 之后 → 在 subpass1/Pass2，**深度只读、颜色可写**。✅

**结论**：核心需求（标记物体深度先于半透明写入、半透明后再画颜色读深度遮挡半透明）**在两条路径上都成立**。

### 2.4 VR / Multiview

- `RenderForward`（1513-1517）读取 `vr.MobileMultiView`，`BasePassRenderTargets.MultiViewCount = bIsMobileMultiViewEnabled ? 2 : ...`。
- 新 Pass 复用 `View.ParallelMeshDrawCommandPasses[..].DispatchDraw` 与 `BuildRenderingCommands`，multiview 由既有 GPU Scene / 实例化机制自动处理，**无需额外特殊处理**。✅

---

## 3. 关键正确性问题

### 3.1【高】未处理 `bIsFullDepthPrepassEnabled` 分支

**事实**：`bIsFullDepthPrepassEnabled = (Scene->EarlyZPassMode == DDM_AllOpaque)`（`MobileShadingRenderer.cpp:302`）。它直接决定 base pass 的深度访问模式：

```
// InitRenderTargetBindings_Forward :1494-1496
BasePassRenderTargets.DepthStencil = bIsFullDepthPrepassEnabled
  ? FDepthStencilBinding(SceneDepth, ELoad, ELoad, DepthRead_StencilWrite)   // 深度只读！
  : FDepthStencilBinding(SceneDepth, EClear, EClear, DepthWrite_StencilWrite); // 深度可写
```

**对 Plan 的影响**：

- 当 `bIsFullDepthPrepassEnabled == true` 时：
  1. subpass0/Pass1 的深度附件是 **`DepthRead_StencilWrite`（只读）**，Plan 的深度写 Pass 调 `DispatchDraw` 写深度会被**静默丢弃**。
  2. 更糟：`FMobileBasePassMeshProcessor::Process()`（`MobileBasePass.cpp:952`）有早 Pass 优化：
     ```cpp
     if((MeshBatch.bUseForDepthPass && Scene->EarlyZPassMode == DDM_AllOpaque) || bMaskedInEarlyPass)
         DrawRenderState.SetDepthStencilState(TStaticDepthStencilState<false, CF_Equal>::GetRHI()); // 不写、Equal 测试
     ```
     即标记物体（默认 `bUseForDepthPass=true`）走这里 → **不写深度、CF_Equal 测试** → 整个深度写 Pass 变成纯空操作。
  3. 此时标记物体的深度**实际来自 PrePass**（`FDepthPassMeshProcessor`，`bUseForDepthPass` 门控），因为 Plan 没动 `EMeshPass::DepthPass` 的可见性，标记物体仍在 PrePass 里。

- 当 `bIsFullDepthPrepassEnabled == false`（移动端常见、VR 常用）时：subpass0 深度 `DepthWrite_StencilWrite` 可写，`Process()` 走 `SetOpaqueRenderState`（写深度）→ Plan 的深度写 Pass **正常写深度**。✅

**所以**：深度写 Pass **只在 `!bIsFullDepthPrepassEnabled` 时有意义**。在 `bIsFullDepthPrepassEnabled` 时它是浪费的空操作（功能靠 PrePass 兜底，仍能工作，但白跑一遍 draw）。

**必须处理**：
1. 在 `RenderForwardSinglePass`/`RenderForwardMultiPass` 调用深度写 Pass 处用 `if (!bIsFullDepthPrepassEnabled)` 包裹，跳过空操作；**或**接受其为无害空操作但写明注释。
2. **边界风险**：若用户对标记物体的材质设了 `bUseForDepthPass=false`，且 `bIsFullDepthPrepassEnabled=true`，则 PrePass 不写、深度写 Pass 又写不进 → 标记物体深度**完全缺失** → 半透明不会被遮挡 → 功能失效。需在文档/`SetRenderAfterTranslucency` 里约束"标记物体必须参与深度 Pass"。

### 3.2【中】`Process()` 覆盖处理器的 DepthStencilState

**事实**：`FMobileBasePassMeshProcessor::Process()`（`MobileBasePass.cpp:945-961`）：

```cpp
FMeshPassProcessorRenderState DrawRenderState(PassDrawRenderState);  // 拷贝构造器状态
if (!bForcePassDrawRenderState)   // Plan 没设 ForcePassDrawRenderState
{
    if (bTranslucentBasePass) SetTranslucentRenderState(...);
    else if ((bUseForDepthPass && EarlyZPassMode==AllOpaque) || bMaskedInEarlyPass)
        DrawRenderState.SetDepthStencilState(<false, CF_Equal>);     // 覆盖！
    else
        MobileBasePass::SetOpaqueRenderState(...);                    // 覆盖！
}
```

`SetOpaqueRenderState`（`MobileBasePassRendering.cpp:570`）固定设 `TStaticDepthStencilState<true, CF_DepthNearOrEqual>`。**注意 `Process()` 只覆盖 DepthStencilState，不碰 BlendState。**

**对 Plan 的影响**：

- Plan 在 `CreateMobileAfterTranslucencyDepthPassProcessor`/`CreateMobileAfterTranslucencyPassProcessor` 里显式设的 `SetDepthStencilState(<true/false, CF_DepthNearOrEqual>)` **会被 `Process()` 覆盖为 `SetOpaqueRenderState` 的 `<true, CF_DepthNearOrEqual>`**（标记物体非透明、非 `bTranslucentBasePass`）。
  - 深度写 Pass：被覆盖成 `<true, CF_DepthNearOrEqual>`（写+近等测试）——恰好就是想要的效果。✅（但不是 Plan 显式设的那个值，是覆盖后的）
  - 颜色读 Pass：Plan 想设 `<false, CF_DepthNearOrEqual>`（不写），但被覆盖成 `<true, CF_DepthNearOrEqual>`（写）。所幸颜色 Pass 跑在 subpass1/Pass2 的**只读深度**里，写请求被附件丢弃，净效果仍为"不写、近等测试"。✅ **歪打正着**。
- BlendState 不被覆盖：Plan 设的 `CW_NONE`（深度 Pass）/`CW_RGBA`（颜色 Pass）**保留生效**。✅

**建议**：
1. 若希望颜色 Pass 的"不写深度"由状态保证（而非依赖 subpass 只读），需给 `FMobileBasePassMeshProcessor` 传 `EFlags::ForcePassDrawRenderState`，这样 `Process()` 不覆盖、Plan 设的 `<false, ...>` 直接生效。否则维持现状亦可，但应在注释里写明"深度不写靠 subpass 只读附件保证"。
2. Plan 文档里"显式设 `<false, CF_DepthNearOrEqual>` 即可不写深度"的表述**不准确**——该值会被覆盖；需更正说明。

### 3.3【中】`MobileBasePassCSM` 路由矛盾

**事实**：
- Plan 在 `SceneVisibility.cpp:1565-1567` 保留了对标记物体的 `AddCommandsForMesh(... EMeshPass::MobileBasePassCSM)`（仅当 `!bMobileBasePassAlwaysUsesCSM`）。
- 但 Plan 在 `MobileBasePass.cpp` 的 `AddMeshBatch` 早退逻辑是：`if (bShouldRenderAfterTranslucency) return;`（对**非** AfterTranslucency 的所有 MeshPassType 生效）。

`MobileBasePassCSM` 用的也是 `FMobileBasePassMeshProcessor`（`CreateMobileBasePassCSMProcessor:1165`，MeshPassType=`MobileBasePassCSM`，**非** AfterTranslucency）。于是标记物体在 `MobileBasePassCSM` 的 `AddMeshBatch` 里被早退拒绝 → **`AddCommandsForMesh` 产生空命令**。

**后果**：
- `SceneVisibility` 侧加了 `MobileBasePassCSM`，`MobileBasePass.cpp` 侧又拒绝 → 死代码 + 不一致。
- 标记物体**永远拿不到 CSM**：
  - `bMobileBasePassAlwaysUsesCSM=true`（CSM 烤进 BasePass）：标记物体不在 BasePass → 无 CSM；
  - `bMobileBasePassAlwaysUsesCSM=false`：`MobileBasePassCSM` 被处理器拒绝 → 无 CSM。
- 颜色读 Pass（`CreateMobileAfterTranslucencyPassProcessor`）复用 `FMobileBasePassMeshProcessor` 但**不带 `CanReceiveCSM` Flag** → 即便有 CSM pass，标记物体颜色也不带 CSM 阴影。

**建议**：
- 若标记物体是"始终在最前/前景"用途（不需要 CSM 阴影），直接在 `SceneVisibility` 的 AfterTranslucency 分支里**移除** `MobileBasePassCSM` 的 `AddCommandsForMesh`，消除死代码与不一致。
- 若需要 CSM，则 `CreateMobileAfterTranslucencyPassProcessor` 必须带 `CanReceiveCSM` Flag，并在 `AddMeshBatch` 早退里**放行 `MobileBasePassCSM`**——但这会让标记物体同时在 `MobileBasePassCSM` 和颜色 Pass 被画两遍，需重新设计。

### 3.4【中低】PSO 预缓存被早退跳过

**事实**：Plan 在 `FMobileBasePassMeshProcessor::CollectPSOInitializers`（`MobileBasePass.cpp:1056`）顶部对两个新 MeshPassType 早退 `return`。

**后果**：两个新 Pass **不进入 PSO 预缓存**。运行时首次绘制时按需编译 PSO，在 VR 里会**卡顿**（ hitch）。

**建议**：
- 原型阶段可接受。
- 生产化时应**不要早退**，而是像既有 `CollectPSOInitializersForLMPolicy`（1006）那样为新 Pass 生成 PSO 初始值（注意渲染目标格式：深度 Pass `CW_NONE`、颜色 Pass `CW_RGBA`，深度访问不同会生成不同 PSO key）。
- 或至少确认 `r.PSO.CollectAfterAtlas`/运行时 PSO 收集能兜底。

---

## 4. 针对用户疑虑（Plan1.md 第 6–9 行）：是否改用 `RenderPrePass`/`FDepthPassMeshProcessor`？

用户担心：复用 `FMobileBasePassMeshProcessor` 写深度"仍会绑定颜色相关 VS/PS"。

### 4.1 事实核对

- 复用 `FMobileBasePassMeshProcessor` 时，`Process()` 经 `MobileBasePass::GetShaders`（`MobileBasePass.cpp:930`，实现 `:207`）选的是 **BasePass 的 `TMobileBasePassVSPolicyParamType<FUniformLightMapPolicy>` / PS**，且**选择不依赖 MeshPassType**——即无论传 `MobileAfterTranslucencyDepthPass` 还是 `BasePass`，都绑 BasePass 着色器。用户的担心**属实**：深度 Pass 会跑完整 BasePass PS，靠 `CW_NONE` 丢弃颜色输出。
- 注意：`CW_NONE`（blend write mask）在移动 GPU 上**不跳过 PS 执行**，PS 仍逐像素跑，只是不写 color target。所以是**性能开销**，不是正确性问题。

### 4.2 为什么"反而应保留复用"——深度一致性

颜色读 Pass 用 `CF_DepthNearOrEqual` 测试深度，要求"深度写 Pass 写入的深度"与"颜色 Pass 计算的深度"**逐像素相等**才能通过测试。这要求**两个 Pass 用完全相同的 VS**（含 WorldPositionOffset、骨骼蒙皮、顶点动画等）。

- 复用 `FMobileBasePassMeshProcessor`：深度写 Pass 与颜色读 Pass 都用 BasePass VS → **深度完全一致** → 近等测试稳定通过。✅
- 改用 `FDepthPassMeshProcessor`（`DepthRendering.cpp`，`AddMeshBatch:1021` 门控 `bUseForDepthPass`）：其使用 **depth-only VS**（`PositionOnlyDepthVS` 类）。若标记材质带 WPO/蒙皮，depth-only VS 可能**不计算 WPO 或算得不同** → 深度与颜色 Pass 的 BasePass VS **发散** → 近等测试失败 → 出现**破洞/锯齿/深度冲突**。⚠️

### 4.3 为什么不直接复用 `FMobileSceneRenderer::RenderPrePass`（`DepthRendering.cpp:666`）

- 它是**全场景**深度 Pass（对所有 `bUseForDepthPass` 物体），放在 `RenderMobileBasePass` 之后会**重画全部不透明深度**（冗余、且可能覆盖）。
- 它的语义是"PrePass"，设计上在 BasePass **之前**清/填深度，不是"在某物体之后补深度"。
- 它没有 `bRenderAfterTranslucency` 门控，无法只画标记物体。
- 集成它进 subpass 路径比新建专用 Pass 更绕。

### 4.4 结论

**保留 Plan 的"复用 `FMobileBasePassMeshProcessor` 做深度 Pass"方案，不建议改用 `RenderPrePass`/`FDepthPassMeshProcessor`。** 代价是深度 Pass 跑了完整 BasePass PS（性能可接受，因为标记物体通常不多）。若要省这点性能，更稳妥的做法是：仅在 `!bIsFullDepthPrepassEnabled` 且标记物体无 WPO/蒙皮时，再考虑 depth-only 优化——但复杂度收益比不高，建议先不做。

---

## 5. 其它问题与风险

### 5.1 `PrimitiveViewRelevance.h` 的冗余初始化

`FPrimitiveViewRelevance` 构造函数（`:90`）先 `memset` 把整个结构清零，再设 `bRenderInMainPass=true` 等。故 Plan 加的 `bRenderAfterTranslucency = false`（`:103` 附近）**是冗余的**（已被 memset 置 0）。无害，但可删。需确认 `bRenderAfterTranslucency` 位域确实落在 `memset` 覆盖范围内（同一结构内，是的）。

### 5.2 `bRenderInMainPass` 仍需保持 true

标记物体必须保持 `bRenderInMainPass=true`（默认），才能进入 `SceneVisibility.cpp:1556` 的 `bUseForMaterial && (bRenderInMainPass || bRenderCustomDepth)` 门，进而走到 AfterTranslucency 分支。Plan 没要求用户改 `bRenderInMainPass`，正确。但需在文档写明："不要把标记物体的 `bRenderInMainPass` 设为 false"。

### 5.3 `RenderCustomRenderPassBasePass`（`MobileShadingRenderer.cpp:857`）未覆盖

Plan 未在该路径（SceneCapture / 自定义渲染 Pass / Planar Reflection）加新 Pass。即**这些场景下标记物体不会有 AfterTranslucency 效果**。若 VR 应用用了 SceneCapture（如反射、抓取、合成纹理），需评估是否也要支持。

### 5.4 静态缓存命令的脏标记

新 Pass 用 `EMeshPassFlags::CachedMeshCommands | MainView`，命令在场景添加时缓存。`SetRenderAfterTranslucency` 必须触发 `MarkRenderStateDirty()` 重建缓存命令（Plan 的 setter 应确保这点）。建议确认 `FPrimitiveSceneInfo::AddToScene`/`UpdateStaticDrawLists` 的脏标记集是否自动包含新 Pass——一般由 `CachedMeshCommands` Flag + 注册机制自动覆盖，但仍建议运行时验证"开/关标记后画面正确变化"。

### 5.5 `bRenderAfterTranslucency` 的位域与序列化

- `PrimitiveComponent`/`PrimitiveSceneProxy`/`Desc` 三处加的位域需保证位对齐与默认值一致（默认 false）。`Desc` 是游戏线程→渲染线程的传输结构，三处必须同步加（Plan 已覆盖）。
- 无需序列化（运行时标记，不存盘）。

---

## 6. 建议修改清单（给 Plan1.md）

1. **【必须】** 深度写 Pass 调用处用 `if (!bIsFullDepthPrepassEnabled)` 包裹（`MobileShadingRenderer.cpp:1609`、`:1682` 两处之后），避免空操作；或在 `RenderMobileAfterTranslucencyDepthPass` 内部判断。
2. **【必须】** 文档约束：标记物体不得设 `bUseForDepthPass=false`，否则 `bIsFullDepthPrepassEnabled` 模式下深度缺失。
3. **【必须】** 澄清 Plan 里"显式设 `<false, CF_DepthNearOrEqual>` 即可不写深度"——实际被 `SetOpaqueRenderState` 覆盖；颜色 Pass 不写深度靠 subpass1/Pass2 只读附件。若要由状态保证，给处理器传 `ForcePassDrawRenderState`。
4. **【建议】** 在 `SceneVisibility.cpp` 的 AfterTranslucency 分支移除 `MobileBasePassCSM` 的 `AddCommandsForMesh`（消除死代码/不一致）；并在文档写明"标记物体不参与 CSM"。
5. **【建议】** `CollectPSOInitializers` 不要早退，或确认运行时 PSO 收集兜底，避免 VR 卡顿。
6. **【修正】** 行号/路径订正：
   - `MobileBasePass.cpp` `REGISTER` 行 1223 → 1218；
   - `MobileBasePassRendering.cpp` `RenderMobileBasePass` 行 324/492 → 470；
   - `SceneRendering.h`、`BasePassRendering.h` 路径 `Public` → `Private`。
7. **【建议】** 第 6–9 行疑虑更正为："复用 `FMobileBasePassMeshProcessor` 确会跑完整 BasePass PS（CW_NONE 丢弃颜色），但这是**深度一致性所需**，不建议改用 `FDepthPassMeshProcessor`/`RenderPrePass`（会导致 WPO/蒙皮材质深度发散）。"
8. **【建议】** `PrimitiveViewRelevance.h` 里 `bRenderAfterTranslucency = false` 可删（memset 已清零）。
9. **【可选】** 评估是否需在 `RenderCustomRenderPassBasePass`（SceneCapture）也支持新 Pass。

---

## 7. 附：核对所涉文件与关键行号（5.4.4 实测）

| 文件 | 关键符号 | 行 |
|------|----------|----|
| `Renderer/Public/MeshPassProcessor.h` | `enum Type`、`NumBits=6`、`Num==32+4/32` asserts | 34/77/128/130 |
| `Renderer/Private/MobileBasePass.cpp` | `AddMeshBatch`、`Process`、`CollectPSOInitializers`、`CreateMobileBasePassProcessor`、`REGISTER` | 867/892/1056/1151/1218 |
| `Renderer/Private/MobileBasePassRendering.cpp` | `RenderMobileBasePass`、`SetOpaqueRenderState`(<true,CF_DepthNearOrEqual>) | 470/570 |
| `Renderer/Private/MobileShadingRenderer.cpp` | `bIsFullDepthPrepassEnabled`、`RenderForwardSinglePass`、`RenderForwardMultiPass`、`BuildInstanceCullingDrawParams`、`InitRenderTargetBindings_Forward` | 302/1578/1662/1433/1448 |
| `Renderer/Private/SceneVisibility.cpp` | 移动静态块、`ComputeDynamicMeshRelevance` | 1556-1583/2198-2231 |
| `Engine/Public/PrimitiveViewRelevance.h` | `bRenderInMainPass`、ctor(memset) | 54/90 |
| `Engine/Private/StaticMeshRender.cpp` | `GetViewRelevance` | 2055 |
| `Engine/Private/SkeletalMesh.cpp` | `GetViewRelevance` | 7107 |
| `Engine/Classes/Components/PrimitiveComponent.h` | `bRenderInMainPass:1`、`SetRenderInMainPass` | 408/1918 |
| `Engine/Private/Components/PrimitiveComponent.cpp` | `bRenderInMainPass=true`、`SetRenderInMainPass` | 333/4457 |
| `Engine/Public/PrimitiveSceneProxy.h` | `ShouldRenderInMainPass`、`bRenderInMainPass:1` | 700/1200 |
| `Engine/Private/PrimitiveSceneProxy.cpp` | `InitializeFrom`(Desc)、构造函数 | 265-277/398-428 |
| `Engine/Public/PrimitiveSceneProxyDesc.h` | `bRenderInMainPass=true/:1` | 25/93 |
| `Renderer/Private/SceneRendering.h` | `RenderMobileBasePass` 声明、InstanceCullingDrawParams 成员 | 2695/2793-2796 |
| `RenderCore/Public/RenderCore.h` | `STAT_BasePassDrawTime` EXTERN | 44 |
| `RenderCore/Private/RenderCore.cpp` | `DEFINE_STAT(STAT_BasePassDrawTime)` | 65 |
| `Renderer/Private/BasePassRendering.h` | `DECLARE_GPU_DRAWCALL_STAT_EXTERN(Basepass)` | 144 |
| `Renderer/Private/DepthRendering.cpp` | `FMobileSceneRenderer::RenderPrePass`、`FDepthPassMeshProcessor::AddMeshBatch`(bUseForDepthPass) | 666/1021 |

---

*分析完成。核心方案可行；按 §6 清单处理 4 项正确性问题后即可落地实现。*
