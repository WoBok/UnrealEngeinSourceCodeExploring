我现在需要知道移动端前向渲染下的透明物体是如何进行混合到渲染到屏幕最终RT中的，这个RT的Alpha默认值是多少？这个混合公式是什么？这个RT中的Alpha是如何变化的？
举例说一个半透明物体的Alpha值是0.3，那么这个值是如何影响最终RT的Alpha的？
https://developer-cn.picoxr.com/document/unreal/seethrough/这个是Pico官方的文档，其中提到Eye Buffer，这个Eye Buffer是不是就是这个最终RT，文档中提到
会根据Alpha值做Eye Buffer与VST的混合，这里的Alpha应该就是指的Eye Buffer中的Alpha吧？也即最终RT的Alpha？
我现在需要弄明白的是我透明物体的Alpha如何影响了最终RT（Eye Buffer）的Alpha，进而影响了Eye Buffer与VST的混合？我这句话中的理解是否正确，如果正确给出答案，
如果理解有误则纠正，
你必须要列出的一些问题：
- 透明物体的Alpha如何与最终RT的Alpha混合，公式是什么？
- 最终RT的默认Alpha值是多少？
- r.Mobile.PICO.BlendModeSetting三种模式的混合方式是如何进行的？
- 透明物体的Alpha最终如何影响了Eye Buffer与VST的混合？
- 材质中的Write Alpha Only是如何进行的？它会直接覆盖RT中的Alpha吗？

Engine\PICO-Unreal-Integration-SDK路径下是Pico的SDK，你可以进行查看并进行验证

将结果保存到Engine\Docs中，md格式