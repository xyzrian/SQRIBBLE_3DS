#include <3ds.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define SCREEN_WIDTH 320
#define SCREEN_HEIGHT 240
#define FB_WIDTH 240
#define FB_HEIGHT 320

#define MAX_HISTORY 10  // Max undo/redo steps

// Frame buffers for base and rotated checkerboard patterns
u8 baseImage[FB_WIDTH * FB_HEIGHT * 3];
u8 rotatedImage[FB_WIDTH * FB_HEIGHT * 3];
u8 scratchMask[FB_WIDTH * FB_HEIGHT];  // Alpha mask for scratched areas

// Undo/Redo stacks - stores previous states of the scratch mask
u8 undoStack[MAX_HISTORY][FB_WIDTH * FB_HEIGHT];
u8 redoStack[MAX_HISTORY][FB_WIDTH * FB_HEIGHT];
int undoTop = 0;
int redoTop = 0;

// Rainbow color palette - array of RGB values
typedef struct {
    u8 r, g, b;
} Color;

Color rainbowColors[] = {
    {65, 105, 225},    // Blue (Royal Blue)
    {138, 43, 226},    // Purple (Blue Violet)
    {220, 20, 60},     // Red (Crimson)
    {255, 140, 0},     // Orange (Dark Orange)
    {255, 215, 0},     // Yellow (Gold)
    {34, 139, 34}      // Green (Forest Green)
};
int numColors = 6;
int currentColorIndex = 0;  // Start with blue

bool useBlackBackground = false;  // Toggle for white/black secondary color

/**
 * Push current scratch mask state onto undo stack
 * Shifts stack if full to maintain history limit
 */
void pushUndo() {
    if (undoTop >= MAX_HISTORY) {
        // Shift all entries left to make room for new entry
        for (int i = 1; i < MAX_HISTORY; i++) {
            memcpy(undoStack[i - 1], undoStack[i], sizeof(scratchMask));
        }
        undoTop = MAX_HISTORY - 1;
    }
    memcpy(undoStack[undoTop++], scratchMask, sizeof(scratchMask));
    redoTop = 0; // Clear redo stack when new action is performed
}

/**
 * Restore previous state from undo stack
 */
void undo() {
    if (undoTop > 0) {
        // Save current state to redo stack before undoing
        memcpy(redoStack[redoTop++], scratchMask, sizeof(scratchMask));
        // Restore previous state from undo stack
        memcpy(scratchMask, undoStack[--undoTop], sizeof(scratchMask));
    }
}

/**
 * Restore next state from redo stack
 */
void redo() {
    if (redoTop > 0) {
        // Save current state to undo stack before redoing
        memcpy(undoStack[undoTop++], scratchMask, sizeof(scratchMask));
        // Restore next state from redo stack
        memcpy(scratchMask, redoStack[--redoTop], sizeof(scratchMask));
    }
}

/**
 * Generate checkerboard pattern with current primary color and white/black
 * Uses standard orientation (not rotated)
 */
void generateCheckerboard(u8* buffer, int cellSize) {
    Color primaryColor = rainbowColors[currentColorIndex];
    
    for (int x = 0; x < SCREEN_WIDTH; x++) {
        for (int y = 0; y < SCREEN_HEIGHT; y++) {
            // Determine which checkerboard cell this pixel belongs to
            int cellX = x / cellSize;
            int cellY = y / cellSize;
            int isColored = (cellX + cellY) % 2;  // Alternating pattern
            
            // Select color based on pattern and background toggle
            u8 r, g, b;
            if (isColored) {
                r = primaryColor.r;
                g = primaryColor.g;
                b = primaryColor.b;
            } else {
                // Use white (255) or black (0) based on toggle
                u8 bgValue = useBlackBackground ? 0 : 255;
                r = g = b = bgValue;
            }
            
            // Convert screen coordinates to framebuffer coordinates
            // 3DS framebuffer is rotated 90° counterclockwise
            int fbX = x;
            int fbY = y;
            int offset = (fbX * FB_WIDTH + (FB_HEIGHT - 1 - fbY)) * 3;
            
            // Write BGR pixel data (3DS uses BGR format, not RGB)
            if (offset >= 0 && offset < FB_WIDTH * FB_HEIGHT * 3 - 2) {
                buffer[offset + 0] = b;
                buffer[offset + 1] = g;
                buffer[offset + 2] = r;
            }
        }
    }
}

/**
 * Generate rotated checkerboard pattern (90° rotation)
 * This creates the "hidden" layer revealed by scratching
 */
void generateRotatedCheckerboard(u8* buffer, int cellSize) {
    Color primaryColor = rainbowColors[currentColorIndex];
    
    for (int x = 0; x < SCREEN_WIDTH; x++) {
        for (int y = 0; y < SCREEN_HEIGHT; y++) {
            // Apply 90° rotation to checkerboard pattern
            int rotX = y;
            int rotY = SCREEN_WIDTH - x - 1;
            
            int cellX = rotX / cellSize;
            int cellY = rotY / cellSize;
            int isColored = (cellX + cellY) % 2;
            
            u8 r, g, b;
            if (isColored) {
                r = primaryColor.r;
                g = primaryColor.g;
                b = primaryColor.b;
            } else {
                u8 bgValue = useBlackBackground ? 0 : 255;
                r = g = b = bgValue;
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
 * Composite two images using alpha mask
 * dest = bottom * (1 - alpha) + top * alpha
 * This blends the two checkerboard patterns based on scratch mask
 */
void compositeImage(u8* dest, u8* bottom, u8* top, u8* mask) {
    for (int i = 0; i < FB_WIDTH * FB_HEIGHT; i++) {
        int pixelIdx = i * 3;
        u8 alpha = mask[i];  // 255 = fully top layer, 0 = fully bottom layer
        
        // Alpha blend each color channel
        dest[pixelIdx + 0] = (bottom[pixelIdx + 0] * (255 - alpha) + top[pixelIdx + 0] * alpha) / 255;
        dest[pixelIdx + 1] = (bottom[pixelIdx + 1] * (255 - alpha) + top[pixelIdx + 1] * alpha) / 255;
        dest[pixelIdx + 2] = (bottom[pixelIdx + 2] * (255 - alpha) + top[pixelIdx + 2] * alpha) / 255;
    }
}

/**
 * Apply circular brush to scratch mask at touch coordinates
 * Sets alpha to 0 (fully transparent) revealing bottom layer
 */
void scratchAt(int touchX, int touchY, int brushSize) {
    if (touchX < 0 || touchX >= 320 || touchY < 0 || touchY >= 240) return;
    
    // Draw circular brush using distance formula
    for (int dx = -brushSize; dx <= brushSize; dx++) {
        for (int dy = -brushSize; dy <= brushSize; dy++) {
            int px = touchX + dx;
            int py = touchY + dy;
            
            if (px < 0 || px >= 320 || py < 0 || py >= 240) continue;
            
            // Check if pixel is within circular brush radius
            if (dx*dx + dy*dy <= brushSize*brushSize) {
                int maskIdx = px * 240 + (239 - py);
                if (maskIdx >= 0 && maskIdx < FB_WIDTH * FB_HEIGHT) {
                    scratchMask[maskIdx] = 0;  // Make transparent
                }
            }
        }
    }
}

int main(int argc, char **argv) {
    gfxInitDefault();
    gfxSet3D(true);  // Enable stereoscopic 3D
    
    // Initialize checkerboard patterns and scratch mask
    generateCheckerboard(baseImage, 20);
    generateRotatedCheckerboard(rotatedImage, 20);
    memset(scratchMask, 255, sizeof(scratchMask));  // Start fully opaque (top layer visible)
    
    // Allocate working buffers
    u8* compositeBuffer = (u8*)malloc(FB_WIDTH * FB_HEIGHT * 3);
    u8* topScreenBuffer = (u8*)malloc(240 * 400 * 3);
    
    int brushSize = 5;
    bool wasTouching = false;
    int depthOffset = 3;  // Initial 3D depth offset in pixels

    while (aptMainLoop()) {
        hidScanInput();
        u32 kDown = hidKeysDown();   // Buttons pressed this frame
        u32 kHeld = hidKeysHeld();   // Buttons held down

        // Exit application
        if (kDown & KEY_START) break;

        // Clear canvas - reset scratch mask to fully opaque
        if (kDown & KEY_X) {
            memset(scratchMask, 255, sizeof(scratchMask));
            undoTop = redoTop = 0;  // Clear undo/redo history
        }

        // Toggle white/black background with B button
        if (kDown & KEY_B) {
            useBlackBackground = !useBlackBackground;
            // Regenerate checkerboards with new background color
            generateCheckerboard(baseImage, 20);
            generateRotatedCheckerboard(rotatedImage, 20);
        }

        // Color cycling with D-pad left/right
        if (kDown & KEY_DRIGHT) {
            // Cycle forward through rainbow colors
            currentColorIndex = (currentColorIndex + 1) % numColors;
            generateCheckerboard(baseImage, 20);
            generateRotatedCheckerboard(rotatedImage, 20);
        }
        if (kDown & KEY_DLEFT) {
            // Cycle backward through rainbow colors
            currentColorIndex = (currentColorIndex - 1 + numColors) % numColors;
            generateCheckerboard(baseImage, 20);
            generateRotatedCheckerboard(rotatedImage, 20);
        }

        // Brush size adjustment with D-pad up/down
        if (kDown & KEY_DUP) { 
            brushSize++; 
            if (brushSize > 50) brushSize = 50; 
        }
        if (kDown & KEY_DDOWN) { 
            brushSize--; 
            if (brushSize < 1) brushSize = 1; 
        }

        // Circle pad controls 3D depth
        circlePosition pos;
        hidCircleRead(&pos);
        // Map circle pad Y axis (-156 to +156) to depth offset
        // Negative Y (down) = more depth, Positive Y (up) = less depth/inverted
        // Scale: -156 maps to ~10 pixels, +156 maps to ~-10 pixels
        depthOffset = 3 - (pos.dy / 16);  // Center around 3, range approximately -7 to +13
        if (depthOffset < -10) depthOffset = -10;  // Clamp to reasonable range
        if (depthOffset > 15) depthOffset = 15;

        // Undo with L button
        if (kDown & KEY_L) undo();
        
        // Redo with R button
        if (kDown & KEY_R) redo();

        // Touch input for scratching
        if (kHeld & KEY_TOUCH) {
            touchPosition touch;
            hidTouchRead(&touch);

            // Save state when starting new stroke
            if (!wasTouching) pushUndo();
            
            scratchAt(touch.px, touch.py, brushSize);
            wasTouching = true;
        } else {
            wasTouching = false;
        }

        // Composite the two layers based on scratch mask
        // Rotated layer on bottom, base layer on top (revealed by scratching)
        compositeImage(compositeBuffer, rotatedImage, baseImage, scratchMask);

        // Draw to bottom screen (touchscreen) - shows composite result
        u8* fbBottom = gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, NULL, NULL);
        memcpy(fbBottom, compositeBuffer, FB_WIDTH * FB_HEIGHT * 3);

        // Draw left eye view (top screen left)
        // Centers 320px wide image in 400px wide screen with 40px black bars
        u8* fbTopLeft = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL);
        memset(topScreenBuffer, 0, 240 * 400 * 3);  // Black background
        for (int x = 0; x < 320; x++) {
            for (int y = 0; y < 240; y++) {
                int srcIdx = (x * 240 + (239 - y)) * 3;
                int dstX = x + 40;  // Center horizontally
                int dstIdx = (dstX * 240 + (239 - y)) * 3;
                memcpy(&topScreenBuffer[dstIdx], &compositeBuffer[srcIdx], 3);
            }
        }
        memcpy(fbTopLeft, topScreenBuffer, 240 * 400 * 3);

        // Draw right eye view (top screen right) with parallax offset
        // Creates 3D effect by shifting scratched areas based on depth
        u8* fbTopRight = gfxGetFramebuffer(GFX_TOP, GFX_RIGHT, NULL, NULL);
        memset(topScreenBuffer, 0, 240 * 400 * 3);
        for (int x = 0; x < 320; x++) {
            for (int y = 0; y < 240; y++) {
                int srcIdx = (x * 240 + (239 - y)) * 3;
                int maskIdx = x * 240 + (239 - y);
                u8 alpha = scratchMask[maskIdx];
                
                // Apply depth offset based on scratch mask value
                // Scratched areas (alpha < 128) appear at base depth
                // Unscratched areas (alpha >= 128) get parallax offset for 3D effect
                int dstX = (alpha > 128 ? x + 40 + depthOffset : x + 40);
                
                if (dstX >= 0 && dstX < 400) {
                    int dstIdx = (dstX * 240 + (239 - y)) * 3;
                    memcpy(&topScreenBuffer[dstIdx], &compositeBuffer[srcIdx], 3);
                }
            }
        }
        memcpy(fbTopRight, topScreenBuffer, 240 * 400 * 3);

        // Display the frames
        gfxFlushBuffers();
        gfxSwapBuffers();
        gspWaitForVBlank();  // Wait for vertical blank to avoid tearing
    }

    // Cleanup
    free(compositeBuffer);
    free(topScreenBuffer);
    gfxExit();
    return 0;
}