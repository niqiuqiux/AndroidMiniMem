#pragma once

#include <cstdint>
#include <string>

enum MemoryType {
    All = -1,
    Anonymous = 1 << 5,//32
    C_Alloc = 1 << 2, //4
    C_Heap = 1 << 0, //1
    C_Data = 1 << 3, //8
    C_Bss = 1 << 4, //16
    Java_Heap = 1 << 1, //2
    Java = 1 << 16, //65536
    Stack = 1 << 6, //64
    Video = 1 << 20, //1048576
    Code_App = 1 << 14, //16384
    Code_System = 1 << 15, //32768
    Ashmem = 1 << 19, //524288
    Bad = 1 << 17, //131072
    Other = -2080896 //-2080896
};

enum SCAN_TYPE {
  UNKNOW_VAL = 0,   // 模糊 未知数 （只有第一次搜索才能用
  ACCURATE_VAL,     // 精确数值
  LARGER_THAN_VAL,  // 值大于
  LESS_THAN_VAL,    // 值小于
  BETWEEN_VAL,      // 值在两值之间
  ADD_UNKNOW_VAL,   // 值增加了未知值
  ADD_ACCURATE_VAL, // 值增加了精确值
  SUB_UNKNOW_VAL,   // 值减少了未知值
  SUB_ACCURATE_VAL, // 值减少了精确值
  CHANGED_VAL,      // 变动了的数值
  UNCHANGED_VAL,    // 未变动的数值
};

enum SCAN_1_TYPE {
    _GROUP_VALUE = 1 << 31,
  _UNKNOW_VAL = 1 << 30,        //模糊 未知数 
  _ACCURATE_VAL = 1 << 29,    // 精确数值
  _LARGER_THAN_VAL = 1 << 28, // 值大于
  _LESS_THAN_VAL = 1 << 27,   // 值小于
  _BETWEEN_VAL = 1 << 26,     // 值在两值之间 模糊还是二次都要处理
  _ADD_UNKNOW_VAL = 1 << 25,   // 值增加了未知值
  _ADD_ACCURATE_VAL = 1 << 24, // 值增加了精确值
  _SUB_UNKNOW_VAL = 1 << 23,   // 值减少了未知值
  _SUB_ACCURATE_VAL = 1 << 22, // 值减少了精确值
  _CHANGED_VAL = 1 << 21,      // 变动了的数值
  _UNCHANGED_VAL = 1 << 20,    // 未变动的数值
};

enum TYPE{
    BYTE_=1,//1
    WORD_=1<<1,//2
    DWORD_=1<<2,//4
    XOR_=1<<3,//8
    FLOAT_=1<<4,//16
    QWORD_=1<<5,//32
    DOUBLE_=1<<6,//64
};

struct ScanResultItem {
    uint64_t address;
    uint64_t value;
    std::string valueStr;
    uint64_t previousValue = 0;  // 用于检测变化
    bool valueChanged = false;    // 值是否改变
    float changeTime = 0.0f;      // 变化时间（用于高亮淡出）
};

struct AddressListItem {
    bool active = false;  // 默认未选中，用户需要手动勾选才能删除
    std::string description;
    uint64_t address;
    int valueType;
    std::string currentValue;
    char editValueBuffer[64] = "";
    bool valueEditActive = false;
};
