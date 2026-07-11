> 分析 Engine/Source/Runtime/Renderer/Private/MobileBasePass.cpp:1151 CreateMobileBasePassProcessor 中：
>
> ```cpp
> PassDrawRenderState.SetBlendState(TStaticBlendStateWriteMask<CW_RGBA>::GetRHI());
> PassDrawRenderState.SetDepthStencilAccess(DefaultBasePassDepthStencilAccess);
> PassDrawRenderState.SetDepthStencilState(TStaticDepthStencilState<true, CF_DepthNearOrEqual>::GetRHI());
> ```
>
> 这三个函数的主要作用。

# CreateMobileBasePassProcessor 中三个渲染状态设置的作用

## 所在上下文

`CreateMobileBasePassProcessor` 位于 `Engine/Source/Runtime/Renderer/Private/MobileBasePass.cpp:1151`，用于创建移动端 BasePass 的 `FMobileBasePassMeshProcessor`。函数先构造 `FMeshPassProcessorRenderState PassDrawRenderState`，再设置 BasePass 默认的混合、深度/模板访问方式和深度/模板测试状态，最后把该状态传给 `FMobileBasePassMeshProcessor`。

相关代码：

```cpp
FMeshPassProcessorRenderState PassDrawRenderState;
PassDrawRenderState.SetBlendState(TStaticBlendStateWriteMask<CW_RGBA>::GetRHI());
const FExclusiveDepthStencil::Type DefaultBasePassDepthStencilAccess = FScene::GetDefaultBasePassDepthStencilAccess(FeatureLevel);
PassDrawRenderState.SetDepthStencilAccess(DefaultBasePassDepthStencilAccess);
PassDrawRenderState.SetDepthStencilState(TStaticDepthStencilState<true, CF_DepthNearOrEqual>::GetRHI());
```

`FMeshPassProcessorRenderState` 本质上是 Mesh Pass 生成绘制命令时携带的一组渲染状态。它内部保存 `BlendState`、`DepthStencilState` 和 `DepthStencilAccess`，并在 `ApplyToPSO` 中把 `BlendState`、`DepthStencilState` 写入 `FGraphicsPipelineStateInitializer`，参与最终图形 PSO 的创建和缓存。

## 1. `SetBlendState(TStaticBlendStateWriteMask<CW_RGBA>::GetRHI())`

主要作用：设置 BasePass 的颜色混合状态为“不混合，直接写入 RGBA”。

`TStaticBlendStateWriteMask<CW_RGBA>` 是一个只控制 Render Target 写掩码的静态 Blend State。它继承自 `TStaticBlendState`，默认混合参数等价于：

- ColorBlendOp：`BO_Add`
- ColorSrcBlend：`BF_One`
- ColorDestBlend：`BF_Zero`
- AlphaBlendOp：`BO_Add`
- AlphaSrcBlend：`BF_One`
- AlphaDestBlend：`BF_Zero`
- ColorWriteMask：`CW_RGBA`

因此最终写入结果为：

```text
FinalColor = SourceColor * 1 + DestColor * 0
```

也就是像素着色器输出直接覆盖当前 Render Target 内容，不进行透明混合、加法混合或调制混合。

`CW_RGBA` 表示允许写入 R、G、B、A 四个通道。对于移动端 BasePass，这通常就是不透明/Masked 几何的默认颜色写入方式。

简要结论：

- 开启 RGBA 四通道写入。
- 使用默认 opaque blend。
- 不读取目标颜色参与混合。
- 确保 BasePass 材质输出直接写入当前颜色附件。

## 2. `SetDepthStencilAccess(DefaultBasePassDepthStencilAccess)`

主要作用：记录本 Pass 对深度/模板附件的访问权限，也就是深度和模板资源在 Render Pass 中应该以“读”还是“写”的方式使用。

这里的 `DefaultBasePassDepthStencilAccess` 来自：

```cpp
FScene::GetDefaultBasePassDepthStencilAccess(FeatureLevel)
```

其默认值是：

```cpp
FExclusiveDepthStencil::DepthWrite_StencilWrite
```

即 BasePass 默认可以写深度，也可以写模板。

但在 Deferred Shading 路径下，如果平台强制完整 Depth Prepass，并且 `r.BasePassWriteDepthEvenWithFullPrepass` 为 0，则会改为：

```cpp
FExclusiveDepthStencil::DepthRead_StencilWrite
```

也就是 BasePass 只读深度、不写深度，但仍可写模板。

`FExclusiveDepthStencil` 的含义不是“深度测试函数”，而是 Render Pass / RHI 资源访问层面的权限描述：

- `DepthWrite_StencilWrite`：深度可读写，模板可读写；用于没有完整预深度或 BasePass 仍需写深度的场景。
- `DepthRead_StencilWrite`：深度只读，模板可写；用于完整 Depth Prepass 已经写好深度，BasePass 只需要依据深度测试绘制，不再修改深度。
- `DepthRead_StencilRead`：深度和模板都只读，常见于透明 Pass。

这个状态会影响深度/模板目标的访问模式、资源屏障、DSV 视图选择，以及某些情况下 MeshDrawCommand/PSO 的状态修正。桌面 BasePass 的 `SetupBasePassState` 会根据该访问权限决定深度状态是否开启 Depth Write；移动端这里则直接设置了 Depth Write 开启的 `DepthStencilState`，同时保留 Access 用于 Render Pass 访问语义和后续逻辑判断。

简要结论：

- 表达 BasePass 对 Depth/Stencil 资源的访问权限。
- 默认通常是深度写 + 模板写。
- 在完整预深度场景下可能变成深度只读 + 模板写。
- 它偏向资源访问/RenderPass 语义，不等同于 GPU 深度比较函数本身。

## 3. `SetDepthStencilState(TStaticDepthStencilState<true, CF_DepthNearOrEqual>::GetRHI())`

主要作用：设置 BasePass 的 GPU 深度/模板测试状态：开启深度写入，并使用“更近或相等”作为深度测试条件。

模板参数含义：

```cpp
TStaticDepthStencilState<true, CF_DepthNearOrEqual>
```

- 第一个参数 `true`：开启 Depth Write。
- 第二个参数 `CF_DepthNearOrEqual`：深度测试通过条件是当前像素深度“更近或相等”。

`CF_DepthNearOrEqual` 是 UE 对普通 Z 和反向 Z 的抽象：

```cpp
CF_DepthNearOrEqual = ERHIZBuffer::IsInverted ? CF_GreaterEqual : CF_LessEqual
```

也就是说：

- 如果使用反向 Z，越大的 depth 值越靠近相机，因此使用 `CF_GreaterEqual`。
- 如果使用普通 Z，越小的 depth 值越靠近相机，因此使用 `CF_LessEqual`。

这样代码不需要关心底层 Z Buffer 方向，统一表达“离相机更近或相等的片元通过测试”。

该状态一般用于不透明/Masked BasePass：

- 通过深度测试剔除被遮挡片元。
- 对通过测试的片元写入深度，供后续 Pass 进行遮挡判断。
- `OrEqual` 允许与已有深度相等的片元通过，适配预深度、相同几何重复绘制、Masked/Early-Z 等情况。

注意：该行设置的是实际 GPU `DepthStencilState`，会进入 PSO；上一行 `SetDepthStencilAccess` 设置的是深度/模板附件访问权限。二者相关但不是同一个概念。

简要结论：

- 开启深度测试。
- 开启深度写入。
- 使用平台无关的“更近或相等”比较。
- 使移动端 BasePass 能正确遮挡并维护深度缓冲。

## 三者合起来的效果

这三行共同定义了移动端 BasePass 默认绘制不透明/Masked 网格时的核心固定管线状态：

1. 颜色输出：RGBA 全通道直接写入 Render Target，不做透明混合。
2. 深度/模板访问：按当前 FeatureLevel 和渲染路径决定深度/模板附件是读还是写。
3. 深度测试状态：按“更近或相等”进行深度测试，并默认开启深度写入。

因此，它们不是单纯的局部变量赋值，而是在创建 `FMobileBasePassMeshProcessor` 前，为后续 MeshDrawCommand 和 PSO 准备默认渲染状态。后续移动端 BasePass 中每个 MeshBatch 生成绘制命令时，都会以这组状态为基础，再结合材质、光照策略、CSM、Local Light、Rasterizer State 等信息生成最终绘制命令。

## 与相邻 Pass 的对比

在同文件后面的透明 Pass 创建函数中，可以看到类似但不同的状态：

```cpp
PassDrawRenderState.SetDepthStencilState(TStaticDepthStencilState<false, CF_DepthNearOrEqual>::GetRHI());
PassDrawRenderState.SetDepthStencilAccess(FExclusiveDepthStencil::DepthRead_StencilRead);
```

透明 Pass 使用 `false` 关闭深度写入，并将深度/模板访问设为只读。这与 BasePass 的默认行为形成对比：BasePass 负责建立主要场景颜色和深度，透明 Pass 通常只依据已有深度进行排序/遮挡，不应再改写深度。

## 参考代码位置

- `CreateMobileBasePassProcessor`：`Engine/Source/Runtime/Renderer/Private/MobileBasePass.cpp:1151`
- `FMeshPassProcessorRenderState::SetBlendState / SetDepthStencilState / SetDepthStencilAccess`：`Engine/Source/Runtime/Renderer/Public/MeshPassProcessor.h:1968`
- `TStaticDepthStencilState`：`Engine/Source/Runtime/RenderCore/Public/RHIStaticStates.h:197`
- `TStaticBlendStateWriteMask`：`Engine/Source/Runtime/RenderCore/Public/RHIStaticStates.h:371`
- `FScene::GetDefaultBasePassDepthStencilAccess`：`Engine/Source/Runtime/Renderer/Private/RendererScene.cpp:4648`
- `FExclusiveDepthStencil`：`Engine/Source/Runtime/RHI/Public/RHIResources.h:416`
- `CF_DepthNearOrEqual`：`Engine/Source/Runtime/RHI/Public/RHIDefinitions.h:302`
