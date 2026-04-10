# Apache Doris ARRAY 类型实现机制

> 基于早期版本，Pre-Nereids，无 Pipeline 引擎

## 一、总体架构

ARRAY 是 Doris 的复杂类型之一（与 MAP、STRUCT 并列），默认关闭，需设置 `enable_complex_type_support = true` 启用。实现涉及 FE 类型系统、BE 运行时存储、Segment 编解码、UDF 函数四层。

```
  SQL 层                    FE 类型系统                 Thrift 传输              BE 存储层
  =========                 ==========                 ==========              =========

  ARRAY<INT>               ArrayType                   TTypeDesc               Segment V2
       |                       |                         |                       |
       v                       v                         v                       v
  TypeDef.analyze()     itemType=ScalarType(INT)   [ARRAY, SCALAR(INT)]    3 个子列:
  Column("item", INT)   MAX_NESTING_DEPTH=2        (扁平化 TTypeNode)       [0] 元素列 (INT)
  (子列表示法)                                                             [1] 偏移列 (uint32, bitshuffle)
                                                                          [2] NULL 列 (可选, uint8)
                                                                               |
                                                                               v
  CollectionValue { void* data, uint32 length, bool has_null, bool* null_signs }
```

## 二、端到端数据流

```
  +-- 创建表 (FE) -------------------------------------------------------------+
  |                                                                            |
  |  CREATE TABLE t (a ARRAY<INT>)                                            |
  |       |                                                                    |
  |       +-- TypeDef.analyze()                                                |
  |       |     +-- enable_complex_type_support == true ? (否则报错)           |
  |       |     +-- 检查嵌套深度 <= MAX_NESTING_DEPTH (2)                      |
  |       |     +-- 创建 ArrayType(ScalarType(INT))                            |
  |       |                                                                    |
  |       +-- Column.createChildrenColumn()                                    |
  |       |     +-- 创建子列 Column("item", ScalarType(INT))                   |
  |       |     +-- 存储到 Column.children 列表                                |
  |       |                                                                    |
  |       +-- EditLog.logCreateTable() (持久化)                                |
  |             +-- ColumnType.write(): 递归序列化 ARRAY + INT                 |
  |             +-- GSON: {"itemType": {...}}                                  |
  +----------------------------------------------------------------------------+


  +-- 数据写入 (BE) -----------------------------------------------------------+
  |                                                                            |
  |  INSERT INTO t VALUES ([1, 2, 3])                                          |
  |       |                                                                    |
  |       +-- FE: ArrayLiteral([1, 2, 3]) -> Thrift TExpr                     |
  |       |     TExprNodeType = ARRAY_LITERAL                                  |
  |       |                                                                    |
  |       +-- BE: ArrayColumnWriter.append_data()                              |
  |             |                                                              |
  |             +-- _length_writer: 写入 offset (uint32)                       |
  |             |     array[0].length = 3                                      |
  |             |                                                              |
  |             +-- _item_writer: 批量写入元素数据                              |
  |             |     1, 2, 3                                                  |
  |             |                                                              |
  |             +-- _null_writer: 写入 NULL 标志 (可选)                        |
  |             |                                                              |
  |             +-- FlushPageCallback: 记录 first_ordinal 到 Page Footer       |
  +----------------------------------------------------------------------------+


  +-- 数据读取 (BE) -----------------------------------------------------------+
  |                                                                            |
  |  SELECT a FROM t                                                          |
  |       |                                                                    |
  |       +-- ArrayFileColumnIterator.next_batch()                             |
  |             |                                                              |
  |             +-- _length_iterator: 读取 n+1 个 offset 值                   |
  |             |     get_offset_by_length() 长度->累积偏移                    |
  |             |     offsets = [0, 3]                                          |
  |             |                                                              |
  |             +-- _null_iterator: 读取 NULL 标志                             |
  |             |                                                              |
  |             +-- _item_iterator: 读取 offsets[n]-offsets[0] 个元素          |
  |             |     读取 3 个 INT: 1, 2, 3                                   |
  |             |                                                              |
  |             +-- prepare_for_read(): 重建 CollectionValue 指针              |
  |             |     CollectionValue { data->1, length=3, has_null=false }    |
  |             |                                                              |
  |             +-- 返回 ArrayColumnVectorBatch                                |
  +----------------------------------------------------------------------------+


  +-- 函数执行 (BE) -----------------------------------------------------------+
  |                                                                            |
  |  SELECT array(4, 5, 6)                                                    |
  |       |                                                                    |
  |       +-- ArrayFunctions::array(context, 3, IntVal*)                       |
  |             |                                                              |
  |             +-- CollectionValue::init_collection(pool, 3, TYPE_INT, &v)    |
  |             |     分配 3*sizeof(int) 字节 + 3 个 bool (null bitmap)        |
  |             |                                                              |
  |             +-- v.set(0, TYPE_INT, &val_4)                                 |
  |             +-- v.set(1, TYPE_INT, &val_5)                                 |
  |             +-- v.set(2, TYPE_INT, &val_6)                                 |
  |             |                                                              |
  |             +-- v.to_collection_val(&ret)                                  |
  |             |     CollectionVal { data, length=3, has_null, null_signs }   |
  |             |                                                              |
  |             +-- return ret                                                 |
  +----------------------------------------------------------------------------+
```

## 三、FE 类型系统

### 3.1 ArrayType 定义

```
  Type (abstract)
    |
    +-- ScalarType (INT, VARCHAR, DOUBLE, ...)
    +-- ArrayType (复杂类型)
    +-- MapType (复杂类型)
    +-- StructType (复杂类型)


  ArrayType extends Type
  |
  +-- itemType: Type                    // 元素类型 (递归)
  |     ARRAY<INT>          -> ScalarType(INT)
  |     ARRAY<ARRAY<INT>>  -> ArrayType(ScalarType(INT))  [但被嵌套深度限制]
  |
  +-- @SerializedName("itemType")       // GSON 序列化
  |
  +-- slotSize = 24 bytes               // PrimitiveType.ARRAY 固定 slot 大小
  |
  +-- toThrift():
        TTypeDesc = [
          TTypeNode(type=ARRAY),
          itemType.toThrift()   // 递归, 添加元素类型节点
        ]
```

### 3.2 类型约束

| 约束 | 值 | 说明 |
|------|-----|------|
| `enable_complex_type_support` | `false` (默认关闭) | 功能开关 |
| `MAX_NESTING_DEPTH` | 2 | 最大嵌套深度 (ARRAY<INT> = 深度 2) |
| 支持的元素类型 | 所有标量类型 | FE 理论支持, 实际受限于 BE |
| 隐式类型转换 | 无 | ARRAY 不参与任何隐式转换 |
| 默认值 | 仅 NULL | ARRAY 列不允许非 NULL 默认值 |
| 是否可做 Key 列 | 否 | ARRAY 列不能作为排序键 |
| 索引支持 | 无 | 不支持 BloomFilter / BitmapIndex / ZoneMap |

### 3.3 子列表示法 (Catalog 层)

```
  Column("a", ArrayType(INT))          // 父列
    |
    +-- children:
          +-- Column("item", ScalarType(INT))   // 子列, 固定名称 "item"

  序列化 (ColumnType.write):
    out: "ARRAY" + ColumnType.write(ScalarType(INT))
    out: "INT"

  反序列化 (ColumnType.read):
    读取 "ARRAY" -> 创建 ArrayType
    递归读取 itemType -> ScalarType(INT)
    创建子列 Column("item", INT)
```

## 四、BE 运行时表示

### 4.1 CollectionValue (核心数据结构)

```
  CollectionValue (collection_value.h)
  |
  +-- void*    _data        // 指向连续元素数据缓冲区
  +-- uint32_t _length      // 元素个数
  +-- bool     _has_null    // 是否包含 NULL 元素
  +-- bool*    _null_signs  // NULL 位图 (每元素一个 bool)

  内存布局示例: ARRAY<INT> = [10, NULL, 30]
  +-- _data:       [10][  ][30]    (连续 int 数组, NULL 位空)
  +-- _length:     3
  +-- _has_null:   true
  +-- _null_signs: [false, true, false]

  支持的元素类型 (type_check):
    TYPE_INT, TYPE_CHAR, TYPE_VARCHAR, TYPE_NULL
```

### 4.2 CollectionVal (UDF 层)

```
  CollectionVal extends AnyVal (udf.h)
  |
  +-- void*    data         // 与 CollectionValue._data 对应
  +-- uint32_t length       // 与 CollectionValue._length 对应
  +-- bool     has_null     // 与 CollectionValue._has_null 对应
  +-- bool*    null_signs   // 与 CollectionValue._null_signs 对应

  转换关系:
    CollectionValue -> CollectionVal:  to_collection_val()
    CollectionVal   -> CollectionValue: from_collection_val()
```

### 4.3 ArrayColumnVectorBatch (列式存储)

```
  ArrayColumnVectorBatch (column_vector.h)
  |
  +-- _data: DataBuffer<CollectionValue>     // 每行一个 CollectionValue 指针
  +-- _elements: ColumnVectorBatch           // 所有行的元素数据 (连续)
  +-- _offsets: ScalarColumnVectorBatch<uint32_t>  // 每行数组的偏移量

  示例: 3 行 ARRAY<INT>
  Row 0: [1, 2]
  Row 1: [3]
  Row 2: [4, 5, 6]

  _data:    [CV0, CV1, CV2]    (CV = CollectionValue 指针)
  _offsets: [0, 2, 3, 6]      (累积偏移, 长度数组 = [2,1,3] -> 前缀和)
  _elements:[1, 2, 3, 4, 5, 6] (所有元素连续存储)

  查找 Row i 的数据:
    start = _offsets[i]
    end   = _offsets[i+1]
    elements = _elements[start..end)
    CollectionValue._data = &_elements[start]
    CollectionValue._length = end - start
```

### 4.4 ArrayTypeInfo (类型元信息)

```
  ArrayTypeInfo extends TypeInfo (types.h)
  |
  +-- item_type_info: shared_ptr<TypeInfo>   // 元素类型信息
  |
  +-- equal(left, right)                     // 逐元素比较, 处理 NULL
  +-- cmp(left, right)                       // 字典序比较, NULL 排最前
  +-- hash_code(data, seed)                  // 逐元素哈希

  ArrayTypeInfoResolver (单例):
    缓存已创建的 ArrayTypeInfo, 按 FieldType(item_type) 索引
    支持: TINYINT, SMALLINT, INT, BIGINT, FLOAT, DOUBLE,
          DECIMAL, DATE, DATETIME, CHAR, VARCHAR, STRING, BOOL
```

## 五、Segment V2 存储编码

### 5.1 存储布局

```
  Segment 文件中 ARRAY 列的物理布局:
  ============================================

  Column: a (ARRAY<INT>)
    |
    +-- children_columns[0]: Item Column (INT)
    |     +-- Page 0: [1, 2, 3, 4, 5, 6]    (元素数据, 按 INT 编码)
    |     +-- Page 1: [7, 8, 9, 10]          (继续)
    |     +-- 编码: 取决于元素类型 (INT 可用 BIT_SHUFFLE 等)
    |
    +-- children_columns[1]: Offset Column (uint32)
    |     +-- Page 0: [0, 3, 4, 7, 11]       (累积偏移)
    |     +-- 编码: BIT_SHUFFLE (作为 UNSIGNED_BIGINT)
    |     +-- Page Footer: first_array_item_ordinal
    |
    +-- children_columns[2]: Null Column (可选, uint8)
          +-- Page 0: [0, 0, 1, 0]            (0=非 NULL, 1=NULL)
          +-- 编码: 标量编码


  数据示例:
  +------+-------------------+-------+
  | Row  | a (ARRAY<INT>)    | Null  |
  +------+-------------------+-------+
  |   0  | [1, 2, 3]         | false |
  |   1  | [4, 5, 6, 7]     | false |
  |   2  | NULL              | true  |
  |   3  | [8, 9]            | false |
  +------+-------------------+-------+

  Offset 列: [0, 3, 7, 7, 9]
                  ^  ^  ^  ^
                  |  |  |  +-- Row3 的 end (Row2 为 NULL, 长度=0)
                  |  |  +---- Row2 的 end (NULL, 偏移不变)
                  |  +------- Row1 的 end
                  +---------- Row0 的 end

  Item 列:   [1, 2, 3, 4, 5, 6, 7, 8, 9]
                |-----|  |--------|       |--|
                Row0      Row1             Row3
```

### 5.2 ArrayColumnWriter 写入流程

```
  ArrayColumnWriter (column_writer.cpp)
  |
  +-- _item_writer: ColumnWriter         // 元素数据写入器 (递归, 支持嵌套)
  +-- _length_writer: ScalarColumnWriter // 偏移量写入器 (uint32)
  +-- _null_writer: ScalarColumnWriter   // NULL 标志写入器 (可选)
  +-- 实现 FlushPageCallback             // Page 刷新回调

  append_data(values, count):
       |
       +-- for each row i:
       |     |
       |     +-- CollectionValue cv = values[i]
       |     |
       |     +-- _length_writer->append(&cv._length)   // 写入数组长度
       |     |
       |     +-- _item_writer->append_data(cv._data, cv._length)
       |     |     // 批量写入 cv._length 个元素到元素列
       |     |
       |     +-- [如果 _length page 满了]
       |           finish_current_page()
       |           page_footer.first_array_item_ordinal = 当前位置
       |
       +-- [如果可 NULL]
             _null_writer->append(&is_null)

  FlushPageCallback (Page 刷新时回调):
       +-- 记录 first_array_item_ordinal 到 Page Footer
       +-- 用于 Reader 定位元素数据起始位置
```

### 5.3 ArrayFileColumnIterator 读取流程

```
  ArrayFileColumnIterator (column_reader.cpp)
  |
  +-- _length_iterator: ColumnIterator    // 偏移量读取器
  +-- _item_iterator: ColumnIterator      // 元素数据读取器
  +-- _null_iterator: ColumnIterator      // NULL 标志读取器

  next_batch(batch):
       |
       +-- 1. 读取偏移
       |     _length_iterator->next_batch(lengths, n)
       |     _length_iterator->peek_next_batch(&next_len)
       |     // 多读一个偏移值以获取最后一个数组的结束位置
       |
       +-- 2. 长度 -> 累积偏移
       |     get_offset_by_length(start_idx, n)
       |     [0, 3, 4, 2] -> [0, 3, 7, 9]
       |
       +-- 3. 读取 NULL 标志
       |     _null_iterator->next_batch(nulls, n)
       |
       +-- 4. 计算元素总数
       |     total_items = offsets[n] - offsets[0]
       |
       +-- 5. 读取元素数据
       |     _item_iterator->next_batch(elements, total_items)
       |
       +-- 6. 重建 CollectionValue 指针
       |     prepare_for_read(start_idx, n, item_has_null)
       |     for each row i:
       |       cv._data = &elements[offsets[i]]
       |       cv._length = offsets[i+1] - offsets[i]
       |       cv._null_signs = &null_signs[...]
       |
       +-- 7. 返回填充好的 batch
```

### 5.4 编码策略

| 子列 | 逻辑类型 | 物理编码 | Page 编码 |
|------|---------|---------|----------|
| Item Column | 取决于元素类型 | 取决于元素类型 | 取决于元素类型 |
| Offset Column | uint32 | BIT_SHUFFLE (作为 UNSIGNED_BIGINT) | BIT_SHUFFLE |
| Null Column | bool/uint8 | 标量编码 | 标量编码 |

## 六、Thrift 序列化

### 6.1 类型描述 (TTypeDesc)

ARRAY 类型使用扁平化的 TTypeNode 列表表示嵌套结构：

```
  ARRAY<INT>:
    TTypeDesc.types = [
      TTypeNode(type=ARRAY),
      TTypeNode(type=SCALAR, scalar_type=INT)
    ]

  ARRAY<VARCHAR(100)>:
    TTypeDesc.types = [
      TTypeNode(type=ARRAY),
      TTypeNode(type=SCALAR, scalar_type=VARCHAR, len=100)
    ]

  ARRAY<ARRAY<INT>> (超出嵌套限制, 实际不允许):
    TTypeDesc.types = [
      TTypeNode(type=ARRAY),
      TTypeNode(type=ARRAY),
      TTypeNode(type=SCALAR, scalar_type=INT)
    ]
```

### 6.2 表达式序列化

```
  ArrayLiteral 表达式:
    TExpr.node_type = ARRAY_LITERAL
    TExpr.child_type = ARRAY (可选)
    TExpr.nodes = [
      TExprNode(type=ARRAY_LITERAL, num_children=3),
      TExprNode(type=INT_LITERAL, int_literal=1),
      TExprNode(type=INT_LITERAL, int_literal=2),
      TExprNode(type=INT_LITERAL, int_literal=3)
    ]
```

## 七、数组函数

### 7.1 函数注册

| 函数 | 签名 | BE 实现 | 说明 |
|------|------|--------|------|
| `array` | `ARRAY array(INT, ...)` | `ArrayFunctions::array(IntVal*)` | 构造整数数组 |
| `array` | `ARRAY array(VARCHAR, ...)` | `ArrayFunctions::array(StringVal*)` | 构造字符串数组 |
| `array` | `ARRAY array(ARRAY, ...)` | 无 (桩注册) | 嵌套数组 (未实现) |
| `[]` | `VARCHAR %element_extract%(ARRAY<INT>, INT)` | 无实现 | 下标访问 (桩注册) |
| `[]` | `VARCHAR %element_extract%(ARRAY<VARCHAR>, INT)` | 无实现 | 下标访问 (桩注册) |

### 7.2 array() 函数实现

```
  array(4, 5, 6) 执行流程:
  |
  +-- [FE 常量折叠]
  |     FEFunctions.array(4, 5, 6) -> ArrayLiteral([4, 5, 6])
  |     (如果所有参数都是常量, 在 FE 侧直接折叠)
  |
  +-- [BE UDF 执行]
        ArrayFunctions::array(context, num_children=3, IntVal* values)
             |
             +-- CollectionValue::init_collection(pool, 3, TYPE_INT, &v)
             |     分配 3 * sizeof(int) 字节 -> _data
             |     分配 3 * sizeof(bool) 字节 -> _null_signs
             |     v._length = 3
             |     v._has_null = false
             |
             +-- v.set(0, TYPE_INT, &values[0])  // 4
             +-- v.set(1, TYPE_INT, &values[1])  // 5
             +-- v.set(2, TYPE_INT, &values[2])  // 6
             |
             +-- v.to_collection_val(&ret)
             |     ret.data = v._data
             |     ret.length = 3
             |     ret.has_null = false
             |     ret.null_signs = v._null_signs
             |
             +-- return ret
```

## 八、配置与限制

### 8.1 关键配置

| 配置 | 默认值 | 说明 |
|------|--------|------|
| `enable_complex_type_support` | `false` | ARRAY/MAP/STRUCT 功能开关, 默认关闭 |
| `MAX_NESTING_DEPTH` | 2 | 最大嵌套深度 (ARRAY 本身占 1) |

### 8.2 功能限制

| 维度 | 支持情况 |
|------|---------|
| 元素类型 | INT, VARCHAR (完整实现); CHAR, DATE, DATETIME 等 (存储支持, 运行时部分) |
| 嵌套数组 | 存储支持, 但超出 MAX_NESTING_DEPTH 限制 |
| NULL 元素 | 支持 (has_null + null_signs 位图) |
| 数组整体 NULL | 支持 (独立 NULL 列) |
| 默认值 | 仅 NULL |
| Key 列 | 不允许 |
| BloomFilter | 不支持 |
| BitmapIndex | 不支持 |
| ZoneMap | 不支持 |
| 向量化引擎 | 此版本未实现 (get_data_type_ptr 无 ARRAY case) |
| 批量写入 | 逐行写入 (TODO: bulk write) |
| 比较排序 | 支持 (字典序, NULL 排最前) |
| 哈希 | 支持 (逐元素哈希) |

## 九、关键源文件

### FE 侧

| 文件 | 说明 |
|------|------|
| `fe/.../catalog/ArrayType.java` | ARRAY 类型定义 (itemType, toThrift, 序列化) |
| `fe/.../catalog/PrimitiveType.java` | 原始类型枚举 (ARRAY = 24) |
| `fe/.../catalog/Type.java` | 类型基类 (isArrayType, MAX_NESTING_DEPTH) |
| `fe/.../catalog/Column.java` | 列定义 (createChildrenColumn 子列表示) |
| `fe/.../catalog/ColumnType.java` | 元数据序列化 (递归 write/read) |
| `fe/.../analysis/TypeDef.java` | SQL 类型解析 (ARRAY<INT> 语法) |
| `fe/.../analysis/ArrayLiteral.java` | 数组字面量表达式 |
| `fe/.../analysis/Expr.java` | 表达式序列化 (ARRAY_LITERAL code) |
| `fe/.../rewrite/FEFunctions.java` | FE 常量折叠 (array 函数) |
| `gensrc/script/doris_builtins_functions.py` | 函数注册 (array, %element_extract%) |

### BE 侧

| 文件 | 说明 |
|------|------|
| `be/src/runtime/collection_value.h` | CollectionValue 核心结构 |
| `be/src/runtime/collection_value.cpp` | 初始化/设置/迭代/类型转换 |
| `be/src/udf/udf.h` | CollectionVal (UDF 层) |
| `be/src/runtime/primitive_type.h` | TYPE_ARRAY 枚举 (=17) |
| `be/src/runtime/types.h` | TypeDescriptor (children, from_thrift) |
| `be/src/vec/core/types.h` | 向量化 TypeIndex::Array |
| `be/src/olap/column_vector.h` | ArrayColumnVectorBatch (列式批量) |
| `be/src/olap/types.h` | ArrayTypeInfo (equal, cmp, hash_code) |
| `be/src/olap/types.cpp` | ArrayTypeInfoResolver |
| `be/src/olap/rowset/segment_v2/column_writer.cpp` | ArrayColumnWriter (3 子列写入) |
| `be/src/olap/rowset/segment_v2/column_reader.cpp` | ArrayFileColumnIterator (3 子列读取) |
| `be/src/exprs/array_functions.cpp` | array() UDF 实现 |
| `be/src/exprs/anyval_util.h` | AnyVal 转换工具 |

### Thrift/Proto 定义

| 文件 | 说明 |
|------|------|
| `gensrc/thrift/Types.thrift` | TPrimitiveType.ARRAY, TTypeNodeType.ARRAY |
| `gensrc/thrift/Exprs.thrift` | TExprNodeType.ARRAY_LITERAL |
| `gensrc/proto/types.proto` | PTypeNode (type=ARRAY) |
