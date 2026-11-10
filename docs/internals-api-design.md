# VVdeC Internals API 使用说明

VVdeC 默认只对外提供解码后的 YUV 帧（`vvdecFrame`）。`vvdec_internals_*` 是在此之外的一组可选 C 接口，把解码器内部的编码结构（CTU slice 划分、CU 划分树、预测模式、运动信息、帧内方向、变换深度、GOP 元数据）导出为扁平缓冲区，供 YUView 这类分析工具直接消费，无需解析 trace 文本。

接口全部声明在 `include/vvdec/vvdec_internals.h`，实现位于 `source/Lib/vvdec/vvdec_internals.cpp`。

- 所有函数返回 `int`，只有 `vvdec_internals_get_image_dts_dec` 返回 `int64_t`。
- 标量出参可以为 `NULL`（表示不关心该值）；CTU/min-CU 填表类函数的缓冲区出参必须非 `NULL`，否则返回 `VVDEC_ERR_PARAMETER`。
- 查询只读，不修改解码器状态，也不需要加锁——但必须在帧被 `vvdec_frame_unref` 之前调用。

## 1. 启用与支持性检测

### 1.1 构建开关

```bash
cmake -S . -B build -DVVDEC_ENABLE_INTERNALS=ON
```

| 项目 | 行为 |
|---|---|
| CMake 缓存变量 | `VVDEC_ENABLE_INTERNALS`，默认 `OFF` |
| `OFF` | `vvdec_internals.cpp` 与 `vvdec_internals.h` 被 `FILTER ... EXCLUDE` 从源文件列表中剔除，不参与编译，库中不存在任何 `vvdec_internals_*` 符号 |
| `ON` | 编译 `vvdec_internals.cpp` 并导出 13 个 `VVDEC_DECL` 符号 |
| 编译定义 | `VVDEC_INTERNALS_ENABLED=1`，`PRIVATE`，只影响库自身编译，不进入下游编译命令行 |

注意：`vvdec_internals.h` 会随 `install(DIRECTORY include/vvdec ...)` 一起被安装，**无论开关是否打开**。因此"头文件存在"并不代表库里有实现；必须按 1.2 检测符号。

### 1.2 运行时检测

当前没有版本函数。唯一的支持性判据是能否解析出符号：

```c
/* QLibrary::resolve / dlsym / GetProcAddress 均可 */
if( resolve( "vvdec_internals_get_ctb_info_layout_dec" ) )
{
  /* internals API 可用 */
}
```

`vvdec_internals_get_ctb_info_layout_dec` 是布局查询入口，所有消费方第一步都要调用它，用它作为探测符号可以一次性确认整组接口可用。符号存在即认为整套 API 可用（同一构建产物中 13 个符号总是同时存在）。

**限制**：没有 `vvdec_internals_version()` 之类的版本号，也没有任何位布局的自我描述。若将来 CB 位域、intra 编码或 GOP 出参发生变化，旧调用方无法在运行时察觉。在这种情况下只能同步升级调用方与库；引入版本机制需要在 `vvdec_internals.h` 中新增导出的版本函数，当前不存在。

## 2. 最小可用示例

```c
#include <stdio.h>
#include <stdlib.h>
#include <vvdec/vvdec.h>
#include <vvdec/vvdec_internals.h>

static void dump_cu_info( vvdecDecoder* dec, const vvdecFrame* frame )
{
  int w = 0, h = 0, log2MinCu = 0;

  if( vvdec_internals_get_cb_info_layout_dec( dec, frame, &w, &h, &log2MinCu ) != VVDEC_OK )
  {
    return;   /* frame 不属于该 decoder，或已被 unref */
  }

  const size_t n = (size_t) w * h;
  uint16_t*    cb = (uint16_t*) malloc( n * sizeof( uint16_t ) );
  if( !cb )
  {
    return;
  }

  if( vvdec_internals_get_cb_info_dec( dec, frame, cb ) == VVDEC_OK )
  {
    for( int y = 0; y < h; y++ )
    {
      for( int x = 0; x < w; x++ )
      {
        const uint16_t v = cb[y * w + x];
        if( !( v & ( 1 << 6 ) ) )   /* bit 6 = CU 左上角标记，其余格子为 0 */
        {
          continue;
        }
        const int cuW = 1 << ( ( v & 0x7 ) + 2 );
        const int cuH = 1 << ( ( ( v >> 3 ) & 0x7 ) + 2 );
        printf( "%4d %4d  %2dx%-2d  pred=%d qt=%d mt=%d skip=%d\n",
                x << log2MinCu, y << log2MinCu, cuW, cuH,
                ( v >> 7 ) & 0x3, ( v >> 9 ) & 0x7, ( v >> 12 ) & 0x7, v >> 15 );
      }
    }
  }
  free( cb );
}

int main( void )
{
  vvdecParams params;
  vvdec_params_default( &params );

  vvdecDecoder* dec = vvdec_decoder_open( &params );
  if( !dec )
  {
    return 1;
  }

  vvdecAccessUnit* au = vvdec_accessUnit_alloc();
  vvdec_accessUnit_alloc_payload( au, 4 << 20 );

  /* 逐个 access unit：填 au->payload / au->payloadSize / au->payloadUsedSize */
  vvdecFrame* frame = NULL;
  int         ret   = vvdec_decode( dec, au, &frame );

  if( frame )
  {
    dump_cu_info( dec, frame );        /* 查询必须在 unref 之前完成 */
    vvdec_frame_unref( dec, frame );   /* unref 后 Picture 归还，查询全部失效 */
  }

  /* 码流结束后重复调用 vvdec_flush( dec, &frame ) 直到返回 VVDEC_EOF */

  vvdec_decoder_close( dec );
  vvdec_accessUnit_free_payload( au );
  vvdec_accessUnit_free( au );
  return 0;
}
```

## 3. 调用契约

### 3.1 frame 的来源

所有带 `const vvdecFrame* frame` 参数的函数都通过解码器内部的 frame 列表把 `vvdecFrame*` 反查回 `Picture*`：

- `frame` 必须是**同一个** `decoder` 通过 `vvdec_decode` / `vvdec_flush` 返回的指针。
- 传入其他解码器返回的 frame、栈上伪造的 frame，或已经 `vvdec_frame_unref` 过的 frame，反查失败，函数返回 `VVDEC_ERR_INITIALIZE`。
- `vvdec_flush` 返回的帧同样可以查询。

### 3.2 时间窗口

`vvdec_frame_unref` 会解除对该 `Picture` 的应用层锁定（`releasePicture` 只清 `lockedByApplication`），之后这块内存可被后续解码重用，所有 internals 查询结果失效。因此顺序固定为：

```
vvdec_decode -> 查询 -> vvdec_frame_unref
```

### 3.3 Picture 保留

启用 internals（`VVDEC_INTERNALS_ENABLED`）时，解码器会把内部 `Picture` 一直保留到应用调用 `vvdec_frame_unref`，包括 8bit 输出走独立输出缓冲区的情形；未启用时，8bit 帧在拷贝到输出缓冲区后立刻解锁 `Picture`，使其可被后续解码重用。这是 internals 查询能读到结构信息的先决条件。

### 3.4 线程

单次查询是纯只读，不解锁、不阻塞。可以在不同解码器实例上并发调用；同一实例内应在 `vvdec_decode` / `vvdec_flush` 返回之后调用，不要与正在进行的解码并发。

## 4. 网格约定

两套网格，都是行优先、`idx = y * width + x`。

### 4.1 CTU 网格

- `widthInCtbs` = 图像宽度方向的 CTU 数，`heightInCtbs` = 高度方向。
- `log2CtuSize` = CTU 尺寸的 log2（`6` 表示 64x64）。
- 缓冲区长度 `widthInCtbs * heightInCtbs`，元素类型 `uint16_t`。

### 4.2 min-CU 网格

- 单元固定为 4x4 亮度样点（`log2MinCuSize` 恒为 2）。
- `widthInMinCUs = (lumaWidth + minCUWidth - 1) >> log2MinCuSize`，高度同理，即**向上取整**。
- 每个格子映射到覆盖该位置的 `CodingUnit`。
- 缓冲区长度 `widthInMinCUs * heightInMinCUs`。
- 落在图像实际宽高之外的尾格（宽高不是 4 的倍数时）在 CB 表中写 0，在 PB 表中写 `refIdx = -1, mv = 0`，在 intra 表中写 0，在 TU 表中写 0。

CB / PB / intra / TU 四类数据共用同一套 min-CU 网格，四个 layout 函数返回的尺寸完全一致。

## 5. API 参考

### 5.1 函数总表

| # | 函数 | 网格 | 数组出参 | 数组长度 |
|---|---|---|---|---|
| 1 | `vvdec_internals_get_ctb_info_layout_dec` | CTU | 无（3 个标量） | — |
| 2 | `vvdec_internals_get_ctb_slice_idx_dec` | CTU | 1 个 `uint16_t*` | `widthInCtbs*heightInCtbs` |
| 3 | `vvdec_internals_get_cb_info_layout_dec` | min-CU | 无（3 个标量） | — |
| 4 | `vvdec_internals_get_cb_info_dec` | min-CU | 1 个 `uint16_t*` | `widthInMinCUs*heightInMinCUs` |
| 5 | `vvdec_internals_get_pb_info_layout_dec` | min-CU | 无（3 个标量） | — |
| 6 | `vvdec_internals_get_pb_info_dec` | min-CU | 6 个 `int16_t*` | `widthInMinCUs*heightInMinCUs` |
| 7 | `vvdec_internals_get_intra_dir_info_layout_dec` | min-CU | 无（3 个标量） | — |
| 8 | `vvdec_internals_get_intra_dir_info_dec` | min-CU | 2 个 `uint8_t*` | `widthInMinCUs*heightInMinCUs` |
| 9 | `vvdec_internals_get_tu_info_layout_dec` | min-CU | 无（3 个标量） | — |
| 10 | `vvdec_internals_get_tu_info_dec` | min-CU | 1 个 `uint8_t*` | `widthInMinCUs*heightInMinCUs` |
| 11 | `vvdec_internals_get_gop_info_dec` | 帧 | `refPocL0[16]`、`refPocL1[16]` | 固定 16 |
| 12 | `vvdec_internals_get_gop_info_headeronly` | 帧 | `refPocL0[16]`、`refPocL1[16]` | 固定 16 |
| 13 | `vvdec_internals_get_image_dts_dec` | 帧 | 无 | — |

四个 min-CU 布局函数（#3/#5/#7/#9）是同一份实现：#5/#7/#9 直接调用 #3。用哪一个都行，建议按用途调用以便阅读。

### 5.2 返回码

| 返回码 | 值 | 触发条件 |
|---|---|---|
| `VVDEC_OK` | 0 | 成功 |
| `VVDEC_ERR_INITIALIZE` | -2 | `dec` 为 `NULL`；`frame` 为 `NULL`；`frame` 不属于该解码器或已 unref；图片无 slice 数据；header-only getter 尚无已完成图片 |
| `VVDEC_ERR_PARAMETER` | -7 | 必需的数组出参为 `NULL` |

`VVDEC_ERR_PARAMETER` 只在填表类函数中出现（#2/#4/#6/#8/#10）。布局函数与 GOP 函数不检查数组指针是否为 `NULL`——它们把数组出参当可空出参处理，`NULL` 表示跳过。

### 5.3 CTB（CTU）：布局与 slice index

```c
int vvdec_internals_get_ctb_info_layout_dec( vvdecDecoder *dec,
                                             const vvdecFrame *frame,
                                             int *widthInCtbs,     /* 可空 */
                                             int *heightInCtbs,    /* 可空 */
                                             int *log2CtuSize );   /* 可空 */

int vvdec_internals_get_ctb_slice_idx_dec( vvdecDecoder *dec,
                                           const vvdecFrame *frame,
                                           uint16_t *out );      /* 必须非空 */
```

- `widthInCtbs` / `heightInCtbs`：CTU 网格尺寸。
- `log2CtuSize`：CTU 边长 log2，`1 << log2CtuSize` 得到像素边长（典型 64 → 6）。
- `out`：逐 CTU 写入覆盖该 CTU 的 slice 的**独立 slice 序号**（`slice->getIndependentSliceIdx()`，图片内的 slice 编号）。没有任何 slice 覆盖的 CTU 写 0。

两次调用模式：

```c
int w, h, log2Ctu;
vvdec_internals_get_ctb_info_layout_dec( dec, frame, &w, &h, &log2Ctu );
uint16_t* sliceIdx = malloc( w * h * sizeof( uint16_t ) );
vvdec_internals_get_ctb_slice_idx_dec( dec, frame, sliceIdx );
```

### 5.4 CB（CU）：布局与位域

```c
int vvdec_internals_get_cb_info_layout_dec( vvdecDecoder *dec,
                                            const vvdecFrame *frame,
                                            int *widthInMinCUs,    /* 可空 */
                                            int *heightInMinCUs,   /* 可空 */
                                            int *log2MinCuSize );  /* 可空，恒为 2 */

int vvdec_internals_get_cb_info_dec( vvdecDecoder *dec,
                                     const vvdecFrame *frame,
                                     uint16_t *out );          /* 必须非空 */
```

每个 min-CU 格一个 `uint16_t`。只有 CU 左上角那一格带完整信息（bit 6 置 1），被同一个 CU 覆盖的其他格子写 0。遍历时必须先判断 bit 6。

位布局：

| 位 | 字段 | 含义 |
|---|---|---|
| 0-2 | `log2(cuWidth) - 2` | 0=4, 1=8, 2=16, 3=32, 4=64 |
| 3-5 | `log2(cuHeight) - 2` | 同上；VVC 允许非方 CU，宽高分别编码 |
| 6 | origin | 1 = 该格是 CU 的左上角 |
| 7-8 | predMode | 0=INTER, 1=INTRA, 2=IBC |
| 9-11 | qtDepth | 四叉树深度 |
| 12-14 | MT depth | `cu->depth - cu->qtDepth`，即 QT 叶子以下的 BT/TT 深度，**不含** QT 深度 |
| 15 | skip | 1 = skip 模式 |

解包：

```c
const int log2W    = ( v & 0x7 ) + 2;
const int log2H    = ( ( v >> 3 ) & 0x7 ) + 2;
const int origin   = ( v >> 6 ) & 1;
const int predMode = ( v >> 7 ) & 0x3;
const int qtDepth  = ( v >> 9 ) & 0x7;
const int mtDepth  = ( v >> 12 ) & 0x7;
const int skip     = ( v >> 15 ) & 1;
```

`predMode` 取值为 `PredMode` 枚举原值（`MODE_INTER=0`、`MODE_INTRA=1`、`MODE_IBC=2`）。QT 深度与 MT 深度各占 3 bit，写入时按 `& 0x7` 截断，最大只能表达 7。

### 5.5 PB（PU）：运动信息

```c
int vvdec_internals_get_pb_info_layout_dec( vvdecDecoder *dec,
                                            const vvdecFrame *frame,
                                            int *widthInMinCUs,    /* 可空 */
                                            int *heightInMinCUs,   /* 可空 */
                                            int *log2MinCuSize );  /* 可空 */

int vvdec_internals_get_pb_info_dec( vvdecDecoder *dec,
                                     const vvdecFrame *frame,
                                     int16_t *out_mv0_x,     /* 必须非空 */
                                     int16_t *out_mv0_y,     /* 必须非空 */
                                     int16_t *out_mv1_x,     /* 必须非空 */
                                     int16_t *out_mv1_y,     /* 必须非空 */
                                     int16_t *out_ref_idx0,  /* 必须非空 */
                                     int16_t *out_ref_idx1 );/* 必须非空 */
```

六个数组各 `widthInMinCUs * heightInMinCUs` 个 `int16_t`：

| 数组 | 含义 |
|---|---|
| `out_mv0_x` / `out_mv0_y` | L0 运动矢量，单位 1/16 亮度样点 |
| `out_mv1_x` / `out_mv1_y` | L1 运动矢量，单位 1/16 亮度样点 |
| `out_ref_idx0` | L0 参考索引；该列表未使用时为 -1 |
| `out_ref_idx1` | L1 参考索引；该列表未使用时为 -1 |

数据来自重建阶段写好的 **per-4x4 `MotionInfo` 运动场**，因此给出的是实际生效的 PB 运动，而不是 CU 级原始语法：

- 仿射：每个 4x4 位置按控制点插值后的 MV，不是只给 CP0。
- ATMVP / 子块时域预测：子块实际 MV。
- GEO：各子分区各自的 MV 与 refIdx。
- DMVR：细化后的 MV。

无运动的块（intra、IBC）`refIdx = -1`、`mv = 0`。因此 `ref_idx0 < 0 && ref_idx1 < 0` 是"该格没有可用运动信息"的可靠判据，不要用 `mv == 0` 判断。

**已知限制**：该运动场是解码器内部的 per-frame scratch，只有**最新重建的那一帧**保证有效；更早返回的帧可能读到过期或清零的运动数据。需要在多帧之间比对运动信息时，请在该帧返回后立即取走数据副本。

### 5.6 Intra 方向

```c
int vvdec_internals_get_intra_dir_info_layout_dec( vvdecDecoder *dec,
                                                   const vvdecFrame *frame,
                                                   int *widthInMinCUs,    /* 可空 */
                                                   int *heightInMinCUs,   /* 可空 */
                                                   int *log2MinCuSize );  /* 可空 */

int vvdec_internals_get_intra_dir_info_dec( vvdecDecoder *dec,
                                            const vvdecFrame *frame,
                                            uint8_t *out_intra_dir_y,  /* 必须非空 */
                                            uint8_t *out_intra_dir_c );/* 必须非空 */
```

两个数组各 `widthInMinCUs * heightInMinCUs` 个 `uint8_t`。

亮度 `out_intra_dir_y`：

| 值 | 含义 |
|---|---|
| 0 | PLANAR |
| 1 | DC |
| 2..66 | 角度模式（65 个） |
| 0..15 | **MIP CU**：写入原始 MIP 模式序号（实际个数取决于块尺寸：16 / 8 / 6 个） |

MIP 的序号与 PLANAR/DC/角度模式**数值混叠**：MIP 模式 2 和角度模式 2 在数值上无法区分，光看这一个字节无法判断该 CU 是否用 MIP。需要使用 MIP 信息的调用方需另行获取 MIP 标志，当前 API 不导出。

色度 `out_intra_dir_c`：

| 值 | 含义 |
|---|---|
| 0 | PLANAR |
| 1 | DC |
| 2..66 | 角度模式 |
| 67 | LM_CHROMA |
| 68 | MDLM_L |
| 69 | MDLM_T |
| 70 | DM_CHROMA |

dual tree 时读的是色度 CU 自己的模式；single tree 时亮度与色度共用同一个 CU，两者读到同一组值。无 CU 覆盖的格子写 0。

### 5.7 TU：变换深度

```c
int vvdec_internals_get_tu_info_layout_dec( vvdecDecoder *dec,
                                            const vvdecFrame *frame,
                                            int *widthInMinCUs,    /* 可空 */
                                            int *heightInMinCUs,   /* 可空 */
                                            int *log2MinCuSize );  /* 可空 */

int vvdec_internals_get_tu_info_dec( vvdecDecoder *dec,
                                     const vvdecFrame *frame,
                                     uint8_t *out );           /* 必须非空 */
```

`out` 为 `widthInMinCUs * heightInMinCUs` 个 `uint8_t`，每格直接存放该位置所属 TU 的相对变换深度（0 起，越大表示 TU 越小），没有位域打包。

深度由 TU 相对于所属 CU 的尺寸比值推导：

```
depth = max( log2(cuWidth) - log2(tuWidth), log2(cuHeight) - log2(tuHeight) )
```

`TransformUnit` 不保存树深度，所以只能这样推：

- BT/ISP 沿某一维分割时结果是正确的（另一维比值为 0）。
- **已知近似**：TT（三叉）分割两侧的 TU 会报深一层。例如 64x64 的 CU 沿垂直方向做水平三叉，分成 64x16 / 64x32 / 64x16 三个 TU，两侧 TU 的高度比值算出来是 2，而实际树深度是 1。
- 该格不属于任何 TU 时写 0。

### 5.8 GOP：帧级查询

```c
int vvdec_internals_get_gop_info_dec( vvdecDecoder *dec,
                                      const vvdecFrame *frame,
                                      int *poc,             /* 可空 */
                                      int *temporalLayer,   /* 可空 */
                                      int *sliceType,       /* 可空 */
                                      int *isRAP,           /* 可空 */
                                      int *numRefPocL0,     /* 可空 */
                                      int *numRefPocL1,     /* 可空 */
                                      int refPocL0[16],     /* 可空 */
                                      int refPocL1[16] );   /* 可空 */
```

所有信息取自已解码图片的第一条 slice：

| 出参 | 含义 |
|---|---|
| `poc` | 该图片的 POC |
| `temporalLayer` | 时间层 id |
| `sliceType` | 0=B, 1=P, 2=I |
| `isRAP` | 1 = CLVSS 图片（IDR / CRA / GDR），0 = 其他 |
| `numRefPocL0` / `numRefPocL1` | 该列表有效参考项个数，可能为 0 |
| `refPocL0` / `refPocL1` | 固定 16 项，未使用的项补 0 |

要点：

- `isRAP` 用的是 `isClvssPu()`，判据是 NAL unit type 落在 `IDR_W_RADL .. GDR` 区间且图片未混用 NAL 类型（默认构建 `GDR_ADJ=1` 时还要求 `no_output_before_recovery` 为真）。VVC 没有 BLA，所以 CLVSS 就是 IDR/CRA/GDR 三类；不要把 `isRAP` 再细分成 BLA。
- `refPocL0` / `refPocL1` 无论 `numRefPoc` 是多少都写满 16 项，因此调用方无需预清零，但也必须按 `numRefPoc` 只读前若干项——多余项是 0，而 0 是合法 POC。
- 图片没有任何 slice 时返回 `VVDEC_ERR_INITIALIZE`。

### 5.9 GOP：header-only 查询

```c
int vvdec_internals_get_gop_info_headeronly( vvdecDecoder *dec,
                                             int *poc,             /* 可空 */
                                             int *temporalLayer,   /* 可空 */
                                             int *sliceType,       /* 可空 */
                                             int *nalUnitType,     /* 可空 */
                                             int *isRAP,           /* 可空 */
                                             int *allSlicesIntra,  /* 可空 */
                                             int *numRefPocL0,     /* 可空 */
                                             int *numRefPocL1,     /* 可空 */
                                             int refPocL0[16],     /* 可空 */
                                             int refPocL1[16],     /* 可空 */
                                             int64_t *picSequence );/* 可空 */
```

不需要 `vvdecFrame`，直接读取解析器里"最近一张完成的图片"的状态。与帧级版本相比多了三个出参：

| 出参 | 含义 |
|---|---|
| `nalUnitType` | 第一条 slice 的 NAL unit type 原值，可据此精确区分 IDR_W_RADL / IDR_N_LP / CRA / GDR |
| `allSlicesIntra` | 1 = 图片内所有 slice 都是 I slice |
| `picSequence` | 该图片完成时的单调递增序号 |

`picSequence` 的用途：判断"是不是又完成了一张新图片"。解析器每完成一张图片就把计数器加一，序号只增不减，因此只需判断本次取到的值是否与上次不同。**不要用 POC 判断**：全 IDR 码流里每张图片的 POC 都是 0。

前置条件：

- 解码器必须处于 header-only 模式（已经调用过 `vvdec_decode_headeronly`）。普通 `vvdec_decode` 路径不会设置这张"最近完成图片"，因此该 getter 在普通模式下永远返回 `VVDEC_ERR_INITIALIZE`。
- 至少已经完成一张图片（即某张图的最后一条 slice 已解析）。否则返回 `VVDEC_ERR_INITIALIZE`。
- 图片因 slice 未覆盖全部 CTU 而解析失败时，它不会被当作"最近完成图片"，同样返回 `VVDEC_ERR_INITIALIZE`。

### 5.10 时间戳

```c
int64_t vvdec_internals_get_image_dts_dec( const vvdecFrame *frame );
```

函数名保留了 "dts"，但**实际返回的是 CTS（coding timestamp）**：`vvdecFrame` 只携带 `cts` 和 `ctsValid`，没有 DTS 字段。`ctsValid` 为 0 或 `frame` 为 `NULL` 时返回 `-1`。

## 6. header-only 模式

`vvdec_decode_headeronly` 位于公共头 `include/vvdec/vvdec.h`，与 internals 开关无关（`OFF` 时也存在），但它存在的目的就是给 GOP 扫描供数，实际使用总是与 `vvdec_internals_get_gop_info_headeronly` 搭配。

```c
VVDEC_DECL int vvdec_decode_headeronly( vvdecDecoder *, vvdecAccessUnit *accessUnit, vvdecFrame **frame );
```

### 6.1 行为

- 解析 slice header、构建参考图像列表、维护 DPB；跳过 CABAC slice body 解析与重建。
- 不产生 `vvdecFrame`，`*frame` 恒为 `NULL`。
- `accessUnit->payloadUsedSize == 0` 时立即返回 `VVDEC_OK` 且不做任何解析。这与 `vvdec_decode` 不同：后者对空 access unit 返回 `VVDEC_EOF`。用 header-only 扫码流时，EOF 由调用方自己判断（例如读到文件末尾），不要等 `VVDEC_EOF`。
- 输入校验与 `vvdec_decode` 一致：`payload` 为 `NULL`、`payloadSize <= 0`、`payloadUsedSize > payloadSize` 都返回 `VVDEC_ERR_DEC_INPUT`。

### 6.2 单向不可逆

进入 header-only 模式后**不能回到普通解码**：

- 模式开关是解码器上的单向状态，`vvdec_decode` 与 `vvdec_decode_headeronly` 不能在同一实例上先后混用。
- 一旦开始调用 `vvdec_decode_headeronly`，后续只能继续调用它，直到 `vvdec_decoder_close`。
- 该状态在 `vvdec_decoder_close` 内部被重置，因此关闭后重新 `vvdec_decoder_open` 可以正常走普通解码路径。

### 6.3 内存与图片生命周期

为了不让整段码流的图片都留在内存里，header-only 模式会主动回收：

- 每次 `decodeHeaderOnly` 之后，解析帧列表里除"最近一张完成的图片"之外的图片立即归还图片池。
- 因此内存占用与单帧的结构数据相当，可以扫很长的码流。
- `vvdec_internals_get_gop_info_headeronly` 指向的图片在**下一张图片完成**或 `vvdec_decoder_close` 之前有效。要跨图片比对，请在每次调用后立刻把需要的字段取走。
- 由于图片会立即回收，不要缓存 `picSequence` 之外的任何"当前图片"隐式状态；每轮都以 `picSequence` 变化作为触发条件重新查询。

### 6.4 典型用法

```c
int64_t lastSeq = -1;

for( ;; )
{
  /* 填 au */
  int ret = vvdec_decode_headeronly( dec, au, &frame );   /* frame 恒为 NULL */
  if( ret != VVDEC_OK )
  {
    break;
  }

  int64_t seq = 0;
  int     poc = 0, tlayer = 0, sliceType = 0, nalType = 0, isRAP = 0, allIntra = 0;
  int     numL0 = 0, numL1 = 0;
  int     refPocL0[16], refPocL1[16];

  if( vvdec_internals_get_gop_info_headeronly( dec, &poc, &tlayer, &sliceType, &nalType,
                                               &isRAP, &allIntra, &numL0, &numL1,
                                               refPocL0, refPocL1, &seq ) == VVDEC_OK
      && seq != lastSeq )
  {
    lastSeq = seq;   /* 只有序号变化才代表完成了一张新图片 */
    /* 记录 poc / nalType / isRAP / 参考 POC ... */
  }
}
```

## 7. 已知限制汇总

| 限制 | 说明 |
|---|---|
| 无版本号 | 只能靠符号是否存在判断支持；位布局变化无法在运行时察觉，需同步升级调用方 |
| 布局随版本硬编码 | CB 位域、intra 编码、TU 值域都是编译期约定，没有任何自我描述字段 |
| PB 运动场只对最新帧有效 | per-frame scratch，早于最新重建帧的查询可能读到过期数据 |
| MIP 与角度模式混叠 | 亮度 intra 值可能是 MIP 序号（0..15），无法与同数值的角度/PLANAR/DC 区分 |
| TT 的 TU 深度偏深一层 | 深度由尺寸比推导，三叉分割两侧 TU 会多报一级 |
| 内部图片生命周期依赖 unref | 查询必须在 `vvdec_frame_unref` 之前完成 |
| `get_image_dts_dec` 名不副实 | 返回 CTS，不是 DTS |
| header-only 不可逆 | 不能与 `vvdec_decode` 混用，且不产生帧 |

## 8. YUView 统计类型映射

`decoderVVDec::cacheStatistics` 目前的用法（statistics type id 与实际取值来源）：

| ID | 名称 | 来源 | 取值 / 处理 |
|---|---|---|---|
| 0 | Slice Index | `get_ctb_slice_idx_dec` | 逐 CTU 写 `widthInCtbs*heightInCtbs` 块，值即独立 slice 序号 |
| 1 | Pred Mode | `get_cb_info_dec` bit 7-8 | 只在 CU 左上角按 `cuW x cuH` 写一块；0=Inter, 1=Intra, 2=IBC |
| 2 | QT Depth | `get_cb_info_dec` bit 9-11 | 同上 |
| 3 | MT Depth | `get_cb_info_dec` bit 12-14 | 同上，不含 QT 深度 |
| 4 | Skip | `get_cb_info_dec` bit 15 | 同上，0/1 |
| 5 | Ref POC 0 | `get_pb_info_dec` 的 `ref_idx0` + `get_gop_info_dec` | 仅在 `0 <= refIdx0 < numRefPocL0` 时写 `refPocL0[refIdx0] - poc` |
| 6 | Ref POC 1 | `get_pb_info_dec` 的 `ref_idx1` + `get_gop_info_dec` | 仅在 `0 <= refIdx1 < numRefPocL1` 时写 `refPocL1[refIdx1] - poc` |
| 7 | Motion Vector 0 | `get_pb_info_dec` 的 L0 MV | 单位 1/16 亮度样点，按 CU 矩形写一块 |
| 8 | Motion Vector 1 | `get_pb_info_dec` 的 L1 MV | 同上 |
| 9 | Intra Dir Luma | `get_intra_dir_info_dec` 亮度 | 仅 `predMode == 1` 且 `dirY < 67` 时写；`dirY >= 2` 时另画方向向量 |
| 10 | Intra Dir Chroma | `get_intra_dir_info_dec` 色度 | 仅 `predMode == 1` 且 `dirC <= 70` 时写 |
| 11 | Transform Depth | `get_tu_info_dec` | 逐 min-CU 格写 `4x4` 块，相邻同值由绘制层合并 |

使用注意：

- 类型 1..10 都只在 CU 左上角（CB 表 bit 6）写块，块尺寸取 CB 表里的 `cuW x cuH`；类型 0 用 CTU 网格，类型 11 用 min-CU 网格。
- 类型 5/6 依赖 `get_gop_info_dec` 成功；GOP 信息取不到时该帧跳过参考 POC 统计，其余类型不受影响。
- 运动类统计（5..8）只在 `predMode != 1`（非 intra）时写；intra CU 的 `refIdx = -1`、`mv = 0` 不会被误当作有效运动。
- 类型 10 的有效值域是 0..70（含 67..70 的 LM/MDLM_L/MDLM_T/DM），画色标时不需要为 CCLM 预留 81..83。
