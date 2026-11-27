Ran search DngSceneViewer and upscaling references · Exited with code 1
Ran list prog/samples directory
Ran search DngSceneViewer and upscalers with git grep
Ran list root dirs and grep snapdragon/fsr
Ran list samples directory
Analyzed output.txt:L1-274
Analyzed viewer.entities.blk:L680-800
Ran list dngSceneViewer files and grep upscaling settings
Analyzed settings.blk:L1-278
Analyzed dng_scene_viewer.config.blk:L1-36
Analyzed antialiasing.cpp:L1-280
Analyzed antialiasing.cpp:L1960-2200
Analyzed antialiasing.cpp:L2580-2760
Analyzed settings.cpp:L700-820
Analyzed antialiasing.cpp:L280-600
Analyzed antialiasing.cpp:L1350-1460
Analyzed worldRenderer.cpp:L250-330
Analyzed worldRenderer.cpp:L2000-2170
Ran read viewer cmd files and grep antialiasing config keys
Analyzed game_common_override.blk:L1-40
Analyzed FSR.cpp:L1-103
Analyzed SGSR.cpp:L1-35
Analyzed SGSR2.cpp:L1-48
Ran check dngSceneViewer main and config loading
Analyzed _graphics_presets.blk:L1-65
Analyzed settings.cpp:L90-280
Analyzed jamfile:L1-135
Ran check FSR driver support and config filename
Analyzed dx12.cpp:L1695-1760
Analyzed copy_bundles.jam:L1-47
Analyzing gameProjConfig.h:L1-60
Ran check config_name_prefix and viewer scripts
Ran check config_name_prefix, FSR support, and viewer binaries
Analyzed README.md:L1-9
Analyzed antiAliasingMode.h:L1-25
Analyzed antialiasing.cpp:L840-960
Analyzed antialiasing.cpp:L1920-1960
Analyzed game.jam:L1-60
Ran check GameUserConfigPathPrefix and SGSR shaders
Analyzed antialiasing.cpp:L1330-1360
Analyzed SnapdragonSuperResolution2.h:L1-100
Ran check sharpening and dynamicResolution usage
Ran check fullDeferred and DX12 fsrWrapper
Analyzed antialiasing.cpp:L2195-2235
Analyzed rendererFeatures.cpp:L1-70
Ran locate FsrWrapper in DX12 driver
Analyzed amdFsrD3D12_windows.cpp:L160-285
Analyzed _graphics_presets.blk:L1-65
Analyzed config.blk

在 `dngSceneViewer` 示例中，用户配置文件位于 [dng_scene_viewer.config.blk](file:///c:/DagorEngine/samples/dngSceneViewer/viewer/dng_scene_viewer.config.blk)（也可在开发构建时写入同目录的 [config.blk](file:///c:/DagorEngine/samples/dngSceneViewer/viewer/config.blk)，或通过命令行 `-config:...` 覆盖）。

抗锯齿/超分配置在 `video{}` 块中通过 `antialiasing_mode` 与 `antialiasing_upscaling` 控制（解析逻辑见 [antialiasing.cpp](file:///c:/DagorEngine/prog/gameLibs/render/antialiasing/antialiasing.cpp#L217-L283)）。

---

### 1. 配置 AMD FSR

在 [dng_scene_viewer.config.blk](file:///c:/DagorEngine/samples/dngSceneViewer/viewer/dng_scene_viewer.config.blk) 中配置如下：

```blk
video{
  driver:t="dx12"                    // Windows 下 FSR 必须使用 dx12 或 vulkan
  antialiasing_mode:t="fsr"
  antialiasing_upscaling:t="quality" // 可选: "native" | "quality" | "balanced" | "performance" | "ultra_performance"
  // antialiasing_fgc:i=1            // 可选: 开启 FSR 帧生成 (0=关, 1..N=生成帧数, -1=动态)
}

renderFeatures{
  fullDeferred:b=yes                 // 必须为 yes！FSR 需要 motion vectors，fullDeferred:b=no 会回退到 FXAA
}
```

> [!NOTE]
> - 驱动限制：Windows 下 `fsr` 仅允许 `dx12` 或 `vulkan`（见 [antialiasing.cpp](file:///c:/DagorEngine/prog/gameLibs/render/antialiasing/antialiasing.cpp#L505-L512)），且要求 Shader Model >= 6.2 并加载 FidelityFX DLL（见 [amdFsrD3D12_windows.cpp](file:///c:/DagorEngine/prog/engine/drv/drv3d_DX12/amdFsrD3D12_windows.cpp#L277)）。
> - 架构注意：`samples/dngSceneViewer/viewer/windows-x86_64` 中已包含 `amd_fidelityfx_*.dll`，但 `windows-arm64` 目录当前缺少对应的 ARM64 FidelityFX DLL（见 [copy_bundles.jam](file:///c:/DagorEngine/prog/engine/drv/drv3d_DX12/copy_bundles.jam#L37-L45)）；若在无对应 DLL 的环境下启用 `fsr`，初始化会失败并回退到 `tsr` 或 `off`。
> - 缩放参数：`tryInitFsr` 只认上述英文预设名，不支持 `"150"` 这类百分比数值。

---

### 2. 配置 Snapdragon Game Super Resolution (SGSR / SGSR2)

[game_common_override.blk](file:///c:/DagorEngine/samples/dngSceneViewer/prog/shaders/game_common_override.blk#L19-L20) 已将 `SnapdragonSuperResolution.dshl`（SGSR 1）和 `SnapdragonSuperResolution2.dshl`（SGSR 2）纳入编译列表（请确保已重新编译对应后端的 shader）。

#### 方式 A：SGSR 2（时域超分，推荐，对应 `sgsr2`）
```blk
video{
  antialiasing_mode:t="sgsr2"
  antialiasing_upscaling:t="quality" // 支持预设名，或数字百分比字符串如 "150" (表示 1.5x 缩放)
}

renderFeatures{
  fullDeferred:b=yes                 // 必须为 yes！SGSR2 需要 motion vectors，thin GBuffer 会回退到 FXAA
}
```

#### 方式 B：SGSR 1（空间超分，对应 `sgsr`）
```blk
video{
  antialiasing_mode:t="sgsr"
  antialiasing_upscaling:t="quality" // 同样支持预设名或数字百分比字符串如 "150"
}

renderFeatures{
  fullDeferred:b=no                  // SGSR 1 为非时域空间超分，fullDeferred 为 yes 或 no 均可工作
}
```

---

### 3. `antialiasing_upscaling` 可选值说明

* **通用预设名**（`fsr` / `sgsr` / `sgsr2` 均支持）：
  * `"native"`（1.0x）
  * `"quality"`（SGSR/SGSR2 为 1.5x；FSR 为 Quality）
  * `"balanced"`（SGSR/SGSR2 为 1.7x；FSR 为 Balanced）
  * `"performance"`（SGSR/SGSR2 为 2.0x；FSR 为 Performance）
  * `"ultra_performance"`（SGSR/SGSR2 为 3.0x；FSR 为 UltraPerformance）
  * `"ultra_quality"` / `"ultra_quality_plus"`（SGSR/SGSR2 为 1.3x；FSR 会回退为 Quality）
* **自定义倍率字符串**（仅 `sgsr` / `sgsr2` 支持）：
  * 例如 `"150"` 表示缩放因子 `1.5`（渲染分辨率 = 输出分辨率 / 1.5），`"200"` 表示 `2.0`。

---

### 4. 关键避坑点

1. **`renderFeatures { fullDeferred:b=yes }`**：你当前的 [dng_scene_viewer.config.blk](file:///c:/DagorEngine/samples/dngSceneViewer/viewer/dng_scene_viewer.config.blk#L14-L16) 设置了 `fullDeferred:b=no`。根据 [worldRenderer.cpp](file:///c:/DagorEngine/prog/daNetGame/render/world/worldRenderer.cpp#L2097-L2103)，`FSR` 和 `SGSR2` 在 thin GBuffer 下都会因为缺少 motion vectors 被强制降级为 `FXAA`，启用它们时务必改为 `fullDeferred:b=yes`。
2. **避免被 `consolePreset` 覆盖**：若使用 `viewer_low.cmd` / `viewer_medium.cmd` 启动，命令行传入的 `-config:graphics/consolePreset:t=...` 会从 [_graphics_presets.blk](file:///c:/DagorEngine/samples/dngSceneViewer/prog/gameBase/config/_graphics_presets.blk) 读取并设置 `antialiasing_mode:t="tsr"`；测试 `fsr` / `sgsr` / `sgsr2` 时建议使用 `viewer_dx12.cmd` 或直接通过命令行 `-config:video/antialiasing_mode:t=...` 指定。