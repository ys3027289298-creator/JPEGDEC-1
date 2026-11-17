# JPEGDEC MCU 错误恢复 —— 实现与验证说明

## 约束遵守情况

- 库改动只落在 **`src/jpeg.inl` 一个文件**；未改构建脚本、未改任何公共接口签名。
- 无新增第三方库、无网络访问。样例由纯标准库 Python 脚本生成，测试只用系统 gcc/g++。
- 新增的 `JPEG_RECOVERY_*` 是**附加**的独立 C 链接函数，不改动既有类/函数签名。
- 测试、样例、临时构建全部位于仓库外的 `build/`（以及仓库自带的未跟踪 `artifacts/`），
  `git status` 中被跟踪文件的修改只有 `src/jpeg.inl`。

## 实现概要（全部在 src/jpeg.inl）

1. 非法 Huffman 码在 `JPEGDecodeMCU()` 三处返回 -1。主 MCU 循环改为捕获该返回：
   - 恢复关闭、或 progressive 扫描：保持原有 fail-stop（返回 JPEG_DECODE_ERROR）。
   - 恢复开启（baseline）：进入重同步，整帧继续。
2. 有重启标记（DRI）：在**原始未过滤文件数据**里扫描下一个 `FFD0–FFD7`
   （跳过 `FF00` 填充，遇 EOI/SOS 停止），找到后重建 VLC 缓冲、复位 DC 预测器，
   丢失区间内其余 MCU 用邻近块填充。重启计数按剩余区间长度记账，避免后续区间错位。
3. 无重启标记：位流跳到下一个字节边界，只丢弃当前 MCU 并填充，后续继续解。
4. 填充策略（可配）：`JPEG_FILL_LEFT`（默认，复制左邻）、`JPEG_FILL_ABOVE`
   （复制上邻，仅帧缓冲模式可用，否则回退）、`JPEG_FILL_NEUTRAL`（中性灰）。
   首行/首列无可用邻居时回退中性灰。
5. 可配参数：使能开关、重同步搜索范围（1..1,000,000，默认 4096）、填充策略。
   非法参数返回 `JPEG_INVALID_PARAMETER` 且不改当前配置。
6. 计数/统计：Huffman 错误数、坏 MCU 数、RST 重同步数、单 MCU 跳过数、
   可见像素总数/受损数、恢复占比。
7. 顺带修复两个既有缺陷（均在同一文件内）：
   - 未知 subsampling 时 `mcuCX*mcuCY==0` 的除零 FPE（原 fuzz 测试会崩溃）。
   - `JPEGGetMoreData()` 同缓冲重叠 `memcpy` → 改为 `memmove`（恢复路径会触发该 UB）。

## 目录与复现步骤

```
build/
  gen_samples.py                 纯标准库小样例生成器
  samples/                       3 个极小 JPEG（clean 775B / rst 792B / gray 388B）
  harness/recovery_test.cpp      主测试：四类场景 + 参数校验 + 填充/范围 + 像素不变
  harness/callback_test.cpp      内部缓冲(回调)模式的恢复与越界检查
  harness/realimg.cpp            真实大图 demo.jpg 的大规模损坏稳健性
  pristine/                      从 git HEAD 取出的未改库，用于逐像素对照
```

```sh
# 生成样例
python3 -I gen_samples.py samples

# 主测试（一次跑完，约 10ms）
g++ -D__LINUX__ -O2 -I../src harness/recovery_test.cpp -o harness/recovery_test -lm
./harness/recovery_test samples/sample_clean.jpg samples/sample_rst.jpg samples/sample_gray.jpg

# 内存/UB 检查（alignment、shift 两类告警是库原有的，与本次改动无关）
g++ -D__LINUX__ -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
    -fno-sanitize=alignment,shift -I../src harness/recovery_test.cpp \
    -o harness/recovery_test_san -lm
./harness/recovery_test_san samples/sample_clean.jpg samples/sample_rst.jpg samples/sample_gray.jpg

# 仓库自带测试（MacOS/JPEGDEC_Test，Linux 可直接编译）
g++ -D__LINUX__ -O0 -I../src -I../MacOS/JPEGDEC_Test/JPEGDEC_Test \
    ../MacOS/JPEGDEC_Test/JPEGDEC_Test/main.cpp -o repotest/final_O0 -lm
./repotest/final_O0
```

## 四类场景结果（RGB565，48x40，共 1920 像素）

| 场景 | 坏MCU | Huffman错 | RST重同步 | 跳过 | 恢复像素占比 | 耗时 |
|---|---|---|---|---|---|---|
| 1) 单 MCU 损坏（无RST） | 1 | 1 | 0 | 1 | 86.67%（1664/1920 完好） | ~6us |
| 2) 多 MCU 损坏（无RST） | 3 | 3 | 0 | 3 | 80.00%（1536/1920 完好） | ~4us |
| 3) 带重启标记损坏 | 2 | 1 | 1 | 0 | 73.33%（1408/1920 完好） | ~5us |
| 4) 正常文件 | 0 | 0 | 0 | 0 | 100.00% | ~6us |

- 正常文件：恢复开/关两种方式输出**逐字节相同（0 差异）**；与未改库的转储
  （RGB565/RGB8888/灰度）也逐字节相同；大图 demo.jpg 转 BMP 与未改库逐字节相同。
- 参数非法：search range 0/-1/>1e6、policy 越界、enabled 越界均返回
  JPEG_INVALID_PARAMETER 且配置不变；边界值 1、1000000、三种 policy 正常接受。
- 真实大图 demo.jpg：4243 个单点损坏 + EOF/多点损坏，全部恢复整帧、无崩溃；
  恢复关闭时仍按原逻辑 fail-stop。

## 关于仓库自带测试的说明（重要）

- 修改后库在 **-O0 下 12/12 全部通过**（含两个 fuzz 测试；未改库在 -O0 会在
  单字节 fuzz 阶段因上述除零 FPE 崩溃，退出码 136）。
- -O2 下仅第 3 项“灰度应比彩色快 ≥37.5%”失败：这是基于墙钟耗时的环境相关启发式，
  在本机当前编译器/CPU 下彩色与灰度耗时比约 80%，**未改库在 -O2 下是同样数值、
  同样失败**（且未改库还会在随后的 fuzz 崩溃）。与本次改动无关，无功能回归。
