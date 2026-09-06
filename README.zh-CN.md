# SyncTrack Prep

[English](README.md) | **简体中文**

一个用于**同期声（Production / Sync Track）**的实时 **VST3** 插入效果器。它修复错误的声道布局、压住底噪、把对白电平推到统一区间，并守住真峰值天花板 —— 让同期对白在音乐床下面依然听得清。

<p align="center">
  <img src="assets/screenshot.png" alt="SyncTrack Prep 插件界面" width="320">
</p>

---

## 它解决什么问题

把摄影机或录音机的同期声导进视频后期工程时，遇到的麻烦基本是同一批：素材其实是单声道却只挂在一边、底噪飘、不同镜头之间对白电平跳、一加音乐峰值就削。SyncTrack Prep 就是一个插件，把这一轮"能用起来"的清理做完，让同期轨可以当参考轨或粗混来用。

### 信号链

```
In → ChannelRepair → Leveler → DenoiseStage → ToneShaper → OutputGain → UpwardExpander → PeakCompressor → TruePeakLimiter → Out
```

* **DenoiseStage** 通过 **Denoise Mode** 下拉选择：**Off**（不抑制）、**Classic**（自研频谱 STFT/OLA，延迟 575）、**Live (DFN3)**（DeepFilterNet3 / libDF，实时，约 30 ms）、**HQ (MossFormer2)**（MossFormer2_SE_48K + ONNX Runtime，仅离线渲染，4 秒前瞻）。延迟按模式上报，宿主据此补偿。
* Leveler 故意放在降噪**之前**：让选中的降噪引擎能把刚被抬起来的安静段底噪再刮掉一层。
* Leveler 发布每个采样后的 *scene level*，压缩器和向上扩展器都按这个采样级联 —— 动态跟随的是听者实际听到的电平，而不是输入端的快照。
* **Output 是整条链的补偿增益（makeup gain）**，位置在常开的安全级**之前**。因此 −1 dBTP 天花板在**任何**旋钮位置都成立：Output 往上推会把安静段抬到贴近天花板，任何试图越过天花板的部分都会被压缩器和限制器压回来。旋钮在 0 dB 时，整条链的行为跟没有这个旋钮一样。

### 术语

| 术语 | 含义 |
|---|---|
| **同期声（Production track）** | 导入的视频同期音频：对白与现场音效混在一起。 |
| **声道修复（Channel repair）** | 修正错误的声道布局，以及长时间的 L/R 能量失衡。 |
| **L-only / R-only** | 一个声道低于活动阈值、另一个有内容 —— 视为"单声道停在一侧"。 |
| **Leveler** | 慢速自动增益，把有内容的部分拉向目标响度区间（−18 dB）。每个采样后输出一条 scene level，下游动态（压缩器、扩展器）都按这个采样级联。 |
| **峰值控制（Peak control）** | 更快的下行动态，压住峰值和撞击声。 |
| **向上扩展器（Upward expander）** | 低电平向下扩展：把安静段的底噪"再往下推"，避免整条链把安静对白压平。阈值耦 scene level，不是输入包络。 |
| **音色塑形（Tone shaper）** | 倾斜搁架 + 3 kHz 临场峰；单个 Tone 旋钮在 −1（更暖/更暗）与 +1（更亮/更前）之间旋转；0 处严格直通。 |
| **真峰值天花板（True-peak ceiling）** | 允许的真峰值上限（−1 dBTP）。 |
| **降噪模式（Denoise mode）** | 可选的降噪引擎：Off / Classic（自研频谱 STFT）/ Live（DeepFilterNet3，实时）/ HQ（MossFormer2，仅离线）。Amount 旋钮在所选引擎内部统一映射为"降噪强度"。 |
| **对白可懂度** | 音乐床下的同期人声是否听得懂。这是本项目的成功判据 —— **不是**广播 LUFS 合规。 |

---

## 下载与安装

到 [Releases](../../releases) 页面下载对应平台的压缩包。

### macOS（Universal 2 —— Apple Silicon + Intel 通用）

1. 解压 `SyncTrack-Prep-<版本>-macOS-Universal.zip`。
2. 把 `SyncTrack Prep.vst3` 放到以下任一目录：
   * `~/Library/Audio/Plug-Ins/VST3/` —— 仅当前用户，不需要管理员密码，**推荐**
   * `/Library/Audio/Plug-Ins/VST3/` —— 所有用户，需要管理员密码
3. **清掉隔离属性（quarantine）。** 这个构建没有经过 Apple 公证（notarization），所以直接从下载的 zip 里拿出来 macOS 会拒绝加载。在终端执行：

   ```bash
   xattr -dr com.apple.quarantine ~/Library/Audio/Plug-Ins/VST3/"SyncTrack Prep.vst3"
   ```

   如果 DAW 还是不加载，打开**系统设置 → 隐私与安全性**，滑到底部，点被拦截项旁边的**仍要打开**。
4. 完全退出并重新启动 DAW，然后重新扫描 VST3 插件。

### Windows（x64）

1. 解压 `SyncTrack-Prep-<版本>-Windows-x64.zip`。
2. 把 `SyncTrack Prep.vst3` 放到 `C:\Program Files\Common Files\VST3\`（需要管理员权限）。
3. 重启 DAW，重新扫描 VST3 插件。

> Windows 二进制已由外部灰测用户在 Windows 上的 Nuendo 里实测过；残余局限见[已知局限](#已知局限)。

---

## 宿主支持情况

| DAW | 状态 |
|---|---|
| **Cubase** | ✅ 已验证 —— 实时与离线导出均稳定运行（在 macOS 上实测） |
| **Nuendo** | ✅ 已验证 —— 实时与离线导出均稳定运行（在 macOS 上实测） |
| Reaper | ⚠️ 未验证 |
| FL Studio | ⚠️ 未验证 |
| Studio One | ⚠️ 未验证 |
| Pro Tools | ❌ 不支持 —— 需要 AAX，尚未构建 |
| Logic Pro | ❌ 不支持 —— 需要 AU，尚未构建 |

目前只构建 **VST3** 一种格式。AAX 与 AU 在路线图上。

### 如果导出的文件听起来像没处理

如果离线指标正常但导出文件是干声，那基本可以断定插件没有进入渲染路径。按顺序检查：

1. 插入是**启用**状态（电源按钮亮着），而且在你**正在导出的那条轨**上。
2. 用 **Soft** 导一版、用 **Strong** 导一版，两个文件**不应该**逐比特相同。
3. 播放时插件的 IN/OUT 表头在动。
4. 插件挂在**立体声**轨上，不是 multi-mono 槽位。
5. 重新编译插件之后：完全退出 DAW、重新扫描 VST3、重新插入插件。

---

## 怎么用

六个控件，刻意为之 —— 调校都放在预设里。

| 控件 | 范围 | 说明 |
|---|---|---|
| **Preset** | Soft / Strong / Clean | 默认 **Strong**。切换预设时也带上该预设的 Denoise Mode 默认值（Soft=Off，Strong=Live，Clean=Classic）；在你下次切换预设之前，你自己改过的 mode 选择会被保留。 |
| **Denoise Mode** | Off / Classic / Live (DFN3) / HQ (MossFormer2) | Classic = 自研频谱；Live = 实时 NN；HQ = 离线 NN（实时回放时静默降级为 Live 并显示提示）。 |
| **Amount** | 0–100 % | 统一"降噪强度"：Classic 内部映射过减因子，Live 映射衰减上限，HQ 映射湿/干混合比。 |
| **Tone** | −1 … +1 | 倾斜 + 3 kHz 临场；0 处比特直通。 |
| **Output** | −inf … +24 dB | 安全级之前的补偿增益。skewed 范围，0 dB 居中；低于 −60 dB 显示 "-inf"。−1 dBTP 天花板始终成立。 |
| **Bypass** | 开 / 关 | 真旁通 —— 输入原样通过。 |

**该选哪个预设？**

* **Soft** —— 轻手。素材本身比较干净，只想修声道布局 + 温和整平时用。
* **Strong** *（默认）* —— 主力档。更重的整平（最多 +18 dB 增益、3:1 峰值控制）+ 实时 NN 降噪。电平起伏大、外景底噪重的镜头首选。
* **Clean** —— Leveler 退让，让降噪器干活（默认 Classic 模式）。稳态底噪是主要矛盾时用。

三个预设共享同一组目标：Leveler 目标 −18 dB、降噪器内置 70 Hz 高通、真峰值天花板 −1 dBTP、峰值压缩器常开。

只支持立体声进 / 立体声出。

> **存档兼容说明：** v0.1.x 没有 `denoiseMode` 的旧工程加载时，若旧 Denoise 开关为 on 则映射到 **Live**，off 映射到 **Off** —— 这是有意的升级映射，已记入 changelog。Output 范围也变了，旧值加载会发生偏移，见[已知局限](#已知局限)。

---

## 已知局限

这些提前讲清楚，能省掉一次 issue。

* **HQ（MossFormer2）需要离线渲染。** 只有当宿主标明非实时（Nuendo 的 Direct Offline Processing、导出/bounce 路径）才会真正运行。实时回放时该 mode 会静默降级为 **Live (DFN3)** 并显示提示。HQ 比 Live 多 ~4 秒的宿主延迟补偿，宿主能吃下。
* **Classic（自研频谱）在真实素材上的稳态降噪量可能不大。** 合成稳态噪声上明显，真实房间底噪上效果偏温和。底噪是主要问题时请选 **Live（DFN3）** 或 **HQ（MossFormer2）**。
* **Classic 噪声估计器需要约一秒才稳定。** 它是在流中学习噪声频谱的，所以如果一段素材开头就是人声，最初几个 STFT 帧可能会把对白当成噪声学进去。大约一秒内会自行纠正。
* **v0.1.x 的旧工程加载会发生漂移。** 新 **Output** 范围是 −inf…+24 dB（skewed，0 dB 居中）—— 旧范围 −24…+12 把 0 dB 存在归一化值 0.667，新映射是 0.5，所以旧的 Output 值读数会偏。旧工程里用 Denoise 复选框的，加载时会变成 **Live (DFN3)**。两者都是有意的升级映射，不是 bug。
* **仅立体声。** 不支持环绕或多声道。
* **它不是交付工具。** SyncTrack Prep 不做广播响度交付（EBU R128 母版）、不做 AI 人声/音乐分离、不做离线频谱修复与去削波。它是同期轨的第一轮清理插件。

---

## 从源码构建

要求：支持 C++17 的编译器、**CMake ≥ 3.21**，首次 configure 需要联网（会自动拉取 JUCE 8.0.8 与 Catch2 3.4.0）。

```bash
git clone <本仓库>
cd "SyncTrack Prep"
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 8
ctest --test-dir build --output-on-failure
```

在 macOS 上会产出 Universal 2（arm64 + x86_64）二进制。`COPY_PLUGIN_AFTER_BUILD` 是打开的，所以构建过程会顺手把 VST3 拷进你的用户插件目录。

本地已经有 JUCE 或 Catch2？让 CMake 指过去就能跳过下载：

```bash
cmake -B build -DSTP_JUCE_PATH=/path/to/JUCE -DSTP_CATCH2_PATH=/path/to/Catch2
```

构建目标：

| 目标 | 是什么 |
|---|---|
| `SyncTrackPrep` | VST3 插件（外加一个方便试听的 Standalone 应用） |
| `SyncTrackPrepOffline` | 命令行渲染器 —— 不用 DAW 就能处理 WAV |
| `SyncTrackPrepTests` | Catch2 DSP 单元测试 |
| `SyncTrackPrepStateTests` | Catch2 processor / APVTS 状态测试 |

加 `-DSTP_BUILD_TESTS=OFF` 可以跳过测试目标。

---

## 离线渲染与 A/B 分析

离线渲染器跑的是和插件完全相同的 DSP 链，所以你可以在完全不开 DAW 的情况下验证处理效果、对比预设。

```bash
cmake --build build -j 8 --target SyncTrackPrepOffline
./build/SyncTrackPrepOffline input.wav output-strong.wav strong 1
```

参数：`<输入.wav> <输出.wav> [soft|strong|clean] [off|classic|live|hq] [amount 0-100|-1] [tone -1..1]`，另有 `--tap denoise|final`、`--expander 0|1`、`--flush <秒>`、`--dfn3 <模型.tar.gz>`、`--moss <模型.onnx>`。

`scripts/` 下有三个分析工具（Python 3；`analyze_ab.py` 和 `diagnose_stereo_noise.py` 需要 `ffmpeg` 在 `PATH` 里）：

```bash
# 完整 A/B 报告：响度差、稳态噪声电平、真峰值
python3 scripts/analyze_ab.py --gold input.wav --proc output-strong.wav --run-id my-run

# Leveler 的噪声门在这里为什么开/关？
python3 scripts/diagnose_gate.py input.wav --noise-window 30:40 --dialogue-window 0:14

# 噪声真的是立体声，还是某一个声道在漏？
python3 scripts/diagnose_stereo_noise.py --gold input.wav
```

`analyze_ab.py` 会输出逐秒 CSV、带 pass/fail 标记的单行汇总 CSV，以及一份可读的 Markdown 报告。

---

## 路线图

* MossFormer2 流式化（因果化 + INT8 再试）—— 让 HQ 变成可实时档
* AAX（Pro Tools）与 AU（Logic Pro）构建
* 在 Reaper、FL Studio、Studio One 上做验证
* 更多 Linux 覆盖（LV2 / CLAP）

---

## 许可证

SyncTrack Prep 以 **GNU Affero 通用公共许可证 v3.0 或更高版本**（AGPL-3.0-or-later）发布。全文见 [LICENSE](LICENSE)。

为什么是 AGPL：本插件链接了 [JUCE](https://juce.com) 框架，而 JUCE 的模块采用 AGPLv3 与商业 JUCE 许可的双授权。以 AGPLv3 发布，是把它作为自由软件分发时唯一许可兼容的路径。如果你 fork 并分发二进制，你必须以相同条款公开你的源码。

随插件一起打包的第三方组件（完整表格见 `NOTICE`）：

* **JUCE 8.0.8** —— AGPLv3 或商业 JUCE 许可。在 configure 阶段拉取，不随本仓库分发。
* **Steinberg VST3 SDK** —— 随 JUCE 一起提供，专有许可与 GPLv3 双授权。
* **Catch2 3.4.0** —— Boost Software License 1.0。仅测试依赖，在 configure 阶段拉取。
* **DeepFilterNet3**（`libdf`）+ **DeepFilterNet3 模型** —— MIT / Apache-2.0。Live 档引擎，打包进 VST3。
* **ONNX Runtime** —— MIT。MossFormer2 推理运行时，打包进 VST3 的 `Contents/Frameworks/libonnxruntime.dylib`。
* **MossFormer2_SE_48K** 权重 —— Apache-2.0。HQ 档模型，打包进 VST3 的 `Contents/Resources/mossformer2/`。

VST 是 Steinberg Media Technologies GmbH 在欧洲及其他国家的注册商标。
