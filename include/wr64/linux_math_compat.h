#pragma once

// glibc 2.43 gives these float functions new default symbol versions. Their
// longstanding x86-64 ABI is still exported by both older and newer glibc.
// Select it in every C/C++ translation unit, including static dependencies,
// without wrapping the functions or changing any game code. Other platforms
// and libc implementations retain their normal math linkage.
#if defined(WR64_LIBM_COMPAT_GLIBC) && defined(__linux__) && defined(__x86_64__) && !defined(__ILP32__)
__asm__(".symver atan2f,atan2f@GLIBC_2.2.5");
__asm__(".symver asinf,asinf@GLIBC_2.2.5");
__asm__(".symver acosf,acosf@GLIBC_2.2.5");
__asm__(".symver sqrtf,sqrtf@GLIBC_2.2.5");
#endif
