1. Docs\DepthPassPlan_CX_GPT_7_1.md 其中第二条2. 透明后颜色 pass 复用 MobileBasePass
是合理的，但要强制 render state
MobileAfterTranslucencyPass 需要画 opaque 颜色，可以复用
FMobileBasePassMeshProcessor。不过它必须固定为：

color write：CW_RGBA
depth access：DepthRead_StencilRead
depth state：TStaticDepthStencilState<false, CF_DepthNearOrEqual>
flags：需要带 ForcePassDrawRenderState
否则 FMobileBasePassMeshProcessor::Process 内部可能根据 masked/full prepass 逻辑改
成 CF_Equal，或者通过 MobileBasePass::SetOpaqueRenderState 又把 depth write 打开。
这里的会改成CF_Equal或者打开depth write吗？Engine/Source/Runtime/Renderer/Private/
MobileShadingRenderer.cpp:1614这里开启了NextSubpass,这些渲染状态设置和深度图读写设
置等是初始化Subpass时就已经设置好了还是根据Pass的Processor创建时确定的？// scene
depth is read only and can be fetched这是NextSubpass的注释，找到Subpass如何创建如
何设置等，哪里确定了是否可读写深度，只关注移动端Forward管线，需要给出代码佐证，最
终确认2中否则中的疑问，将结果附到文档后方

