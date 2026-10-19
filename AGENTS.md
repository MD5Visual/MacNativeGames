# 项目范围与文件存放

用户明确要求：不要把不属于 MacNativeGames 项目的东西放进本目录。

- 这个目录是源码项目，只保存本项目需要的源码、必要的预编译工具、正式依赖、测试代码和文档。
- 生成的游戏 App、解包数据、着色器缓存和编译产物也必须放在项目目录外；不能因为它们由本项目生成就留在源码目录内。不得在项目内留下指回这些大目录的链接。
- 临时验证工作区、重复完整构建、解包副本、测试存档副本和其他项目的分析数据放在项目目录外，不能塞进 `games/totk/analysis`。
- 完整打包的中间文件使用项目目录外的 `TOTK_build` 或通过 `--work` 指定外部目录；测试运行数据使用用户数据目录下独立的 `tests` 子目录。
- 对比其他项目时优先直接读取其原目录，不将整个项目或其构建产物复制进来；正式引入的依赖须保留必要的来源和许可说明。
- 本项目文档记录重要结论、测试方法和外部证据位置；仅为留证据，不复制大体积资源或个人数据进项目。
- `.gitignore` 只控制提交，不代表可以在项目内堆积临时文件。既有开发产物和用户数据不得因此擅自删除。

本机现有产物统一放在 `/Volumes/mac-APFS/MacNativeGames-artifacts`：`apps` 存放已生成 App，`development` 存放已有开发缓存，按日期的目录存放历史测试归档。桌面保留原来的唯一游戏入口，直接指向外部 App。

## Pro Controller gyro — measured findings (2026-10, macOS 26.6.2)

Implemented in `games/totk/runtime/procon_motion.c` (wired in `gamepad_feedback.m`, fusion in `motion_fusion.c`). Playtested working; not yet upstream.

- macOS GCMotion advertises Pro Controller motion sensors but streams dead zeros (works for DualSense/DS4/MFi). The pad's real IMU is reachable from a second, non-seizing IOKit HID client (`kIOHIDOptionsTypeNone`) — the system gamepad driver keeps buttons/sticks.
- Protocol: VID 0x057E, PID 0x2009 BT / 0x200E USB. Send EnableIMU (sub 0x40 arg 1) + SetInputReportMode (sub 0x03 arg 0x30) via output report 0x01 (48-byte payload: [0]=packet counter, [1-8]=neutral rumble, [9]=sub, [10]=arg). Input report 0x30 then carries 3 IMU samples at [12..47] (int16 LE accel,gyro per sample). Apple delivers the report ID as byte 0 of the callback buffer.
- macOS delivers only ~24 reports/s to the second client (≈72 IMU samples/s, not 200 Hz). The report timer byte advances ~8.3 units/report with unit ≈5 ms — sample spacing must be measured from wall clock, not assumed 5 ms (assuming 5 ms under-integrates rotation ~3×).
- SPI subcommand replies arrive as report 0x21 with ack 0x80 but echo the report mode and carry no SPI data → per-unit factory/user calibration (0x6020/0x8026) is NOT readable through this path. Use nominal scales (1/4096 G/LSB; π/180/14.2842 rad/s/LSB) + auto-zero gyro bias (2 s time constant while still).
- Measured raw axes on this pad (contradicts SDL/datasheet labels X=back,Y=left,Z=top): raw X = top edge (charging-port side; upright → +4096), raw Y = left-right, raw Z = face normal (flat face-up → +4096). Correct feed into the GCMotion frame (x=right, y=top, z=out of face): gyro = (-rawY, +rawX, +rawZ); acc must be the GRAVITY vector like GCMotion reports (face-up → z=-1 G), i.e. acc = (+rawY, -rawX, -rawZ).
- Fusion must mirror real hid: direction matrix = identity at StartSixAxisSensor (the game re-centers by restarting the sensor; it does this once at title). The previous gravity-locked fusion (align_to + kp correction) added a world rotation into every relative pose → diagonal aim drift and remap-invariance (any TOTK_GYRO_AXES change cancelled out). `motion_fusion.c` now integrates pure gyro from the last re-center. This shared path also affects DualSense/GCMotion — flag in PR.
- Knobs: `TOTK_GYRO_DEADZONE` (rad/s, default 0/off), `TOTK_GYRO_GAIN=x,y,z` (default 1.2,1.0,0.75 — playtested sweet spot on Pro Controller). `open --env` on an already-running app is silently ignored; quit first.
- Zero-drift over long sessions: the game calls StartSixAxisSensor only once per launch (title screen), so there is no in-game recenter hook to ride. Auto-zero only adapts while the pad is still, so bias error accumulates during continuous aiming. Mitigation: hold L3+R3 for 0.5 s → `runtime_motion_start()` (same identity recenter the game does at title); detected from the 0x30 report's own button bytes ([4] bits 0/1).
- Upstream PR #25 (LudovicLemarinel, 2026-10-19, unmerged) solves the same problem for an 8BitDo in Switch mode (`057e:2009`): same protocol, same measured raw axes (independent confirmation), falls back after 1 s of all-zero GCMotion instead of matching the name, and its 8BitDO DOES answer SPI calibration reads (0x6020/0x8026) — real Nintendo Pro Controller firmware echoes the report mode instead, so per-unit calibration stays unavailable for us. It has no bias estimation and no recentering; our auto-zero + L3+R3 + gain are the delta. Coordinate/merge with that PR before filing ours.
- Idle: after ~1–2 min untouched the pad's 0x30 stream stops (power save, no device removal); resumes instantly on use.
- Probe tool: `games/totk/scripts/procon_probe.c` (builds to `build/procon-probe`; run it directly — it streams forever; `PROCON_DEBUG_ALL=1` dumps all reports). Pose visualizer + guided rotation tests (source of the axis/scale measurements above) live outside the repo per scope rules: `/private/var/folders/pf/1xggvqj97txb78t192_cjjb00000gn/T/opencode/procon-view/` (`procon_view.c`, `view.html`, test results in `server.log`).
- SDK quirk: current `IOHIDKeys.h` defines `kIOHIDVendorIDKey`/`kIOHIDProductIDKey` as plain C strings — pass `CFSTR("VendorID")`/`CFSTR("ProductID")` to CF APIs, not the constants.
- Local-only, keep OUT of any PR: `games/totk/assets/expected-v1.0.0.json` hash patch (digital NSP vs card-image reference; documented in its `_comment`).

