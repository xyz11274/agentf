/***
   mockcpp is a C/C++ mock framework.
   Copyright [2008] [Darwin Yuan <darwin.yuan@gmail.com>]

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

      http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.
***/

#ifndef __MOCKCPP_H
#define __MOCKCPP_H

#include <fake_boost/fake_boost.h>

#if !defined(MOCKCPP_NO_NAMESPACE) || (MOCKCPP_NO_NAMESPACE == 0)
# define MOCKCPP_NS mockcpp
# define MOCKCPP_NS_START namespace MOCKCPP_NS {
# define MOCKCPP_NS_END }
# define USING_MOCKCPP_NS using namespace MOCKCPP_NS;
#else
# define MOCKCPP_NS 
# define MOCKCPP_NS_START 
# define MOCKCPP_NS_END 
# define USING_MOCKCPP_NS 
#endif

#ifdef _MSC_VER
# define MOCKCPP_EXPORT __declspec(dllexport)
#else
# define MOCKCPP_EXPORT 
#endif

#if  defined(__aarch64__)

#define BUILD_FOR_AARCH64

#elif defined(__arm__)
#include <cstdint>
#define BUILD_FOR_AARCH32

/*
 * 参考《DDI0406C_d_armv7ar_arm.pdf》 A4.1 About the instruction sets
 * armv7总共有4中指令集:
 * thumb指令集: 主要指令集, 16bit/32bit, 能够自由的和arm指令集切换。
 * arm指令集: 主要指令集, 32bit, 能够自由的和thumb指令集切换。
 * thumbee: thumb的变种，用于动态代码生成，不能与thumb和arm自由切换。
 * Jazelle: 扩展指令, 使得arm处理器能够直接执行一些java字节码。
 *
 * 此处只适配了主流的thumb和arm指令集。
 * 参考A2.3.1 Writing to the PC 和 https://stackoverflow.com/questions/37004954/function-address-in-arm-assembly-have-one-byte-offset
 * 在仅考虑arm和thumb指令集的前提下, 区分这两种指令集的方式是：thumb指令集下, 函数指针地址是奇数, arm指令集下函数指针地址是偶数。
 */
#define IS_USE_THUMB_INST_SET(ptr) (((uintptr_t)(ptr) % 2) != 0)
#define CONVERT_THUMB_INST_SET_FUNC_POINTER(ptr) ((void *)((uintptr_t)ptr - 1))

#elif  ( defined (__LP64__) \
    || defined (__64BIT__) \
    || defined (_LP64) \
    || ((defined(__WORDSIZE)) && (__WORDSIZE == 64)) \
	|| defined(WIN64))

#define BUILD_FOR_X64

#else	

#define BUILD_FOR_X86

#endif


#endif // __MOCKCPP_H

