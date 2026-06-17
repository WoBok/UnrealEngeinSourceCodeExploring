// 不透明 Processor：根据自身所属 Pass 与 Proxy 标记做"分流"
if (!bIsTranslucent && !Material.IsDeferredDecal())
{
    // 普通 BasePass / MobileBasePassCSM：跳过"延后绘制"的对象
    if (MeshPassType == EMeshPass::BasePass || MeshPassType == EMeshPass::MobileBasePassCSM)
    {
        return !bAfterTranslucent;
    }

    // 新增的 MobileBasePassAfterTranslucent：只接收"延后绘制"的对象
    if (MeshPassType == EMeshPass::MobileBasePassAfterTranslucent)
    {
        return bAfterTranslucent;
    }
}
return false;
这段代码修改成
if (!bIsTranslucent && !Material.IsDeferredDecal())
{
    // 普通 BasePass / MobileBasePassCSM：跳过"延后绘制"的对象
    if (MeshPassType == EMeshPass::BasePass || MeshPassType == EMeshPass::MobileBasePassCSM)
    {
        return !bAfterTranslucent;
    }

    // 新增的 MobileBasePassAfterTranslucent：只接收"延后绘制"的对象
    if (MeshPassType == EMeshPass::MobileBasePassAfterTranslucent)
    {
        return bAfterTranslucent;
    }
}