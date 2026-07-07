#pragma once

#include "aisf_platform/common_define.h"

namespace AisfPlat::StringUtils {
const int RADIX_DECIMAL = 10;
const int INT_MAX_LEN = 11;
const int UINT_MAX_LEN = 10;

// 从一个字符串中，去开始字符串到结束字符串 之间的数据，不包括开始和结束字符串
// 例如源数据：aaabbbcccdddeeefff，开始bbb，结束是fff，结果是cccdddeee
bool GetSubString(const char* data, const char* start, const char* end, string& result);

// 剔除右侧空白，包括' ' (0x20) space (SPC); '\t'(0x09) horizontal tab (TAB); '\n'0x0a) newline (LF);
// '\v'(0x0b) vertical tab (VT); '\f'(0x0c) feed (FF); '\r'(0x0d) carriage return (CR)
string TrimRight(const string& s);

// 剔除左侧空白，空白内容如上
string TrimLeft(const string& s);

// 剔除 左侧和右侧 空白，TrimRight和TrimLeft  空白内容如上
string Strip(const string& s);

// 剔除右侧字符，例如aaabbbccc，剔除bbbccc，结果是aaa
string TrimRightString(const string& s, const string& trimStr);

// 剔除左侧字符
string TrimLeftString(const string& s, const string& trimStr);

// 按一个子串拆分子串
vector<string> Split(const string& s, const string& delimStr);

// 按一个字符拆分子串
vector<string> Split(const string& s, char delimChar);

// 替换子串
string Replace(const string& str, const string& oldStr, const string& newStr);

// 替换字符
string Replace(const string& s, char oldChar, char newChar);

// 组合一个vector到字符串
string Join(const vector<string>& elems, const string& delimiter);

// 路径的粘合
string JoinPath(const string& base, const string& child);

// 整字符串变化为全小写
string Lower(const string& s);

// 整字符串变化为全小大写
string Upper(const string& s);

// 是否含字符，允许不区分大小写比较
bool Contains(const string& s, const string& sub, bool ignoreCase = false);

// 是否以某字符串结尾，允许不区分大小写比较
bool EndsWith(const string& s, const string& tail, bool ignoreCase = false);

// 是否以某字符串开始，允许不区分大小写比较
bool StartsWith(const string& s, const string& head, bool ignoreCase = false);

// 统计子串的个数，允许不区分大小写
int CountSubStr(const string& s, const string& sub, bool ignoreCase = false);

// 统计子字符的个数，允许不区分大小写
int CountSubChar(const string& s, char c, bool ignoreCase = false);

// 格式化一个字符串，指定buffer的大小
string Format(size_t bufSize, const char* format, ...);

// 格式化一个字符串，指定一个buffer
int Format(char* buffer, size_t bufSize, const char* format, ...);

// 强校验将一个字符串转换为整数
bool SafeAtoi(const char* data, int& result, int radix = RADIX_DECIMAL);

// 强校验将一个字符串转换为无符号整数
bool SafeAtoU(const char* data, unsigned& result, int radix = RADIX_DECIMAL);

// 从一个字符串中，删除从子串到结尾的子串，例如aaabbbcccdddfff，删除第一个bbb到第一个ddd的子串，结果就是aaafff
bool RemoveSubString(char* data, int dataLen, const char* start, const char* end);

}

