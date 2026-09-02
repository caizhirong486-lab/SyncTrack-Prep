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
In → ChannelRepair → Leveler → HPF → NoiseSuppressor → OutputGain → PeakCompressor → TruePeakLimiter → Out
```

* **固定延迟 575 samples**（降噪 STFT 511 + 限制器前视 64）。在 `prepareToPlay` 时上报一次；**切换 Denoise 不会改变延迟**，所以宿主的延迟补偿始终有效。
* Leveler 故意放在降噪**之前**：这样频谱级可以把 Leveler 刚抬起来的底噪再刮掉一层（**Clean** 预设就是靠这个逻辑）。
* **Output 是整条链的补偿增益（makeup gain）**，位置在常开的安全级**之前**。因此 −1 dBTP 天花板在**任何**旋钮位置都成立：Output 往上推会把安静段抬到贴近天花板，任何试图越过天花板的部分都会被压缩器和限制器压回来。旋钮在 0 dB 时，整条链的行为跟没有这个旋钮一样。

### 术语

| 术语 | 含义 |
|---|---|
| **同期声（Production track）** | 导入的视频同期音频：对白与现场音效混在一起。 |
| **声道修复（Channel repair）** | 修正错误的声道布局，以及长时间的 L/R 能量失衡。 |
| **L-only / R-only** | 一个声道低于活动阈值、另一个有内容 —— 视为"单声道停在一侧"。 |
| **Leveler** | 慢速自动增益，把有内容的部分拉向目标响度区间（−18 dB）。 |
| **峰值控制（Peak control）** | 更快的下行动态，压住峰值和撞击声。 |
| **真峰值天花板（True-peak ceiling）** | 允许的真峰值上限（−1 dBTP）。 |
| **噪声轮廓（Noise profile）** | 从低能量间隙学到的稳态噪声频谱估计。 |
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

> Windows 二进制由 CI 构建，但**还没有在任何 Windows DAW 里实测过** —— 见[已知局限](#已知局限)。

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

只有四个控件，这是刻意的 —— 调校都放在预设里。

| 控件 | 范围 | 说明 |
|---|---|---|
| **Preset** | Soft / Strong / Clean | 默认 **Strong**。切换预设时也会带上该预设的 Denoise 默认值（Clean 开，Soft/Strong 关）；在你下次切换预设之前，你自己改过的 Denoise 选择会被保留。 |
| **Denoise** | 开 / 关 | 频谱降噪。开关都不改变延迟。 |
| **Output** | −24 … +12 dB | 安全级之前的补偿增益。−1 dBTP 天花板始终成立。 |
| **Bypass** | 开 / 关 | 真旁通 —— 输入原样通过。 |

**该选哪个预设？**

* **Soft** —— 轻手。素材本身比较干净，只想修声道布局 + 温和整平时用。
* **Strong** *（默认）* —— 主力档。更重的整平（最多 +18 dB 增益、3:1 峰值控制），适合电平起伏大的镜头。
* **Clean** —— Leveler 退让，让降噪器干活（Denoise 默认打开）。稳态底噪是主要矛盾时用。

三个预设共享同一组目标：Leveler 目标 −18 dB、70 Hz 高通、真峰值天花板 −1 dBTP、峰值压缩器常开。

只支持立体声进 / 立体声出。

---

## 已知局限

这些提前讲清楚，能省掉一次 issue。

* **在真实素材上，稳态降噪量可能并不明显。** 因为 Leveler 跑在降噪**之前**，安静段的底噪会先被抬起来（在我们的参考素材上大约抬了 9 dB），降噪器再去往回刮。在我们的测试源上，**Clean** 输出的稳态噪声电平最终比源**还高**，而不是更低。这是链路顺序的权衡，不是频谱级本身的 bug：同一个降噪器在单元测试里能把合成的稳态底噪压下 4 dB 以上。下决定之前，请用下面的离线渲染工具在你自己的素材上 A/B 听一遍。
* **噪声估计器需要约一秒才稳定。** 它是在流中学习噪声频谱的，所以如果一段素材开头就是人声，最初约 16 个 STFT 帧可能会把对白当成噪声学进去。大约一秒内会自行纠正。
* **Windows 二进制未经验证。** CI 会编译并跑单元测试，但还没有人把它装进 Windows 的 DAW 里试过。
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

参数：`<输入.wav> <输出.wav> [soft|strong|clean] [denoise 0|1]`

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

* 在 Windows 上的 Cubase / Nuendo 里实测 Windows 构建
* AAX（Pro Tools）与 AU（Logic Pro）构建
* 重新审视 Leveler-在-降噪-之前 的顺序，让稳态降噪在真实素材上可测量
* 在 Reaper、FL Studio、Studio One 上做验证

---

## 许可证

SyncTrack Prep 以 **GNU Affero 通用公共许可证 v3.0 或更高版本**（AGPL-3.0-or-later）发布。全文见 [LICENSE](LICENSE)。

为什么是 AGPL：本插件链接了 [JUCE](https://juce.com) 框架，而 JUCE 的模块采用 AGPLv3 与商业 JUCE 许可的双授权。以 AGPLv3 发布，是把它作为自由软件分发时唯一许可兼容的路径。如果你 fork 并分发二进制，你必须以相同条款公开你的源码。

第三方组件：

* **JUCE 8.0.8** —— AGPLv3 或商业 JUCE 许可。在 configure 阶段拉取，不随本仓库分发。
* **Steinberg VST3 SDK** —— 随 JUCE 一起提供，专有许可与 GPLv3 双授权。
* **Catch2 3.4.0** —— Boost Software License 1.0。仅测试依赖，在 configure 阶段拉取。

VST 是 Steinberg Media Technologies GmbH 在欧洲及其他国家的注册商标。
