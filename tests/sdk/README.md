# SDK 控制事件交错回归

直接调用真实 SDK 的协议分派代码，不连接网络、不使用设备凭据。覆盖板上已观察到的
`START(new) → ChatBreak(old) → 无ID的ASR/TTS`，以及晚到旧END、匹配新END、无ID控制事件。
旧控制事件必须向回调报告旧ID，同时保留新会话的默认ID；匹配END才清空该默认ID。
旧END也不能关闭新上行事件。

在已准备依赖的隔离SDK副本上应用补丁，再运行：

```sh
patch -p1 -d /path/to/sdk-copy < patches/agentic-kit-late-control-event-id.patch
cmake -S tests/sdk -B build/sdk-event-host -DAK=/path/to/sdk-copy -DCMAKE_BUILD_TYPE=Debug
cmake --build build/sdk-event-host --target test_event_inheritance -j
ctest --test-dir build/sdk-event-host --output-on-failure
```

已用实际构建SDK提交`49ab2af33d67544c3d4531567740c574f62c1173`验证：修复前在新TEXT继承ID
断言处失败，修复后通过；SDK原有`tai_unit_tests`、`tai_integration_tests`均通过。
相同回归链接V821目标静态库后，已在板上运行通过。此结果不代替真人声学和多轮云对话验收。
