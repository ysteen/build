/* GBA MULTI cable fixture. Display helpers come from the MPL-2.0 linktest. */
#define SRAM ((volatile u8*)0x0E000000)

static void store32(int offset, u32 value) {
    for (int i = 0; i < 4; i++) SRAM[offset + i] = value >> (8 * i);
}

static u32 load32(int offset) {
    u32 value = 0;
    for (int i = 0; i < 4; i++) value |= (u32)SRAM[offset + i] << (8 * i);
    return value;
}

static void draw_value(int y, u32 value, int width, int hex, u16 color) {
    for (int digit = width - 1; digit >= 0; digit--) {
        unsigned n = hex ? value & 15 : value % 10;
        value = hex ? value >> 4 : value / 10;
        const u8* glyph = font_bitmap[n < 10 ? '0' + n : 'A' + n - 10];
        for (int row = 0; row < 7; row++) {
            volatile u16* pixels = &VRAM[(y + row) * 240 + 112 + digit * 6];
            for (int col = 0; col < 5; col++)
                pixels[col] = glyph[row] & (16 >> col) ? color : C_BLACK;
        }
    }
}

int main(void) {
    REG_DISPCNT = MODE3;
    for (int i = 0; i < 240 * 160; i++) VRAM[i] = C_BLACK;
    font_init();
    draw_str(8, 8, "ROMM GBA LINK TEST", C_WHITE);
    draw_str(8, 24, "CARTRIDGE", C_GRAY);
    draw_dec(80, 24, FIXTURE_ID, 1, C_CYAN);
    draw_str(8, 40, "FRAME", C_GRAY);
    draw_str(8, 56, "LOCAL KEYS", C_GRAY);
    draw_str(8, 72, "PEER DATA", C_GRAY);
    draw_str(8, 88, "RECEIVED", C_GRAY);
    draw_str(8, 104, "SAVED A PRESSES", C_GRAY);
    draw_str(8, 128, "A: SAVE  ARROWS: SEND", C_WHITE);
    draw_str(8, 144, "INPUT ONLY NETPLAY", C_GRAY);
    if (SRAM[0] != 0x47 || SRAM[1] != 0x42 || SRAM[2] != FIXTURE_ID) {
        SRAM[0] = 0x47; SRAM[1] = 0x42; SRAM[2] = FIXTURE_ID;
        store32(4, 0);
    }
    REG_RCNT = 0;
    REG_SIOCNT = SI_MULTI_MODE;
    /* Keep a square wave active while cable snapshots are restored. */
    *(volatile u16*)0x04000084 = 0x80;
    *(volatile u16*)0x04000080 = 0x2277;
    *(volatile u16*)0x04000082 = 2;
    *(volatile u16*)0x04000068 = 0xF080;
    *(volatile u16*)0x0400006C = 0x8700;
    u32 frame = 0, received = 0;
    u16 previous = 0;
    while (1) {
        while (REG_VCOUNT >= 160) {}
        while (REG_VCOUNT < 160) {}
        frame++;
        u16 keys = (~REG_KEYINPUT) & 0x3FF;
        int id = (REG_SIOCNT >> 4) & 3;
        u16 peer = id == 0 ? REG_SIOMULTI1 : REG_SIOMULTI0;
        if (peer != 0xFFFF && (peer >> 12) != 0) received++;
        if ((keys & 1) && !(previous & 1)) store32(4, load32(4) + 1);
        previous = keys;
        store32(16, frame);
        store32(20, received);
        store32(24, keys);
        store32(28, peer);
        store32(32, id);
        for (int slot = 0; slot < 4; slot++)
            store32(36 + slot * 4, ((volatile u16*)0x04000120)[slot]);
        draw_value(40, frame, 8, 0, C_WHITE);
        draw_value(56, keys, 4, 1, C_CYAN);
        draw_value(72, peer, 4, 1, C_GREEN);
        draw_value(88, received, 8, 0, C_GREEN);
        draw_value(104, load32(4), 8, 0, C_YELLOW);
        REG_SIOMLT_SEND = (FIXTURE_ID << 12) | keys;
        if (id == 0 && !(REG_SIOCNT & SI_BUSY))
            REG_SIOCNT = SI_MULTI_MODE | SI_BUSY;
    }
}
