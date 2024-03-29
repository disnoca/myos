#pragma once

/* Panic */
#define PANIC(msg) panic("Kernel Panic: " msg, __FILE__, __LINE__)
#define ASSERT(x) if(!(x)) panic("Assertion Failed: " #x, __FILE__, __LINE__)

void panic(const char* msg, const char* file, int line) __attribute__ ((noreturn));
