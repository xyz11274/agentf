#include "aisf_platform/utils/utils.h"
#include <climits>
#include <bits/stl_algo.h>

namespace AisfPlat::StringUtils {
using namespace std;

string Replace(const string& str, const string& oldStr, const string& newStr)
{
    if (oldStr.empty()) {
        return str;
    }

    size_t pos = 0;
    string subject = str;
    while ((pos = subject.find(oldStr, pos)) != string::npos) {
        subject.replace(pos, oldStr.length(), newStr);
        pos += newStr.length();
    }

    return subject;
}

string Replace(const string& s, char oldChar, char newChar)
{
    string rst = s;
    std::replace(rst.begin(), rst.end(), oldChar, newChar);
    return rst;
}

vector<string> Split(const string& s, const string& delimStr)
{
    vector<string> elems;
    if (delimStr.empty()) {
        elems.push_back(s);
        return elems;
    }

    string::size_type startPos = 0;
    auto endPos = s.find(delimStr, startPos);

    while (true) {
        elems.push_back(s.substr(startPos, endPos - startPos));
        if (endPos == string::npos) {
            break;
        }

        startPos = endPos + delimStr.size();
        endPos = s.find(delimStr, startPos);
    }

    return elems;
}

vector<string> Split(const string& s, char delimChar)
{
    vector<string> elems;

    stringstream ss(s);
    string item;
    while (std::getline(ss, item, delimChar)) {
        elems.push_back(item);
    }

    if (s.empty()) {
        elems.emplace_back("");
    } else {
        if (*s.rbegin() == delimChar) {
            elems.emplace_back("");
        }
    }

    return elems;
}

string Join(const vector<string>& elems, const string& delimiter)
{
    string ret;
    for (const auto& e : elems) {
        ret += e + delimiter;
    }

    if (!ret.empty()) {
        return ret.substr(0, ret.size() - delimiter.size());
    }

    return ret;
}

string JoinPath(const string& base, const string& child)
{
    string rst = base;
    string c = child;

    if (rst.find('\\') != string::npos) {
        std::replace(rst.begin(), rst.end(), '\\', '/');
    }
    if (c.find('\\') != string::npos) {
        std::replace(c.begin(), c.end(), '\\', '/');
    }

    if (*rst.rbegin() == '/') {
        rst.pop_back();
    }
    if (*c.begin() == '/') {
        return rst + c;
    }

    return rst + "/" + c;
}

string Strip(const string& s)
{
    string rst = s;
    rst = TrimLeft(rst);
    return TrimRight(rst);
}

string TrimLeft(const string& s)
{
    string rst = s;
    rst.erase(rst.begin(), std::find_if(rst.begin(), rst.end(), [](int ch) { return !std::isspace(ch); }));
    return rst;
}

string TrimRight(const string& s)
{
    string rst = s;
    rst.erase(std::find_if(rst.rbegin(), rst.rend(), [](int ch) { return !std::isspace(ch); }).base(), rst.end());
    return rst;
}

string TrimRightString(const string& s, const string& trimStr)
{
    if (EndsWith(s, trimStr)) {
        return s.substr(0, s.size() - trimStr.size());
    }

    return s;
}

string TrimLeftString(const string& s, const string& trimStr)
{
    if (StartsWith(s, trimStr)) {
        return s.substr(trimStr.size());
    }

    return s;
}

bool StartsWith(const string& s, const string& head, bool ignoreCase)
{
    if (ignoreCase) {
        return (Lower(s).find(Lower(head)) == 0);
    }

    return (s.find(head) == 0);
}

bool EndsWith(const string& s, const string& tail, bool ignoreCase)
{
    auto pos = s.rfind(tail);
    if (ignoreCase) {
        pos = Lower(s).rfind(Lower(tail));
    }

    if (pos == string::npos) {
        return false;
    }

    return (s.size() == pos + tail.size());
}

bool Contains(const string& s, const string& sub, bool ignoreCase)
{
    if (ignoreCase) {
        return (Upper(s).find(Upper(sub)) != string::npos);
    }

    return (s.find(sub) != string::npos);
}

int CountSubStr(const string& s, const string& sub, bool ignoreCase)
{
    CHECK_TRUE_RETURN(s.empty() || sub.empty(), 0);

    int count = 0;

    if (ignoreCase) {
        auto lowS = Lower(s);
        auto lowSub = Lower(sub);
        for (auto offset = lowS.find(lowSub); offset != string::npos;
             offset = lowS.find(lowSub, offset + lowSub.size())) {
            ++count;
        }
    } else {
        for (auto offset = s.find(sub); offset != string::npos; offset = s.find(sub, offset + sub.size())) {
            ++count;
        }
    }

    return count;
}

int CountSubChar(const string& s, char c, bool ignoreCase)
{
    if (ignoreCase) {
        auto lowS = Upper(s);
        auto lowC = std::toupper(c);
        return std::count(lowS.begin(), lowS.end(), lowC);
    } else {
        return std::count(s.begin(), s.end(), c);
    }
}

string Format(size_t bufSize, const char* format, ...)
{
    CHECK_TRUE_RETURN(format == nullptr || bufSize == 0, "");

    SafeString buffer(__FUNCTION__, __LINE__, bufSize);
    if (buffer.Empty()) {
        return "";
    }

    va_list pArgs;
    va_start(pArgs, format);
    if (vsnprintf_s(buffer.Get(), bufSize, bufSize, format, pArgs) == -1) {
        LOG_ERR("Format vsnprintf_s error");
    }
    va_end(pArgs);

    return buffer.Data();
}

int Format(char* buffer, size_t bufSize, const char* format, ...)
{
    if (buffer == nullptr || format == nullptr || bufSize == 0) {
        return -1;
    }

    va_list pArgs;
    va_start(pArgs, format);
    auto length = vsnprintf_s(buffer, bufSize, bufSize - 1, format, pArgs);
    va_end(pArgs);

    return length;
}

string Lower(const string& s)
{
    auto ret = s;
    std::transform(s.begin(), s.end(), ret.begin(), [](unsigned char c) { return std::tolower(c); });
    return ret;
}

string Upper(const string& s)
{
    auto ret = s;
    std::transform(s.begin(), s.end(), ret.begin(), [](unsigned char c) { return std::toupper(c); });
    return ret;
}

bool SafeAtoi(const char* data, int& result, int radix)
{
    CHECK_NULL_RETURN(data, false);
    CHECK_TRUE_RETURN(strnlen(data, INT_MAX_LEN) == 0, false);

    char* stop = nullptr;
    errno = 0;
    auto i = strtol(data, &stop, radix);
    if (stop == nullptr || *stop != '\0') {
        return false;
    }
    if (errno == ERANGE) {
        return false;
    }
    if (i > INT_MAX || i < INT_MIN) {
        return false;
    }
    result = i;
    return true;
}

bool SafeAtoU(const char* data, unsigned& result, int radix)
{
    CHECK_NULL_RETURN(data, false);
    CHECK_TRUE_RETURN(strnlen(data, UINT_MAX_LEN) == 0, false);

    char* stop = nullptr;
    errno = 0;
    auto i = strtoul(data, &stop, radix);
    if (stop == nullptr || *stop != '\0') {
        return false;
    }
    if (errno == ERANGE) {
        return false;
    }
    if (i > UINT_MAX) {
        return false;
    }
    result = i;
    return true;
}

bool GetSubString(const char* data, const char* start, const char* end, string& result)
{
    CHECK_TRUE_RETURN(data == nullptr || start == nullptr || end == nullptr, false);

    auto nlsStart = strstr(data, start);
    if (nlsStart != nullptr) {
        nlsStart += strlen(start);

        auto nlsEnd = strstr(nlsStart, end);
        if (nlsEnd != nullptr) {
            result.clear();
            result.insert(0, nlsStart, nlsEnd - nlsStart);
            return true;
        }
    }

    return false;
}

bool RemoveSubString(char* data, int dataLen, const char* start, const char* end)
{
    CHECK_TRUE_RETURN(data == nullptr || dataLen == 0 || start == nullptr || end == nullptr, false);

    auto startPos = strstr(data, start);
    if (startPos != nullptr) {
        auto endPos = strstr(startPos + strlen(start), end);
        if (endPos != nullptr) {
            endPos += strlen(end);
            auto ret = memmove_s(startPos, dataLen - (startPos - data), endPos, strlen(endPos));
            if (ret != 0) {
                return false;
            }

            *(startPos + strlen(endPos)) = 0;
            return true;
        }
    }

    return false;
}
}
