#ifndef TEXT_H
#define TEXT_H

#include <3ds.h>

// Text rendering functions for 5x7 bitmap font
// Supports ASCII characters 32-122 (space through 'z')

/**
 * Draw single character at specified position
 * @param framebuffer Target framebuffer (BGR format, rotated 90°)
 * @param c Character to draw (ASCII 32-122)
 * @param startX X coordinate on screen
 * @param startY Y coordinate on screen
 * @param r Red color component (0-255)
 * @param g Green color component (0-255)
 * @param b Blue color component (0-255)
 */
void drawChar(u8* framebuffer, char c, int startX, int startY, u8 r, u8 g, u8 b);

/**
 * Draw text string with automatic newline handling
 * Characters are spaced 6 pixels apart, lines 9 pixels apart
 * @param framebuffer Target framebuffer (BGR format, rotated 90°)
 * @param str Null-terminated string to draw (supports \n for newlines)
 * @param x Starting X coordinate
 * @param y Starting Y coordinate
 * @param r Red color component (0-255)
 * @param g Green color component (0-255)
 * @param b Blue color component (0-255)
 */
void drawString(u8* framebuffer, const char* str, int x, int y, u8 r, u8 g, u8 b);

#endif // TEXT_H
