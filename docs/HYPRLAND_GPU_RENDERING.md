# Luxaxis GPU Wallpaper 渲染可行性

研究日期：2026-09-28。本文保留迁移前的源码研究与设计依据；该路线随后已实现，
当前代码与实测范围见 [验证记录](./VALIDATION.md#gpu-renderer)。本文不是性能
基准。产品行为以 [已接受规格](./HYPRLAND_PLUGIN.md) 为准。
Noctalia 和 Hyprland 两个交付物保持独立。

## 固定基线与证据边界

- Hyprland：`v0.56.2`，提交
  `efb50993780079460b0cbed1363e2166a2de1d9f`，对应规格的 CachyOS/Arch
  `hyprland 0.56.2-3`。下文宿主链接固定到此 tag。
- Noctalia：官方 `v5.1.0`，提交
  `c7b9197af77ff22bfb9a83c52a95643a1d90ca86`，不是 v4/QML。
- 官方 Hyprland plugins 的相邻示例：`v0.56.0`，提交
  `7644cecdb947060682891a0db2a0cdc5c0b9e704`。它仅说明已有插件如何使用
  `EK_CUSTOM`，不是对 `v0.56.2` 的 ABI 或运行验证。
- 本文的接口结论来自源码。C++ 编译成功不能证明 shader 已经在真实 EGL/GPU
  上链接、像素正确、无状态污染或性能达标。

迁移前的 CPU Spotlight 栅格、动态纹理上传和 tiled transition 路径见
[此前的源码研究](./NOCTALIA_WALLPAPER_PERFORMANCE.md)。迁移目标是把这些像素
计算放到 GPU，保留 Engine、异步解码、cache、workspace/output 语义和 damage
调度，不把 Noctalia 的客户端事件循环搬入 compositor。

## 已核实的 Hyprland 扩展点

### 自定义 pass 与 damage

`IPassElement` 已定义 `EK_CUSTOM`。`v0.56.2` 的准确入口是：

```cpp
virtual std::vector<UP<IPassElement>> draw();
```

不是旧式 `draw(const CRegion&)`。`IElementRenderer::drawCustom` 调用它，再将返回
的各个 element 交给标准 dispatcher，并传入原 pass 的 damage。因此自定义
pass 可以直接绘制后返回空 vector，也可以先在 GPU 合成，再返回一个标准
`CTexPassElement`。不需要修改宿主 enum 或替换 `IElementRenderer`。
来源：[PassElement.hpp，6 行][h-pass-interface]；
[ElementRenderer.cpp，626 行][h-custom-dispatch]。

`CRenderPass` 在调用每个 element 前把该 element 的 damage 写入
`g_pHyprRenderer->m_renderData.damage`，所以 `draw()` 可在执行时读取它。
`boundingBox()` 与 `opaqueRegion()` 使用 output-local **逻辑坐标**；宿主进行
scale 换算和从后向前的遮挡简化。完整不透明的 wallpaper pass 可声明完整
逻辑 bounds/opaque region，让完全遮挡的绘制被简化掉，而不是强行设置
`undiscardable` 或全局禁用 simplification。
来源：[Pass.cpp，31 行][h-simplify]、[187 行][h-pass-draw]；
[PassElement.hpp，35 行][h-pass-coordinates]。

这只说明宿主能跳过绘制，不自动替代 Engine 的动画推进、Spotlight damage
停止与重新显露时刷新策略；这些仍需按照规格验证。

### 正确的执行时机

宿主先加入 background layer-shell passes，再发送 `RENDER_POST_WALLPAPER`，
随后加入 bottom layer-shell passes；实际 GL pass 执行在后续
`m_renderPass.render(...)`。因此 stage callback 应**加入自定义 pass**，在它的
`draw()` 中执行 GL。若在 stage callback 立即 GL draw，便没有沿用延迟 pass
的绘制顺序和遮挡判定。
来源：[Renderer.cpp，1134 行][h-stage]、[1162 行][h-stage-other]；
[GLRenderer.cpp，84 行][h-gl-pass]。

这与 Luxaxis 要求的 background < wallpaper < bottom 顺序相容。标准 texture
pass 的 data 没有供调用者指定任意 shader 的字段，所以不能只给现有
`CTexPassElement` 填一个 shader 指针来完成迁移。
来源：[TexPassElement.hpp，34 行][h-tex-data]。

官方相邻版本的 hyprbars 也使用 `EK_CUSTOM` 和上述 vector-return 签名；其
`draw()` 执行自定义绘制后返回空 vector。该示例支持这种模式，但不会证明
Luxaxis 的 wallpaper shader 已运行正确。
来源：[BarPassElement.hpp，16 行][p-bar-interface]；
[BarPassElement.cpp，12 行][p-bar-draw]。

## GPU 合成与标准呈现路径

研究时提出、随后实现采用的路线如下；实测范围仍以验证记录为准：

```text
RENDER_POST_WALLPAPER 加入自定义 pass
  -> pass 执行时获取简化后的 damage
  -> plugin-owned、明确标记 sRGB 的 GPU FBO
     合成 source textures + transition + Spotlight
  -> 恢复宿主 GL/render state
  -> 返回 CTexPassElement
  -> 宿主标准 texture/color-management/output-transform 路径呈现
```

两个绘制阶段都在 GPU；没有必要读回 CPU 像素。相比直接绘制到宿主当前
framebuffer，这条路线增加一个中间 render target 和它的写入/采样带宽，但能
继续使用宿主标准纹理呈现处理。不能把它称为“整条管线单次 draw”或“零额外
显存”。静态 `none` 路径是否仍直接返回源纹理，可在迁移设计中单独确定。
API 依据：[GLRenderer.cpp，273 行][h-create-fb]；
[Framebuffer.hpp，19 行][h-fb-interface]；[h-custom-dispatch]。

compose 应发生在未被舍弃的 pass 的 `draw()` 中。按 dirty region 更新持久
FBO 需要额外保证：首次分配、resize、profile/fit 改变或失效时完整初始化；
局部更新保留未改像素；遮挡恢复时不能呈现过期区域。它不是仅把全帧 draw
换成一个 scissor 就自动正确。

### Context、FBO 与生命周期

GL renderer 在 `initRender` 使 compositor EGL context current；`createFB`
也使用这一 context。shader 创建、纹理上传及 GL 资源删除都应留在正确的
context 和宿主 render-thread 生命周期中，而不是在异步解码 worker 调用 GL。
不用嵌入新的 EGL window 或 Wayland backend。
来源：[GLRenderer.cpp，45 行][h-context]、[273 行][h-create-fb]；
[OpenGL.cpp，709 行][h-make-current]。

`bindTempFB` 的 scope guard **仅恢复 framebuffer**，不是完整 GL state guard。
而且 `CGLFramebuffer::bind()` 在有当前 output 时把 viewport 设成 output 的
pixel size，不一定是插件 FBO 的尺寸。插件必须显式设置自己的 viewport，并
正确恢复 projection、renderModif、damage/scissor 等相关状态。
来源：[Renderer.cpp，871 行][h-bind-fb]；
[GLFramebuffer.cpp，91 行][h-fb-bind]。

`IFramebuffer::alloc` 在尺寸和 format 相同时可复用已有分配；
`setImageDescription` 会把描述传到 attached texture。FBO attachment 完整性
检查使用 `RASSERT`，不能假定分配失败一定可作为普通错误恢复。应预检尺寸和
GL 上限，并明确 output hotplug/resize、plugin unload 与部分初始化失败的
资源回收顺序。
来源：[Framebuffer.cpp，9 行][h-fb-alloc]、[57 行][h-fb-description]；
[GLFramebuffer.cpp，15 行][h-fb-internal]。

shader `CShader::createProgram` 默认 compile/link 失败也使用 `RASSERT`；
`dynamic=true` 分支才返回失败。新 shader 不应因为编译不通过直接终止宿主，
还需检查失败分支的资源清理，不能把返回 false 等同于所有临时 GL 对象已清理。
自定义 uniform 名称可通过 program 查询；宿主的标准 uniform location 表是
硬编码的。内置 VAO 使用 `pos` 和 `texcoord` attributes，不能原样移植 Noctalia
的 `a_position` 顶点接口。
来源：[Shader.cpp，51 行][h-shader-create]、[121 行][h-shader-uniforms]、
[228 行][h-shader-vao]、[403 行][h-shader-destroy]。

宿主接口有 renderer type 和 GL backend 入口，shader adapter 仍需确认实际
backend 是 GL；不能从 `RT_VK` enum 推断此版本已有可直接复用的 Vulkan 路径。
来源：[Renderer.hpp，64 行][h-renderer-types]；
[Renderer.cpp，226 行][h-gl-backend]。

### 不污染宿主的 GL 状态

Hyprland 缓存 current program、viewport、scissor 与 blend/scissor/stencil enable
状态。裸用 `glUseProgram/glViewport/glEnable/glDisable/glScissor` 后若缓存与
实际状态不一致，后续窗口、bar 或其他 pass 会拿到错误状态。应沿用宿主的
`useShader`、`setViewport`、cap/scissor helpers，并准确恢复所有改动。
来源：[OpenGL.cpp，981 行][h-blend-scissor]、[2561 行][h-state-cache]。

另外仍要检查 active texture、texture binding/parameters、VAO/VBO、color mask、
blend functions、framebuffer 和 projection。`ITexture` 自身也持有 texture
参数状态；不要绕过其 API 修改参数后让宿主继续使用旧缓存。宿主的常用 alpha
混合为 `GL_ONE, GL_ONE_MINUS_SRC_ALPHA`，shader/source 必须遵守对应的
premultiplied-alpha 约定，最终 wallpaper 还须满足不透明语义。
来源：[Texture.hpp，40 行][h-texture]；[OpenGL.cpp，981 行][h-blend-scissor]。

### 色彩管理与输出变换

宿主 `renderToFBInternal` 按 source texture image description 和 target
framebuffer/work-buffer description 选择转换。当前 framebuffer 不保证是
sRGB；标准 CM uniform 配置方法也不是任意插件可直接调用的公开接口。因此，
直接把普通 sRGB fragment output 写入当前 work buffer，不能假定与标准 texture
pass 的颜色相同。
来源：[OpenGL.cpp，1285 行][h-cm-path]；
[OpenGL.hpp，344 行][h-cm-private]。

插件 sRGB FBO 应携带真实的 image description，再作为 texture 交回标准 pass。
`getDefaultImageDescription()` 会受配置影响，不等于固定 sRGB；宿主另有明确的
`DEFAULT_SRGB_IMAGE_DESCRIPTION`。描述是像素的标签，不会自动修复 shader
错误的颜色/alpha 计算。
来源：[ColorManagement.hpp，397 行][h-srgb]；
[ColorManagement.cpp，528 行][h-default-desc]；[h-fb-description]。

标准 texture path 已处理 monitor transform、mirror projection 等；自定义合成
FBO 与交回的 texture 仍须统一坐标方向，避免重复旋转、Y 反转、错误 fractional
scale 或 cursor 位置。源码证明宿主有这些处理，不代表新增 shader 已经过这些
情况的像素测试。
来源：[Renderer.cpp，1820 行][h-projection]；
[ElementRenderer.cpp，414 行][h-tex-transform]；
[OpenGL.cpp，1521 行][h-texture-draw]。

## 借鉴 Noctalia v5 的 Transition

Noctalia `WallpaperProgram` 把两个 source、progress 和 effect 参数传给独立
fragment programs；fade 用 `mix`，wipe/disc 用每 fragment 的几何和
`smoothstep`，最终 wallpaper draw 是一个 fullscreen quad。可借鉴这些算法与
uniform 组织，而不是复制它自己的窗口、scene 或 texture cache。
来源：[wallpaper_program.cpp，72 行][n-fragments]、[365 行][n-draw]。

| Luxaxis 已接受类型 | Noctalia v5.1.0 可借鉴部分 | 必要适配 |
| --- | --- | --- |
| `fade` | fade 双源 `mix` | 两个 profile 的 sampling、Spotlight、alpha 语义 |
| `wipe` | wipe 的移动边界与 `smoothstep` | Luxaxis 的方向/origin 与 endpoint 语义 |
| `grow` | disc 的 aspect-correct 距离及最远角覆盖 | 按 Luxaxis origin 决定圆心，处理 softness 和 endpoints |
| `outer` | 通用双源 sampling 与柔边框架 | 本版本没有同名 shader，需实现向 origin 收缩的几何 |
| `clock` | 通用双源 sampling 与柔边框架 | 本版本没有同名 shader，需实现角度 sweep 及接缝 |
| `random` | 选择具体 shader 的机制 | 保留 Luxaxis allowlist 且不能立即重复 |
| `none` | 无需转场混合 | 立即切换，并保留 Spotlight 当前语义 |

可用 program 集合由 `ensureInitialized()` 明确列出 fade/wipe/disc/stripes/zoom/
honeycomb，不能把没有的 outer/clock 说成现成实现，也不应为复用而扩大产品
类型集合。来源：[wallpaper_program.cpp，288 行][n-programs]。

不能直接照搬的边界：

- **两侧 fit/position 独立。** Noctalia 用一个共享 `u_fillMode`，cover/contain
  采样偏移居中，没有 Luxaxis 的两个 profile 各自 `position` 的输入。Luxaxis
  应分别计算 from/to 的 UV，不能让目的 profile 的裁剪方式提前改变旧图。
  来源：[wallpaper_sampling_glsl.h，7 行][n-sampling]；
  [wallpaper_program.cpp，397 行][n-common-uniforms]；[规格](./HYPRLAND_PLUGIN.md)。
- **origin/direction 由 Luxaxis 决定。** Noctalia 为 wipe 随机选方向，为 disc
  随机选 `0.2..0.8` 的中心。Luxaxis 允许 cursor、center 或配置坐标，不能复制
  随机参数策略。来源：[wallpaper.cpp，78 行][n-random-params]；
  [Transition Model](./HYPRLAND_PLUGIN.md#transition-model-phase-2b)。
- **中断策略不同。** Noctalia 对 current/pending 之间的请求可反向或继续；
  无关第三张仅写 `queuedPath`，当前动画结束后才运行。Luxaxis 要求立即打断，
  从当前合成结果向最新目标转场。可借鉴 GPU FBO snapshot，但不得以 queue
  替换规格。snapshot 必须为独立 GPU 目标，不能同一纹理一边采样一边覆盖。
  来源：[wallpaper.cpp，1322 行][n-load]、[1383 行][n-redirect]、
  [1461 行][n-finish]；[Transition Model](./HYPRLAND_PLUGIN.md#transition-model-phase-2b)。
- **Spotlight 继续 live。** Noctalia 普通 wallpaper shader 没有 Luxaxis 的
  cursor Spotlight。快照不能把当时的 cursor 效果永久烘焙成固定区域；需确定
  wallpaper 合成结果与 live/interpolated mask 的表达，并验证连续多次中断。
  同类型参数插值、异类型 evaluated-mask 混合、同 wallpaper 跳过纹理混合仍以
  Luxaxis 规格为准，不由 Noctalia transition 代码替代。

Noctalia 此固定提交的 LICENSE 为 MIT。后续实现改编了双源 shader 设计与
fade/wipe/disc 的数学逻辑，版权和许可通知已保留在
[`noctalia-wallpaper.LICENSE`](../hyprland/third_party/noctalia-wallpaper.LICENSE)。
来源：[LICENSE][n-license]。

## 主要源码链接

[h-pass-interface]: https://github.com/hyprwm/Hyprland/blob/v0.56.2/src/render/pass/PassElement.hpp#L6
[h-pass-coordinates]: https://github.com/hyprwm/Hyprland/blob/v0.56.2/src/render/pass/PassElement.hpp#L35
[h-custom-dispatch]: https://github.com/hyprwm/Hyprland/blob/v0.56.2/src/render/ElementRenderer.cpp#L626
[h-simplify]: https://github.com/hyprwm/Hyprland/blob/v0.56.2/src/render/pass/Pass.cpp#L31
[h-pass-draw]: https://github.com/hyprwm/Hyprland/blob/v0.56.2/src/render/pass/Pass.cpp#L187
[h-stage]: https://github.com/hyprwm/Hyprland/blob/v0.56.2/src/render/Renderer.cpp#L1134
[h-stage-other]: https://github.com/hyprwm/Hyprland/blob/v0.56.2/src/render/Renderer.cpp#L1162
[h-gl-pass]: https://github.com/hyprwm/Hyprland/blob/v0.56.2/src/render/GLRenderer.cpp#L84
[h-tex-data]: https://github.com/hyprwm/Hyprland/blob/v0.56.2/src/render/pass/TexPassElement.hpp#L34
[p-bar-interface]: https://github.com/hyprwm/hyprland-plugins/blob/7644cecdb947060682891a0db2a0cdc5c0b9e704/hyprbars/BarPassElement.hpp#L16
[p-bar-draw]: https://github.com/hyprwm/hyprland-plugins/blob/7644cecdb947060682891a0db2a0cdc5c0b9e704/hyprbars/BarPassElement.cpp#L12
[h-context]: https://github.com/hyprwm/Hyprland/blob/v0.56.2/src/render/GLRenderer.cpp#L45
[h-create-fb]: https://github.com/hyprwm/Hyprland/blob/v0.56.2/src/render/GLRenderer.cpp#L273
[h-make-current]: https://github.com/hyprwm/Hyprland/blob/v0.56.2/src/render/OpenGL.cpp#L709
[h-fb-interface]: https://github.com/hyprwm/Hyprland/blob/v0.56.2/src/render/Framebuffer.hpp#L19
[h-bind-fb]: https://github.com/hyprwm/Hyprland/blob/v0.56.2/src/render/Renderer.cpp#L871
[h-fb-bind]: https://github.com/hyprwm/Hyprland/blob/v0.56.2/src/render/gl/GLFramebuffer.cpp#L91
[h-fb-alloc]: https://github.com/hyprwm/Hyprland/blob/v0.56.2/src/render/Framebuffer.cpp#L9
[h-fb-description]: https://github.com/hyprwm/Hyprland/blob/v0.56.2/src/render/Framebuffer.cpp#L57
[h-fb-internal]: https://github.com/hyprwm/Hyprland/blob/v0.56.2/src/render/gl/GLFramebuffer.cpp#L15
[h-shader-create]: https://github.com/hyprwm/Hyprland/blob/v0.56.2/src/render/Shader.cpp#L51
[h-shader-uniforms]: https://github.com/hyprwm/Hyprland/blob/v0.56.2/src/render/Shader.cpp#L121
[h-shader-vao]: https://github.com/hyprwm/Hyprland/blob/v0.56.2/src/render/Shader.cpp#L228
[h-shader-destroy]: https://github.com/hyprwm/Hyprland/blob/v0.56.2/src/render/Shader.cpp#L403
[h-renderer-types]: https://github.com/hyprwm/Hyprland/blob/v0.56.2/src/render/Renderer.hpp#L64
[h-gl-backend]: https://github.com/hyprwm/Hyprland/blob/v0.56.2/src/render/Renderer.cpp#L226
[h-blend-scissor]: https://github.com/hyprwm/Hyprland/blob/v0.56.2/src/render/OpenGL.cpp#L981
[h-state-cache]: https://github.com/hyprwm/Hyprland/blob/v0.56.2/src/render/OpenGL.cpp#L2561
[h-texture]: https://github.com/hyprwm/Hyprland/blob/v0.56.2/src/render/Texture.hpp#L40
[h-cm-path]: https://github.com/hyprwm/Hyprland/blob/v0.56.2/src/render/OpenGL.cpp#L1285
[h-cm-private]: https://github.com/hyprwm/Hyprland/blob/v0.56.2/src/render/OpenGL.hpp#L344
[h-srgb]: https://github.com/hyprwm/Hyprland/blob/v0.56.2/src/helpers/cm/ColorManagement.hpp#L397
[h-default-desc]: https://github.com/hyprwm/Hyprland/blob/v0.56.2/src/helpers/cm/ColorManagement.cpp#L528
[h-projection]: https://github.com/hyprwm/Hyprland/blob/v0.56.2/src/render/Renderer.cpp#L1820
[h-tex-transform]: https://github.com/hyprwm/Hyprland/blob/v0.56.2/src/render/ElementRenderer.cpp#L414
[h-texture-draw]: https://github.com/hyprwm/Hyprland/blob/v0.56.2/src/render/OpenGL.cpp#L1521
[n-fragments]: https://github.com/noctalia-dev/noctalia/blob/c7b9197af77ff22bfb9a83c52a95643a1d90ca86/src/render/programs/wallpaper_program.cpp#L72
[n-draw]: https://github.com/noctalia-dev/noctalia/blob/c7b9197af77ff22bfb9a83c52a95643a1d90ca86/src/render/programs/wallpaper_program.cpp#L365
[n-programs]: https://github.com/noctalia-dev/noctalia/blob/c7b9197af77ff22bfb9a83c52a95643a1d90ca86/src/render/programs/wallpaper_program.cpp#L288
[n-sampling]: https://github.com/noctalia-dev/noctalia/blob/c7b9197af77ff22bfb9a83c52a95643a1d90ca86/src/render/programs/wallpaper_sampling_glsl.h#L7
[n-common-uniforms]: https://github.com/noctalia-dev/noctalia/blob/c7b9197af77ff22bfb9a83c52a95643a1d90ca86/src/render/programs/wallpaper_program.cpp#L397
[n-random-params]: https://github.com/noctalia-dev/noctalia/blob/c7b9197af77ff22bfb9a83c52a95643a1d90ca86/src/shell/wallpaper/wallpaper.cpp#L78
[n-load]: https://github.com/noctalia-dev/noctalia/blob/c7b9197af77ff22bfb9a83c52a95643a1d90ca86/src/shell/wallpaper/wallpaper.cpp#L1322
[n-redirect]: https://github.com/noctalia-dev/noctalia/blob/c7b9197af77ff22bfb9a83c52a95643a1d90ca86/src/shell/wallpaper/wallpaper.cpp#L1383
[n-finish]: https://github.com/noctalia-dev/noctalia/blob/c7b9197af77ff22bfb9a83c52a95643a1d90ca86/src/shell/wallpaper/wallpaper.cpp#L1461
[n-license]: https://github.com/noctalia-dev/noctalia/blob/c7b9197af77ff22bfb9a83c52a95643a1d90ca86/LICENSE
