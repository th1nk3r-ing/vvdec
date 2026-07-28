# VVdeC Internals API — 静态分析支持

## 状态

- **提案阶段**：设计文档，尚未实现
- **作者**：YUView 团队（首个在 VVdeC 中实现 internals API 的）
- **参考实现**：`libde265-internals`、`libVTMDec`（均已被 YUView 消费）
- **目标**：通过 C API 暴露 VVC 编码结构统计信息（CTU/CU/PU/TU/IntraDir/TransformDepth），使外部工具（YUView）无需解析 trace 文本即可渲染逐块分析叠加层

## 背景

### 为什么 VVdeC 需要 internals API

VVdeC 的公共 API（`include/vvdec/vvdec.h`）只暴露解码后的 YUV 帧（`vvdecFrame`）。所有编码结构信息（CU 划分树、预测模式、运动矢量、变换深度）都存在于内部 C++ 类（`CodingUnit`、`TransformUnit`、`CtuData`，位于 `source/Lib/CommonLib/`）中，从不外露。

YUView 为 HEVC 通过 `libde265-internals`、为 VVC 通过 `libVTMDec` 渲染逐块统计叠加层（slice index、part size、pred mode、intra direction、inter direction、motion vector、transform depth）。要对 VVdeC 解码的流提供相同能力，VVdeC 必须暴露 internals API。

### 为什么不复用现有 dtrace 机制

VVdeC 自带 `CDTrace` 系统（`source/Lib/CommonLib/dtrace.h` / `dtrace.cpp`），用于文本调试 trace。乍看是零修改的分析路径，但不适合作为 internals 传输主通道：

| 维度 | dtrace（文本） | internals C API（本提案） |
|---|---|---|
| 传输 | `vfprintf` + `fflush` 写文件（`dtrace.cpp:314`），每次调用都同步刷盘 | 通过函数指针做内存结构体读回 |
| 单次调用开销 | `va_list` 格式化 + 磁盘 I/O + flush | 一次指针解引用 + memcpy |
| 消费端工作 | 把自由格式文本解析回结构化数据（正则/状态机） | 直接访问结构体字段 |
| ABI 稳定性 | 无 — trace 字符串是临时约定 | 版本化结构体 + 枚举，只增不改 |
| 线程 | 并发解码下 `fflush` 是竞争点 | 解码后只读，无需锁 |
| 粒度 | 整帧 trace dump，过滤仅到 channel 级 | 按需逐 CTU / 逐 CU / 逐 PU / 逐 TU |
| 可复现性 | 依赖 trace 字符串格式（跨版本漂移） | 由 `vvdec_internals_version()` 约束 |

**结论**：dtrace 是调试工具，不是稳定的分析传输通道。internals C API 才是正确做法；dtrace 可作为辅助调试保留，但不是主路径。

### YUView 中的现有先例

YUView 已消费两个可比的 internals API：

- **`libde265-internals`**（`YUViewLib/src/decoder/externalHeader/libde265/de265_internals.h`）：平铺 C 函数 `de265_internals_get_CTB_Info_Layout`、`get_CB_info`、`get_PB_info`、`get_IntraDir_info`、`get_TUInfo_info`、`get_gop_info`。通过 `QLibrary::resolve` 加载；`internalsSupported` 由能否 resolve 出 `de265_internals_*` 符号决定，而非文件名。
- **`libVTMDec`**（`YUViewLib/src/decoder/externalHeader/libVTMDecoder.h`）：VTM 参考解码器的 internals 表面，供 YUView 的 `decoderVTM` 使用。

VVdeC internals API 应镜像 `libde265-internals` 的形状（平铺 C、layout+data 两次调用模式、可选符号 resolve），这样 YUView 现有的 `decoderLibde265::cacheStatistics` 实现可几乎原样移植到 `decoderVVDec`。

## VVdeC 侧改动

### 1. 新增公共头 `include/vvdec/vvdec_internals.h`

自包含的 C 头（不含 C++ 类型），声明 internals 访问器。镜像 `de265_internals.h` 布局。

```c
#ifndef VVDEC_INTERNALS_H
#define VVDEC_INTERNALS_H

#include "vvdec.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 结构体布局变更时递增。消费端必须检查 >= 预期版本。 */
VVDEC_DECL int vvdec_internals_version(void);

/* 不透明的 internals 快照句柄。生命周期与 vvdecFrame 绑定。 */
typedef struct vvdec_internals_snapshot vvdec_internals_snapshot;

/* --- CTU 网格布局（对应 de265_internals_get_CTB_Info_Layout） --- */
VVDEC_DECL void vvdec_internals_get_ctu_layout(
    const vvdecFrame *frame,
    int *widthInCtu,
    int *heightInCtu,
    int *log2CtuSize);

/* 逐 CTU 的 slice index。缓冲区大小 = widthInCtu * heightInCtu。 */
VVDEC_DECL void vvdec_internals_get_ctu_slice_idx(
    const vvdecFrame *frame,
    uint16_t *sliceIdx);

/* --- CU（coding unit）网格 --- */
VVDEC_DECL void vvdec_internals_get_cu_layout(
    const vvdecFrame *frame,
    int *widthInCu,
    int *heightInCu,
    int *log2MinCuSize);

/* 打包的 CU 信息。每个 uint16_t 在 min-CU 网格上编码 predMode/partSize/depth。
 * 布局镜像 de265_internals_get_CB_info：调用方分配
 * widthInCu * heightInCu 个 uint16_t。 */
VVDEC_DECL void vvdec_internals_get_cu_info(
    const vvdecFrame *frame,
    uint16_t *cuInfo);

/* --- PU（prediction unit） --- */
VVDEC_DECL void vvdec_internals_get_pb_layout(
    const vvdecFrame *frame,
    int *widthInPb,
    int *heightInPb,
    int *log2MinPbSize);

/* 逐 PB：运动矢量（每个参考列表 x,y）、参考索引（每个列表）、inter dir。 */
VVDEC_DECL void vvdec_internals_get_pb_info(
    const vvdecFrame *frame,
    int16_t *mvL0X, int16_t *mvL0Y,
    int16_t *mvL1X, int16_t *mvL1Y,
    int16_t *refIdxL0, int16_t *refIdxL1,
    int16_t *interDir);

/* --- Intra direction --- */
VVDEC_DECL void vvdec_internals_get_intra_dir_layout(
    const vvdecFrame *frame,
    int *widthInBlock,
    int *heightInBlock,
    int *log2BlockSize);

VVDEC_DECL void vvdec_internals_get_intra_dir_info(
    const vvdecFrame *frame,
    uint8_t *intraDirLuma,
    uint8_t *intraDirChroma);

/* --- TU（transform unit） --- */
VVDEC_DECL void vvdec_internals_get_tu_info_layout(
    const vvdecFrame *frame,
    int *widthInTu,
    int *heightInTu,
    int *log2MinTuSize);

VVDEC_DECL void vvdec_internals_get_tu_info(
    const vvdecFrame *frame,
    uint8_t *tuInfo);

/* --- GOP / 参考图像列表（用于 GOP 图） --- */
VVDEC_DECL void vvdec_internals_get_gop_info(
    const vvdecFrame *frame,
    int *poc,
    int *isRefPic,
    int *sliceType,
    int *temporalLayer,
    int *nalType,
    int *qp,
    int refPocL0[16],
    int refPocL1[16]);

VVDEC_DECL int64_t vvdec_internals_get_image_dts(const vvdecFrame *frame);

#ifdef __cplusplus
}
#endif

#endif /* VVDEC_INTERNALS_H */
```

### 2. 实现 `source/Lib/vvdec/vvdec_internals.cpp`

新增翻译单元，链接进 `vvdec` target。直接读取 `CommonLib` 和 `DecoderLib` 中已有的内部 `Picture` / `CodingStructure` / `CtuData` / `CodingUnit` / `TransformUnit` 类型。

关键映射（内部 → 导出）：

| 导出函数 | 内部来源 | 备注 |
|---|---|---|
| `get_ctu_layout` | `Picture::cs->pcv->widthInCtus`、`heightInCtus`、`sps->CTUSize` | `CodingStructure.h`、`CtuData::ctuIdx` |
| `get_ctu_slice_idx` | 每个 CTU 的 `CtuData::slice->sliceIdx` | `CtuData` 在 `CodingStructure.h:85` |
| `get_cu_layout` | `sps::MaxCUDepth`，min CU size = CTUSize >> MaxCUDepth | |
| `get_cu_info` | 遍历 `CtuData::firstCU` → `CodingUnit::next`；在 min-CU 网格上把 `_predMode`、`PartSize`（由 splitSeries / depth 推导）、`depth` 打包进 uint16_t | `Unit.h:314` |
| `get_pb_info` | `CodingUnit::mv[][]`、`refIdx[]`、`interDir()` | `Unit.h:332-345` |
| `get_intra_dir_info` | `CodingUnit::intraDir[MAX_NUM_CHANNEL_TYPE]` | `Unit.h:336` |
| `get_tu_info` | 遍历 `CodingUnit::firstTU` → `TransformUnit::next`；打包 transform depth | `Unit.h:285` |
| `get_gop_info` | `Slice::TLayer`、`nalType`、`Slice::sliceType`、参考图像列表取自 `Slice::getRefPicListL0/L1` | `Slice.h` |
| `get_image_dts` | 若存在 `Picture::dts`，否则 -1 | |

实现必须：

- **只读**：绝不修改解码器状态。
- **null 安全**：若 `frame == NULL` 或底层 `Picture` 未保留，直接返回不写入。
- 从 `vvdecFrame` 获取 `Picture*`：`VVDecImpl` 保存 `FrameListEntry = tuple<vvdecFrame, Picture*>`（`vvdecimpl.h:203`）；internals 实现需要从 `vvdecFrame*` 查到 `Picture*`。在 `vvdecimpl.cpp` 暴露私有 helper `vvdecimpl_get_picture(vvdecFrame*)`（文件作用域，不导出），由 `vvdec_internals.cpp` 调用。

### 3. CMake 接线（`source/Lib/vvdec/CMakeLists.txt`）

- 把 `vvdec_internals.cpp` 加入 `SRC_FILES`。
- 把 `vvdec_internals.h`（由 `vvdec_internals.h.in` 生成，与 `vvdec.h.in` 使用的 `.h.in` 约定一致）加入 `INC_FILES` 和已安装公共头集合。
- 用 `VVDEC_ENABLE_INTERNALS`（默认 `OFF`）门控编译，这样不需要分析的 release 构建不会引入额外表面。`OFF` 时符号直接缺失 — 消费端通过 `QLibrary::resolve` 返回 null 检测，与 `libde265-internals` 完全一致。

```cmake
option( VVDEC_ENABLE_INTERNALS "Build VVdeC with internals analysis API (vvdec_internals_*)" OFF )
if( VVDEC_ENABLE_INTERNALS )
  list( APPEND SRC_FILES vvdec_internals.cpp )
  list( APPEND INC_FILES ${CMAKE_CURRENT_SOURCE_DIR}/vvdec_internals.h )
  target_compile_definitions( ${LIB_NAME} PUBLIC VVDEC_INTERNALS_ENABLED )
endif()
```

### 4. 头文件安装

在顶层安装规则中（`vvdec.h` 安装到 `include/vvdec/` 的位置），同时安装 `vvdec_internals.h`，使下游消费端可 `#include <vvdec/vvdec_internals.h>`。

### 5. 库命名（可选，推荐）

当 `VVDEC_ENABLE_INTERNALS=ON` 时，设置不同的输出名，使 YUView 可优先加载 internals 变体：

```cmake
if( VVDEC_ENABLE_INTERNALS )
  set_target_properties( ${LIB_NAME} PROPERTIES OUTPUT_NAME vvdec-internals )
else()
  set_target_properties( ${LIB_NAME} PROPERTIES OUTPUT_NAME vvdec )
endif()
```

这镜像了 `libde265-internals` / `libde265`，让 YUView 的 `getLibraryNames()` 返回 `["vvdec-internals", "vvdec"]`。internals 变体是超集（仍导出所有 `vvdec_*` 符号），故为直接替换。

## YUView 侧改动

### 1. 新增外部头 `YUViewLib/src/decoder/externalHeader/vvdec/vvdec_internals.h`

复制 VVdeC 安装的 `vvdec_internals.h`。这是 `externalHeader/libde265/de265_internals.h` 和 `externalHeader/libVTMDecoder.h` 已有的模式。

### 2. 扩展 `decoderVVDec.h` / `.cpp`

镜像 `decoderLibde265.h:68-90`（`lib` 函数指针 struct）和 `decoderLibde265.cpp:524`（`cacheStatistics`）的结构。

在 `decoderVVDec.h`：

- 在库函数指针 struct（由 `resolveLibraryFunctionPointers()` 填充）中添加：

```cpp
// vvdec internals 函数指针（仅在存在时 resolve）
int  (*vvdec_internals_version)(){nullptr};
void (*vvdec_internals_get_ctu_layout)(const vvdecFrame *, int *, int *, int *){nullptr};
void (*vvdec_internals_get_ctu_slice_idx)(const vvdecFrame *, uint16_t *){nullptr};
void (*vvdec_internals_get_cu_layout)(const vvdecFrame *, int *, int *, int *){nullptr};
void (*vvdec_internals_get_cu_info)(const vvdecFrame *, uint16_t *){nullptr};
void (*vvdec_internals_get_pb_layout)(const vvdecFrame *, int *, int *, int *){nullptr};
void (*vvdec_internals_get_pb_info)(const vvdecFrame *, int16_t *, int16_t *, int16_t *, int16_t *, int16_t *, int16_t *, int16_t *){nullptr};
void (*vvdec_internals_get_intra_dir_layout)(const vvdecFrame *, int *, int *, int *){nullptr};
void (*vvdec_internals_get_intra_dir_info)(const vvdecFrame *, uint8_t *, uint8_t *){nullptr};
void (*vvdec_internals_get_tu_info_layout)(const vvdecFrame *, int *, int *, int *){nullptr};
void (*vvdec_internals_get_tu_info_info)(const vvdecFrame *, uint8_t *){nullptr};
void (*vvdec_internals_get_gop_info)(const vvdecFrame *, int *, int *, int *, int *, int *, int *, int[16], int[16]){nullptr};
int64_t (*vvdec_internals_get_image_dts)(const vvdecFrame *){nullptr};
```

- 添加镜像 `decoderVTM.h:123-129` 的成员：

```cpp
void cacheStatistics(const vvdecFrame *frame);
bool internalsSupported{false};
void fillStatisticList(stats::StatisticsData &statisticsData) const override;
```

- 在 `resolveLibraryFunctionPointers()` 中：以 `optional = true` resolve 每个 `vvdec_internals_*` 符号。仅当**所有**必需 internals 符号都 resolve 成功时才置 `internalsSupported = true`（与 `decoderLibde265.cpp:166` 对 `de265_internals_*` 的规则相同）。

- 在 `getLibraryNames()` 中：所有平台返回 `["vvdec-internals", "vvdec"]`（mac 加 `.dylib` 后缀变体），镜像 `decoderLibde265.cpp:1007`。

### 3. `cacheStatistics` 实现

把 `decoderLibde265::cacheStatistics`（`decoderLibde265.cpp:524-840`）移植到 VVdeC。结构相同：

1. `get_ctu_layout` → 分配 CTU 网格缓冲区。
2. `get_ctu_slice_idx` → 写 `sliceIdx` 统计（stat ID 0）。
3. `get_cu_layout` + `get_cu_info` → 遍历 min-CU 网格，解包 `predMode`/`partSize`/`depth`，递归划分树（复用 `decoderLibde265.cpp:765` 的 `cacheStatistics_TUTree_recursive` 模式）。
4. `get_pb_layout` + `get_pb_info` → 运动矢量（`mvL0`/`mvL1`）、`refIdx`、`interDir`。
5. `get_intra_dir_layout` + `get_intra_dir_info` → `intraDirLuma`（ID 9）、`intraDirChroma`（ID 10）。
6. `get_tu_info` → `transformDepth`（ID 11）。
7. `get_gop_info` → 为 GOP 图填充 `GOPFrame`（镜像 `decoderLibde265` 在 `HEVCGOPScanner.h:50` 的 GOP 用法）。

### 4. `fillStatisticList` 实现

逐字移植 `decoderLibde265::fillStatisticList`（`decoderLibde265.cpp:890-986`）— 相同 stat ID、相同颜色映射、相同描述。这样跨解码器叠加层可对比（libde265 与 VVdeC 渲染的 CU pred-mode 块使用相同 colormap）。

统计类型目录（ID 与 `decoderLibde265` 对齐）：

| ID | 名称 | 来源 | 说明 |
|---|---|---|---|
| 0 | Slice Index | `get_ctu_slice_idx` | 逐 CTU 的 slice index |
| 1 | Part Size | `get_cu_info`（打包） | CU 划分尺寸 |
| 2 | Pred Mode | `get_cu_info`（打包） | intra/inter/skip |
| 3 | Intra Mode Luma | `get_intra_dir_info` | luma intra 方向 |
| 4 | Intra Mode Chroma | `get_intra_dir_info` | chroma intra 方向 |
| 5 | Inter Dir | `get_pb_info` | L0/L1/bi |
| 6 | RefIdx L0 | `get_pb_info` | L0 参考索引 |
| 7 | RefIdx L1 | `get_pb_info` | L1 参考索引 |
| 8 | MV L0 | `get_pb_info` | L0 运动矢量 |
| 9 | MV L1 | `get_pb_info` | L1 运动矢量 |
| 10 | Transform Depth | `get_tu_info` | 变换树中的 TU 深度 |

> 注：实现前需与 `decoderLibde265::fillStatisticList`（`decoderLibde265.cpp:890`）核对确切 ID 编号 — 上表是目标对齐，但 libde265 的 ID 可能略有出入。目标是：**相同语义的统计 → 在 libde265/HM/VVdeC 间使用相同 ID**，以便 YUView 的叠加层渲染代码共享。

## VVC 新增结构划分支持（数据结构 + 绘制）

VVC 相对 HEVC 在编码结构上有大量新增。本节列出 VVC 独有的、需要 internals API 暴露并在 YUView 侧新增绘制支持的字段。**这是本 API 与 `libde265-internals` 的关键差异点** —— 仅移植 HEVC 字段不足以表达 VVC 编码结构。

### 1. CU 划分树：QTMT（Quad-Tree + Multi-Type Tree）

VVC 用 QTMT 取代 HEVC 的纯四叉树。`CodingUnit::splitSeries`（`Unit.h:338`，类型 `SplitSeries = uint16_t`）以 5-bit/level 的位打包形式编码从 CTU 到当前 CU 的完整划分序列。

**VVdeC 内部数据结构**（`CommonDef.h:348`、`UnitPartitioner.h:66`）：

```c
// 每层划分类型占 5 bit（SPLIT_DMULT=5, SPLIT_MASK=31）
enum PartSplit {
  CTU_LEVEL        = 0,   // CTU 根
  CU_QUAD_SPLIT,          // 四叉树（HEVC 也有）
  CU_HORZ_SPLIT,          // 水平二叉（VVC 新增）
  CU_VERT_SPLIT,          // 垂直二叉（VVC 新增）
  CU_TRIH_SPLIT,          // 水平三叉（VVC 新增）
  CU_TRIV_SPLIT,          // 垂直三叉（VVC 新增）
  TU_MAX_TR_SPLIT,        // TU 变换分割
  TU_NO_ISP, TU_1D_HORZ_SPLIT, TU_1D_VERT_SPLIT,  // ISP
  SBT_VER_HALF_POS0_SPLIT ... SBT_HOR_QUAD_POS1_SPLIT,  // SBT
};
// splitSeries = Σ (split_at_depth_d << (d * 5))
// 解码：从低到高每 5 bit 取一个 PartSplit，直到 CTU_LEVEL 或到达 CU 深度
```

**API 需暴露**（新增，超出 libde265 对齐集合）：

```c
// 获取 min-CU 网格上每个 CU 的 splitSeries（用于重建划分边界）
VVDEC_DECL void vvdec_internals_get_cu_split_series(
    const vvdecFrame *frame, uint16_t *out_split_series);
// out_split_series[i] = min-CU 网格第 i 格所属 CU 的 splitSeries
// 调用方按 spec §6.4.1 解码此值重建划分树
```

**YUView 侧绘制**：

- 当前 `paintStatisticsData`（`StatisticsDataPainting.cpp:252`）只画矩形 `drawRect`（`displayRect`）。二叉/三叉分割的子块**仍是矩形**，所以 part-size 叠加层用 min-CU 网格 + `splitSeries` 解码后的 CU 边界即可，无需新绘制原语。
- 新增统计类型 `Partition Tree`（建议 ID 12），绘制 CU 边界线（用 `gridStyle`，`StatisticsType.h:133`）。颜色按 `splitSeries` 末层 split 类型分色（QT=蓝、BT=绿、TT=红）。

### 2. VVC 独有的 Intra 模式

VVC intra 预测模式数从 HEVC 的 35 扩展到 67+，并新增 MIP、ISP、BDPCM 三类。

**VVdeC 内部数据结构**（`CommonDef.h:230`、`Unit.h:336/359/364`）：

```c
// intraDir[] 取值范围（luma）：
//   0      = INTRA_PLANAR
//   1      = INTRA_DC
//   2..66  = INTRA_ANGULAR_2..34（65 个方向模式）
// chroma 额外：LM_CHROMA_IDX=67, DM_CHROMA_IDX=68
static const int NUM_LUMA_MODE    = 67;
static const int MAX_NUM_MIP_MODE = 32;

// CU 上的标志位（Unit.h）
bool    _mipTranspose;   // MIP 转置标志
int8_t  intraDir[2];     // [0]=luma [1]=chroma（MAX_NUM_CHANNEL_TYPE=2）
uint8_t _bdpcmL : 2;     // luma BDPCM 模式
uint8_t _bdpcmC : 2;     // chroma BDPCM 模式
```

**API 需暴露**（扩展 `get_intra_dir_info` 的输出）：

```c
// 在现有 intraDir luma/chroma 之外，新增 MIP/BDPCM 标志
VVDEC_DECL void vvdec_internals_get_intra_mode_flags(
    const vvdecFrame *frame,
    uint8_t *out_mip_flag,      // 1 = 该 CU 用 MIP
    uint8_t *out_mip_transpose, // MIP 转置
    uint8_t *out_bdpcm_luma,    // 0=无 1=水平 2=垂直
    uint8_t *out_bdpcm_chroma);
```

**YUView 侧绘制**：

- `fillStatisticList` 中 `intraDirC`（ID 3）的 `valueRange` 从 HEVC 的 35 扩到 68，颜色映射需重做（`decoderLibde265.cpp:960` 现有的 35 项 `PredefinedType::Jet` 不够）。
- 新增统计类型 `MIP Mode`（建议 ID 13）、`BDPCM`（建议 ID 14）。
- MIP 的 32 个子模式若需可视化，单独 ID 15。

### 3. VVC 独有的 Inter 模式

VVC 新增 Affine、MMVD、Geo (GPM)、IBC 四类 inter 预测，运动模型从 1 个 MV/方向 扩展到 3 个（仿射控制点）。

**VVdeC 内部数据结构**（`Unit.h:332/340/341/348`）：

```c
Mv      mv[NUM_REF_PIC_LIST_01][3];  // [list][0..2]，仿射用 3 个控制点
uint8_t geoSplitDir;                  // Geo 分割方向（0..7）
uint8_t mmvdIdx;                      // MMVD 候选索引
uint8_t _mergeType : 2;               // 普通/子块/IBC/Geo
// MotionInfo.h: refIdx, mv, interDir, affineType, bcwIdx, ...
```

**API 需暴露**（扩展 `get_pb_info`）：

```c
// 现有 get_pb_info 返回 1 个 mv/方向；仿射需 3 个
VVDEC_DECL void vvdec_internals_get_pb_info_ex(
    const vvdecFrame *frame,
    int16_t *mvL0_cp0_x, int16_t *mvL0_cp0_y,  // 控制点 0（左上）
    int16_t *mvL0_cp1_x, int16_t *mvL0_cp1_y,  // 控制点 1（右上）
    int16_t *mvL0_cp2_x, int16_t *mvL0_cp2_y,  // 控制点 2（左下，仅仿射）
    int16_t *mvL1_cp0_x, int16_t *mvL1_cp0_y,
    int16_t *mvL1_cp1_x, int16_t *mvL1_cp1_y,
    int16_t *mvL1_cp2_x, int16_t *mvL1_cp2_y,
    int16_t *refIdxL0, int16_t *refIdxL1,
    int16_t *interDir,
    uint8_t *mergeType,    // 0=普通 1=子块 2=IBC 3=Geo
    uint8_t *geoSplitDir,  // Geo 方向
    uint8_t *mmvdIdx);
```

**YUView 侧绘制**：

- `FrameTypeData::addBlockAffineTF`（`FrameTypeData.h:126`）已存在，**可直接复用**绘制仿射运动场（3 控制点插值）。这是 YUView 已有的能力，只需在 `cacheStatistics` 中检测 `mergeType == AFFINE` 时调用。
- 新增统计类型 `Merge Type`（建议 ID 16，值 0-3）、`Geo Split Dir`（建议 ID 17，值 0-7）。
- Geo 分割线（非矩形）需用 `polygonValueData`（`FrameTypeData.h:150`）—— YUView 已支持多边形绘制，用于 Geo 的三角形/四边形子块。

### 4. VVC 独有的 Transform 结构

VVC 新增 MTS（多变换集）、LFNST（低频不可分离变换）、SBT（子块变换）、ISP（子分区变换）。

**VVdeC 内部数据结构**（`Unit.h:285` `TransformUnit`）：

```c
struct TransformUnit : public UnitArea {
  uint8_t _chType : 2;
  uint8_t jointCbCr : 2;     // jointCbCr 残差
  uint8_t _mtsIdx : 2;       // MTS 索引
  uint8_t _lfnstIdx : 2;     // LFNST 索引
  uint8_t sbtInfo : 4;       // SBT 模式（水平/垂直 + 半/四分 + 位置）
  // ...
};
```

**API 需暴露**：

```c
VVDEC_DECL void vvdec_internals_get_tu_info_ex(
    const vvdecFrame *frame,
    uint8_t *out_mts_idx,     // 0=DCT2 1-5=MTS 集
    uint8_t *out_lfnst_idx,   // 0=无 1/2=水平/垂直
    uint8_t *out_sbt_info,    // 编码 SBT
    uint8_t *out_isp_mode,    // 0=无 1=水平 2=垂直
    uint8_t *out_jcbcr);      // jointCbCr
```

**YUView 侧绘制**：

- 现有 `transformDepth`（ID 10）保留。
- 新增 `MTS Index`（ID 18）、`LFNST Index`（ID 19）、`SBT Mode`（ID 20）、`ISP Mode`（ID 21）、`JointCbCr`（ID 22）。

### 5. VVdeC internals API 完整统计类型目录（含 VVC 扩展）

| ID | 名称 | 来源 | VVC 新增？ |
|---|---|---|---|
| 0 | Slice Index | `get_ctu_slice_idx` | 否 |
| 1 | Part Size | `get_cu_info` | 否（但取值集扩展） |
| 2 | Pred Mode | `get_cu_info` | 否（新增 IBC） |
| 3 | Intra Mode Luma | `get_intra_dir_info` | 否（范围扩到 67） |
| 4 | Intra Mode Chroma | `get_intra_dir_info` | 否（范围扩到 68） |
| 5 | Inter Dir | `get_pb_info` | 否 |
| 6 | RefIdx L0 | `get_pb_info` | 否 |
| 7 | RefIdx L1 | `get_pb_info` | 否 |
| 8 | MV L0 | `get_pb_info` | 否（仿射需 3 控制点） |
| 9 | MV L1 | `get_pb_info` | 否（仿射需 3 控制点） |
| 10 | Transform Depth | `get_tu_info` | 否 |
| **12** | **Partition Tree** | `get_cu_split_series` | **是** |
| **13** | **MIP Mode** | `get_intra_mode_flags` | **是** |
| **14** | **BDPCM** | `get_intra_mode_flags` | **是** |
| **15** | **MIP Sub-Mode** | `get_intra_mode_flags` | **是** |
| **16** | **Merge Type** | `get_pb_info_ex` | **是** |
| **17** | **Geo Split Dir** | `get_pb_info_ex` | **是** |
| **18** | **MTS Index** | `get_tu_info_ex` | **是** |
| **19** | **LFNST Index** | `get_tu_info_ex` | **是** |
| **20** | **SBT Mode** | `get_tu_info_ex` | **是** |
| **21** | **ISP Mode** | `get_tu_info_ex` | **是** |
| **22** | **JointCbCr** | `get_tu_info_ex` | **是** |

> ID 11 保留给 `transformDepth`（与 libde265 对齐，见 `decoderLibde265.cpp:982`）。VVC 新增项从 12 起。

### 6. 绘制原语复用情况

| VVC 结构 | YUView 现有绘制原语 | 是否需新增 |
|---|---|---|
| QT/BT/TT CU 边界 | `drawRect`（矩形） | 否，子块仍是矩形 |
| Affine 运动场 | `addBlockAffineTF`（3 控制点） | 否，已存在 |
| Geo 分割子块 | `polygonValueData`（多边形） | 否，已存在 |
| MIP/ISP/SBT 块 | `drawRect` + 颜色映射 | 否 |
| LFNST/MTS 块 | `drawRect` + 颜色映射 | 否 |

**结论**：YUView 现有绘制基础设施（`StatisticsDataPainting.cpp`）**无需新增原语**即可支持所有 VVC 结构。工作量集中在 internals API 数据采集 + `fillStatisticList` 注册新 `StatisticsType`。

## TODO：按 spec 对齐的划分树遍历

`splitSeries` 解码需严格按 ITU-T H.266/VVC §6.4.1（"Partition tree derivation process"）。首版可先用 min-CU 网格打包渲染 part-size 叠加层；递归树遍历（`cacheStatistics_partitionTree_recursive`，仿 `decoderLibde265.cpp:765` 的 `cacheStatistics_TUTree_recursive`）作为 ID 12 的精确实现延后。

**关键约束**：三叉分割（`CU_TRIH_SPLIT`/`CU_TRIV_SPLIT`）的中间块尺寸为 (size/4, size/2, size/4)，不是均分；二叉分割是均分。`splitSeries` 解码时必须按 spec 处理这个不对称性，否则边界画错。

## 防御性实现要点

- **空帧**：每个 `vvdec_internals_*` 函数必须在 `frame == NULL` 时提前返回（YUView 在首帧可用前就会调用 `cacheStatistics`）。
- **版本检查**：YUView 必须先调用 `vvdec_internals_version()`，若 `< VVDEC_INTERNALS_MIN_VERSION` 则拒绝使用 internals。在 YUView 中定义 `VVDEC_INTERNALS_MIN_VERSION 1`。
- **过期图像**：`vvdecimpl.cpp:1535` 注明 `vvdecFrame` "may still be used internally after already returned to the caller."。internals 实现不得假设独占所有权；只读不写。
- **线程**：YUView 的缓存解码器在 worker 线程运行。internals API 必须支持在*不同*解码器实例上与 `vvdec_decode` 并发调用；同一实例内，YUView 只在 `vvdec_decode` 返回帧后调用 `cacheStatistics`，故无实例内并发。需文档化此契约。
- **符号解析是唯一判据**：按 AGENTS.md 约定（`decoderLibde265.cpp:166`），`internalsSupported` 由 `vvdec_internals_*` 符号能否通过 `QLibrary::resolve` 决定，**而非**库文件名。`getLibraryNames()` 中 `vvdec-internals` vs `vvdec` 的名称偏好仅是加载顺序提示。

## 验证计划

1. 用 `VVDEC_ENABLE_INTERNALS=ON` 构建 VVdeC，确认 `vvdec-internals` 库导出 `vvdec_internals_*` 符号（如 `nm -D libvvdec-internals.so | grep vvdec_internals`）。
2. 基于新外部头构建 YUView；确认加载 `vvdec-internals` 时 `decoderVVDec::internalsSupported` 翻转为 `true`。
3. 解码一路 VVC 一致性流，渲染 slice-index / pred-mode / intra-dir 叠加层，与同流用 `libVTMDec`（VTM 参考解码器）解码的结果视觉对比 — 叠加层必须一致。
4. 运行 YUView 单元测试：`./build/YUViewUnitTest/YUViewUnitTest --gtest_filter=StatisticsFile*`。
5. 确认 `vvdec`（非 internals）库仍可加载并解码；`internalsSupported` 必须为 `false`，解码器仍能产出 YUV（internals 严格可选）。

## 范围外（本提案不涉及）

- 修改现有 dtrace 系统。保持原样用于调试。
- 改动 `vvdecFrame` 结构体布局。internals API 是旁路通道；帧结构体不动。
- ALF/SAO 滤波器系数可视化（`CtuData::alfParam`/`saoParam`，`CodingStructure.h:87`）—— 可后续作为独立 internals API 扩展。
- LMCS 重映射曲线可视化 —— 属于 frame 级而非 block 级统计，不在 internals API 范畴。
