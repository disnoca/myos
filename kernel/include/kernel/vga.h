#ifndef VGA
#define VGA

#include <stdint.h>

/**	vga_init:
 * 	Clears the framebuffer and configures and enables the cursor.
*/
void vga_init(void);

/** cursor_enable:
 *  Enables the cursor and sets the start and end scanlines
 * 
 * @param cursor_start highest (minimum) scanline
 * @param cursor_end   lowest (maximum) scanline
*/
void cursor_enable(uint8_t cursor_start, uint8_t cursor_end);

/** cursor_disable:
 *  Disables the cursor
*/
void cursor_disable(void);

/** cursor_move:
 *  Moves the cursor of the framebuffer to the given position
 *
 *  @param pos new position of the cursor
*/
void cursor_move(uint16_t pos);

/** cursor_init:
 *  Configures and enables the cursor.
*/
void cursor_init(void);

/** fb_clear:
 * 	Clears the framebuffer and resets the cursor's position.
*/
void fb_clear(void);

/** fb_write:
 *  Writes the given string to the screen.
 *  Accepts newline characters.
 * 
 *  @param str the string to be written
*/
void fb_write(char* str);

#endif