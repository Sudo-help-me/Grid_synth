#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <input/input.h>
#include <storage/storage.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

#define VISIBLE_COLS 12
#define MAX_COLS 256
#define GRID_ROWS_NOTES 12
#define GRID_ROWS_DRUMS 3
#define NUM_OCTAVES 5
#define SAVE_PATH_DIR EXT_PATH("grid_synth")
#define SAVE_FILE_PATH EXT_PATH("grid_synth/song.txt")

static const float BASE_NOTE_FREQS[GRID_ROWS_NOTES] = {
    1046.50f, 987.77f, 932.33f, 880.00f, 830.61f, 783.99f,
    739.99f,  698.46f, 659.25f, 622.25f, 587.33f, 554.37f
};

typedef enum {
    SectionNotes,
    SectionDrums
} GridSection;

typedef enum {
    ModeGrid,
    ModeMenu
} AppMode;

typedef enum {
    MenuItemTempo,
    MenuItemVolume,
    MenuItemHowTo,
    MenuItemNew,
    MenuItemSave,
    MenuItemLoad,
    MenuItemBack,
    MenuItemCount
} MenuItem;

typedef enum {
    AppEventTypeInput,
    AppEventTypeTick
} AppEventType;

typedef struct {
    AppEventType type;
    InputEvent input;
} AppEvent;

typedef struct {
    bool is_drum;
    uint8_t octave;
    uint8_t row;
    uint32_t col;
} Note;

typedef struct {
    Note* notes;
    size_t note_count;
    size_t note_capacity;
    uint32_t max_col;

    GridSection section;
    uint32_t cursor_x;
    uint8_t cursor_y;
    uint32_t camera_x;
    uint32_t play_head;
    bool is_playing;
    uint16_t bpm;
    uint8_t volume;
    bool show_how_to;
    uint8_t current_octave_idx;

    AppMode mode;
    MenuItem menu_cursor;
    char status_msg[64];
    FuriMutex* mutex;
    bool speaker_acquired;
    float current_freq;
} AppState;

static float get_note_frequency(uint8_t row, uint8_t octave_idx) {
    float base_freq = BASE_NOTE_FREQS[row];
    int8_t shift = (int8_t)octave_idx - 2;
    if(shift > 0) {
        for(int8_t i = 0; i < shift; i++) base_freq *= 2.0f;
    } else if(shift < 0) {
        for(int8_t i = 0; i < -shift; i++) base_freq /= 2.0f;
    }
    return base_freq;
}

static bool has_note(AppState* state, bool is_drum, uint8_t oct, uint8_t row, uint32_t col) {
    for(size_t i = 0; i < state->note_count; i++) {
        if(state->notes[i].is_drum == is_drum && state->notes[i].row == row &&
           state->notes[i].col == col) {
            if(is_drum || state->notes[i].octave == oct) {
                return true;
            }
        }
    }
    return false;
}

static bool toggle_note(AppState* state, bool is_drum, uint8_t oct, uint8_t row, uint32_t col) {
    for(size_t i = 0; i < state->note_count; i++) {
        if(state->notes[i].is_drum == is_drum && state->notes[i].row == row &&
           state->notes[i].col == col) {
            if(is_drum || state->notes[i].octave == oct) {
                state->notes[i] = state->notes[state->note_count - 1];
                state->note_count--;
                
                if(col == state->max_col) {
                    state->max_col = 0;
                    for(size_t j = 0; j < state->note_count; j++) {
                        if(state->notes[j].col > state->max_col) {
                            state->max_col = state->notes[j].col;
                        }
                    }
                }
                return false;
            }
        }
    }

    if(state->note_count >= state->note_capacity) {
        size_t new_cap = state->note_capacity == 0 ? 32 : state->note_capacity * 2;
        Note* new_notes = realloc(state->notes, new_cap * sizeof(Note));
        if(!new_notes) return false;
        state->notes = new_notes;
        state->note_capacity = new_cap;
    }

    state->notes[state->note_count].is_drum = is_drum;
    state->notes[state->note_count].octave = is_drum ? 0 : oct;
    state->notes[state->note_count].row = row;
    state->notes[state->note_count].col = col;
    state->note_count++;
    
    if(col > state->max_col) {
        state->max_col = col;
    }
    
    return true;
}

static void stop_tone_safe(AppState* state) {
    if(state->speaker_acquired) {
        furi_hal_speaker_stop();
        furi_hal_speaker_release();
        state->speaker_acquired = false;
        state->current_freq = 0.0f;
    }
}

static void play_tone_safe(AppState* state, float freq) {
    if(state->volume == 0) {
        stop_tone_safe(state);
        return;
    }

    if(!state->speaker_acquired) {
        if(furi_hal_speaker_acquire(10)) {
            state->speaker_acquired = true;
        }
    }

    if(state->speaker_acquired) {
        if(state->current_freq != freq) {
            furi_hal_speaker_stop();
            float vol_float = (float)state->volume / 10.0f;
            furi_hal_speaker_start(freq, vol_float);
            state->current_freq = freq;
        }
    }
}

static void play_preview_note(AppState* state) {
    if(state->section == SectionDrums) {
        float drum_freqs[] = {8000.0f, 1500.0f, 60.0f};
        if(state->cursor_y < 3) {
            play_tone_safe(state, drum_freqs[state->cursor_y]);
            furi_delay_ms(20);
            stop_tone_safe(state);
        }
    } else {
        float freq = get_note_frequency(state->cursor_y, state->current_octave_idx);
        play_tone_safe(state, freq);
    }
}

static void create_new_sheet(AppState* state) {
    state->note_count = 0;
    state->max_col = 0;
    state->bpm = 120;
    state->play_head = 0;
    state->cursor_x = 0;
    state->cursor_y = 0;
    state->camera_x = 0;
    state->section = SectionNotes;
    state->current_octave_idx = 2;
    snprintf(state->status_msg, sizeof(state->status_msg), "Created New Sheet");
}

static void save_song(AppState* state) {
    furi_mutex_acquire(state->mutex, FuriWaitForever);
    uint16_t bpm = state->bpm;
    size_t count = state->note_count;
    Note* snapshot = NULL;

    if(count > 0) {
        snapshot = malloc(count * sizeof(Note));
        if(snapshot) {
            memcpy(snapshot, state->notes, count * sizeof(Note));
        }
    }
    furi_mutex_release(state->mutex);

    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_common_mkdir(storage, SAVE_PATH_DIR);

    File* file = storage_file_alloc(storage);
    if(storage_file_open(file, SAVE_FILE_PATH, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        char buffer[64];

        snprintf(buffer, sizeof(buffer), "BPM:%u\n", bpm);
        storage_file_write(file, buffer, strlen(buffer));

        if(snapshot) {
            for(size_t i = 0; i < count; i++) {
                Note note = snapshot[i];
                if(note.is_drum) {
                    snprintf(buffer, sizeof(buffer), "D,%u,%u,%lu\n", note.octave, note.row, (unsigned long)note.col);
                } else {
                    snprintf(
                        buffer,
                        sizeof(buffer),
                        "N,%u,%u,%lu\n",
                        note.octave,
                        note.row,
                        (unsigned long)note.col);
                }
                storage_file_write(file, buffer, strlen(buffer));
            }
        }

        furi_mutex_acquire(state->mutex, FuriWaitForever);
        snprintf(state->status_msg, sizeof(state->status_msg), "Saved to song.txt");
        furi_mutex_release(state->mutex);
    } else {
        furi_mutex_acquire(state->mutex, FuriWaitForever);
        snprintf(state->status_msg, sizeof(state->status_msg), "Save Failed!");
        furi_mutex_release(state->mutex);
    }

    if(snapshot) free(snapshot);
    storage_file_close(file);
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
}

static bool parse_line(AppState* state, char* line) {
    bool valid_data_parsed = false;

    if(strncmp(line, "BPM:", 4) == 0) {
        int bpm = atoi(line + 4);
        if(bpm < 40) bpm = 40;
        if(bpm > 300) bpm = 300;
        state->bpm = (uint16_t)bpm;
        valid_data_parsed = true;
    } else if(strncmp(line, "D,", 2) == 0) {
        unsigned int oct = 0, row = 0;
        unsigned long col = 0;
        if(sscanf(line + 2, "%u,%u,%lu", &oct, &row, &col) == 3) {
            if(row < GRID_ROWS_DRUMS && col < MAX_COLS) {
                toggle_note(state, true, (uint8_t)oct, (uint8_t)row, (uint32_t)col);
                valid_data_parsed = true;
            }
        } else if(sscanf(line + 2, "%u,%lu", &row, &col) == 2) {
            if(row < GRID_ROWS_DRUMS && col < MAX_COLS) {
                toggle_note(state, true, 0, (uint8_t)row, (uint32_t)col);
                valid_data_parsed = true;
            }
        }
    } else if(strncmp(line, "N,", 2) == 0) {
        unsigned int oct = 0, row = 0;
        unsigned long col = 0;
        if(sscanf(line + 2, "%u,%u,%lu", &oct, &row, &col) == 3) {
            if(oct < NUM_OCTAVES && row < GRID_ROWS_NOTES && col < MAX_COLS) {
                toggle_note(state, false, (uint8_t)oct, (uint8_t)row, (uint32_t)col);
                valid_data_parsed = true;
            }
        }
    }
    
    return valid_data_parsed;
}

static void load_song(AppState* state) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* file = storage_file_alloc(storage);

    if(storage_file_open(file, SAVE_FILE_PATH, FSAM_READ, FSOM_OPEN_EXISTING)) {
        furi_mutex_acquire(state->mutex, FuriWaitForever);
        
        Note* backup_notes = state->notes;
        size_t backup_count = state->note_count;
        size_t backup_cap = state->note_capacity;
        uint32_t backup_max_col = state->max_col;

        state->notes = NULL;
        state->note_count = 0;
        state->note_capacity = 0;
        state->max_col = 0;

        char buffer[128];
        char line[128];
        size_t line_pos = 0;
        uint16_t read_bytes;
        bool file_is_valid = false;

        while((read_bytes = storage_file_read(file, buffer, sizeof(buffer))) > 0) {
            for(uint16_t i = 0; i < read_bytes; i++) {
                if(buffer[i] == '\n' || buffer[i] == '\r') {
                    if(line_pos > 0) {
                        line[line_pos] = '\0';
                        if(parse_line(state, line)) {
                            file_is_valid = true;
                        }
                        line_pos = 0;
                    }
                } else {
                    if(line_pos < sizeof(line) - 1) {
                        line[line_pos++] = buffer[i];
                    }
                }
            }
        }
        if(line_pos > 0) {
            line[line_pos] = '\0';
            if(parse_line(state, line)) {
                file_is_valid = true;
            }
        }

        if(file_is_valid) {
            if(backup_notes) free(backup_notes);
            snprintf(state->status_msg, sizeof(state->status_msg), "Loaded song.txt");
        } else {
            if(state->notes) free(state->notes);
            state->notes = backup_notes;
            state->note_count = backup_count;
            state->note_capacity = backup_cap;
            state->max_col = backup_max_col;
            snprintf(state->status_msg, sizeof(state->status_msg), "Invalid File/Empty");
        }
        
        furi_mutex_release(state->mutex);
    } else {
        furi_mutex_acquire(state->mutex, FuriWaitForever);
        snprintf(state->status_msg, sizeof(state->status_msg), "File Not Found!");
        furi_mutex_release(state->mutex);
    }

    storage_file_close(file);
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
}

static void draw_callback(Canvas* canvas, void* context) {
    AppState* state = (AppState*)context;
    if(furi_mutex_acquire(state->mutex, 30) != FuriStatusOk) return;

    canvas_clear(canvas);
    canvas_set_color(canvas, ColorBlack);

    if(state->mode == ModeGrid) {
        const uint8_t cell_w = 8;
        const uint8_t offset_y = 12;

        canvas_set_font(canvas, FontSecondary);
        if(state->is_playing) {
            canvas_draw_str(canvas, 2, 9, "PLAY");
        } else {
            canvas_draw_str(canvas, 2, 9, "PAUSE");
        }

        char status_str[32];
        if(state->section == SectionNotes) {
            int8_t display_octave = (int8_t)state->current_octave_idx - 2;
            snprintf(status_str, sizeof(status_str), "[N] OCT:%+d BPM:%d", display_octave, state->bpm);
        } else {
            snprintf(status_str, sizeof(status_str), "[D] BPM:%d", state->bpm);
        }
        canvas_draw_str(canvas, 36, 9, status_str);

        if(state->section == SectionNotes) {
            const uint8_t cell_h = 4;
            const uint8_t keyboard_w = 12;
            const uint8_t grid_offset_x = 30;

            for(uint8_t r = 0; r < GRID_ROWS_NOTES; r++) {
                uint8_t y = offset_y + (r * cell_h);
                bool is_sharp = (r == 2 || r == 4 || r == 6 || r == 9 || r == 11);

                if(is_sharp) {
                    canvas_draw_box(canvas, 0, y, 7, cell_h);
                } else {
                    canvas_draw_frame(canvas, 0, y, keyboard_w - 1, cell_h + 1);
                }
            }

            char oct_str[8];
            int display_oct = (int)state->current_octave_idx + 4;
            snprintf(oct_str, sizeof(oct_str), "C%d", display_oct);
            canvas_draw_str(canvas, keyboard_w + 3, offset_y + 6, oct_str);

            for(uint8_t r = 0; r < GRID_ROWS_NOTES; r++) {
                for(uint8_t c = 0; c < VISIBLE_COLS; c++) {
                    uint32_t world_col = state->camera_x + c;
                    uint8_t x = grid_offset_x + (c * cell_w);
                    uint8_t y = offset_y + (r * cell_h);

                    if(has_note(state, false, state->current_octave_idx, r, world_col)) {
                        canvas_draw_box(canvas, x + 1, y + 1, cell_w - 1, cell_h - 1);
                    } else {
                        canvas_draw_frame(canvas, x + 1, y + 1, cell_w - 1, cell_h - 1);
                    }
                }
            }

            if(state->is_playing && state->play_head >= state->camera_x &&
               state->play_head < state->camera_x + VISIBLE_COLS) {
                uint8_t px = grid_offset_x + (state->play_head - state->camera_x) * cell_w + (cell_w / 2);
                canvas_draw_line(canvas, px, offset_y, px, offset_y + (GRID_ROWS_NOTES * cell_h));
            }

            if(state->cursor_x >= state->camera_x && state->cursor_x < state->camera_x + VISIBLE_COLS) {
                uint8_t cur_x = grid_offset_x + (state->cursor_x - state->camera_x) * cell_w;
                uint8_t cur_y = offset_y + (state->cursor_y * cell_h);
                canvas_draw_frame(canvas, cur_x, cur_y, cell_w + 1, cell_h + 1);
            }
        } else {
            const uint8_t cell_h = 16;
            const char* drum_labels[] = {"HH", "SD", "BD"};

            for(uint8_t r = 0; r < GRID_ROWS_DRUMS; r++) {
                canvas_draw_str(canvas, 1, offset_y + (r * cell_h) + 12, drum_labels[r]);

                for(uint8_t c = 0; c < VISIBLE_COLS - 1; c++) {
                    uint32_t world_col = state->camera_x + c;
                    uint8_t x = 20 + (c * cell_w);
                    uint8_t y = offset_y + (r * cell_h);

                    if(has_note(state, true, 0, r, world_col)) {
                        canvas_draw_box(canvas, x + 1, y + 1, cell_w - 1, cell_h - 2);
                    } else {
                        canvas_draw_frame(canvas, x + 1, y + 1, cell_w - 1, cell_h - 2);
                    }
                }
            }

            if(state->is_playing && state->play_head >= state->camera_x &&
               state->play_head < state->camera_x + (VISIBLE_COLS - 1)) {
                uint8_t px = 20 + (state->play_head - state->camera_x) * cell_w + (cell_w / 2);
                canvas_draw_line(canvas, px, offset_y, px, offset_y + (GRID_ROWS_DRUMS * cell_h));
            }

            if(state->cursor_x >= state->camera_x && state->cursor_x < state->camera_x + (VISIBLE_COLS - 1)) {
                uint8_t cur_x = 20 + (state->cursor_x - state->camera_x) * cell_w;
                uint8_t cur_y = offset_y + (state->cursor_y * cell_h);
                canvas_draw_frame(canvas, cur_x, cur_y, cell_w + 1, cell_h - 1);
            }
        }

    } else if(state->mode == ModeMenu) {
        if(state->show_how_to) {
            canvas_set_font(canvas, FontPrimary);
            canvas_draw_str(canvas, 32, 9, "HOW TO USE");

            canvas_set_font(canvas, FontSecondary);
            canvas_draw_str(canvas, 2, 20, "OK: Toggle Note / Preview");
            canvas_draw_str(canvas, 2, 29, "Hold OK: Play / Pause");
            canvas_draw_str(canvas, 2, 38, "Hold Left: Notes/Drums");
            canvas_draw_str(canvas, 2, 47, "Hold Up/Down: Change Octave");
            canvas_draw_str(canvas, 2, 56, "Back: Open Menu / Exit");
        } else {
            canvas_set_font(canvas, FontPrimary);
            canvas_draw_str(canvas, 38, 8, "SETTINGS");

            canvas_set_font(canvas, FontSecondary);

            char tempo_str[32], volume_str[32];
            snprintf(tempo_str, sizeof(tempo_str), "Tempo (BPM): <%d>", state->bpm);
            snprintf(volume_str, sizeof(volume_str), "Volume: <%d/10>", state->volume);

            const char* menu_items[] = {
                tempo_str,
                volume_str,
                "How to Use",
                "New Sheet",
                "Save Song",
                "Load Song",
                "Back to Grid"};

            const uint8_t max_visible = 4;
            uint8_t start_idx = 0;
            
            if(state->menu_cursor > 2) {
                start_idx = state->menu_cursor - 2;
                if(start_idx > MenuItemCount - max_visible) {
                    start_idx = MenuItemCount - max_visible;
                }
            }

            for(uint8_t i = 0; i < max_visible; i++) {
                uint8_t item_idx = start_idx + i;
                if(item_idx >= MenuItemCount) break;

                uint8_t y = 18 + (i * 11);
                if(state->menu_cursor == item_idx) {
                    canvas_draw_str(canvas, 2, y, ">");
                }
                canvas_draw_str(canvas, 12, y, menu_items[item_idx]);
            }

            // Draw scrollbar
            uint8_t scrollbar_h = 42;
            uint8_t scroll_step_y = (scrollbar_h - (scrollbar_h / 2)) / (MenuItemCount - 1);
            uint8_t scroll_y = 12 + (state->menu_cursor * scroll_step_y);
            
            canvas_draw_frame(canvas, 125, 12, 3, scrollbar_h);
            canvas_draw_box(canvas, 125, scroll_y, 3, scrollbar_h / 2);

            if(strlen(state->status_msg) > 0) {
                canvas_draw_str(canvas, 2, 62, state->status_msg);
            }
        }
    }

    furi_mutex_release(state->mutex);
}

static void input_callback(InputEvent* input_event, void* context) {
    FuriMessageQueue* event_queue = (FuriMessageQueue*)context;
    AppEvent event = {
        .type = AppEventTypeInput,
        .input = *input_event
    };
    furi_message_queue_put(event_queue, &event, 0);
}

static void timer_callback(void* context) {
    FuriMessageQueue* event_queue = (FuriMessageQueue*)context;
    AppEvent event = {
        .type = AppEventTypeTick
    };
    furi_message_queue_put(event_queue, &event, 0);
}

static uint32_t calculate_timer_period(uint16_t bpm) {
    if(bpm < 40) bpm = 40;
    if(bpm > 300) bpm = 300;
    return furi_ms_to_ticks(60000 / bpm / 4);
}

static void process_tick(AppState* state) {
    if(furi_mutex_acquire(state->mutex, FuriWaitForever) != FuriStatusOk) return;

    if(state->is_playing) {
        bool play_sound = false;
        bool is_drum_tick = false;
        float tone_freq = 0.0f;

        uint32_t active_max_col = state->max_col;
        if(active_max_col < VISIBLE_COLS) active_max_col = VISIBLE_COLS - 1;

        state->play_head++;
        if(state->play_head > active_max_col) {
            state->play_head = 0;
        }

        uint8_t vis_cols = (state->section == SectionDrums) ? (VISIBLE_COLS - 1) : VISIBLE_COLS;
        if(state->play_head >= state->camera_x + vis_cols) {
            state->camera_x = state->play_head - vis_cols + 1;
        } else if(state->play_head < state->camera_x) {
            state->camera_x = state->play_head;
        }

        for(size_t i = 0; i < state->note_count; i++) {
            if(state->notes[i].col == state->play_head) {
                if(state->notes[i].is_drum) {
                    float drum_freqs[] = {8000.0f, 1500.0f, 60.0f};
                    if(state->notes[i].row < 3) {
                        tone_freq = drum_freqs[state->notes[i].row];
                        play_sound = true;
                        is_drum_tick = true;
                        break; 
                    }
                } else if(!play_sound) {
                    tone_freq = get_note_frequency(state->notes[i].row, state->notes[i].octave);
                    play_sound = true;
                }
            }
        }

        if(play_sound) {
            play_tone_safe(state, tone_freq);
            
            if(is_drum_tick) {
                furi_delay_ms(15); 
                stop_tone_safe(state);
            }
        } else {
            stop_tone_safe(state);
        }
    }

    furi_mutex_release(state->mutex);
}

int32_t grid_synth_app(void* p) {
    UNUSED(p);

    AppState state = {
        .notes = NULL,
        .note_count = 0,
        .note_capacity = 0,
        .max_col = 0,
        .section = SectionNotes,
        .cursor_x = 0,
        .cursor_y = 0,
        .camera_x = 0,
        .play_head = 0,
        .is_playing = false,
        .bpm = 120,
        .volume = 8,
        .show_how_to = false,
        .current_octave_idx = 2,
        .mode = ModeGrid,
        .menu_cursor = MenuItemTempo,
        .status_msg = "",
        .mutex = furi_mutex_alloc(FuriMutexTypeNormal),
        .speaker_acquired = false,
        .current_freq = 0.0f,
    };

    FuriMessageQueue* event_queue = furi_message_queue_alloc(16, sizeof(AppEvent));
    ViewPort* view_port = view_port_alloc();
    view_port_draw_callback_set(view_port, draw_callback, &state);
    view_port_input_callback_set(view_port, input_callback, event_queue);

    Gui* gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(gui, view_port, GuiLayerFullscreen);

    FuriTimer* timer = furi_timer_alloc(timer_callback, FuriTimerTypePeriodic, event_queue);
    furi_timer_start(timer, calculate_timer_period(state.bpm));

    AppEvent event;
    bool running = true;

    while(running) {
        if(furi_message_queue_get(event_queue, &event, FuriWaitForever) == FuriStatusOk) {
            if(event.type == AppEventTypeTick) {
                process_tick(&state);
            } else if(event.type == AppEventTypeInput) {
                InputEvent input = event.input;
                bool execute_save = false;
                bool execute_load = false;

                furi_mutex_acquire(state.mutex, FuriWaitForever);

                if(input.type == InputTypeRelease) {
                    if(!state.is_playing) {
                        stop_tone_safe(&state);
                    }
                    if(state.mode == ModeMenu && state.menu_cursor == MenuItemTempo) {
                        furi_timer_restart(timer, calculate_timer_period(state.bpm));
                    }
                }

                if(state.mode == ModeGrid) {
                    uint8_t max_rows = (state.section == SectionNotes) ? GRID_ROWS_NOTES : GRID_ROWS_DRUMS;
                    uint8_t vis_cols = (state.section == SectionDrums) ? (VISIBLE_COLS - 1) : VISIBLE_COLS;

                    if(input.type == InputTypeShort || input.type == InputTypeRepeat) {
                        switch(input.key) {
                        case InputKeyLeft:
                            if(state.cursor_x > 0) {
                                state.cursor_x--;
                                if(state.cursor_x < state.camera_x) {
                                    state.camera_x = state.cursor_x;
                                }
                            }
                            break;
                        case InputKeyRight:
                            if(state.cursor_x < MAX_COLS - 1) {
                                state.cursor_x++;
                                if(state.cursor_x >= state.camera_x + vis_cols) {
                                    state.camera_x = state.cursor_x - vis_cols + 1;
                                }
                            }
                            break;
                        case InputKeyUp:
                            if(state.cursor_y > 0) state.cursor_y--;
                            break;
                        case InputKeyDown:
                            if(state.cursor_y < max_rows - 1) state.cursor_y++;
                            break;
                        case InputKeyOk: {
                            bool note_added = toggle_note(
                                &state,
                                state.section == SectionDrums,
                                state.current_octave_idx,
                                state.cursor_y,
                                state.cursor_x);
                            if(note_added && !state.is_playing) {
                                play_preview_note(&state);
                            }
                            break;
                        }
                        case InputKeyBack:
                            state.mode = ModeMenu;
                            state.status_msg[0] = '\0';
                            break;
                        default:
                            break;
                        }
                    } else if(input.type == InputTypeLong) {
                        switch(input.key) {
                        case InputKeyLeft: {
                            state.section = (state.section == SectionNotes) ? SectionDrums : SectionNotes;
                            state.cursor_y = 0;
                            uint8_t new_vis_cols = (state.section == SectionDrums) ? (VISIBLE_COLS - 1) : VISIBLE_COLS;
                            if(state.cursor_x >= state.camera_x + new_vis_cols) {
                                state.camera_x = state.cursor_x - new_vis_cols + 1;
                            }
                            break;
                        }
                        case InputKeyUp:
                            if(state.section == SectionNotes && state.current_octave_idx < NUM_OCTAVES - 1) {
                                state.current_octave_idx++;
                            }
                            break;
                        case InputKeyDown:
                            if(state.section == SectionNotes && state.current_octave_idx > 0) {
                                state.current_octave_idx--;
                            }
                            break;
                        case InputKeyOk:
                            state.is_playing = !state.is_playing;
                            if(state.is_playing) {
                                state.play_head = state.cursor_x;
                            } else {
                                stop_tone_safe(&state);
                            }
                            break;
                        case InputKeyBack:
                            running = false;
                            break;
                        default:
                            break;
                        }
                    }
                } else if(state.mode == ModeMenu) {
                    if(state.show_how_to) {
                        if(input.type == InputTypeShort && (input.key == InputKeyBack || input.key == InputKeyOk)) {
                            state.show_how_to = false;
                        }
                    } else {
                        if(input.type == InputTypeShort || input.type == InputTypeRepeat) {
                            switch(input.key) {
                            case InputKeyUp:
                                if(state.menu_cursor > 0) state.menu_cursor--;
                                break;
                            case InputKeyDown:
                                if(state.menu_cursor < MenuItemCount - 1) state.menu_cursor++;
                                break;
                            case InputKeyLeft:
                                if(state.menu_cursor == MenuItemTempo && state.bpm > 40) {
                                    state.bpm--;
                                } else if(state.menu_cursor == MenuItemVolume && state.volume > 0) {
                                    state.volume--;
                                }
                                break;
                            case InputKeyRight:
                                if(state.menu_cursor == MenuItemTempo && state.bpm < 300) {
                                    state.bpm++;
                                } else if(state.menu_cursor == MenuItemVolume && state.volume < 10) {
                                    state.volume++;
                                }
                                break;
                            case InputKeyOk:
                                if(state.menu_cursor == MenuItemHowTo) {
                                    state.show_how_to = true;
                                } else if(state.menu_cursor == MenuItemNew) {
                                    state.is_playing = false;
                                    stop_tone_safe(&state);
                                    create_new_sheet(&state);
                                    furi_timer_restart(timer, calculate_timer_period(state.bpm));
                                } else if(state.menu_cursor == MenuItemSave) {
                                    state.is_playing = false;
                                    stop_tone_safe(&state);
                                    execute_save = true;
                                } else if(state.menu_cursor == MenuItemLoad) {
                                    state.is_playing = false;
                                    stop_tone_safe(&state);
                                    execute_load = true;
                                } else if(state.menu_cursor == MenuItemBack) {
                                    state.mode = ModeGrid;
                                }
                                break;
                            case InputKeyBack:
                                state.mode = ModeGrid;
                                break;
                            default:
                                break;
                            }
                        } else if(input.type == InputTypeLong) {
                            if(input.key == InputKeyBack) {
                                running = false;
                            }
                        }
                    }
                }

                furi_mutex_release(state.mutex);

                if(execute_save) {
                    save_song(&state);
                } else if(execute_load) {
                    load_song(&state);
                    furi_mutex_acquire(state.mutex, FuriWaitForever);
                    uint16_t bpm = state.bpm;
                    furi_mutex_release(state.mutex);
                    furi_timer_restart(timer, calculate_timer_period(bpm));
                }
            }
        }

        view_port_update(view_port);
    }

    furi_timer_stop(timer);
    furi_timer_free(timer);

    stop_tone_safe(&state);

    gui_remove_view_port(gui, view_port);
    view_port_free(view_port);
    furi_record_close(RECORD_GUI);
    furi_message_queue_free(event_queue);

    if(state.notes) free(state.notes);
    furi_mutex_free(state.mutex);

    return 0;
}
