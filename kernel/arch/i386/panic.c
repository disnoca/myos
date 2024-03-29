/**
 * Code for Kernel Panic.
 * 
 * @author Samuel Pires
*/

#include <kernel/system.h>

#include <kernel/arch/i386/system.h>
#include <kernel/arch/i386/drivers/vga.h>

#include <stdio.h>

void panic(const char* msg, const char* file, int line)
{
	IRQ_OFF;

	vga_clear();
	printf("%s in %s at line %d\n", msg, file, line);
	cursor_disable();

	STOP;
}
