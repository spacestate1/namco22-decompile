/*
 * glibc_compat.h -- force-included into every C file (CMake, PORTABLE=ON).
 *
 * A binary links the NEWEST version of each glibc symbol the build host has, so
 * one built on glibc 2.44 refuses to start on an older system -- the Steam
 * Deck's SteamOS among them ("version GLIBC_2.43 not found"). Measured on this
 * tree: sqrtf pulled in GLIBC_2.43 and fmod 2.38, and _GNU_SOURCE (glibc >= 2.38)
 * redirects strtol/sscanf to the C23 __isoc23_* entry points (2.38). Bind them
 * to the x86-64 baseline versions, which every glibc still exports. The C23
 * variants differ only in accepting "0b" binary literals, which nothing here
 * parses. What remains is __libc_start_main@GLIBC_2.34: the floor is glibc
 * 2.34, below every current SteamOS. Check with:
 *   objdump -T <binary> | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -1
 */
#if defined(__x86_64__) && defined(__GLIBC__) || defined(__x86_64__) && defined(__linux__)
__asm__(".symver sqrtf,sqrtf@GLIBC_2.2.5");
__asm__(".symver fmod,fmod@GLIBC_2.2.5");
__asm__(".symver __isoc23_strtol,strtol@GLIBC_2.2.5");
__asm__(".symver __isoc23_strtoul,strtoul@GLIBC_2.2.5");
__asm__(".symver __isoc23_strtoll,strtoll@GLIBC_2.2.5");
__asm__(".symver __isoc23_strtoull,strtoull@GLIBC_2.2.5");
__asm__(".symver __isoc23_sscanf,sscanf@GLIBC_2.2.5");
__asm__(".symver __isoc23_fscanf,fscanf@GLIBC_2.2.5");
__asm__(".symver __isoc23_scanf,scanf@GLIBC_2.2.5");
__asm__(".symver __isoc23_vsscanf,vsscanf@GLIBC_2.2.5");
#endif
