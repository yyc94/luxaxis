# Noctalia v5 Wallpaper 源码研究

研究目标：确认 Noctalia 官方 wallpaper 的实现方式，判断其 transition
实现能否直接复用到 Luxaxis Hyprland 插件。本文固定引用 Noctalia 官方仓库
`noctalia-dev/noctalia` 的提交
[`58f71922ea9ad5aa7225e88f2da80c976cc17005`](https://github.com/noctalia-dev/noctalia/tree/58f71922ea9ad5aa7225e88f2da80c976cc17005)，
避免随 `main` 漂移。

## 结论

不能把 Noctalia wallpaper 源码原样嵌入 Luxaxis Hyprland 插件。Noctalia v5
的 wallpaper 是一个 Wayland layer-shell 客户端子系统：它为每个 output 创建
background surface，在自己的 EGL/OpenGL ES context 中渲染，再通过 Wayland
提交 surface。Luxaxis 则在 Hyprland compositor 的 render stage 内绘制已经
拥有的 wallpaper pass；两者的 surface、事件循环、GPU context、present 和
生命周期边界不同。

可以复用或移植的范围是：

- transition 的视觉语义、参数随机化和 GLSL 片段中的数学逻辑；
- “当前纹理/目标纹理 + progress + transition 参数”的数据模型；
- 正向切换、反向中断和完成后提升目标纹理的状态机思路。

不能直接复用的范围是：

- `LayerSurface`、`wl_surface`、Wayland layer-shell 初始化和 click-through；
- Noctalia 的 EGL/GLES render backend、window surface、swap/present 路径；
- Noctalia 的 `RenderContext`、scene graph、`WallpaperNode` 和
  `AnimationManager` 对象；
- Noctalia 的配置、输出发现、自动轮换、IPC 和客户端资源释放流程。

因此 Luxaxis 应保持自己的 Hyprland adapter 和 Engine，只把经过审查的
transition 公式/语义作为参考或在 MIT 许可条件下移植。当前 Luxaxis 的
transition adapter 已采用同等的独立实现，不应把 Noctalia 的 Wayland 客户端
代码复制进插件。

## 官方仓库、版本与许可证

- 官方仓库：[noctalia-dev/noctalia](https://github.com/noctalia-dev/noctalia)。
- 本次研究提交：
  [`58f71922ea9ad5aa7225e88f2da80c976cc17005`](https://github.com/noctalia-dev/noctalia/commit/58f71922ea9ad5aa7225e88f2da80c976cc17005)。
- 仓库根目录的 [`LICENSE`](https://github.com/noctalia-dev/noctalia/blob/58f71922ea9ad5aa7225e88f2da80c976cc17005/LICENSE)
  是 MIT License，版权归 `noctalia-dev`（2026）。若复制或改编其源码的实质
  部分，需要保留 MIT 版权和许可声明；不能把这理解为 Noctalia 代码没有归属
  或可以去掉声明。
- 仓库另含独立第三方目录和许可证（例如 `third_party/wuffs`、Luau、fzy、
  Material Color Utilities）；移植 Noctalia 文件时必须分别检查其依赖的
  许可证，不能仅凭根目录 MIT 声明覆盖第三方代码。参见官方
  [`meson.build`](https://github.com/noctalia-dev/noctalia/blob/58f71922ea9ad5aa7225e88f2da80c976cc17005/meson.build)
  的依赖列表和仓库 `third_party/` 目录。

官方 FAQ 明确说明 v5 是原生 C++ 和 OpenGL ES shell，v4 才是 Quickshell：
[`docs/user/getting-started/faq.mdx#L39-L46`](https://github.com/noctalia-dev/noctalia/blob/58f71922ea9ad5aa7225e88f2da80c976cc17005/docs/user/getting-started/faq.mdx#L39-L46)。
这排除了把当前 v5 wallpaper 当作 QML/Qt 组件直接接入插件的假设。

## Wallpaper 的客户端渲染路径

### Per-output background surface

`Wallpaper::createInstance` 在每个输出上构造一个四边锚定的
`LayerSurfaceConfig`，layer 为 `background`，并调用 `setClickThrough(true)`。
configure callback 根据 surface 尺寸设置 scene root 和 `WallpaperNode` 大小；
surface 的 update callback 在客户端动画更新时刷新 renderer 状态。

源码：

- [`src/shell/wallpaper/wallpaper.cpp#L1227-L1275`](https://github.com/noctalia-dev/noctalia/blob/58f71922ea9ad5aa7225e88f2da80c976cc17005/src/shell/wallpaper/wallpaper.cpp#L1227-L1275)
- `WallpaperInstance` 保存的是 Wayland output 指针、`LayerSurface`、scene root、
  wallpaper node、两套纹理和 transition 状态：
  [`src/shell/wallpaper/wallpaper_instance.h#L23-L54`](https://github.com/noctalia-dev/noctalia/blob/58f71922ea9ad5aa7225e88f2da80c976cc17005/src/shell/wallpaper/wallpaper_instance.h#L23-L54)。

这一路径依赖 Wayland client protocol 和 Noctalia 自己的 layer-surface
封装。Hyprland 插件没有一个可安全嵌入的 Wayland client surface；插件应在
Hyprland 提供的 render pass 中画 wallpaper，而不是再建立一个客户端 surface。

### EGL/OpenGL ES renderer

`WallpaperRenderer::bind` 为一个 Wayland surface 创建默认 render backend、
`RenderTarget` 并绑定 shared EGL context；`render` 设置 viewport、清屏、调用
`drawWallpaper`，最后通过 `endFrame` 交换 Wayland surface buffer。

源码：

- [`src/render/wallpaper_renderer.cpp#L30-L139`](https://github.com/noctalia-dev/noctalia/blob/58f71922ea9ad5aa7225e88f2da80c976cc17005/src/render/wallpaper_renderer.cpp#L30-L139)
- offscreen framebuffer 路径和 present/blit 也在同一 renderer 中：
  [`src/render/wallpaper_renderer.cpp#L141-L247`](https://github.com/noctalia-dev/noctalia/blob/58f71922ea9ad5aa7225e88f2da80c976cc17005/src/render/wallpaper_renderer.cpp#L141-L247)。
- EGL/GLES context、Wayland-EGL surface、GPU reset 恢复等属于 Noctalia 的
  backend：[`src/render/backend/gles_render_backend.cpp`](https://github.com/noctalia-dev/noctalia/blob/58f71922ea9ad5aa7225e88f2da80c976cc17005/src/render/backend/gles_render_backend.cpp)
  和 [`src/render/gl_shared_context.cpp`](https://github.com/noctalia-dev/noctalia/blob/58f71922ea9ad5aa7225e88f2da80c976cc17005/src/render/gl_shared_context.cpp)。

官方 `meson.build` 把 wallpaper shader、renderer、GLES/EGL、Wayland-EGL 和
image/decode 组件编进整个 Noctalia 可执行程序，而不是产出一个独立 wallpaper
库：参见 [`meson.build#L540-L586`](https://github.com/noctalia-dev/noctalia/blob/58f71922ea9ad5aa7225e88f2da80c976cc17005/meson.build#L540-L586)、
[`meson.build#L1030-L1080`](https://github.com/noctalia-dev/noctalia/blob/58f71922ea9ad5aa7225e88f2da80c976cc17005/meson.build#L1030-L1080)。

## Transition 实现

### 状态机与中断

`Wallpaper::loadWallpaper` 将新路径加载为 `nextTexture`，保留
`currentTexture`，并在 transition 完成后调用 `promotePendingWallpaper`。
切换期间再次请求当前目标会复用或反向动画；不相关的新路径暂存到
`queuedPath`，当前动画结束后再处理。

关键源码：

- 加载、current/next texture 和首次显示：
  [`wallpaper.cpp#L1322-L1381`](https://github.com/noctalia-dev/noctalia/blob/58f71922ea9ad5aa7225e88f2da80c976cc17005/src/shell/wallpaper/wallpaper.cpp#L1322-L1381)
- transition 重定向、随机参数、动画启动：
  [`wallpaper.cpp#L1383-L1459`](https://github.com/noctalia-dev/noctalia/blob/58f71922ea9ad5aa7225e88f2da80c976cc17005/src/shell/wallpaper/wallpaper.cpp#L1383-L1459)
- 完成、纹理提升/丢弃和 queued wallpaper：
  [`wallpaper.cpp#L1461-L1505`](https://github.com/noctalia-dev/noctalia/blob/58f71922ea9ad5aa7225e88f2da80c976cc17005/src/shell/wallpaper/wallpaper.cpp#L1461-L1505)

这部分可以作为 Luxaxis transition state machine 的行为参考，但不能直接
复制：Noctalia 的状态机依赖 `AnimationManager`、`WallpaperInstance`、
`LayerSurface::requestRedraw` 和客户端纹理管理；Luxaxis 必须使用自己的
compositor frame scheduling、immutable render plan、texture pinning 和
Hyprland damage 机制。

### GLSL transition shader

Noctalia 将 transition 变成一组 GLES 片段 shader。公共代码负责按 fill mode
采样两个源（图片或纯色），随后按 transition 类型选择 program：

- `fade`：两个源直接 `mix`；
- `wipe`：按方向和 smoothness 移动直线边界；
- `disc`：以中心和 output aspect ratio 推导扩张圆/椭圆区域；
- `stripes`：按角度、stripe 数和局部延迟产生条带；
- `zoom`：两套 UV 做反向缩放后 cross-fade；
- `honeycomb`：蜂窝单元按距离展开。

源码：

- 公共采样、fill mode 和 `fade/wipe/disc/stripes/zoom/honeycomb` shader：
  [`src/render/programs/wallpaper_program.cpp#L13-L288`](https://github.com/noctalia-dev/noctalia/blob/58f71922ea9ad5aa7225e88f2da80c976cc17005/src/render/programs/wallpaper_program.cpp#L13-L288)
- shader program 初始化：
  [`wallpaper_program.cpp#L290-L310`](https://github.com/noctalia-dev/noctalia/blob/58f71922ea9ad5aa7225e88f2da80c976cc17005/src/render/programs/wallpaper_program.cpp#L290-L310)
- 所有 draw 输入集中在 `WallpaperDrawParams`，包括两层 source、progress、
  fill mode、transition 参数和变换：
  [`src/render/core/wallpaper_types.h#L56-L94`](https://github.com/noctalia-dev/noctalia/blob/58f71922ea9ad5aa7225e88f2da80c976cc17005/src/render/core/wallpaper_types.h#L56-L94)
- renderer 将纹理、progress 和 transition 参数转成 draw call：
  [`src/render/wallpaper_renderer.cpp#L86-L139`](https://github.com/noctalia-dev/noctalia/blob/58f71922ea9ad5aa7225e88f2da80c976cc17005/src/render/wallpaper_renderer.cpp#L86-L139)。

这些 shader 片段在 MIT 许可范围内可以作为实现参考或经过保留版权声明后
移植，但要重新适配 Hyprland 的 texture 类型、坐标系、output transform、
damage 和 render pass。直接复制整个 `WallpaperProgram` 仍会把 Noctalia 的
GLES shader wrapper、GL state 假设和 renderer 依赖带入插件，不符合 Luxaxis
的 adapter 边界。

### 参数随机化

Noctalia 在 C++ 侧为 wipe、disc、stripes、honeycomb 生成随机方向、中心、
条带数量、角度或 cell size，并使用固定的 `EaseInOutCubic` 将动画时间映射
到 progress：[`wallpaper.cpp#L44-L108`](https://github.com/noctalia-dev/noctalia/blob/58f71922ea9ad5aa7225e88f2da80c976cc17005/src/shell/wallpaper/wallpaper.cpp#L44-L108)。
这说明 transition 的随机参数应在状态机启动时固定，而不是每个 fragment
重新随机；Luxaxis 需要保持自己的配置语义和 random allowlist 规则。

## 与 Luxaxis Hyprland 插件的边界

Luxaxis 的规格要求插件在 Hyprland 的 compositor render stage 中拥有完整的
wallpaper pass，并明确不嵌入 hyprpaper 的 Wayland client loop。Noctalia 的
实现则恰好依赖 layer-shell client surface 和自己的 EGL swap 路径。因此：

| Noctalia 部分 | 是否直接复用 | Luxaxis 处理 |
| --- | --- | --- |
| transition 名称/视觉语义 | 否，语义可参考 | 用自己的 `Transition`/Engine/renderer 实现 |
| GLSL 几何和 `mix` 公式 | 可移植参考，MIT 条件下可改编 | 重新适配 Hyprland shader/render-pass 和坐标系 |
| current/next texture + progress 模型 | 行为可参考 | 使用 Luxaxis immutable RenderPlan 与全局缓存 pin |
| `WallpaperInstance` / `AnimationManager` | 不可直接复用 | Engine 状态和 compositor frame 调度 |
| `LayerSurface` / `wl_surface` / background layer | 不可复用 | Hyprland 内部 opaque wallpaper pass |
| `WallpaperRenderer` / `RenderTarget` / EGL swap | 不可复用 | `hyprland/src/hyprland_adapter.cpp` |
| Noctalia 配置、IPC、自动轮换 | 不在范围内 | 读取 Luxaxis TOML，只读 Hyprland workspace 配置 |

特别需要避免的误解：复用 Noctalia wallpaper 的“源码”不能让 Luxaxis 自动
获得 Noctalia 的 transition。Noctalia transition 只有在它自己的 surface
和 renderer 仍在运行时才生效；一旦 Luxaxis 接管 managed output 的 wallpaper
ownership，必须由 Luxaxis 自己执行纹理加载、transition、spotlight 和 damage。

## 对当前实现的决策

1. 保留 Luxaxis 自己的 render adapter、Engine、ImageCache 和 Transition
   实现，不引入 Noctalia 的 Wayland/EGL/scene 对象。
2. 允许参考或在 MIT 许可声明下改编 Noctalia transition shader 的视觉公式；
   任何实际复制应在代码文件中保留上游版权/许可证说明，并重新做 Hyprland
   output transform、fractional scale、damage、GPU 生命周期测试。
3. 不把 Noctalia 源码作为编译期依赖。两项目保持独立可构建、独立卸载，符合
   [`docs/HYPRLAND_PLUGIN.md`](./HYPRLAND_PLUGIN.md) 的 adapter seam 和
   wallpaper ownership 决策。
4. Noctalia 自带的 transition 种类不自动改变 Luxaxis 已接受的 Phase 2B
   合同；Luxaxis 仍以自身规格的 `fade/wipe/grow/outer/clock/random/none`
   为准。Noctalia 的 `disc/stripes/zoom/honeycomb` 仅是可选的视觉参考，
   不是新增接口承诺。

## 一手来源索引

- [Noctalia 官方仓库](https://github.com/noctalia-dev/noctalia)
- [固定提交 `58f71922`](https://github.com/noctalia-dev/noctalia/tree/58f71922ea9ad5aa7225e88f2da80c976cc17005)
- [MIT LICENSE](https://github.com/noctalia-dev/noctalia/blob/58f71922ea9ad5aa7225e88f2da80c976cc17005/LICENSE)
- [v5 C++/OpenGL ES FAQ](https://github.com/noctalia-dev/noctalia/blob/58f71922ea9ad5aa7225e88f2da80c976cc17005/docs/user/getting-started/faq.mdx#L39-L46)
- [Wallpaper lifecycle](https://github.com/noctalia-dev/noctalia/blob/58f71922ea9ad5aa7225e88f2da80c976cc17005/src/shell/wallpaper/wallpaper.cpp)
- [Wallpaper transition shaders](https://github.com/noctalia-dev/noctalia/blob/58f71922ea9ad5aa7225e88f2da80c976cc17005/src/render/programs/wallpaper_program.cpp)
- [Wallpaper render parameters](https://github.com/noctalia-dev/noctalia/blob/58f71922ea9ad5aa7225e88f2da80c976cc17005/src/render/core/wallpaper_types.h)
- [Wallpaper renderer](https://github.com/noctalia-dev/noctalia/blob/58f71922ea9ad5aa7225e88f2da80c976cc17005/src/render/wallpaper_renderer.cpp)
- [Build/dependency graph](https://github.com/noctalia-dev/noctalia/blob/58f71922ea9ad5aa7225e88f2da80c976cc17005/meson.build)
