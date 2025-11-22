#include "raylib.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_LINE_LEN 8192
#define FONT_SIZE 25
#define ROW_HEIGHT 40.0f
#define HEADER_HEIGHT 48.0f

typedef struct {
    char **cells;
    int cellCount;
} Row;

typedef struct {
    char **headers;
    Row *rows;
    Row *original;
    int rowCount;
    int colCount;
    float *colWidths;
    int sortCol;
    bool sortAsc;
    float scrollX;
    float scrollY;
    int hoverRow;
    int hoverCol;
    float hoverAlpha;
    char message[256];
    double messageUntil;
    Font font;
    bool fontLoaded;
    bool draggingH;
    bool draggingV;
    float dragOffsetX;
    float dragOffsetY;
} Table;

static char *str_dup(const char *s) {
    if (!s) return NULL;
    size_t len = strlen(s);
    char *out = malloc(len + 1);
    if (!out) return NULL;
    memcpy(out, s, len + 1);
    return out;
}

static void free_rows(Row *rows, int count, int colCount) {
    if (!rows) return;
    for (int i = 0; i < count; i++) {
        if (rows[i].cells) {
            for (int c = 0; c < colCount; c++) free(rows[i].cells[c]);
            free(rows[i].cells);
        }
    }
    free(rows);
}

static char **split_tsv_line(const char *line, int *outCount) {
    int cap = 8;
    int count = 0;
    char **cells = malloc(sizeof(char *) * cap);
    const char *p = line;
    const char *start = p;
    while (1) {
        if (*p == '\t' || *p == '\0' || *p == '\n' || *p == '\r') {
            int len = (int)(p - start);
            char *cell = malloc(len + 1);
            memcpy(cell, start, len);
            cell[len] = '\0';
            if (count >= cap) {
                cap *= 2;
                cells = realloc(cells, sizeof(char *) * cap);
            }
            cells[count++] = cell;
            if (*p == '\0' || *p == '\n' || *p == '\r') break;
            start = p + 1;
        }
        p++;
    }
    *outCount = count;
    return cells;
}

static bool load_tsv(FILE *f, Table *t) {
    char buf[MAX_LINE_LEN];
    int lineIndex = 0;
    int colCount = 0;
    Row *rows = NULL;
    int rowCap = 0;
    while (fgets(buf, sizeof(buf), f)) {
        int count = 0;
        char **cells = split_tsv_line(buf, &count);
        if (count > colCount) colCount = count;
        if (lineIndex == 0) {
            t->headers = cells;
        } else {
            if (t->rowCount >= rowCap) {
                rowCap = rowCap == 0 ? 64 : rowCap * 2;
                rows = realloc(rows, sizeof(Row) * rowCap);
            }
            rows[t->rowCount].cells = cells;
            rows[t->rowCount].cellCount = count;
            t->rowCount++;
        }
        lineIndex++;
    }
    if (lineIndex == 0) return false;
    t->colCount = colCount;
    t->rows = rows;
    return true;
}

static void pad_rows(Table *t) {
    for (int i = 0; i < t->rowCount; i++) {
        Row *r = &t->rows[i];
        r->cells = realloc(r->cells, sizeof(char *) * t->colCount);
        for (int c = r->cellCount; c < t->colCount; c++) {
            r->cells[c] = str_dup("");
        }
        r->cellCount = t->colCount;
    }
    // Pad headers
    t->headers = realloc(t->headers, sizeof(char *) * t->colCount);
    for (int c = 0; c < t->colCount; c++) {
        if (t->headers[c] == NULL) {
            char tmp[32];
            snprintf(tmp, sizeof(tmp), "Col %d", c + 1);
            t->headers[c] = str_dup(tmp);
        }
    }
}

static void copy_original(Table *t) {
    t->original = malloc(sizeof(Row) * t->rowCount);
    for (int i = 0; i < t->rowCount; i++) {
        t->original[i].cells = malloc(sizeof(char *) * t->colCount);
        t->original[i].cellCount = t->colCount;
        for (int c = 0; c < t->colCount; c++) {
            t->original[i].cells[c] = str_dup(t->rows[i].cells[c]);
        }
    }
}

static void restore_original(Table *t) {
    free_rows(t->rows, t->rowCount, t->colCount);
    t->rows = malloc(sizeof(Row) * t->rowCount);
    for (int i = 0; i < t->rowCount; i++) {
        t->rows[i].cells = malloc(sizeof(char *) * t->colCount);
        t->rows[i].cellCount = t->colCount;
        for (int c = 0; c < t->colCount; c++) {
            t->rows[i].cells[c] = str_dup(t->original[i].cells[c]);
        }
    }
    t->sortCol = -1;
    t->sortAsc = true;
    snprintf(t->message, sizeof(t->message), "Sort reset");
    t->messageUntil = GetTime() + 1.2;
}

static void measure_col_widths(Table *t) {
    t->colWidths = malloc(sizeof(float) * t->colCount);
    float minW = 80.0f;
    float padding = 22.0f;
    float indicatorW = MeasureTextEx(t->font, " ^", FONT_SIZE, 1).x;
    for (int c = 0; c < t->colCount; c++) {
        float maxText = MeasureTextEx(t->font, t->headers[c], FONT_SIZE, 1).x;
        for (int r = 0; r < t->rowCount; r++) {
            float w = MeasureTextEx(t->font, t->rows[r].cells[c], FONT_SIZE, 1).x;
            if (w > maxText) maxText = w;
        }
        float span = maxText + padding; // padding
        float withIndicator = maxText + indicatorW + padding;
        if (withIndicator > span) span = withIndicator;
        if (span < minW) span = minW;
        t->colWidths[c] = span;
    }
}

static Table *gSortTable = NULL;

static int cmp_rows_global(const void *a, const void *b) {
    const Row *ra = (const Row *)a;
    const Row *rb = (const Row *)b;
    if (!gSortTable) return 0;
    int col = gSortTable->sortCol;
    const char *va = (col < ra->cellCount) ? ra->cells[col] : "";
    const char *vb = (col < rb->cellCount) ? rb->cells[col] : "";
    int cmp = strcmp(va, vb);
    return gSortTable->sortAsc ? cmp : -cmp;
}

static void sort_by(Table *t, int col) {
    if (col < 0 || col >= t->colCount) return;
    if (t->sortCol == col) {
        t->sortAsc = !t->sortAsc;
    } else {
        t->sortCol = col;
        t->sortAsc = true;
    }
    gSortTable = t;
    qsort(t->rows, t->rowCount, sizeof(Row), cmp_rows_global);
    gSortTable = NULL;
}

static Font load_font(Table *t) {
    const char *path = getenv("RAYLIB_FONT");
    if (path && path[0]) {
        Font f = LoadFontEx(path, FONT_SIZE + 2, NULL, 0);
        if (f.texture.id != 0) {
            t->fontLoaded = true;
            return f;
        }
    }
    const char *candidates[] = {
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
        "/usr/share/fonts/liberation/LiberationSans-Regular.ttf",
        "/usr/share/fonts/truetype/freefont/FreeSans.ttf",
        NULL};

    for (int i = 0; candidates[i] != NULL; i++) {
        if (FileExists(candidates[i])) {
            Font f = LoadFontEx(candidates[i], FONT_SIZE + 2, NULL, 0);
            if (f.texture.id != 0) {
                t->fontLoaded = true;
                return f;
            }
        }
    }

    t->fontLoaded = false;
    return GetFontDefault();
}

static void draw_table(Table *t) {
    float margin = 12.0f;
    float statusY = margin;
    float tableX = margin;
    float tableY = margin + 32.0f;
    float viewW = (float)GetScreenWidth() - margin * 2.0f;
    float viewH = (float)GetScreenHeight() - tableY - margin;

    // Reset button (top right)
    float btnW = 140.0f, btnH = 36.0f;
    Rectangle btn = {viewW + margin - btnW, statusY - 6, btnW, btnH};
    DrawRectangleRec(btn, (Color){45, 70, 110, 255});
    DrawRectangleLinesEx(btn, 1.5f, (Color){160, 200, 255, 255});
    Vector2 textSize = MeasureTextEx(t->font, "Reset Sort", FONT_SIZE, 1);
    float tx = btn.x + (btnW - textSize.x) * 0.5f;
    float ty = btn.y + (btnH - textSize.y) * 0.5f;
    DrawTextEx(t->font, "Reset Sort", (Vector2){tx, ty}, FONT_SIZE, 1, (Color){220, 235, 255, 255});

    // Outer frame
    DrawRectangleLines((int)tableX - 1, (int)tableY - 1, (int)viewW + 2, (int)viewH + 2, (Color){80, 100, 130, 255});

    // Header
    float x = tableX - t->scrollX;
    float y = tableY - t->scrollY;
    float headerTextY = y + (HEADER_HEIGHT - FONT_SIZE) * 0.5f;
    for (int c = 0; c < t->colCount; c++) {
        float w = t->colWidths[c];
        Rectangle rect = {x, y, w, HEADER_HEIGHT};
        DrawRectangleRec(rect, (Color){35, 50, 80, 255});
        DrawRectangleLinesEx(rect, 1.0f, (Color){70, 90, 120, 255});
        char label[256];
        const char *title = t->headers[c] && t->headers[c][0] ? t->headers[c] : "Col";
        if (t->sortCol == c) {
            snprintf(label, sizeof(label), "%s %c", title, t->sortAsc ? '^' : 'v');
        } else {
            snprintf(label, sizeof(label), "%s", title);
        }
        DrawTextEx(t->font, label, (Vector2){x + 6, headerTextY}, FONT_SIZE, 1, (Color){220, 235, 255, 255});
        x += w;
    }

    // Rows
    float startY = y + HEADER_HEIGHT;
    float rowTextOffset = (ROW_HEIGHT - FONT_SIZE) * 0.5f;
    for (int r = 0; r < t->rowCount; r++) {
        float rowY = startY + ROW_HEIGHT * r;
        float cx = tableX - t->scrollX;
        for (int c = 0; c < t->colCount; c++) {
            float w = t->colWidths[c];
            Rectangle rect = {cx, rowY, w, ROW_HEIGHT};
            Color base = (r % 2 == 0) ? (Color){28, 34, 48, 255} : (Color){32, 40, 55, 255};
            if (t->hoverRow == r && t->hoverCol == c && t->hoverAlpha > 0.01f) {
                Color hc = (Color){120, 170, 255, (unsigned char)(100 + 120 * t->hoverAlpha)};
                DrawRectangleRec(rect, hc);
            } else {
                DrawRectangleRec(rect, base);
            }
            DrawRectangleLinesEx(rect, 1.0f, (Color){50, 65, 90, 255});
            if (t->rows[r].cells[c]) {
                DrawTextEx(t->font, t->rows[r].cells[c], (Vector2){cx + 6, rowY + rowTextOffset}, FONT_SIZE, 1, (Color){230, 240, 255, 255});
            }
            cx += w;
        }
    }

    // Scrollbars
    float contentW = 0.0f;
    for (int c = 0; c < t->colCount; c++) contentW += t->colWidths[c];
    float contentH = HEADER_HEIGHT + ROW_HEIGHT * t->rowCount;

    if (contentW > viewW) {
        float barH = 10.0f;
        Rectangle track = {tableX, tableY + viewH - barH, viewW, barH};
        DrawRectangleRec(track, (Color){25, 30, 40, 255});
        float thumbW = viewW * (viewW / contentW);
        if (thumbW < 24) thumbW = 24;
        float thumbX = tableX + (t->scrollX / (contentW - viewW)) * (viewW - thumbW);
        Rectangle thumb = {thumbX, track.y, thumbW, barH};
        DrawRectangleRec(thumb, (Color){120, 160, 220, 200});
    }
    if (contentH > viewH) {
        float barW = 10.0f;
        Rectangle track = {tableX + viewW - barW, tableY, barW, viewH};
        DrawRectangleRec(track, (Color){25, 30, 40, 255});
        float thumbH = viewH * (viewH / contentH);
        if (thumbH < 24) thumbH = 24;
        float thumbY = tableY + (t->scrollY / (contentH - viewH)) * (viewH - thumbH);
        Rectangle thumb = {track.x, thumbY, barW, thumbH};
        DrawRectangleRec(thumb, (Color){120, 160, 220, 200});
    }

    // Sticky status message overlay (top-left)
    if (GetTime() < t->messageUntil && t->message[0]) {
        Vector2 sz = MeasureTextEx(t->font, t->message, FONT_SIZE, 1);
        float pad = 6.0f;
        Rectangle panel = {tableX, 8.0f, sz.x + pad * 2.0f, sz.y + pad * 2.0f};
        DrawRectangleRounded(panel, 0.2f, 6, (Color){15, 20, 30, 200});
        DrawRectangleRoundedLines(panel, 0.2f, 6, (Color){140, 180, 240, 220});
        DrawTextEx(t->font, t->message, (Vector2){panel.x + pad, panel.y + pad}, FONT_SIZE, 1, (Color){220, 235, 255, 255});
    }
}

static void handle_input(Table *t) {
    float margin = 12.0f;
    float tableX = margin;
    float tableY = margin + 32.0f;
    float viewW = (float)GetScreenWidth() - margin * 2.0f;
    float viewH = (float)GetScreenHeight() - tableY - margin;

    // Scroll
    float wheel = GetMouseWheelMove();
    if (IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT)) {
        t->scrollX -= wheel * 40.0f;
    } else {
        t->scrollY -= wheel * 40.0f;
    }

    float contentW = 0.0f;
    for (int c = 0; c < t->colCount; c++) contentW += t->colWidths[c];
    float contentH = HEADER_HEIGHT + ROW_HEIGHT * t->rowCount;
    if (t->scrollX < 0) t->scrollX = 0;
    if (t->scrollY < 0) t->scrollY = 0;
    if (t->scrollX > contentW - viewW) t->scrollX = contentW - viewW;
    if (t->scrollY > contentH - viewH) t->scrollY = contentH - viewH;
    if (contentW < viewW) t->scrollX = 0;
    if (contentH < viewH) t->scrollY = 0;

    Vector2 mouse = GetMousePosition();

    // Scrollbar drag handling
    bool hasH = contentW > viewW;
    bool hasV = contentH > viewH;
    float barH = 10.0f;
    float barW = 10.0f;
    float thumbW = hasH ? viewW * (viewW / contentW) : 0.0f;
    if (thumbW < 24 && hasH) thumbW = 24;
    float thumbH = hasV ? viewH * (viewH / contentH) : 0.0f;
    if (thumbH < 24 && hasV) thumbH = 24;
    float trackHX = tableX;
    float trackHY = tableY + viewH - barH;
    float trackVW = tableX + viewW - barW;
    float trackVY = tableY;

    if (!IsMouseButtonDown(MOUSE_LEFT_BUTTON)) {
        t->draggingH = false;
        t->draggingV = false;
    }

    if (hasH && IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
        float thumbX = trackHX + (t->scrollX / (contentW - viewW)) * (viewW - thumbW);
        Rectangle thumbRect = {thumbX, trackHY, thumbW, barH};
        if (CheckCollisionPointRec(mouse, thumbRect)) {
            t->draggingH = true;
            t->dragOffsetX = mouse.x - thumbRect.x;
        }
    }
    if (hasV && IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
        float thumbY = trackVY + (t->scrollY / (contentH - viewH)) * (viewH - thumbH);
        Rectangle thumbRectV = {trackVW, thumbY, barW, thumbH};
        if (CheckCollisionPointRec(mouse, thumbRectV)) {
            t->draggingV = true;
            t->dragOffsetY = mouse.y - thumbRectV.y;
        }
    }

    if (t->draggingH && hasH) {
        float thumbX = mouse.x - t->dragOffsetX;
        float minX = trackHX;
        float maxX = trackHX + viewW - thumbW;
        if (thumbX < minX) thumbX = minX;
        if (thumbX > maxX) thumbX = maxX;
        float ratio = (thumbX - trackHX) / (viewW - thumbW);
        t->scrollX = ratio * (contentW - viewW);
    }
    if (t->draggingV && hasV) {
        float thumbY = mouse.y - t->dragOffsetY;
        float minY = trackVY;
        float maxY = trackVY + viewH - thumbH;
        if (thumbY < minY) thumbY = minY;
        if (thumbY > maxY) thumbY = maxY;
        float ratio = (thumbY - trackVY) / (viewH - thumbH);
        t->scrollY = ratio * (contentH - viewH);
    }

    // If dragging, skip hover/click logic
    if (t->draggingH || t->draggingV) {
        return;
    }

    // Hover detection
    t->hoverRow = -1;
    t->hoverCol = -1;
    float startX = tableX - t->scrollX;
    float startY = tableY + HEADER_HEIGHT - t->scrollY;
    if (mouse.x >= tableX && mouse.x <= tableX + viewW && mouse.y >= tableY && mouse.y <= tableY + viewH) {
        for (int r = 0; r < t->rowCount; r++) {
            float rowY = startY + ROW_HEIGHT * r;
            if (mouse.y >= rowY && mouse.y <= rowY + ROW_HEIGHT) {
                float cx = startX;
                for (int c = 0; c < t->colCount; c++) {
                    float w = t->colWidths[c];
                    if (mouse.x >= cx && mouse.x <= cx + w) {
                        t->hoverRow = r;
                        t->hoverCol = c;
                        break;
                    }
                    cx += w;
                }
                break;
            }
        }
    }

    // Hover fade
    float target = (t->hoverRow >= 0 && t->hoverCol >= 0) ? 1.0f : 0.0f;
    t->hoverAlpha += (target - t->hoverAlpha) * 0.2f;

    if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
        // Reset button
        float btnW = 110.0f, btnH = 26.0f;
        Rectangle btn = {viewW + margin - btnW, margin - 4, btnW, btnH};
        if (CheckCollisionPointRec(mouse, btn)) {
            restore_original(t);
            return;
        }

        // Header click for sort
        float hx = tableX - t->scrollX;
        float hy = tableY - t->scrollY;
        if (mouse.y >= hy && mouse.y <= hy + HEADER_HEIGHT && mouse.x >= tableX && mouse.x <= tableX + viewW) {
            for (int c = 0; c < t->colCount; c++) {
                float w = t->colWidths[c];
                if (mouse.x >= hx && mouse.x <= hx + w) {
                    sort_by(t, c);
                    return;
                }
                hx += w;
            }
        }

        // Cell click for copy
        if (t->hoverRow >= 0 && t->hoverCol >= 0) {
            const char *text = t->rows[t->hoverRow].cells[t->hoverCol];
            SetClipboardText(text);
            char buf[200];
            snprintf(buf, sizeof(buf), "Copied: %.60s", text);
            strncpy(t->message, buf, sizeof(t->message) - 1);
            t->message[sizeof(t->message) - 1] = '\0';
            t->messageUntil = GetTime() + 1.8;
        }
    }
}

int main(void) {
    Table t = {0};
    t.sortCol = -1;
    t.sortAsc = true;
    t.scrollX = 0;
    t.scrollY = 0;

    if (!load_tsv(stdin, &t)) {
        fprintf(stderr, "No TSV data on stdin.\n");
        return 1;
    }
    pad_rows(&t);
    copy_original(&t);

    SetConfigFlags(FLAG_MSAA_4X_HINT | FLAG_WINDOW_RESIZABLE);
    InitWindow(1200, 800, "TSV Grid (raylib)");
    SetTargetFPS(60);
    t.font = load_font(&t);
    measure_col_widths(&t);

    while (!WindowShouldClose()) {
        handle_input(&t);

        BeginDrawing();
        ClearBackground((Color){20, 20, 28, 255});
        draw_table(&t);
        EndDrawing();
    }

    free_rows(t.rows, t.rowCount, t.colCount);
    free_rows(t.original, t.rowCount, t.colCount);
    if (t.headers) {
        for (int c = 0; c < t.colCount; c++) free(t.headers[c]);
        free(t.headers);
    }
    free(t.colWidths);
    if (t.fontLoaded) {
        UnloadFont(t.font);
    }
    CloseWindow();
    return 0;
}
