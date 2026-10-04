/* Software PlayStation GPU (VRAM + GP0 drawing commands). */
#ifndef PORT_GPU_H
#define PORT_GPU_H

extern unsigned short gpu_vram[512][1024];

/* Executes `count` GP0 words (one or more commands). */
void gpu_gp0(const unsigned long *words, int count);
/* Walks an ordering table (PsyQ DrawOTag). */
void gpu_draw_otag(const unsigned long *ot);

void gpu_load_image(int x, int y, int w, int h, const void *src);
void gpu_store_image(int x, int y, int w, int h, void *dst);
void gpu_move_image(int sx, int sy, int dx, int dy, int w, int h);
void gpu_fill(int x, int y, int w, int h, int r, int g, int b);

/* Drawing environment (PutDrawEnv) */
void gpu_set_draw_area(int x0, int y0, int x1, int y1);
void gpu_set_draw_offset(int x, int y);
void gpu_set_texpage(unsigned short tpage);
void gpu_set_texwindow(int x, int y, int w, int h);
void gpu_set_dither(int on);

/* Display (PutDispEnv / SetDispMask) */
void gpu_set_display(int x, int y, int w, int h, int rgb24);
void gpu_set_display_enabled(int on);
/* Converts the display area to RGBA and hands it to the host. */
void gpu_present(void);

#endif
