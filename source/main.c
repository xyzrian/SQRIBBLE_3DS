#include <3ds.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <math.h>
#include "text.h"  

// Screen dimensions - 3DS has 320x240 bottom screen, 400x240 top screen
#define SCREEN_WIDTH 320
#define SCREEN_HEIGHT 240
#define FB_WIDTH 240      // Framebuffer width (rotated 90°)
#define FB_HEIGHT 320     // Framebuffer height (rotated 90°)

#define MAX_HISTORY 20    // Maximum number of undo/redo steps to store

// Three framebuffers store different visual layers:
// 1. baseImage: The "top" layer that gets scratched away
// 2. rotatedImage: The "hidden" layer revealed underneath
// 3. scratchMask: Alpha mask determining which layer is visible (0-255)
u8 baseImage[FB_WIDTH * FB_HEIGHT * 3];
u8 rotatedImage[FB_WIDTH * FB_HEIGHT * 3];
u8 scratchMask[FB_WIDTH * FB_HEIGHT];

// Undo/Redo system: Circular buffers storing previous scratch mask states
u8 undoStack[MAX_HISTORY][FB_WIDTH * FB_HEIGHT];
u8 redoStack[MAX_HISTORY][FB_WIDTH * FB_HEIGHT];
int undoTop = 0;  // Points to next available undo slot
int redoTop = 0;  // Points to next available redo slot

// RGB color structure for easy color management
typedef struct {
    u8 r, g, b;
} Color;

// Rainbow palette: 6 vibrant colors the user can cycle through
Color rainbowColors[] = {
    {65, 105, 225},    // Royal Blue
    {138, 43, 226},    // Blue Violet (Purple)
    {220, 20, 60},     // Crimson (Red)
    {255, 140, 0},     // Dark Orange
    {255, 215, 0},     // Gold (Yellow)
    {34, 139, 34}      // Forest Green
};
int numColors = 6;
int currentColorIndex = 0;  // Current selected color (starts with blue)

// Drawing modes determine what patterns are generated
typedef enum {
    MODE_CHECKERBOARD_BLACK,    // Checkerboard with black squares
    MODE_CHECKERBOARD_WHITE,    // Checkerboard with white squares
    MODE_COLOR_ON_WHITE,        // Solid color strokes on white canvas
    MODE_COLOR_ON_BLACK         // Solid color strokes on black canvas
} DrawingMode;

DrawingMode currentMode = MODE_CHECKERBOARD_WHITE;

// Brush shapes affect how the scratch mask is modified
typedef enum {
    BRUSH_CIRCLE,   // Hard circular brush
    BRUSH_SQUARE,   // Hard square brush
    BRUSH_SOFT      // Soft circular brush with feathered edges
} BrushShape;

BrushShape currentBrushShape = BRUSH_CIRCLE;

bool allowDrawing = false;

bool showInstructions = true;     // Show instruction screen on startup
float depthOffset = 3.0f;         // 3D stereoscopic depth offset

// Previous touch position for line interpolation (smooth drawing)
int prevTouchX = -1;
int prevTouchY = -1;

/**
 * UNDO/REDO SYSTEM
 * 
 * Push current scratch mask state onto undo stack.
 * When stack is full, oldest entry is discarded (FIFO).
 * Any new action clears the redo stack (standard undo/redo behavior).
 */
void pushUndo() {
    if (undoTop >= MAX_HISTORY) {
        // Shift all entries left to discard oldest
        for (int i = 1; i < MAX_HISTORY; i++) {
            memcpy(undoStack[i - 1], undoStack[i], sizeof(scratchMask));
        }
        undoTop = MAX_HISTORY - 1;
    }
    memcpy(undoStack[undoTop++], scratchMask, sizeof(scratchMask));
    redoTop = 0; // New action invalidates redo history
}

/**
 * Restore previous scratch mask state from undo stack
 * Current state is saved to redo stack before reverting
 */
void undo() {
    if (undoTop > 0) {
        // Save current state to redo stack
        memcpy(redoStack[redoTop++], scratchMask, sizeof(scratchMask));
        // Restore previous state
        memcpy(scratchMask, undoStack[--undoTop], sizeof(scratchMask));
    }
}

/**
 * Restore next scratch mask state from redo stack
 * Current state is saved to undo stack before advancing
 */
void redo() {
    if (redoTop > 0) {
        // Save current state to undo stack
        memcpy(undoStack[undoTop++], scratchMask, sizeof(scratchMask));
        // Restore next state
        memcpy(scratchMask, redoStack[--redoTop], sizeof(scratchMask));
    }
}

/**
 * FRAMEBUFFER GENERATION
 * 
 * Generate the base (top) layer with checkerboard or solid pattern.
 * The 3DS framebuffer is rotated 90° clockwise, so coordinates are transformed:
 * Screen(x,y) -> Framebuffer(x, HEIGHT-1-y)
 * 
 * Framebuffer uses BGR color format (not RGB).
 */
void generateCheckerboard(u8* buffer, int cellSize) {
    Color primaryColor = rainbowColors[currentColorIndex];
    
    for (int x = 0; x < SCREEN_WIDTH; x++) {
        for (int y = 0; y < SCREEN_HEIGHT; y++) {
            u8 r, g, b;
            
            if (currentMode == MODE_COLOR_ON_WHITE) {
                // Solid white canvas for color drawing mode
                r = g = b = 255;
            } else if (currentMode == MODE_COLOR_ON_BLACK) {
                // Solid dark grey (draw on dark canvas)
                r = g = b = 20;
            } else {
                // Checkerboard pattern: alternate between color and black/white
                int cellX = x / cellSize;
                int cellY = y / cellSize;
                int isColored = (cellX + cellY) % 2;  // Checkerboard logic
                
                if (isColored) {
                    r = primaryColor.r;
                    g = primaryColor.g;
                    b = primaryColor.b;
                } else {
                    // Background is either black (0) or white (255)
                    u8 bgValue = (currentMode == MODE_CHECKERBOARD_BLACK) ? 0 : 255;
                    r = g = b = bgValue;
                }
            }
            
            // Transform screen coordinates to framebuffer coordinates
            int fbX = x;
            int fbY = y;
            int offset = (fbX * FB_WIDTH + (FB_HEIGHT - 1 - fbY)) * 3;
            
            // Write BGR pixel data (3DS uses BGR, not RGB!)
            if (offset >= 0 && offset < FB_WIDTH * FB_HEIGHT * 3 - 2) {
                buffer[offset + 0] = b;
                buffer[offset + 1] = g;
                buffer[offset + 2] = r;
            }
        }
    }
}

/**
 * Generate the rotated (bottom) layer - the "hidden" pattern revealed by scratching.
 * For checkerboard modes, pattern is rotated 90° for visual interest.
 * For solid color modes, this layer contains the drawing color.
 */
void generateRotatedCheckerboard(u8* buffer, int cellSize) {
    Color primaryColor = rainbowColors[currentColorIndex];
    
    for (int x = 0; x < SCREEN_WIDTH; x++) {
        for (int y = 0; y < SCREEN_HEIGHT; y++) {
            u8 r, g, b;
            
            if (currentMode == MODE_COLOR_ON_WHITE || currentMode == MODE_COLOR_ON_BLACK) {
                // Hidden layer is the current color for drawing modes
                r = primaryColor.r;
                g = primaryColor.g;
                b = primaryColor.b;
            } else {
                // Apply 90° rotation to checkerboard coordinates
                int rotX = y;
                int rotY = SCREEN_WIDTH - x - 1;
                
                int cellX = rotX / cellSize;
                int cellY = rotY / cellSize;
                int isColored = (cellX + cellY) % 2;
                
                if (isColored) {
                    r = primaryColor.r;
                    g = primaryColor.g;
                    b = primaryColor.b;
                } else {
                    u8 bgValue = (currentMode == MODE_CHECKERBOARD_BLACK) ? 0 : 255;
                    r = g = b = bgValue;
                }
            }
            
            int offset = (x * FB_WIDTH + (FB_HEIGHT - 1 - y)) * 3;
            
            if (offset >= 0 && offset < FB_WIDTH * FB_HEIGHT * 3 - 2) {
                buffer[offset + 0] = b;
                buffer[offset + 1] = g;
                buffer[offset + 2] = r;
            }
        }
    }
}

/**
 * ALPHA COMPOSITING
 * 
 * Blend two layers using the scratch mask as alpha channel.
 * Formula: dest = bottom * (1 - alpha) + top * alpha
 * 
 * alpha = 0:   Show bottom layer fully (scratched away)
 * alpha = 255: Show top layer fully (unscratched)
 */
void compositeImage(u8* dest, u8* bottom, u8* top, u8* mask) {
    for (int i = 0; i < FB_WIDTH * FB_HEIGHT; i++) {
        int pixelIdx = i * 3;
        u8 alpha = mask[i];
        
        // Blend each color channel separately (B, G, R)
        dest[pixelIdx + 0] = (bottom[pixelIdx + 0] * (255 - alpha) + top[pixelIdx + 0] * alpha) / 255;
        dest[pixelIdx + 1] = (bottom[pixelIdx + 1] * (255 - alpha) + top[pixelIdx + 1] * alpha) / 255;
        dest[pixelIdx + 2] = (bottom[pixelIdx + 2] * (255 - alpha) + top[pixelIdx + 2] * alpha) / 255;
    }
}

/**
 * DRAWING ENGINE
 * 
 * Apply brush to scratch mask at touch coordinates.
 * Modifies alpha values in scratchMask to reveal hidden layer.
 * 
 * Supports three brush shapes:
 * - CIRCLE: Hard-edged circular brush
 * - SQUARE: Hard-edged square brush
 * - SOFT: Feathered circular brush with smooth falloff
 */
void scratchAt(int touchX, int touchY, int brushSize) {
    if (touchX < 0 || touchX >= 320 || touchY < 0 || touchY >= 240) return;
    
    // Iterate over bounding box of brush
    for (int dx = -brushSize; dx <= brushSize; dx++) {
        for (int dy = -brushSize; dy <= brushSize; dy++) {
            int px = touchX + dx;
            int py = touchY + dy;
            
            // Bounds check
            if (px < 0 || px >= 320 || py >= 240) continue;
            if (py < 0) py = 0;  // Allow slight negative y to prevent gaps
            
            // Convert screen coordinates to mask index
            int maskIdx = px * 240 + (239 - py);
            if (maskIdx < 0 || maskIdx >= FB_WIDTH * FB_HEIGHT) continue;
            
            bool shouldDraw = false;
            u8 alphaValue = 0;
            
            switch (currentBrushShape) {
                case BRUSH_CIRCLE:
                    // Circular brush: check if pixel is within radius
                    if (dx*dx + dy*dy <= brushSize*brushSize) {
                        shouldDraw = true;
                        alphaValue = 0;  // Fully reveal bottom layer
                    }
                    break;
                    
                case BRUSH_SQUARE:
                    // Square brush: always draw within bounding box
                    shouldDraw = true;
                    alphaValue = 0;
                    break;
                    
                case BRUSH_SOFT:
                    // Soft brush: feathered edges with improved falloff curve
                    {
                        float distance = sqrtf(dx*dx + dy*dy);
                        if (distance <= brushSize) {
                            shouldDraw = true;
                            // Improved falloff: quadratic easing for smoother appearance
                            float falloff = distance / brushSize;
                            falloff = falloff * falloff;  // Square for smoother gradient
                            alphaValue = (u8)(falloff * 255);
                            
                            // Blend with existing alpha for accumulation
                            u8 currentAlpha = scratchMask[maskIdx];
                            alphaValue = (currentAlpha < alphaValue) ? currentAlpha : alphaValue;
                        }
                    }
                    break;
            }
            
            if (shouldDraw) {
                scratchMask[maskIdx] = alphaValue;
            }
        }
    }
}

/**
 * LINE INTERPOLATION
 * 
 * Draw smooth line between two points using Bresenham-style algorithm.
 * This prevents gaps when touch moves quickly between frames.
 * Called with previous and current touch positions.
 */
void drawLine(int x0, int y0, int x1, int y1, int brushSize) {
    int dx = abs(x1 - x0);
    int dy = abs(y1 - y0);
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx - dy;
    
    while (1) {
        scratchAt(x0, y0, brushSize);
        
        if (x0 == x1 && y0 == y1) break;
        
        int e2 = 2 * err;
        if (e2 > -dy) {
            err -= dy;
            x0 += sx;
        }
        if (e2 < dx) {
            err += dx;
            y0 += sy;
        }
    }
}


/**
 * INSTRUCTION SCREEN
 * 
 * Draw help screen with controls and game info.
 * Uses gradient background and colored text for visual hierarchy.
 * Coordinates are inverted due to framebuffer rotation.
 */
void drawInstructions(u8* framebuffer) {
    // Fill with dark gray background
    for (int x = 0; x < 320; x++) {
        for (int y = 0; y < 240; y++) {
            u8 gradientValue = 70;  // Dark gray
            int offset = (x * FB_WIDTH + (FB_HEIGHT - 1 - y)) * 3;
            framebuffer[offset + 0] = gradientValue;      // B
            framebuffer[offset + 1] = gradientValue;      // G
            framebuffer[offset + 2] = gradientValue;      // R
        }
    }
    
    // Title and Instructions 
    // y_framebuffer = 319 - y 
    drawString(framebuffer, "SQRIBBLE 3DS", 120, 90, 100, 255, 255); //x, y - FIX drawChar offset 
    drawString(framebuffer, "v1.0", 280, 90, 150, 150, 150);
    
    drawString(framebuffer, "BASIC CONTROLS:", 10, 110, 255, 255, 100);
    drawString(framebuffer, "Touch: Draw", 10, 120, 255, 255, 255);
    drawString(framebuffer, "L/R: Undo/Redo", 10, 130, 255, 255, 255);
    drawString(framebuffer, "D-Pad Up/Down: Brush Size", 10, 140, 255, 255, 255);
    drawString(framebuffer, "D-Pad L/R: Cycle Primary Color", 10, 150, 255, 255, 255);
    drawString(framebuffer, "A: Cycle Brush shape", 10, 160, 255, 255, 255);
    drawString(framebuffer, "B: Cycle canvas style", 10, 170, 255, 255, 255);
    drawString(framebuffer, "X: Clear canvas", 10, 180, 255, 255, 255);
    drawString(framebuffer, "Y: Save screenshot", 10, 190, 255, 255, 255);
    drawString(framebuffer, "Circle Pad: 3D depth", 10, 200, 255, 255, 255);
    drawString(framebuffer, "START: Toggle help", 10, 210, 255, 255, 255);
    
    // Prompt near bottom
    drawString(framebuffer, "Press any button to begin!", 80, 230, 100, 255, 255);
}

/**
 * SCREENSHOT SYSTEM
 * 
 * Save current canvas to SD card as BMP file.
 * Filename format: sqribble_YYYYMMDD_HHMMSS.bmp
 * BMP format is uncompressed 24-bit RGB (actually BGR on 3DS).
 * Returns true on success, false on failure.
 */
bool saveScreenshot(u8* framebuffer) {
    // Get current time for unique filename
    time_t rawtime;
    struct tm* timeinfo;
    char filename[256];
    
    time(&rawtime);
    timeinfo = localtime(&rawtime);
    
    // Format timestamp into filename
    snprintf(filename, sizeof(filename), "sdmc:/sqribble_%04d%02d%02d_%02d%02d%02d.bmp",
             timeinfo->tm_year + 1900, timeinfo->tm_mon + 1, timeinfo->tm_mday,
             timeinfo->tm_hour, timeinfo->tm_min, timeinfo->tm_sec);
    
    FILE* file = fopen(filename, "wb");
    if (!file) return false;
    
    // BMP file structure: File Header (14 bytes) + Info Header (40 bytes) + Pixel Data
    u32 fileSize = 54 + (320 * 240 * 3);
    u32 dataOffset = 54;
    u32 headerSize = 40;
    u32 width = 320;
    u32 height = 240;
    u16 planes = 1;
    u16 bitsPerPixel = 24;
    
    // Write BMP file header (14 bytes)
    fwrite("BM", 1, 2, file);              // Magic number
    fwrite(&fileSize, 4, 1, file);        // File size
    fwrite("\0\0\0\0", 1, 4, file);        // Reserved
    fwrite(&dataOffset, 4, 1, file);      // Pixel data offset
    
    // Write BMP info header (40 bytes)
    fwrite(&headerSize, 4, 1, file);      // Header size
    fwrite(&width, 4, 1, file);           // Image width
    fwrite(&height, 4, 1, file);          // Image height
    fwrite(&planes, 2, 1, file);          // Color planes
    fwrite(&bitsPerPixel, 2, 1, file);    // Bits per pixel
    fwrite("\0\0\0\0", 1, 4, file);        // Compression (none)
    fwrite("\0\0\0\0", 1, 4, file);        // Image size (can be 0 for uncompressed)
    fwrite("\0\0\0\0", 1, 4, file);        // X pixels per meter
    fwrite("\0\0\0\0", 1, 4, file);        // Y pixels per meter
    fwrite("\0\0\0\0", 1, 4, file);        // Colors in palette
    fwrite("\0\0\0\0", 1, 4, file);        // Important colors
    
    // Write pixel data (BMP stores bottom-to-top, which matches our coordinate system)
    for (int y = 239; y >= 0; y--) {
        for (int x = 0; x < 320; x++) {
            int idx = (x * 240 + (239 - y)) * 3;
            // BMP uses BGR format, same as 3DS framebuffer
            fwrite(&framebuffer[idx], 1, 3, file);
        }
    }
    
    fclose(file);
    return true;
}

/**
 * MAIN PROGRAM
 * 
 * Game loop structure:
 * 1. Initialize graphics and 3D
 * 2. Generate initial patterns
 * 3. Main loop: Process input, update state, render
 * 4. Cleanup and exit
 */
int main(int argc, char **argv) {
    gfxInitDefault();
    gfxSet3D(true);  // Enable stereoscopic 3D rendering

    // Generate initial checkerboard patterns (20px cells)
    generateCheckerboard(baseImage, 20);
    generateRotatedCheckerboard(rotatedImage, 20);
    memset(scratchMask, 255, sizeof(scratchMask));  // Start fully opaque (top layer visible)
    
    // Allocate working buffers for rendering
    u8* compositeBuffer = (u8*)malloc(FB_WIDTH * FB_HEIGHT * 3);
    u8* topScreenBuffer = (u8*)malloc(240 * 400 * 3);
    u8* instructionBuffer = (u8*)malloc(FB_WIDTH * FB_HEIGHT * 3);
    
    // Pre-render instruction screen (only drawn once)
    drawInstructions(instructionBuffer);
    
    int brushSize = 5;
    bool wasTouching = false;

    // Main game loop - runs until user exits
    while (aptMainLoop()) {
        hidScanInput();
        u32 kDown = hidKeysDown();   // Buttons pressed this frame
        u32 kHeld = hidKeysHeld();   // Buttons held down
        // u32 kUp = hidKeysUp();       // Buttons released this frame

        // START button toggles help screen on/off
        if (kDown & KEY_START) {
            showInstructions = !showInstructions;
            allowDrawing = !showInstructions;
            if (showInstructions) {
                drawInstructions(instructionBuffer);
            }
        }

        // Any button press dismisses instruction screen
        if (showInstructions && kDown && (kDown != KEY_START)) {
            showInstructions = false;
            allowDrawing = false;  // Don't allow drawing on the dismissal tap
        }

        if (!showInstructions && !allowDrawing && !(kHeld & KEY_TOUCH)) {
            allowDrawing = true;
        }

        // Only process game controls when not showing instructions
        if (!showInstructions) {
            // X button: Clear canvas (reset to fully unscratched)
            if (kDown & KEY_X) {
                pushUndo();  // Save current state before clearing
                memset(scratchMask, 255, sizeof(scratchMask));
                depthOffset = 3.0f;  // Reset 3D depth to default
            }

            // B button: Cycle through drawing modes
            if (kDown & KEY_B) {
                currentMode = (DrawingMode)((currentMode + 1) % 4);
                // Regenerate both layers with new mode
                generateCheckerboard(baseImage, 20);
                generateRotatedCheckerboard(rotatedImage, 20);
            }

            // A button: Cycle through brush shapes
            if (kDown & KEY_A) {
                currentBrushShape = (BrushShape)((currentBrushShape + 1) % 3);
            }

            // Y button: Save screenshot to SD card
            if (kDown & KEY_Y) {
                saveScreenshot(compositeBuffer);
                // Could add visual feedback here (flash, message, etc.)
            }

            // D-Pad Right: Next color in rainbow palette
            if (kDown & KEY_DRIGHT) {
                currentColorIndex = (currentColorIndex + 1) % numColors;
                generateCheckerboard(baseImage, 20);
                generateRotatedCheckerboard(rotatedImage, 20);
            }
            
            // D-Pad Left: Previous color in rainbow palette
            if (kDown & KEY_DLEFT) {
                currentColorIndex = (currentColorIndex - 1 + numColors) % numColors;
                generateCheckerboard(baseImage, 20);
                generateRotatedCheckerboard(rotatedImage, 20);
            }

            // D-Pad Up/Down: Adjust brush size (1-50 pixels)
            if (kDown & KEY_DUP) { 
                brushSize++; 
                if (brushSize > 50) brushSize = 50; 
            }
            if (kDown & KEY_DDOWN) { 
                brushSize--; 
                if (brushSize < 1) brushSize = 1; 
            }

            // Circle Pad: Adjust 3D stereoscopic depth
            circlePosition pos;
            hidCircleRead(&pos);
            
            if (abs(pos.dy) > 20) {  // Deadzone to prevent drift
                // Convert stick input to depth adjustment (negative y = increase depth)
                float adjustment = -(float)pos.dy / 1000.0f;
                depthOffset += adjustment;
                
                // Clamp depth to reasonable range
                if (depthOffset < -10.0f) depthOffset = -10.0f;
                if (depthOffset > 15.0f) depthOffset = 15.0f;
            }

            // L button: Undo last action
            if (kDown & KEY_L) undo();
            
            // R button: Redo last undone action
            if (kDown & KEY_R) redo();

            // Touch input: Drawing system with interpolation for smooth lines
            if (allowDrawing && (kHeld & KEY_TOUCH)) {
                touchPosition touch;
                hidTouchRead(&touch);

                // Save undo state when starting new stroke
                if (!wasTouching) {
                    pushUndo();
                    prevTouchX = touch.px;
                    prevTouchY = touch.py;
                }
                
                // Draw line from previous position to current (prevents gaps)
                if (prevTouchX >= 0 && prevTouchY >= 0) {
                    drawLine(prevTouchX, prevTouchY, touch.px, touch.py, brushSize);
                }
                
                // Update previous position for next frame
                prevTouchX = touch.px;
                prevTouchY = touch.py;
                wasTouching = true;
            } else {
                // Reset touch tracking when stylus lifted
                if (wasTouching) {
                    prevTouchX = -1;
                    prevTouchY = -1;
                }
                wasTouching = false;
            }
        }

        // RENDERING PIPELINE
        
        // Step 1: Composite the two layers based on scratch mask
        // (Rotated layer below, base layer above)
        compositeImage(compositeBuffer, rotatedImage, baseImage, scratchMask);

        // Step 2: Render to bottom screen (touch screen)
        u8* fbBottom = gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, NULL, NULL);
        if (showInstructions) {
            // Show instruction screen
            memcpy(fbBottom, instructionBuffer, FB_WIDTH * FB_HEIGHT * 3);
        } else {
            // Show drawing canvas
            memcpy(fbBottom, compositeBuffer, FB_WIDTH * FB_HEIGHT * 3);
        }

        // Step 3: Render to top screen left eye (center 320px in 400px screen)
        u8* fbTopLeft = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL);
        memset(topScreenBuffer, 0, 240 * 400 * 3);  // Black bars on sides
        for (int x = 0; x < 320; x++) {
            for (int y = 0; y < 240; y++) {
                int srcIdx = (x * 240 + (239 - y)) * 3;
                int dstX = x + 40;  // Center horizontally (40px black border each side)
                int dstIdx = (dstX * 240 + (239 - y)) * 3;
                memcpy(&topScreenBuffer[dstIdx], &compositeBuffer[srcIdx], 3);
            }
        }
        memcpy(fbTopLeft, topScreenBuffer, 240 * 400 * 3);

        // Step 4: Render to top screen right eye with parallax for 3D effect
        u8* fbTopRight = gfxGetFramebuffer(GFX_TOP, GFX_RIGHT, NULL, NULL);
        memset(topScreenBuffer, 0, 240 * 400 * 3);
        for (int x = 0; x < 320; x++) {
            for (int y = 0; y < 240; y++) {
                int srcIdx = (x * 240 + (239 - y)) * 3;
                int maskIdx = x * 240 + (239 - y);
                u8 alpha = scratchMask[maskIdx];
                
                // Apply horizontal shift based on scratch state
                // Scratched areas (low alpha): base depth
                // Unscratched areas (high alpha): shifted by depthOffset for 3D pop
                int dstX = (alpha > 128 ? x + 40 + (int)depthOffset : x + 40);
                
                if (dstX >= 0 && dstX < 400) {
                    int dstIdx = (dstX * 240 + (239 - y)) * 3;
                    memcpy(&topScreenBuffer[dstIdx], &compositeBuffer[srcIdx], 3);
                }
            }
        }
        memcpy(fbTopRight, topScreenBuffer, 240 * 400 * 3);

        // Display rendered frames and wait for next frame
        gfxFlushBuffers();
        gfxSwapBuffers();
        gspWaitForVBlank();  // Sync to 60fps
    }

    // Cleanup: Free allocated memory and exit graphics
    free(compositeBuffer);
    free(topScreenBuffer);
    free(instructionBuffer);
    gfxExit();
    return 0;
}