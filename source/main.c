#include <3ds.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define SCREEN_WIDTH 320
#define SCREEN_HEIGHT 240
#define FB_WIDTH 240
#define FB_HEIGHT 320

#define MAX_HISTORY 10  // Max undo/redo steps

u8 baseImage[FB_WIDTH * FB_HEIGHT * 3];
u8 rotatedImage[FB_WIDTH * FB_HEIGHT * 3];
u8 scratchMask[FB_WIDTH * FB_HEIGHT];

// Undo/Redo stacks
u8 undoStack[MAX_HISTORY][FB_WIDTH * FB_HEIGHT];
u8 redoStack[MAX_HISTORY][FB_WIDTH * FB_HEIGHT];
int undoTop = 0;
int redoTop = 0;

void pushUndo() {
    if (undoTop >= MAX_HISTORY) {
        // Shift left to make room
        for (int i = 1; i < MAX_HISTORY; i++) {
            memcpy(undoStack[i - 1], undoStack[i], sizeof(scratchMask));
        }
        undoTop = MAX_HISTORY - 1;
    }
    memcpy(undoStack[undoTop++], scratchMask, sizeof(scratchMask));
    redoTop = 0; // Clear redo stack on new action
}

void undo() {
    if (undoTop > 0) {
        memcpy(redoStack[redoTop++], scratchMask, sizeof(scratchMask));
        memcpy(scratchMask, undoStack[--undoTop], sizeof(scratchMask));
    }
}

void redo() {
    if (redoTop > 0) {
        memcpy(undoStack[undoTop++], scratchMask, sizeof(scratchMask));
        memcpy(scratchMask, redoStack[--redoTop], sizeof(scratchMask));
    }
}

void generateCheckerboard(u8* buffer, int cellSize) {
    for (int x = 0; x < SCREEN_WIDTH; x++) {
        for (int y = 0; y < SCREEN_HEIGHT; y++) {
            int cellX = x / cellSize;
            int cellY = y / cellSize;
            int isBlue = (cellX + cellY) % 2;
            
            u8 r = isBlue ? 65 : 255;
            u8 g = isBlue ? 105 : 255;
            u8 b = isBlue ? 225 : 255;
            
            int fbX = x;
            int fbY = y;
            int offset = (fbX * FB_WIDTH + (FB_HEIGHT - 1 - fbY)) * 3;
            
            if (offset >= 0 && offset < FB_WIDTH * FB_HEIGHT * 3 - 2) {
                buffer[offset + 0] = b;
                buffer[offset + 1] = g;
                buffer[offset + 2] = r;
            }
        }
    }
}

void generateRotatedCheckerboard(u8* buffer, int cellSize) {
    for (int x = 0; x < SCREEN_WIDTH; x++) {
        for (int y = 0; y < SCREEN_HEIGHT; y++) {
            int rotX = y;
            int rotY = SCREEN_WIDTH - x - 1;
            
            int cellX = rotX / cellSize;
            int cellY = rotY / cellSize;
            int isBlue = (cellX + cellY) % 2;
            
            u8 r = isBlue ? 65 : 255;
            u8 g = isBlue ? 105 : 255;
            u8 b = isBlue ? 225 : 255;
            
            int offset = (x * FB_WIDTH + (FB_HEIGHT - 1 - y)) * 3;
            
            if (offset >= 0 && offset < FB_WIDTH * FB_HEIGHT * 3 - 2) {
                buffer[offset + 0] = b;
                buffer[offset + 1] = g;
                buffer[offset + 2] = r;
            }
        }
    }
}

void compositeImage(u8* dest, u8* bottom, u8* top, u8* mask) {
    for (int i = 0; i < FB_WIDTH * FB_HEIGHT; i++) {
        int pixelIdx = i * 3;
        u8 alpha = mask[i];
        
        dest[pixelIdx + 0] = (bottom[pixelIdx + 0] * (255 - alpha) + top[pixelIdx + 0] * alpha) / 255;
        dest[pixelIdx + 1] = (bottom[pixelIdx + 1] * (255 - alpha) + top[pixelIdx + 1] * alpha) / 255;
        dest[pixelIdx + 2] = (bottom[pixelIdx + 2] * (255 - alpha) + top[pixelIdx + 2] * alpha) / 255;
    }
}

void scratchAt(int touchX, int touchY, int brushSize) {
    if (touchX < 0 || touchX >= 320 || touchY < 0 || touchY >= 240) return;
    
    for (int dx = -brushSize; dx <= brushSize; dx++) {
        for (int dy = -brushSize; dy <= brushSize; dy++) {
            int px = touchX + dx;
            int py = touchY + dy;
            
            if (px < 0 || px >= 320 || py < 0 || py >= 240) continue;
            if (dx*dx + dy*dy <= brushSize*brushSize) {
                int maskIdx = px * 240 + (239 - py);
                if (maskIdx >= 0 && maskIdx < FB_WIDTH * FB_HEIGHT) {
                    scratchMask[maskIdx] = 0;
                }
            }
        }
    }
}

int main(int argc, char **argv) {
    gfxInitDefault();
    gfxSet3D(true);
    
    generateCheckerboard(baseImage, 20);
    generateRotatedCheckerboard(rotatedImage, 20);
    memset(scratchMask, 255, sizeof(scratchMask));
    
    u8* compositeBuffer = (u8*)malloc(FB_WIDTH * FB_HEIGHT * 3);
    u8* topScreenBuffer = (u8*)malloc(240 * 400 * 3);
    
    int brushSize = 5;
    bool layerSwapped = false;
    bool wasTouching = false;

    while (aptMainLoop()) {
        hidScanInput();
        u32 kDown = hidKeysDown();
        u32 kHeld = hidKeysHeld();

        if (kDown & KEY_START) break;

        // Clear canvas with X
        if (kDown & KEY_X) {
            memset(scratchMask, 255, sizeof(scratchMask));
            undoTop = redoTop = 0;
        }

        // Toggle layer order
        if (kDown & KEY_B) layerSwapped = !layerSwapped;

        // Brush size
        if (kDown & KEY_DUP) { brushSize++; if (brushSize > 50) brushSize = 50; }
        if (kDown & KEY_DDOWN) { brushSize--; if (brushSize < 1) brushSize = 1; }

        // Undo/Redo
        if (kDown & KEY_L) undo();
        if (kDown & KEY_R) redo();

        // Touch input
        if (kHeld & KEY_TOUCH) {
            touchPosition touch;
            hidTouchRead(&touch);

            if (!wasTouching) pushUndo(); // Save state before stroke
            scratchAt(touch.px, touch.py, brushSize);
            wasTouching = true;
        } else {
            wasTouching = false;
        }

        // Composite
        if (layerSwapped) compositeImage(compositeBuffer, baseImage, rotatedImage, scratchMask);
        else compositeImage(compositeBuffer, rotatedImage, baseImage, scratchMask);

        // Draw bottom screen
        u8* fbBottom = gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, NULL, NULL);
        memcpy(fbBottom, compositeBuffer, FB_WIDTH * FB_HEIGHT * 3);

        // Draw top screens
        u8* fbTopLeft = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL);
        memset(topScreenBuffer, 0, 240 * 400 * 3);
        for (int x = 0; x < 320; x++) {
            for (int y = 0; y < 240; y++) {
                int srcIdx = (x * 240 + (239 - y)) * 3;
                int dstX = x + 40;
                int dstIdx = (dstX * 240 + (239 - y)) * 3;
                memcpy(&topScreenBuffer[dstIdx], &compositeBuffer[srcIdx], 3);
            }
        }
        memcpy(fbTopLeft, topScreenBuffer, 240 * 400 * 3);

        u8* fbTopRight = gfxGetFramebuffer(GFX_TOP, GFX_RIGHT, NULL, NULL);
        memset(topScreenBuffer, 0, 240 * 400 * 3);
        int depthOffset = 3;
        for (int x = 0; x < 320; x++) {
            for (int y = 0; y < 240; y++) {
                int srcIdx = (x * 240 + (239 - y)) * 3;
                int maskIdx = x * 240 + (239 - y);
                u8 alpha = scratchMask[maskIdx];
                int dstX = (alpha > 128 ? x + 40 + depthOffset : x + 40);
                if (dstX >= 0 && dstX < 400) {
                    int dstIdx = (dstX * 240 + (239 - y)) * 3;
                    memcpy(&topScreenBuffer[dstIdx], &compositeBuffer[srcIdx], 3);
                }
            }
        }
        memcpy(fbTopRight, topScreenBuffer, 240 * 400 * 3);

        gfxFlushBuffers();
        gfxSwapBuffers();
        gspWaitForVBlank();
    }

    free(compositeBuffer);
    free(topScreenBuffer);
    gfxExit();
    return 0;
}
