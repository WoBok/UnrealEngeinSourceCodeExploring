# MobileBasePassRendering.cpp:472-475 性能分析宏解析

## 源码位置

`Engine/Source/Runtime/Renderer/Private/MobileBasePassRendering.cpp:472-475`

```cpp
void FMobileSceneRenderer::RenderMobileBasePass(FRHICommandList& RHICmdList, const FViewInfo& View, const FInstanceCullingDrawParams* InstanceCullingDrawParams)
{
    CSV_SCOPED_TIMING_STAT_EXCLUSIVE(RenderBasePass);     // 472
    SCOPED_DRAW_EVENT(RHICmdList, MobileBasePass);       // 473
    SCOPE_CYCLE_COUNTER(STAT_BasePassDrawTime);          // 474
    SCOPED_GPU_STAT(RHICmdList, Basepass);               // 475

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
```

## 上下文总览

这四行宏都位于 `FMobileSceneRenderer::RenderMobileBasePass` 的入口,用来在四个不同的 profiler 通道里标记/计时本函数体内的渲染工作。它们全部是在栈上构造的 RAII 对象,作用域到本函数 `}` 处自动析构,各自把这段代码的耗时/事件投递到不同的 profiler。

---

## 1. `CSV_SCOPED_TIMING_STAT_EXCLUSIVE(RenderBasePass)` — CSV Profiler

**宏定义**:`Engine/Source/Runtime/Core/Public/ProfilingDebugging/CsvProfiler.h:62-64`

```cpp
#define CSV_SCOPED_TIMING_STAT_EXCLUSIVE(StatName) \
    TRACE_CSV_PROFILER_INLINE_STAT_EXCLUSIVE(#StatName); \
    FScopedCsvStatExclusive _ScopedCsvStatExclusive_ ## StatName (#StatName, "CSV_"#StatName);
```

**作用**:

- 展开成两件事:`TRACE_CSV_PROFILER_INLINE_STAT_EXCLUSIVE("RenderBasePass")`(在关闭 CSV 时为空)以及一个栈对象 `FScopedCsvStatExclusive`。
- "EXCLUSIVE" 含义:在记录本 stat 时段内,会把同一 `CsvCategory` 下的"非独占"CSV 计时暂时挂起,避免子区间嵌套重复计入(典型用法是放在一帧的最外层工作区段,例如 RenderBasePass、RenderTranslucency 这种相互不重叠的 Pass)。
- `CSV` 是 Unreal 自带的 CSV(Comma-Separated Values)Profiler,通过 `csv.start` / `csv.stop` 控制台命令或 `-csvProfile` 命令行开关开启,生成 `.csv` 文件后可在 **Unreal Insights** 的 **CSV** 通道查看,或导入 Excel 做时序图。
- 这一行产生一行 `CSV_RenderBasePass` 的数据,记录该 Pass 在本帧独占的 CPU 耗时(也包含并行线程归约后的值,具体由 CSV 后端决定)。
- 当 CSV 未启用时(默认 Shipping 编译),整个宏为空,无运行时开销。

---

## 2. `SCOPED_DRAW_EVENT(RHICmdList, MobileBasePass)` — RHI Draw Event

**宏定义**:`Engine/Source/Runtime/RHI/Public/RHIGlobals.h` 与各 RHI 实现中的 `RHIDrawEventScope`。

**作用**:

- 构造一个 `SCOPED_DRAW_EVENT` 包装的对象(RHI 抽象层),它在 `Begin` 时调用 `RHICmdList.PushEvent("MobileBasePass", ...)`,在析构时 `PopEvent()`。
- 这是一对**带名字的 GPU 调试标记**,会被主流图形调试器抓取:
  - **RenderDoc**:会在 Event Browser 中显示为 `MobileBasePass` 节点,里面包含其后续所有 Draw/Dispatch。
  - **Xcode GPU Capture**(Metal):在编码到 GPU 命令流时变为可标记区间,便于在 Xcode timeline 上折叠/筛选。
  - **Android GPU Inspector / Snapdragon Profiler / Mali Graphics Debugger**:也会按这些 event 名解析。
  - **PIX / Razor**:同样识别。
- 与 `SCOPED_GPU_STAT` 不同,**DRAW EVENT 不参与耗时统计**,纯粹用来在 capture 工具里给一段 GPU 工作打标签、便于阅读与按 Pass 折叠。Shipping 默认编译下也基本是 no-op(通过 `#define SCOPED_DRAW_EVENT RHIDrawEventScope` 或空定义切换)。

---

## 3. `SCOPE_CYCLE_COUNTER(STAT_BasePassDrawTime)` — Stats 系统 CPU 计时

**宏定义**:`Engine/Source/Runtime/Core/Public/Stats/Stats.h:257-259`

```cpp
#define SCOPE_CYCLE_COUNTER(Stat) \
    FScopeCycleCounter StatNamedEventsScope_##Stat(TStatId(ANSI_TO_PROFILING(#Stat))); \
    SCOPE_CYCLE_COUNTER_TO_TRACE(#Stat, Stat, true);
```

**作用**:

- 构造一个 `FScopeCycleCounter`,在构造时调用 `FPlatformTime::Cycles64()`,析构时再次取 cycles,差值计入对应 stat id。
- `STAT_BasePassDrawTime` 必须在 `BasePassDrawTime` 这个 stat 组里有 `DECLARE_CYCLE_STAT` 声明(在 `Renderer/Private/StatGroupTrace.h` 或类似文件中),否则会被 stats 系统忽略。
- 写入位置:**引擎自带的 Stats 系统**(`STAT_` 系列),通过 `stat startfile` / `stat stopfile` 落盘,或 `stat namedevents` 在屏幕/控制台输出层级化数值。它也通过 `SCOPE_CYCLE_COUNTER_TO_TRACE` 桥接到 `TRACE_CPUPROFILER_EVENT_SCOPE`,在 **Unreal Insights** 的 CPU 通道里能看到 `BasePassDrawTime` 这个 scope(命名事件)。
- 这是 `RenderBasePass` 函数体被**引擎自己的统计系统**计入的地方,与第 1 条 CSV 不冲突(分别落到不同的 sink)。

---

## 4. `SCOPED_GPU_STAT(RHICmdList, Basepass)` — RHI GPU 耗时

**作用**:

- 构造一个 GPU 区间:在 `Begin` 时往 RHI 命令流里插入 GPU 计时查询(`RHIEndGPUStat`-类 timestamp query,平台抽象为 `FGPUProfiler` / `FGPUTiming`),`End` 时再插一对,析构时回收并计算差值。
- 写入位置:**Unreal Insights 的 GPU 通道**,以及屏幕 HUD 的 GPU stat 折叠区(`stat gpu`)。
- 注意和 #2 区分:`SCOPED_DRAW_EVENT` 仅是给 capture 工具看的标签,**不测量耗时**;`SCOPED_GPU_STAT` 会下发 GPU 计时 query 并把 GPU 时长(微秒,经 `FGPUTiming::GCalibrationTimestamp` 做 GPU↔CPU 时间校准)送回 stats/Insights。
- GPU 平台支持时(PC D3D11/12、Vulkan、Metal、含 PS5/Xbox/Switch),它真的插入 timestamp query;移动端某些 RHI 不可用时降级为空。
- 与 CPU 端的 `STAT_BasePassDrawTime` 不同,这里记录的是**整段 GPU 工作实际执行的时间**,包含并行线程提交后的整段命令流被 GPU 跑完的耗时;CPU 时间则只反映 `DispatchDraw` 调用本身的准备/参数设置成本。

---

## 四者的协同关系

| 宏 | 投递到的 profiler | 测的是什么 | 数据形态 |
|---|---|---|---|
| `CSV_SCOPED_TIMING_STAT_EXCLUSIVE(RenderBasePass)` | CSV Profiler → Insights CSV 通道 / `.csv` 文件 | CPU 耗时(独占) | 整型 tick,按帧聚合 |
| `SCOPED_DRAW_EVENT(RHICmdList, MobileBasePass)` | RenderDoc / Xcode / AGI / PIX 等 GPU 调试器 | 仅**标签**,不计时 | 字符串事件 |
| `SCOPE_CYCLE_COUNTER(STAT_BasePassDrawTime)` | `STAT_` 系统 + Insights CPU 通道 + `stat namedevents` HUD | CPU 耗时(含命名事件 trace) | 64-bit cycles → µs |
| `SCOPED_GPU_STAT(RHICmdList, Basepass)` | Insights GPU 通道 / `stat gpu` HUD | **GPU** 耗时(timestamp query) | 微秒,经校准 |

四者共同构成"RenderBasePass 这一段工作**既能在 Insights 上看到 CPU/GPU/CSV 三条独立时间线,也能在 RenderDoc 等 capture 里看到同名节点**"的完整观测体系。其中 #2 (DRAW EVENT) 是给抓帧调试用的,#1/#3 是 CPU 侧的双通道,#4 是 GPU 侧的耗时。

## 为什么移动端路径要特别强调这一组宏

Mobile BasePass 通常是移动端整帧渲染的瓶颈热点之一,UE 的 BasePass 在移动端是单 Pass forward 着色,所以把 BasePass 的 CPU 准备时间、CPU↔GPU 关联、GPU 耗时都精确打到四条 profiler 上,便于用 Insights 或 GPU capture 做端到端性能分析。

## 引用

- `Engine/Source/Runtime/Renderer/Private/MobileBasePassRendering.cpp:470-491` — `RenderMobileBasePass` 函数实现
- `Engine/Source/Runtime/Core/Public/ProfilingDebugging/CsvProfiler.h:62-64` — CSV_SCOPED_TIMING_STAT_EXCLUSIVE 宏定义
- `Engine/Source/Runtime/Core/Public/Stats/Stats.h:257-259` — SCOPE_CYCLE_COUNTER 宏定义
- `Engine/Source/Runtime/RHI/Public/RHIGlobals.h` — SCOPED_DRAW_EVENT 宏与 RHIDrawEventScope
- `Engine/Source/Runtime/RHI/Public/GPUProfiler.h` — FGPUTiming / GPU timestamp 查询机制