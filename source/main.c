#include <3ds.h>
#include <citro2d.h>
#include <citro3d.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <math.h>
#include <tex3ds.h>
#include <dirent.h>

// Screen dimensions - 3DS has 320x240 bottom screen, 400x240 top screen
#define SCREEN_WIDTH 320
#define SCREEN_HEIGHT 240
#define FB_WIDTH 240      // Framebuffer width (rotated 90°)
#define FB_HEIGHT 320     // Framebuffer height (rotated 90°)

#define MAX_HISTORY 20    // Maximum number of undo/redo steps to store
#define MAX_INSTRUCTION_LINES 15  // Number of text lines in instructions

// Gallery configuration
#define MAX_GALLERY_IMAGES 50
#define THUMBNAIL_WIDTH 80
#define THUMBNAIL_HEIGHT 60
#define THUMBNAILS_PER_ROW 4
#define THUMBNAIL_SPACING 10

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

static C2D_SpriteSheet spriteSheet;
static C2D_Image logoImage;
static bool logoLoaded = false;

// Citro2D render targets and text buffers
static C3D_RenderTarget* topTarget;
static C3D_RenderTarget* bottomTarget;
static C2D_TextBuf staticTextBuf;
static C2D_Text instructionTexts[MAX_INSTRUCTION_LINES];

// Gallery structures
typedef struct {
    char filename[256];
    u8* thumbnailData;  // RGB thumbnail data (THUMBNAIL_WIDTH * THUMBNAIL_HEIGHT * 3)
    bool loaded;
} GalleryImage;

GalleryImage galleryImages[MAX_GALLERY_IMAGES];
int galleryImageCount = 0;
int selectedGalleryIndex = 0;
int galleryScrollOffset = 0;

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
bool showGallery = false;         // Show gallery screen
float depthOffset = 3.0f;         // 3D stereoscopic depth offset

// Previous touch position for line interpolation (smooth drawing)
int prevTouchX = -1;
int prevTouchY = -1;

/**
 * GALLERY FUNCTIONS
 */

/**
 * Simple BMP header reading to get dimensions
 */
bool readBMPHeader(FILE* file, u32* width, u32* height) {
    fseek(file, 0, SEEK_SET);
    
    // Read magic number
    char magic[2];
    fread(magic, 1, 2, file);
    if (magic[0] != 'B' || magic[1] != 'M') return false;
    
    // Skip to width/height (offset 18)
    fseek(file, 18, SEEK_SET);
    fread(width, 4, 1, file);
    fread(height, 4, 1, file);
    
    return true;
}

/**
 * Load a downsampled thumbnail from a BMP file
 * Simple nearest-neighbor sampling for speed
 */
bool loadThumbnail(const char* filename, u8* thumbnailData) {
    FILE* file = fopen(filename, "rb");
    if (!file) return false;
    
    u32 width, height;
    if (!readBMPHeader(file, &width, &height)) {
        fclose(file);
        return false;
    }
    
    // Calculate sampling ratios
    float xRatio = (float)width / THUMBNAIL_WIDTH;
    float yRatio = (float)height / THUMBNAIL_HEIGHT;
    
    // Allocate temporary buffer for full image row
    u8* rowBuffer = (u8*)malloc(width * 3);
    if (!rowBuffer) {
        fclose(file);
        return false;
    }
    
    // BMP data starts at offset 54
    fseek(file, 54, SEEK_SET);
    
    // Sample pixels for thumbnail
    for (int thumbY = 0; thumbY < THUMBNAIL_HEIGHT; thumbY++) {
        int srcY = (int)(thumbY * yRatio);
        
        // BMP stores bottom-to-top, so invert Y
        int fileY = height - 1 - srcY;
        fseek(file, 54 + (fileY * width * 3), SEEK_SET);
        fread(rowBuffer, 1, width * 3, file);
        
        for (int thumbX = 0; thumbX < THUMBNAIL_WIDTH; thumbX++) {
            int srcX = (int)(thumbX * xRatio);
            int srcIdx = srcX * 3;
            int dstIdx = (thumbY * THUMBNAIL_WIDTH + thumbX) * 3;
            
            // BMP is BGR, convert to RGB for easier handling
            thumbnailData[dstIdx + 0] = rowBuffer[srcIdx + 2];  // R
            thumbnailData[dstIdx + 1] = rowBuffer[srcIdx + 1];  // G
            thumbnailData[dstIdx + 2] = rowBuffer[srcIdx + 0];  // B
        }
    }
    
    free(rowBuffer);
    fclose(file);
    return true;
}

/**
 * Scan SD card for sqribble BMP files and load thumbnails
 */
void scanGalleryImages() {
    DIR* dir = opendir("sdmc:/");
    if (!dir) return;
    
    struct dirent* entry;
    galleryImageCount = 0;
    
    while ((entry = readdir(dir)) != NULL && galleryImageCount < MAX_GALLERY_IMAGES) {
        // Check if filename starts with "sqribble_" and ends with ".bmp"
        if (strncmp(entry->d_name, "sqribble_", 9) == 0) {
            size_t len = strlen(entry->d_name);
            if (len > 4 && strcmp(entry->d_name + len - 4, ".bmp") == 0) {
                // Store full path
                snprintf(galleryImages[galleryImageCount].filename, 256, 
                         "sdmc:/%s", entry->d_name);
                
                // Allocate thumbnail buffer
                galleryImages[galleryImageCount].thumbnailData = 
                    (u8*)malloc(THUMBNAIL_WIDTH * THUMBNAIL_HEIGHT * 3);
                
                // Load thumbnail
                galleryImages[galleryImageCount].loaded = 
                    loadThumbnail(galleryImages[galleryImageCount].filename,
                                galleryImages[galleryImageCount].thumbnailData);
                
                if (galleryImages[galleryImageCount].loaded) {
                    galleryImageCount++;
                } else {
                    // Failed to load, free buffer
                    free(galleryImages[galleryImageCount].thumbnailData);
                }
            }
        }
    }
    
    closedir(dir);
}

/**
 * Free all gallery thumbnail data
 */
void freeGalleryImages() {
    for (int i = 0; i < galleryImageCount; i++) {
        if (galleryImages[i].thumbnailData) {
            free(galleryImages[i].thumbnailData);
            galleryImages[i].thumbnailData = NULL;
        }
    }
    galleryImageCount = 0;
}

/**
 * Load a saved drawing into the current canvas
 */
bool loadDrawing(const char* filename) {
    FILE* file = fopen(filename, "rb");
    if (!file) return false;
    
    u32 width, height;
    if (!readBMPHeader(file, &width, &height)) {
        fclose(file);
        return false;
    }
    
    // Only load if dimensions match (320x240)
    if (width != 320 || height != 240) {
        fclose(file);
        return false;
    }
    
    // Allocate temporary buffer for pixel data
    u8* pixelData = (u8*)malloc(width * height * 3);
    if (!pixelData) {
        fclose(file);
        return false;
    }
    
    // Read pixel data (starting at offset 54)
    fseek(file, 54, SEEK_SET);
    fread(pixelData, 1, width * height * 3, file);
    fclose(file);
    
    // Convert to framebuffer format
    // BMP is stored bottom-to-top, BGR format
    for (int y = 0; y < 240; y++) {
        for (int x = 0; x < 320; x++) {
            int bmpIdx = ((239 - y) * 320 + x) * 3;  // BMP bottom-to-top
            int fbIdx = (x * 240 + (239 - y)) * 3;   // Framebuffer format
            
            // Copy BGR data to BOTH layers so drawing reveals the same image
            baseImage[fbIdx + 0] = pixelData[bmpIdx + 0];  // B
            baseImage[fbIdx + 1] = pixelData[bmpIdx + 1];  // G
            baseImage[fbIdx + 2] = pixelData[bmpIdx + 2];  // R
            
            // KEY FIX: Copy to rotatedImage as well
            rotatedImage[fbIdx + 0] = pixelData[bmpIdx + 0];  // B
            rotatedImage[fbIdx + 1] = pixelData[bmpIdx + 1];  // G
            rotatedImage[fbIdx + 2] = pixelData[bmpIdx + 2];  // R
        }
    }
    
    // Clear scratch mask to show loaded image
    memset(scratchMask, 255, sizeof(scratchMask));
    
    // Clear undo/redo stacks when loading new image
    undoTop = 0;
    redoTop = 0;
    
    free(pixelData);
    return true;
}

/**
 * Draw gallery thumbnails on top screen using direct framebuffer rendering
 */
void drawGallery(u8* framebuffer) {
    // Clear framebuffer to dark background
    memset(framebuffer, 20, 240 * 400 * 3);
    
    if (galleryImageCount == 0) {
        // No images - could draw "No saved images" text here
        return;
    }
    
    // Calculate visible range
    int imagesPerScreen = THUMBNAILS_PER_ROW * 2;  // 2 rows visible
    int startIdx = galleryScrollOffset;
    int endIdx = startIdx + imagesPerScreen;
    if (endIdx > galleryImageCount) endIdx = galleryImageCount;
    
    // Draw thumbnails in grid
    for (int i = startIdx; i < endIdx; i++) {
        if (!galleryImages[i].loaded) continue;
        
        int gridIdx = i - startIdx;
        int row = gridIdx / THUMBNAILS_PER_ROW;
        int col = gridIdx % THUMBNAILS_PER_ROW;
        
        // Calculate position (centered on 400px screen)
        int startX = 20 + col * (THUMBNAIL_WIDTH + THUMBNAIL_SPACING);
        int startY = 60 + row * (THUMBNAIL_HEIGHT + THUMBNAIL_SPACING);
        
        // Draw thumbnail
        u8* thumbData = galleryImages[i].thumbnailData;
        for (int ty = 0; ty < THUMBNAIL_HEIGHT; ty++) {
            for (int tx = 0; tx < THUMBNAIL_WIDTH; tx++) {
                int thumbIdx = (ty * THUMBNAIL_WIDTH + tx) * 3;
                
                // Convert to framebuffer coordinates (rotated 90°)
                int fbX = startX + tx;
                int fbY = startY + ty;
                int fbIdx = (fbX * 240 + (239 - fbY)) * 3;
                
                if (fbIdx >= 0 && fbIdx < 240 * 400 * 3 - 2) {
                    // Convert RGB to BGR for framebuffer
                    framebuffer[fbIdx + 0] = thumbData[thumbIdx + 2];  // B
                    framebuffer[fbIdx + 1] = thumbData[thumbIdx + 1];  // G
                    framebuffer[fbIdx + 2] = thumbData[thumbIdx + 0];  // R
                }
            }
        }
        
        // Draw selection border
        if (i == selectedGalleryIndex) {
            // Draw cyan border around selected thumbnail
            for (int tx = 0; tx < THUMBNAIL_WIDTH; tx++) {
                for (int by = 0; by < 3; by++) {  // Border thickness
                    // Top border
                    int fbX1 = startX + tx;
                    int fbY1 = startY + by;
                    int fbIdx1 = (fbX1 * 240 + (239 - fbY1)) * 3;
                    if (fbIdx1 >= 0 && fbIdx1 < 240 * 400 * 3 - 2) {
                        framebuffer[fbIdx1 + 0] = 255;  // B
                        framebuffer[fbIdx1 + 1] = 255;  // G
                        framebuffer[fbIdx1 + 2] = 0;    // R (cyan)
                    }
                    
                    // Bottom border
                    int fbY2 = startY + THUMBNAIL_HEIGHT - 1 - by;
                    int fbIdx2 = (fbX1 * 240 + (239 - fbY2)) * 3;
                    if (fbIdx2 >= 0 && fbIdx2 < 240 * 400 * 3 - 2) {
                        framebuffer[fbIdx2 + 0] = 255;
                        framebuffer[fbIdx2 + 1] = 255;
                        framebuffer[fbIdx2 + 2] = 0;
                    }
                }
            }
            
            for (int ty = 0; ty < THUMBNAIL_HEIGHT; ty++) {
                for (int bx = 0; bx < 3; bx++) {  // Border thickness
                    // Left border
                    int fbX1 = startX + bx;
                    int fbY1 = startY + ty;
                    int fbIdx1 = (fbX1 * 240 + (239 - fbY1)) * 3;
                    if (fbIdx1 >= 0 && fbIdx1 < 240 * 400 * 3 - 2) {
                        framebuffer[fbIdx1 + 0] = 255;
                        framebuffer[fbIdx1 + 1] = 255;
                        framebuffer[fbIdx1 + 2] = 0;
                    }
                    
                    // Right border
                    int fbX2 = startX + THUMBNAIL_WIDTH - 1 - bx;
                    int fbIdx2 = (fbX2 * 240 + (239 - fbY1)) * 3;
                    if (fbIdx2 >= 0 && fbIdx2 < 240 * 400 * 3 - 2) {
                        framebuffer[fbIdx2 + 0] = 255;
                        framebuffer[fbIdx2 + 1] = 255;
                        framebuffer[fbIdx2 + 2] = 0;
                    }
                }
            }
        }
    }
}

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
 * Load logo sprite sheet from romfs
 * Place your logo.t3x file in romfs/gfx/
 * Returns true if successful
 */
bool loadLogo() {
    // Initialize romfs (file system for embedded resources)
    Result rc = romfsInit();
    if (R_FAILED(rc)) {
        return false;
    }
    
    // Load sprite sheet from romfs
    spriteSheet = C2D_SpriteSheetLoad("romfs:/gfx/menu.t3x");
    if (!spriteSheet) {
        romfsExit();
        return false;
    }
    
    // Get first image from sprite sheet (index 0)
    logoImage = C2D_SpriteSheetGetImage(spriteSheet, 0);
    logoLoaded = true;
    
    return true;
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
 * CITRO2D INSTRUCTION SCREEN INITIALIZATION
 * 
 * Pre-parse all static instruction text once for efficient repeated rendering.
 * Text is optimized after parsing to improve GPU performance.
 */
void initInstructionText() {
    // Parse each line of instruction text
    C2D_TextParse(&instructionTexts[0], staticTextBuf, "SQRIBBLE 3DS");
    C2D_TextOptimize(&instructionTexts[0]);
    
    C2D_TextParse(&instructionTexts[1], staticTextBuf, "v1.0");
    C2D_TextOptimize(&instructionTexts[1]);
    
    C2D_TextParse(&instructionTexts[2], staticTextBuf, "BASIC CONTROLS:");
    C2D_TextOptimize(&instructionTexts[2]);
    
    C2D_TextParse(&instructionTexts[3], staticTextBuf, "Touch: Draw");
    C2D_TextOptimize(&instructionTexts[3]);
    
    C2D_TextParse(&instructionTexts[4], staticTextBuf, "L/R: Undo/Redo");
    C2D_TextOptimize(&instructionTexts[4]);
    
    C2D_TextParse(&instructionTexts[5], staticTextBuf, "D-Pad Up/Down: Brush size");
    C2D_TextOptimize(&instructionTexts[5]);
    
    C2D_TextParse(&instructionTexts[6], staticTextBuf, "D-Pad L/R: Cycle primary color");
    C2D_TextOptimize(&instructionTexts[6]);
    
    C2D_TextParse(&instructionTexts[7], staticTextBuf, "A: Cycle Brush shape");
    C2D_TextOptimize(&instructionTexts[7]);
    
    C2D_TextParse(&instructionTexts[8], staticTextBuf, "B: Cycle canvas style");
    C2D_TextOptimize(&instructionTexts[8]);
    
    C2D_TextParse(&instructionTexts[9], staticTextBuf, "X: Clear canvas");
    C2D_TextOptimize(&instructionTexts[9]);
    
    C2D_TextParse(&instructionTexts[10], staticTextBuf, "Y: Save screenshot");
    C2D_TextOptimize(&instructionTexts[10]);
    
    C2D_TextParse(&instructionTexts[11], staticTextBuf, "Circle Pad: 3D depth");
    C2D_TextOptimize(&instructionTexts[11]);
    
    C2D_TextParse(&instructionTexts[12], staticTextBuf, "START: Toggle help");
    C2D_TextOptimize(&instructionTexts[12]);
    
    C2D_TextParse(&instructionTexts[13], staticTextBuf, "SELECT: Open gallery");
    C2D_TextOptimize(&instructionTexts[13]);
    
    C2D_TextParse(&instructionTexts[14], staticTextBuf, "Press any button to begin!");
    C2D_TextOptimize(&instructionTexts[14]);
}

/**
 * CITRO2D INSTRUCTION SCREEN RENDERER
 * 
 * GPU-accelerated instruction screen using Citro2D text rendering.
 * Uses pre-parsed text objects with color formatting and scaling.
 */
void drawInstructionsGPU() {
     // Target top screen "SQRIBBLE 3DS" - ver
    C2D_TargetClear(topTarget, C2D_Color32(0,0,0,0)); // black
    C2D_SceneBegin(topTarget);
    
     if (logoLoaded) {
        // Top screen is 400x240
        float logoWidth = logoImage.subtex->width;
        float logoHeight = logoImage.subtex->height;
        
        // Calculate scale to fill screen while maintaining aspect ratio
        // float scaleX = 400.0f / logoWidth;
        // float scaleY = 240.0f / logoHeight;
        float scaleY = 1.0f;
        float scaleX = 1.0f; // NO SCALE
        
        // Use the larger scale to ensure entire screen is covered
        float scale = (scaleX > scaleY) ? scaleX : scaleY;
        
        // Calculate scaled dimensions
        float scaledWidth = logoWidth * scale;
        float scaledHeight = logoHeight * scale;
        
        // Center the oversized image (this crops edges)
        float posX = (400.0f - scaledWidth) / 2.0f;
        float posY = (240.0f - scaledHeight) / 2.0f;
        
        // Draw the logo - it will fill screen and crop if needed
        C2D_DrawImageAt(logoImage, posX, posY, 0.5f, NULL, scale, scale);
    } else {
        // Fallback to text if logo didn't load
        C2D_DrawText(&instructionTexts[0], C2D_WithColor, 
                     65.0f, 100.0f, 0.5f,
                     1.5f, 1.5f,
                     C2D_Color32(255, 255, 100, 255));
    }

    // version - grey
        C2D_DrawText(&instructionTexts[1], C2D_WithColor,
                 370.0f, 220.0f, 0.5f,
                 0.5f, 0.5f,
                 C2D_Color32(150, 150, 150, 255));  // Gray
        
    // Target bottom screen
    C2D_TargetClear(bottomTarget, C2D_Color32(0,0,0,0));  // Dark gray background
    C2D_SceneBegin(bottomTarget);
    
    // Section header - "basic controls"
    C2D_DrawText(&instructionTexts[2], C2D_WithColor,
                 10.0f, 20.0f, 0.5f,
                 0.6f, 0.6f, // scale
                 C2D_Color32(65, 105, 225, 255));
    
    // Control lines - white, smaller
    float yPos = 38.0f;
    float lineSpacing = 14.0f;
    float controlScale = 0.5f;
    
    for (int i = 3; i < 14; i++) {  // Lines 3-13 are controls
        C2D_DrawText(&instructionTexts[i], C2D_WithColor,
                     10.0f, yPos, 0.5f,
                     controlScale, controlScale,
                     C2D_Color32(255, 255, 255, 255));  // White
        yPos += lineSpacing;
    }
    
    // Prompt at bottom - cyan
    C2D_DrawText(&instructionTexts[14], C2D_WithColor,
                 65.0f, 215.0f, 0.5f,
                 0.6f, 0.6f,
                 C2D_Color32(100, 255, 255, 255));  // Cyan
}

/**
 * Draw gallery instructions on bottom screen
 */
void drawGalleryInstructions(u8* framebuffer) {
    // Fill with dark background
    for (int i = 0; i < FB_WIDTH * FB_HEIGHT * 3; i += 3) {
        framebuffer[i + 0] = 20;   // B
        framebuffer[i + 1] = 20;   // G
        framebuffer[i + 2] = 20;   // R
    }
    
    // Draw a colored header bar at top
    for (int x = 0; x < 320; x++) {
        for (int y = 0; y < 40; y++) {
            int idx = (x * 240 + (239 - y)) * 3;
            if (idx >= 0 && idx < FB_WIDTH * FB_HEIGHT * 3 - 2) {
                framebuffer[idx + 0] = 225;  // B - Royal Blue
                framebuffer[idx + 1] = 105;  // G
                framebuffer[idx + 2] = 65;   // R
            }
        }
    }
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
 * 1. Initialize graphics, Citro2D, and 3D
 * 2. Generate initial patterns
 * 3. Main loop: Process input, update state, render
 * 4. Cleanup and exit
 */
int main(int argc, char **argv) {
    gfxInitDefault();
    gfxSet3D(true);  // Enable stereoscopic 3D rendering
    
    // Initialize Citro3D and Citro2D for GPU-accelerated rendering
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
    C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
    C2D_Prepare();
    
    // Create render targets for each screen
    topTarget = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    bottomTarget = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
    
    // Create text buffer and initialize instruction text
    staticTextBuf = C2D_TextBufNew(4096);
    initInstructionText();

    loadLogo();
    
    // Scan for saved images on startup
    scanGalleryImages();

    // Generate initial checkerboard patterns (20px cells)
    generateCheckerboard(baseImage, 20);
    generateRotatedCheckerboard(rotatedImage, 20);
    memset(scratchMask, 255, sizeof(scratchMask));  // Start fully opaque (top layer visible)
    
    // Allocate working buffers for rendering
    u8* compositeBuffer = (u8*)malloc(FB_WIDTH * FB_HEIGHT * 3);
    u8* topScreenBuffer = (u8*)malloc(240 * 400 * 3);
    
    int brushSize = 5;
    bool wasTouching = false;

    // Main game loop - runs until user exits
    while (aptMainLoop()) {
        hidScanInput();
        u32 kDown = hidKeysDown();   // Buttons pressed this frame
        u32 kHeld = hidKeysHeld();   // Buttons held down

        // START button toggles instructions screen on/off
        if (kDown & KEY_START) {
            if (showGallery) {
                // If gallery is open, close it first
                showGallery = false;
            }
            showInstructions = !showInstructions;
            allowDrawing = !showInstructions && !showGallery;
        }

        // SELECT button toggles gallery screen on/off
        if (kDown & KEY_SELECT) {
            if (showInstructions) {
                // If instructions are open, close them first
                showInstructions = false;
            }
            
            // Toggle gallery state
            showGallery = !showGallery;
            
            // Rescan gallery when opening
            if (showGallery) {
                freeGalleryImages();
                scanGalleryImages();
                selectedGalleryIndex = 0;
                galleryScrollOffset = 0;
                allowDrawing = false;  // Explicitly disable drawing in gallery
            } else {
                allowDrawing = true;   // Re-enable drawing when closing gallery
            }
        }

        // Any button press (except START/SELECT) dismisses instruction screen
        if (showInstructions && kDown && !(kDown & KEY_START) && !(kDown & KEY_SELECT)) {
            showInstructions = false;
            allowDrawing = false;  // Don't allow drawing on the dismissal tap
        }

        // Auto-enable drawing after dismissing instructions (but not during gallery)
        if (!showInstructions && !showGallery && !allowDrawing && !(kHeld & KEY_TOUCH)) {
            allowDrawing = true;
        }

        // Gallery navigation (only when showing gallery)
        if (showGallery && galleryImageCount > 0) {
            // D-Pad navigation
            if (kDown & KEY_DRIGHT) {
                selectedGalleryIndex++;
                if (selectedGalleryIndex >= galleryImageCount) {
                    selectedGalleryIndex = 0;
                    galleryScrollOffset = 0;
                } else {
                    // Auto-scroll if selection goes off-screen
                    int imagesPerScreen = THUMBNAILS_PER_ROW * 2;
                    if (selectedGalleryIndex >= galleryScrollOffset + imagesPerScreen) {
                        galleryScrollOffset = selectedGalleryIndex - imagesPerScreen + 1;
                    }
                }
            }
            
            if (kDown & KEY_DLEFT) {
                selectedGalleryIndex--;
                if (selectedGalleryIndex < 0) {
                    selectedGalleryIndex = galleryImageCount - 1;
                    int imagesPerScreen = THUMBNAILS_PER_ROW * 2;
                    galleryScrollOffset = (galleryImageCount > imagesPerScreen) ? 
                                         galleryImageCount - imagesPerScreen : 0;
                } else {
                    // Auto-scroll if selection goes off-screen
                    if (selectedGalleryIndex < galleryScrollOffset) {
                        galleryScrollOffset = selectedGalleryIndex;
                    }
                }
            }
            
            if (kDown & KEY_DDOWN) {
                selectedGalleryIndex += THUMBNAILS_PER_ROW;
                if (selectedGalleryIndex >= galleryImageCount) {
                    selectedGalleryIndex = galleryImageCount - 1;
                }
                // Auto-scroll
                int imagesPerScreen = THUMBNAILS_PER_ROW * 2;
                if (selectedGalleryIndex >= galleryScrollOffset + imagesPerScreen) {
                    galleryScrollOffset = selectedGalleryIndex - imagesPerScreen + 1;
                }
            }
            
            if (kDown & KEY_DUP) {
                selectedGalleryIndex -= THUMBNAILS_PER_ROW;
                if (selectedGalleryIndex < 0) {
                    selectedGalleryIndex = 0;
                }
                // Auto-scroll
                if (selectedGalleryIndex < galleryScrollOffset) {
                    galleryScrollOffset = selectedGalleryIndex;
                }
            }
            
            // Circle Pad navigation (smoother)
            circlePosition pos;
            hidCircleRead(&pos);
            
            static int circleDelay = 0;
            if (abs(pos.dx) > 100 || abs(pos.dy) > 100) {
                circleDelay++;
                if (circleDelay > 15) {  // Delay for smooth scrolling
                    if (pos.dx > 100) {  // Right
                        selectedGalleryIndex++;
                        if (selectedGalleryIndex >= galleryImageCount) {
                            selectedGalleryIndex = galleryImageCount - 1;
                        }
                    } else if (pos.dx < -100) {  // Left
                        selectedGalleryIndex--;
                        if (selectedGalleryIndex < 0) {
                            selectedGalleryIndex = 0;
                        }
                    }
                    
                    if (pos.dy > 100) {  // Up
                        selectedGalleryIndex -= THUMBNAILS_PER_ROW;
                        if (selectedGalleryIndex < 0) {
                            selectedGalleryIndex = 0;
                        }
                    } else if (pos.dy < -100) {  // Down
                        selectedGalleryIndex += THUMBNAILS_PER_ROW;
                        if (selectedGalleryIndex >= galleryImageCount) {
                            selectedGalleryIndex = galleryImageCount - 1;
                        }
                    }
                    
                    // Auto-scroll
                    int imagesPerScreen = THUMBNAILS_PER_ROW * 2;
                    if (selectedGalleryIndex >= galleryScrollOffset + imagesPerScreen) {
                        galleryScrollOffset = selectedGalleryIndex - imagesPerScreen + 1;
                    } else if (selectedGalleryIndex < galleryScrollOffset) {
                        galleryScrollOffset = selectedGalleryIndex;
                    }
                    
                    circleDelay = 0;
                }
            } else {
                circleDelay = 0;
            }
            
            // A button loads selected image
            if (kDown & KEY_A) {
                if (loadDrawing(galleryImages[selectedGalleryIndex].filename)) {
                    showGallery = false;
                    allowDrawing = true;
                    // Regenerate the rotated layer to match current mode
                    generateRotatedCheckerboard(rotatedImage, 20);
                }
            }
        }

        // Only process game controls when not showing instructions or gallery
        if (!showInstructions && !showGallery) {
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
        
        if (showInstructions) {
            // Begin Citro3D frame for GPU rendering
            C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
            
            // Render GPU-accelerated instruction screen
            drawInstructionsGPU();
            
            // End frame and display
            C3D_FrameEnd(0);
        } else if (showGallery) {
            // Render gallery to top screen using framebuffer
            u8* fbTopLeft = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL);
            drawGallery(fbTopLeft);
            
            // Mirror to right eye for 3D (no parallax needed for menu)
            u8* fbTopRight = gfxGetFramebuffer(GFX_TOP, GFX_RIGHT, NULL, NULL);
            memcpy(fbTopRight, fbTopLeft, 240 * 400 * 3);
            
            // Render gallery instructions to bottom screen
            u8* fbBottom = gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, NULL, NULL);
            drawGalleryInstructions(fbBottom);
            
            gfxFlushBuffers();
            gfxSwapBuffers();
        } else {
            // Use traditional framebuffer rendering for game canvas
            // Step 1: Composite the two layers based on scratch mask
            compositeImage(compositeBuffer, rotatedImage, baseImage, scratchMask);

            // Step 2: Render to bottom screen (touch screen) using framebuffer
            u8* fbBottom = gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, NULL, NULL);
            memcpy(fbBottom, compositeBuffer, FB_WIDTH * FB_HEIGHT * 3);

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
            
            // Flush framebuffers for non-Citro rendering
            gfxFlushBuffers();
            gfxSwapBuffers();
        }
        
        // VSync wait
        gspWaitForVBlank();  // Sync to 60fps
    }

    // Cleanup gallery resources
    freeGalleryImages();
    
    // Cleanup logo resources
    if (logoLoaded) {
        C2D_SpriteSheetFree(spriteSheet);
        romfsExit();
    }

    // Cleanup: Free allocated memory and exit
    free(compositeBuffer);
    free(topScreenBuffer);
    
    // Cleanup Citro2D/3D
    C2D_TextBufDelete(staticTextBuf);
    C2D_Fini();
    C3D_Fini();
    
    gfxExit();
    return 0;
}