# “你好涂鸦”本地唤醒

`core/wake_word.c` 接收16kHz单声道PCM，专用线程运行PocketSphinx
关键词检测。待唤醒的录音不需要上传云端。队列、待处理命中和工作线程均有明确生命周期；
关闭/重新启用检测会清队列、清命中并使正在计算的旧结果失效。队列溢出或推理失败
显式返回错误，不退化为“声音大就唤醒”。

完整应用仅在SLEEP且喇叭不提交内容时，向关键词模块送麦克风原始PCM。板上同一18%
幅度正例在原始输入命中、SDK AEC/降噪后漏检，尽管两者峰值RMS接近。关键词与
云端上送采用独立输入：预录、ASR上传和播放中插话仍使用AEC输出；待唤醒不上传音频。


**这是一条在V821上完成基本对话初测、仍需继续声学验收的实验方案。**
PocketSphinx采用可从C源码编译的5.0.4 API。当前紧凑模型质量集为10/11，仍有音色正例
漏检，不能据此宣称远场识别率或长期误唤醒率合格。

## 连续输入与活动资格

关键词命中使用结构化segment，避免多个命中被显示摘要拼接后漏判。完整应用还检查
原始RMS活动资格：默认350、连续60ms、有效期1秒；满足活动资格仍须真实KWS命中。
`V821_WAKE_MIN_RMS=0`仅用于关闭资格检查的对照，不能把能量本身当成关键词。
独立工具只检查模型/工作线程，完整应用还会执行该资格检查。

持续背景输入可能改变CMN特征均值。代码在连续低能量输入累计一秒及解码器复位时
恢复初始均值，保留完整音频序列。应使用长时间背景输入加正例的组合回放验证，
不能用短样本成功代替实际待机验收。整体流程和已验证边界见
[全双工开发说明](../../docs/voice-aec-wakeword.md)。

## 模型为什么要裁剪

官方 Mandarin 5.2 声学模型约 67MiB，原始权重、词典和上下文表不适合本板的
内存预算。仅删除上下文相关状态、保留全部 192 个带调音素，也仍在 Mac arm64
测到约 54MB 峰值 RSS：PocketSphinx 的上下文索引表按音素数的三次方分配。

`prepare_model.py` 的默认模式保留所有基础音素类别，为每类挑一个声调代表，并
完整保留当前关键词实际用到的声调，删除其余上下文/声调状态。默认短语得到 66
音素、198 状态，全部资源约 1.87MB。主机独立进程初测峰值 RSS 约 6.8MB。
**文件大小不是实际内存证明，Mac 的 RSS/速度也不能代替 V821 测量。**

这会改变原始声学模型的区分能力，并非重新训练或量化。`--full-ci` 可生成全部
192 音素的高内存基准，但不要把它误当成板上方案。原始模型不应直接部署到 V821。

`--keyword-triphones` 可额外保留原模型中与当前关键词音素链完全匹配的上下文音素
（两端 SIL、保留各词位置版本），并同步重排 CI/CD 状态及权重。默认短语保留 14
个 triphone，资源约 2.21MB，主机峰值 RSS 约 7.1MB。该候选在同一组音频、相同
阈值下没有改善已知漏检，因此不作为默认方案；保留开关用于复核此次模型试验。

## 准备资源

使用官方源，所有下载、模型和产物留在被 Git 忽略的 `build/wake-deps/`：

```sh
mkdir -p build/wake-deps
curl -fL 'https://downloads.sourceforge.net/project/cmusphinx/Acoustic%20and%20Language%20Models/Mandarin/cmusphinx-zh-cn-5.2.tar.gz' \
  -o build/wake-deps/cmusphinx-zh-cn-5.2.tar.gz
tar -xzf build/wake-deps/cmusphinx-zh-cn-5.2.tar.gz -C build/wake-deps
python3 tools/wake/prepare_model.py \
  --source build/wake-deps/cmusphinx-zh-cn-5.2 \
  --output build/wake-deps/mandarin-kws-tuya --phrase 你好涂鸦
```

输出目录必须是新目录，禁止覆盖已有资源。脚本校验固定版本原始参数的 SHA256，
保存派生说明、发音、参数来源及原模型许可证。原始词典来自 CC-CEDICT，来源与
归属也记录在 `manifest.json`；资源分发时保留这些材料。

“你好涂鸦”的模型发音为 `n i3 h ao3 t u2 ii ia1`。脚本从原词典分词得到发音，
不靠手写拼音猜测，也不需要把完整的 15 万词字典加载到板上。

**更换自定义短语时，重新生成匹配的模型目录和 `wake.dict`，再同时更新运行配置
中的短语、模型和词典路径。只改 UI 字符串或 CLI 短语不会改变模型。** 支持原词典
可分词的UTF-8短语，单个逻辑关键词不允许空白字符。独立准备工具/识别工作线程最多
接受127个UTF-8字节，完整应用配置进一步限制为96个字节；部署到应用时以96字节为准。
生僻字或词典外发音会明确失败。

## 编译与离线检查

使用 [PocketSphinx 5.0.4 源码](https://github.com/cmusphinx/pocketsphinx/releases/tag/v5.0.4)，
主机和交叉编译各自使用对应架构的安装目录。主机安装自带的英文模型不需要部署。

```sh
cmake -S build/wake-deps/pocketsphinx-5.0.4 -B build/wake-deps/pocketsphinx-build \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF \
  -DCMAKE_INSTALL_PREFIX="$PWD/build/wake-deps/pocketsphinx-install"
cmake --build build/wake-deps/pocketsphinx-build -j
cmake --install build/wake-deps/pocketsphinx-build
python3 tests/wake/test_prepare.py
```

Apple Silicon 如使用 Rosetta 的 CMake，补充 `-DCMAKE_OSX_ARCHITECTURES=arm64`。
V821 使用本仓库工具链重新构建，不能复制 Mac/ARM/aarch64 库到设备。
PocketSphinx 头文件作为 SYSTEM include；5.0.4 上游的重复 typedef 声明会触发
AppleClang 严格 C99 的第三方头文件告警。

`wake_probe` 以实时节奏把裸 PCM 送进相同的工作线程。退出 0 表示检测到关键词，
2 表示没有检测到，1 表示配置、I/O、队列或推理错误。输入必须为 16kHz、单声道、
S16_LE，不接受 WAV/MP3 容器。建议前后保留静音，且生成测试 TTS 后先验证文件
确实包含声音：本机受限环境中的 `say` 曾成功退出但生成空 AIFF，不能把它算成语音。

`check_fixtures.py` 只重放已存在的 PCM，不在测试时生成语音或下载模型。案例 manifest
写明资源路径、短语及预期命中次数（连续两次唤醒必须命中两次）；所有相对路径以
manifest 所在目录为基准。它输出 JSON 并在任意漏检、误检、I/O/推理错误时返回
非零，错误退出不能冒充“负例没有命中”。可选 `threshold` 字段覆盖单个案例阈值。

```sh
python3 tools/wake/check_fixtures.py --probe build/wake-deps/wake_probe \
  --manifest tests/wake/fixtures.example.json --output build/wake-fixtures-report.json
python3 tests/wake/test_fixture_runner.py
```

示例 manifest 仅为一正一负的最小重放例子，不代表完整验收集。加入 Eddy、Meijia
等已知漏检正例时，默认模型的完整质量门应当失败，不能把它们改成负例或排除后
声称“全部唤醒通过”。CTest 只应在显式配置已有音频 manifest 时注册这项检查。

## 已知主机结果与验收边界

代码默认阈值仍为`1e-25`；当前测试板“你好涂鸦”在环境文件中显式使用候选`1e-35`。
板内30秒真人样本（用户说3次）在1e-25仅命中1次，1e-35命中3次，1e-40出现6次，
因此没有直接采用更宽松阈值。候选在板上5个合成负例均0误触发；主机完整质量门
10/11，Meijia正例恢复命中，Eddy仍漏检，质量门仍按失败记录。真人样本未导出。
这些结果不足以证明其他人的声音、噪声或长期误唤醒率合格，完整真人组合测试另行记录。

以下1e-25结果保留为早期基线：

当前默认阈值 `1e-25` 只是实验起点。初测 Tingting 合成“你好涂鸦”可命中；
“你好涂鸦，帮我定一个五分钟的闹钟”也可命中，连续两次“你好涂鸦”检测到两次。
重新生成资源后，自定义“你好小涂”可命中。
“今天天气真不错”“你好小智”“你好图画”“涂鸦你好”“你好吗”没有命中。
Meijia 台湾语音、Eddy 大陆语音的同一短语在该阈值下漏检，属于已知局限，
不为单个正例随意放宽阈值。主机 CPU 时间/音频时长初测约 0.04–0.09，
仅是该 Mac 的离线/定速重放结果。

目标机初测：2026-10-26已通过Mac ADB部署并实际加载该模型。旧版语音程序和官方
xstar同时运行时，不带额外监测负载的Tingting正例重放在第77帧发生输入队列溢出，
返回1，属于推理错误。用户授权停止旧负载后，Tingting正例命中，Eddy/Meijia仍漏检，
普通负例未命中；这四条定速回放未溢出。CPU为performance、960MHz。

完整产品随后已启动“你好涂鸦”模型+AEC+云连接+持续采集，初测进程RSS约7.7MB，
待机期间暂未发生关键词队列溢出；真人唤醒与远场/播放中效果仍待验收。
不能把旧负载并行时的失败直接归因模型本身，也不能以短时间待机替代长期实时预算验收。

已对同一 6 个正例案例（含连续唤醒、自定义短语）和 5 个负例系统扫描阈值：

| 阈值 | 正例案例命中 | 负例误触发 | 说明 |
|---|---:|---:|---|
| `1e-10 / 1e-20 / 1e-25 / 1e-30` | 4/6 | 0/5 | Eddy、Meijia 漏检；连续案例命中两次 |
| `1e-35` | 5/6 | 0/5 | 当前测试板候选；真人板内及实时工作线程回放3/3 |
| `1e-40` | 5/6 | 0/5 | Meijia 恢复，Eddy 仍漏检 |
| `1e-50` | 5/6 | 1/5 | “你好图画”误唤醒，Eddy 仍漏检 |

没有一个扫描阈值同时覆盖三种声音。代码默认仍为 `1e-25`；测试板经过真人样本和
负例检查后在环境文件显式使用 `1e-35`，并保留说话人覆盖不足的质量失败记录。精确关键词 triphone 候选在默认
阈值下也为 4/6 正例、0/5 负例误触发，没有形成整体改善。

这些是很小的合成音频样本，不能换算成真人远场识别率或每小时误唤醒数。正式验收
至少覆盖多说话人、多音量、不同距离/噪声、设备播放中语音插话、连续唤醒和唤醒词
后紧跟命令；同时检查 AEC 后的人声和 CPU/内存峰值。短语为同音词时不能期待
纯声学 KWS 区分汉字语义。

参考：[PocketSphinx API](https://cmusphinx.github.io/doc/pocketsphinx/)、
[官方 Mandarin 模型](https://sourceforge.net/projects/cmusphinx/files/Acoustic%20and%20Language%20Models/Mandarin/)。
