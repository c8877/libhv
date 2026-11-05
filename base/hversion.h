#ifndef HV_VERSION_H_
#define HV_VERSION_H_

#include "hexport.h"

BEGIN_EXTERN_C

// 版本号数据定义在 base/hversion.c（唯一真源），这里只声明 getter。
// 刻意不在头文件里写版本数字：那样调用方在编译期就把版本烤进自己的二进制，
// hv_version() 返回的是调用方头文件的版本，而不是实际链接进来的库的版本。
// 每次提交由 .githooks/pre-commit 把 patch +0.0.1。
HV_EXPORT const char* hv_version(void);         // e.g. "1.3.4"
HV_EXPORT int         hv_version_major(void);
HV_EXPORT int         hv_version_minor(void);
HV_EXPORT int         hv_version_patch(void);
HV_EXPORT int         hv_version_number(void);  // 0x010304

HV_EXPORT const char* hv_compile_version();

// 1.2.3.4 => 0x01020304
HV_EXPORT int version_atoi(const char* str);

// 0x01020304 => 1.2.3.4
HV_EXPORT void version_itoa(int hex, char* str);

END_EXTERN_C

#endif // HV_VERSION_H_
