# agentic-kit 临时集成补丁

`agentic-kit-late-control-event-id.patch`基于实际构建机SDK提交
`49ab2af33d67544c3d4531567740c574f62c1173`，用于V821全双工开发版。
在独立SDK副本上应用，并将主程序CMake的`AK`设为该副本目录，便于保留上游原始版本。

修复范围是晚到的旧ChatBreak/END控制事件：回调仍得到该控制事件的ID，
后续缺省ID的媒体包继续继承当前新会话ID；仅匹配的END关闭对应的继承状态与上行事件。
本补丁没有取消应用的旧轮次过滤，也不保证任意交错媒体都能严格关联。

复现与验证见[SDK回归](../tests/sdk/README.md)。分发应用时记录所用SDK版本与补丁，
不随源码分发设备凭据或厂商二进制库。
