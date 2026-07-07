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
#ifndef __MOCKCPP_GNU_METHOD_INFO_READER_H
#define __MOCKCPP_GNU_METHOD_INFO_READER_H

#include <algorithm>
#include <mockcpp/mockcpp.h>

#include <mockcpp/OutputStringStream.h>
#include <mockcpp/MethodTypeTraits.h>
#include <mockcpp/ReportFailure.h>
#include <mockcpp/TypeString.h>

MOCKCPP_NS_START

///////////////////////////////////////////////////////////
struct GnuMethodDescription
{
   union {
	  void* addr;
     int index;
   }u;
 
   int delta;
};

///////////////////////////////////////////////////////////
template <typename Method>
union MethodDescriptionUnion
{
    GnuMethodDescription desc;
    Method method;
};

///////////////////////////////////////////////////////////
template <typename Method>
void* getAddrOfMethod(Method input)
{
	MethodDescriptionUnion<Method> m;
	m.method = input;

/* 在arm32平台thumb指令集下, 函数指针地址可以是奇数 */
#ifndef __arm__
   oss_t oss;
   oss << "Method address should be even, please make sure the method "
       << TypeString<Method>::value() << " is NOT a virtual method";
 
	MOCKCPP_ASSERT_TRUE(
		oss.str(),
	   !(m.desc.u.index%2));
#endif

   return m.desc.u.addr;
}

///////////////////////////////////////////////////////////
template <typename C, typename Method>
GnuMethodDescription getGnuDescOfVirtualMethod(Method input)
{
   typedef typename MethodTypeTraits<C, Method>::MethodType ExpectedMethodType; 
	MethodDescriptionUnion<ExpectedMethodType> m;
	m.method = input;

// 在arm64和arm32平台下虚函数的 Method 与x86平台的值不同，需要做特殊处理
#if defined(__aarch64__) || defined(__arm__)
   m.desc.u.index++;
   m.desc.delta = m.desc.delta % 2 + m.desc.delta / 2;
#endif

   oss_t oss;
   oss << "Virtual method address should be odd, please make sure the method "
       << TypeString<Method>::value() << " is a virtual method. index is "
       << m.desc.u.index << ", delta is " << m.desc.delta << ".";
 
	MOCKCPP_ASSERT_TRUE(
		oss.str(),
	   (m.desc.u.index%2) && (m.desc.delta >= 0));

	return m.desc;
}

///////////////////////////////////////////////////////////
template <typename C, typename Method>
unsigned int getIndexOfMethod(Method method)
{
   return (getGnuDescOfVirtualMethod<C, Method>(method).u.index - 1)/sizeof(void*);
}

///////////////////////////////////////////////////////////
template <typename C, typename Method>
unsigned int getDeltaOfMethod(Method method)
{
   return getGnuDescOfVirtualMethod<C, Method>(method).delta/sizeof(void*);
}

MOCKCPP_NS_END

#endif

