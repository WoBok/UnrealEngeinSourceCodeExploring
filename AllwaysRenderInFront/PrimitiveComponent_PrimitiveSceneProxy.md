# `UPrimitiveComponent` 与 `FPrimitiveSceneProxy` 的关系

简短回答：**它们是「游戏线程对象」与其在「渲染线程的镜像/代理」之间的对应关系——一个组件 ↔ 一个 SceneProxy**，由组件通过虚函数 `CreateSceneProxy()` 工厂方法创建。

---

## 1. 类型本质不同

| 维度 | `UPrimitiveComponent` | `FPrimitiveSceneProxy` |
|---|---|---|
| 文件 | `Engine/Classes/Components/PrimitiveComponent.h` | `Engine/Public/PrimitiveSceneProxy.h` |
| 基类 | `USceneComponent` → `UActorComponent` → `UObject`（带 GC、反射、蓝图） | 普通 C++ 类，**不是 UObject** |
| 所在线程 | 游戏线程（Game Thread） | 渲染线程（Render Thread） |
| 生命周期 | 由 GC / Actor / 关卡管理 | 由 `FScene` 管理，靠组件注册/反注册驱动 |
| 角色 | “**逻辑端**”：保存可编辑属性、碰撞、Tick、事件、物理 | “**渲染端**”：保存绘制所需的精简数据（Bounds、Material、LOD、可见性、阴影标志等） |

`PrimitiveSceneProxy.h:200` 注释里说得很清楚：
> *Encapsulates the data which is mirrored to render a UPrimitiveComponent parallel to the game thread.*

也就是 SceneProxy 是组件**为渲染线程准备的一份镜像副本**，目的是让渲染线程在不触碰 UObject 的情况下完成绘制——避免线程竞争。

---

## 2. 创建 / 持有关系（一对一）

`PrimitiveComponent.h` 中：

```cpp
// PrimitiveComponent.h:1940
FPrimitiveSceneProxy* SceneProxy;

// PrimitiveComponent.h:2194
virtual FPrimitiveSceneProxy* CreateSceneProxy()  // 子类（StaticMesh、Skeletal、自定义）重写
{
    return nullptr;
}
```

- 组件 **拥有指针**，但实际生命周期由 `FScene` 控制。
- 当组件 `Register` 到世界时，引擎调用 `CreateSceneProxy()`，产物被加入 `FScene`。
- 当组件 `Unregister` / 属性变化触发 `MarkRenderStateDirty()` 时，旧 Proxy 在渲染线程被 `delete`，必要时重建。

反向引用在 `PrimitiveSceneProxy.h`：

```cpp
// PrimitiveSceneProxy.h:1497
FPrimitiveComponentId PrimitiveComponentId;   // 跨线程安全的 id

// PrimitiveSceneProxy.h:1542
const UPrimitiveComponent* ComponentForDebuggingOnly;  // 仅供调试，不能在渲染线程解引用
```

注意这里的设计：Proxy **不直接通过指针使用组件**，只保留一个 `FPrimitiveComponentId` 作为跨线程握手凭证，那个原始指针注释已经写明 *“useful for quickly inspecting properties... while debugging”*——避免渲染线程踩到游戏线程释放的 UObject。

---

## 3. 数据流向

```
Game Thread                    Render Thread
─────────────                  ─────────────
UPrimitiveComponent  ──创建──►  FPrimitiveSceneProxy
   │  属性变更                       │
   │  MarkRenderStateDirty           │
   ├──ENQUEUE_RENDER_COMMAND─►  Proxy::Set...()
   │  / SendRenderTransform          │
   ▼                                 ▼
 GC 销毁 (UObject)              FScene 删除 Proxy
```

- 修改组件属性（如材质、Transform）→ 组件把变化**编码成命令**通过 `ENQUEUE_RENDER_COMMAND` 投递给 Proxy。
- Proxy 永远不主动回查组件，而是等命令到达后更新自身镜像数据。

---

## 4. 实际继承层级配套

通常每实现一种新可绘制组件，都会**同时**继承两套：

| 组件侧（游戏线程） | Proxy 侧（渲染线程） |
|---|---|
| `UPrimitiveComponent` | `FPrimitiveSceneProxy` |
| `UStaticMeshComponent` | `FStaticMeshSceneProxy` |
| `USkeletalMeshComponent` | `FSkeletalMeshSceneProxy` |
| 自定义 `UMyComponent` | `FMyComponentSceneProxy`（在 `CreateSceneProxy()` 里 `new` 出来） |

---

## 一句话总结

> **`UPrimitiveComponent` 是游戏线程上的“业主”**，`FPrimitiveSceneProxy` 是它在渲染线程上对应的“分身”——前者通过 `CreateSceneProxy()` 工厂方法生成后者，并把渲染所需数据单向同步过去；它们一对一对应，但分属不同线程，刻意通过 `FPrimitiveComponentId` 解耦以保证多线程安全。
