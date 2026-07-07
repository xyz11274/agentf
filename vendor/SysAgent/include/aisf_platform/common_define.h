#pragma once

#include <cstdarg>
#include <map>
#include <unordered_map>
#include <vector>
#include <string>
#include <mutex>
#include <shared_mutex>
#include <list>
#include <atomic>
#include <memory>
#include <deque>
#include <unordered_set>
#include <functional>
#include <sys/stat.h>

#include "securec.h"

#include "common_macro.h"
#include "utils/logger.h"

using std::string;
using std::vector;
using std::map;
using std::unordered_map;
using std::unordered_set;
using std::mutex;
using std::shared_mutex;
using std::list;
using std::pair;
using std::unique_ptr;
using std::shared_ptr;
using std::make_shared;
using std::make_unique;
using std::deque;
using std::atomic_bool;
using std::atomic_int;
using std::atomic_uint;
using std::atomic_uchar;
using std::atomic_ullong;
using std::atomic;
using std::stringstream;
using std::recursive_mutex;
using recursive_mutex_guard = std::lock_guard<std::recursive_mutex>;
using mutex_guard = std::lock_guard<std::mutex>;
using shared_mutex_read_guard = std::shared_lock<std::shared_mutex>;
using shared_mutex_write_guard = std::unique_lock<std::shared_mutex>;

namespace AisfPlat {
#define REJECTED_ANSWER "REJECTED_ANSWER"
#define INTENT_VIDEO_SEARCH_PLAY "INTENT_VIDEO_SEARCH_PLAY"


const int NANO_IN_MICRO = 1000;
const int MICRO_IN_MILLI = 1000;
const int MILLI_IN_SECOND = 1000;
const int SECOND_IN_MINUTE = 60;
const int MINUTE_IN_HOUR = 60;
const int SECOND_IN_HOUR = SECOND_IN_MINUTE * MINUTE_IN_HOUR;
const int HOUR_IN_DAY = 24;
const int DAY_IN_WEEK = 7;
const int SECOND_IN_DAY = SECOND_IN_HOUR * HOUR_IN_DAY;

const int DEFAULT_FILE_MODE = (S_IRUSR | S_IWUSR | S_IRGRP);
const int ARCHIVE_FILE_MODE = (S_IRUSR | S_IRGRP);
const int DEFAULT_FOLDER_MODE = (S_IRWXU | S_IRGRP | S_IXGRP);

enum NUMBER {
    NUMBER_ZERO = 0,
    NUMBER_ONE = 1,
    NUMBER_TWO = 2,
    NUMBER_THREE = 3,
    NUMBER_FOUR = 4,
    NUMBER_FIVE = 5,
    NUMBER_SIX = 6,
    NUMBER_SEVEN = 7,
    NUMBER_EIGHT = 8,
    NUMBER_NINE = 9,
    NUMBER_TEN = 10,
    NUMBER_ELEVEN = 11,
    NUMBER_TWELVE = 12,
    NUMBER_THIRTEEN = 13,
    NUMBER_FOURTEEN = 14,
    NUMBER_FIFTEEN = 15,
    NUMBER_SIXTEEN = 16,
    NUMBER_SEVENTEEN = 17,
    NUMBER_EIGHTEEN = 18,
    NUMBER_NINETEEN = 19,
    NUMBER_TWENTY = 20,
    NUMBER_THIRTY = 30,
    NUMBER_FORTY = 40,
    NUMBER_FIFTY = 50,
    NUMBER_SIXTY = 60,
    NUMBER_SEVENTY = 70,
    NUMBER_EIGHTY = 80,
    NUMBER_NINETY = 90,
    NUMBER_HUNDRED = 100,
    NUMBER_THOUSAND = 1000
};

const int KILLO_BYTE_NUM = 1024;
const int MEGA_BYTE_NUM = 1024 * KILLO_BYTE_NUM;

const int HALF = 2;
const int QUARTER = 4;

const int BUFFER_SIZE_16B = 16;
const int BUFFER_SIZE_32B = 32;
const int BUFFER_SIZE_64B = 64;
const int BUFFER_SIZE_128B = 128;
const int BUFFER_SIZE_256B = 256;
const int BUFFER_SIZE_512B = 512;
const int BUFFER_SIZE_1KB = KILLO_BYTE_NUM;
const int BUFFER_SIZE_2KB = BUFFER_SIZE_1KB * NUMBER_TWO;
const int BUFFER_SIZE_4KB = BUFFER_SIZE_2KB * NUMBER_TWO;
const int BUFFER_SIZE_8KB = BUFFER_SIZE_4KB * NUMBER_TWO;
const int BUFFER_SIZE_16KB = BUFFER_SIZE_8KB * NUMBER_TWO;
const int BUFFER_SIZE_32KB = BUFFER_SIZE_16KB * NUMBER_TWO;
const int BUFFER_SIZE_64KB = BUFFER_SIZE_32KB * NUMBER_TWO;
const int BUFFER_SIZE_128KB = BUFFER_SIZE_64KB * NUMBER_TWO;
const int BUFFER_SIZE_256KB = BUFFER_SIZE_128KB * NUMBER_TWO;
const int BUFFER_SIZE_512KB = BUFFER_SIZE_256KB * NUMBER_TWO;
const int BUFFER_SIZE_1MB = BUFFER_SIZE_512KB * NUMBER_TWO;
const int BUFFER_SIZE_2MB = BUFFER_SIZE_1MB * NUMBER_TWO;
const int BUFFER_SIZE_4MB = BUFFER_SIZE_2MB * NUMBER_TWO;
const int BUFFER_SIZE_8MB = BUFFER_SIZE_4MB * NUMBER_TWO;
const int BUFFER_SIZE_16MB = BUFFER_SIZE_8MB * NUMBER_TWO;

const int64_t MAX_INT64 = std::numeric_limits<int64_t>::max();
const uint64_t MAX_UINT64 = std::numeric_limits<uint64_t>::max();

const inline char* APP_ID = "0";
}