/*
 *   Copyright (c) 2025 Jawaid Bazyar

 *   This program is free software: you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation, either version 3 of the License, or
 *   (at your option) any later version.

 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.

 *   You should have received a copy of the GNU General Public License
 *   along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "cpu.hpp"
#include "videx.hpp"
#include "videx_80x24.hpp"
#include "display/display.hpp"

uint32_t videx_color_table[DM_NUM_MONO_MODES] = {
    0xFFFFFFFF, // color, keep it as-is
    0x00C044FF, //0x00e64dFF, // green (10% brighter).
    0xFFBF00FF, // amber.
};

void render_videx_scanline_80x24(cpu_state *cpu, videx_data * videx_d, int y, void *pixels, int pitch) {
    display_state_t *ds = (display_state_t *)get_module_state(cpu, MODULE_DISPLAY);
    //videx_data * videx_d = (videx_data *)get_module_state(cpu, MODULE_VIDEX);
    uint32_t *texturePixels = (uint32_t *)pixels;

    uint32_t color_value = videx_color_table[ds->display_mono_color];

    /**
     * calculate memory address of start of line:
     * y * 80 + reg[VIDEX_REG_START_ADDR_LO] | reg[VIDEX_REG_START_ADDR_HI] >> 8
     * modulus 2048
     */

    uint16_t start_addr_lo = videx_d->reg[R13_START_ADDR_LO];
    uint16_t start_addr_hi = videx_d->reg[R12_START_ADDR_HI];
    uint16_t line_start = (y * 80 + ((uint16_t)start_addr_hi << 8 | start_addr_lo)) % 2048;

    /**
     * if R10[6] is set, cursor blink is enabled
     *    R10[5] = 0: blink at 1/16th rate
     *    R10[5] = 1: blink at 1/32nd rate
     * if R10[6] is clear, no blinking
     *    R10[5] = 0: cursor is displayed
     *    R10[5] = 1: cursor is not displayed
     */
    uint8_t cursor_start = videx_d->reg[R10_CURSOR_START] & 0b00011111;
    uint8_t cursor_end = videx_d->reg[R11_CURSOR_END] & 0b00011111;

    uint16_t cursor_addr_lo = videx_d->reg[R15_CURSOR_LO];
    uint16_t cursor_addr_hi = videx_d->reg[R14_CURSOR_HI];
    uint16_t cursor_pos = ((uint16_t)cursor_addr_hi << 8 | cursor_addr_lo) % 2048;
    bool cursor_blink_mode = (videx_d->reg[R10_CURSOR_START] & 0b01000000) != 0;
    bool cursor_enabled =    (videx_d->reg[R10_CURSOR_START] & 0b00100000) != 0;
    bool cursor_visible = (!cursor_blink_mode && !cursor_enabled) || (cursor_blink_mode && videx_d->cursor_blink_status);   

    //printf("line_start for HI: %02X LO: %02X y: %d: %d\n", start_addr_hi, start_addr_lo, y, line_start);
    for (int ln = 0; ln < 9; ln++) {
        for (int x = 0; x < 80; x++) {
            uint16_t char_addr = (line_start + x) % 2048;
            uint8_t character = videx_d->screen_memory[char_addr];
            uint8_t ch = character & 0x7F;

            uint8_t cmap = (character & 0x80)>0 ? videx_d->alt_char_set[((ch * 16) + ln)] : videx_d->char_set[((ch * 16) + ln)];

            bool is_cursor = (cursor_pos == char_addr);
            bool cursor_this_char = (cursor_visible && is_cursor && (ln >= cursor_start && ln <= cursor_end));

            for (int px = 0; px < 8; px++) {
                if (cursor_this_char) {
                    *texturePixels = (cmap & 0x80) ? 0x00000000 : color_value;
                } else {
                    *texturePixels = (cmap & 0x80) ? color_value : 0x00000000;
                }
                cmap <<= 1;
                texturePixels++;
            }

        }
    }
}

void videx_render_line(cpu_state *cpu, videx_data * videx_d, int y) {
    display_state_t *ds = (display_state_t *)get_module_state(cpu, MODULE_DISPLAY);

    if (y < 0 || y >= 24) {
        return;
    }

    // this writes into texture - do not put border stuff here.
    SDL_Rect updateRect = {
        0,          // X position (left of window))
        y * 8,      // Y position (8 pixels per character)
        BASE_WIDTH,        // Width of line
        8           // Height of line
    };

    // the texture is our canvas. When we 'lock' it, we get a pointer to the pixels, and the pitch which is pixels per row
    // of the area. Since all our chars are the same we can just use the same pitch for all our chars.

    void* pixels;
    int pitch;

    SDL_Rect vupdateRect = {
        0,          // X position (left of window))
        y * 9,      // Y position (8 pixels per character)
        VIDEX_SCREEN_WIDTH,        // Width of line
        9          // Height of line
    };
    if (!SDL_LockTexture(videx_d->videx_texture, &vupdateRect, &pixels, &pitch)) {
        fprintf(stderr, "Failed to lock texture: %s\n", SDL_GetError());
        return;
    }
    render_videx_scanline_80x24(cpu, videx_d, y, pixels, pitch);
    SDL_UnlockTexture(videx_d->videx_texture);
}

/**
 * Update Display: for Display System Videx.
 * Called once per frame (16.67ms, 60fps) to update the display.
 */
void update_display_videx(cpu_state *cpu, /* SlotType_t slot */ videx_data * videx_d) {
    display_state_t *ds = (display_state_t *)get_module_state(cpu, MODULE_DISPLAY);

    // the backbuffer must be cleared each frame. The docs state this clearly
    // but I didn't know what the backbuffer was. Also, I assumed doing it once
    // at startup was enough. NOPE.
    SDL_RenderClear(ds->renderer); 

// openemulator disagrees, claims bit 5 = 1 means display cursor. But the manual clearly says bit 5 = 0 means cursor is on.
    bool cursor_blink_mode = (videx_d->reg[R10_CURSOR_START] & 0b01000000) != 0;
    bool cursor_enabled =    (videx_d->reg[R10_CURSOR_START] & 0b00100000) != 0;

    if (cursor_blink_mode) { // start of field
        videx_d->cursor_blink_count++;
        bool cursor_updated = false;
        if (cursor_enabled && videx_d->cursor_blink_count >= 16) { // TODO: is this a blink on and blink off each 1/32? in which case, my counter should be half.
            videx_d->cursor_blink_status = !videx_d->cursor_blink_status;
            videx_d->cursor_blink_count = 0;
            cursor_updated = true;
        } else if (!cursor_enabled && videx_d->cursor_blink_count >= 8) {
            videx_d->cursor_blink_status = !videx_d->cursor_blink_status;
            videx_d->cursor_blink_count = 0;
            cursor_updated = true;
        }
        if (cursor_updated) {
            videx_set_line_dirty_by_addr(videx_d, videx_d->reg[R14_CURSOR_HI] << 8 | videx_d->reg[R15_CURSOR_LO]);
        }
    }

    for (int line = 0; line < 24; line++) {
        if (videx_d->line_dirty[line]) {
            videx_render_line(cpu, videx_d, line);
            videx_d->line_dirty[line] = false;
        }
    }
    SDL_FRect dstrect = {
        (float)ds->border_width,
        (float)ds->border_height,
        (float)BASE_WIDTH, 
        (float)BASE_HEIGHT
    };
    SDL_SetTextureBlendMode(videx_d->videx_texture, SDL_BLENDMODE_ADD); // double-draw this to increase brightness.
    SDL_RenderTexture(ds->renderer, videx_d->videx_texture, NULL, &dstrect);
    SDL_RenderTexture(ds->renderer, videx_d->videx_texture, NULL, &dstrect);
}

void videx_memory_write(cpu_state *cpu, SlotType_t slot, uint16_t address, uint8_t value) {
    //videx_data * videx_d = (videx_data *)get_module_state(cpu, MODULE_VIDEX);
    videx_data * videx_d = (videx_data *)get_slot_state(cpu, slot);
    uint16_t faddr = (videx_d->selected_page * 2) * 0x100 + (address & 0x1FF);
    videx_d->screen_memory[faddr] = value;
    videx_set_line_dirty_by_addr(videx_d, faddr);
}

uint8_t videx_memory_read(cpu_state *cpu, SlotType_t slot, uint16_t address) {
    //videx_data * videx_d = (videx_data *)get_module_state(cpu, MODULE_VIDEX);
    videx_data * videx_d = (videx_data *)get_slot_state(cpu, slot);
    uint16_t faddr = (videx_d->selected_page * 2) * 0x100 + (address & 0x1FF);
    return videx_d->screen_memory[faddr];
}