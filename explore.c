#include "terminal.c"
#include <dirent.h>
#include <sys/stat.h>
#include <limits.h>

Color clr_bg = {0, 0, 0};
Color clr_bar = {170, 170, 170};
Color clr_text = {255, 255, 255};
Color clr_folder = {255, 255, 85};
Color clr_hover = {170, 170, 170};

typedef struct
{
        char name[256];
        bool is_dir;
} FileEntry;

FileEntry *entries = NULL;
int entry_capacity = 0;
int entry_count = 0;

float target_scroll = 0.0f;
float current_scroll = 0.0f;
int selected_idx = -1;
bool dragging_scroll = false;

char cwd[PATH_MAX];

int cmp_entries(const void *a, const void *b)
{
        FileEntry *ea = (FileEntry *)a, *eb = (FileEntry *)b;
        if (ea->is_dir != eb->is_dir)
                return eb->is_dir - ea->is_dir;
        return strcmp(ea->name, eb->name);
}

void load_dir(const char *path)
{
        DIR *d = opendir(path) orelse return;
        defer closedir(d);

        if (chdir(path) != 0)
                return;
        getcwd(cwd, sizeof(cwd));

        entry_count = 0;
        target_scroll = 0;
        current_scroll = 0;
        selected_idx = -1;
        dragging_scroll = false;

        struct dirent *dir;
        while ((dir = readdir(d)) != NULL)
        {
                if (!strcmp(dir->d_name, "."))
                        continue;

                if (entry_count >= entry_capacity)
                {
                        entry_capacity = entry_capacity == 0 ? 256 : entry_capacity * 2;
                        void *new_entries = realloc(entries, entry_capacity * sizeof(FileEntry)) orelse return;
                        entries = new_entries;
                }

                struct stat st;
                stat(dir->d_name, &st);

                strncpy(entries[entry_count].name, dir->d_name, 255);
                entries[entry_count].name[255] = '\0';
                entries[entry_count].is_dir = S_ISDIR(st.st_mode);
                entry_count++;
        }

        qsort(entries, entry_count, sizeof(FileEntry), cmp_entries);
}

int main(void)
{
        term_init() orelse return 1;
        defer term_restore();
        defer free(entries);

        load_dir(".");

        bool first_frame = true;
        char next_dir[PATH_MAX] = {0};

        while (1)
        {
                float diff = target_scroll - current_scroll;
                if (diff < 0)
                        diff = -diff;
                bool is_animating = !dragging_scroll && (diff > 0.01f);

                int timeout = 1000;
                if (is_animating || dragging_scroll)
                        timeout = 16;

                if (first_frame)
                {
                        timeout = 0;
                        first_frame = false;
                }

                int key = term_poll(timeout);

                int cell_w = 14;
                int cell_h = 7;
                int cols = (term_width - 1) / cell_w;
                if (cols < 1)
                        cols = 1;

                int rows = (entry_count + cols - 1) / cols;
                int list_start_y = 2;

                int track_y = list_start_y;
                int track_h = term_height - 1 - list_start_y;
                if (track_h < 1)
                        track_h = 1;

                if (key == 'q' || key == KEY_ESC)
                        break;
                if (key == 'j')
                        term_mouse.wheel += 1;
                if (key == 'k')
                        term_mouse.wheel -= 1;
                if (key == KEY_BACKSPACE)
                {
                        strncpy(next_dir, "..", sizeof(next_dir) - 1);
                        next_dir[sizeof(next_dir) - 1] = '\0';
                }

                bool selection_changed = false;

                if ((key == KEY_RIGHT || key == KEY_LEFT || key == KEY_DOWN || key == KEY_UP) && selected_idx == -1)
                {
                        if (entry_count > 0)
                        {
                                selected_idx = 0;
                                selection_changed = true;
                        }
                }
                else
                {
                        if (key == KEY_RIGHT && selected_idx < entry_count - 1)
                        {
                                selected_idx++;
                                selection_changed = true;
                        }
                        if (key == KEY_LEFT && selected_idx > 0)
                        {
                                selected_idx--;
                                selection_changed = true;
                        }
                        if (key == KEY_DOWN && selected_idx + cols < entry_count)
                        {
                                selected_idx += cols;
                                selection_changed = true;
                        }
                        if (key == KEY_UP && selected_idx >= cols)
                        {
                                selected_idx -= cols;
                                selection_changed = true;
                        }
                }

                if (key == KEY_ENTER && entry_count > 0 && selected_idx >= 0 && entries[selected_idx].is_dir)
                {
                        strncpy(next_dir, entries[selected_idx].name, sizeof(next_dir) - 1);
                        next_dir[sizeof(next_dir) - 1] = '\0';
                }

                int scroll_offset = (int)current_scroll;
                if (selection_changed && entry_count > 0 && selected_idx >= 0)
                {
                        int sel_r = selected_idx / cols;
                        int sel_y = list_start_y + (sel_r * cell_h) - scroll_offset;

                        if (sel_y < list_start_y)
                                target_scroll -= (list_start_y - sel_y);
                        else if (sel_y + cell_h > term_height - 1)
                                target_scroll += (sel_y + cell_h - (term_height - 1));
                }

                int max_scroll_lines = (rows * cell_h) - track_h;
                if (max_scroll_lines < 0)
                        max_scroll_lines = 0;

                float visible_ratio = (float)track_h / (float)(rows * cell_h);
                if (visible_ratio > 1.0f)
                        visible_ratio = 1.0f;
                int thumb_h = (int)(track_h * visible_ratio);
                if (thumb_h < 1)
                        thumb_h = 1;

                if (term_mouse.clicked && term_mouse.x == term_width - 1 && term_mouse.y >= track_y && term_mouse.y < track_y + track_h)
                {
                        dragging_scroll = true;
                }
                if (!term_mouse.left)
                {
                        dragging_scroll = false;
                }

                if (dragging_scroll && max_scroll_lines > 0)
                {
                        int scrollable_track = track_h - thumb_h;
                        if (scrollable_track <= 0)
                                scrollable_track = 1;

                        float click_ratio = (float)(term_mouse.y - track_y - (thumb_h / 2)) / (float)scrollable_track;
                        if (click_ratio < 0.0f)
                                click_ratio = 0.0f;
                        if (click_ratio > 1.0f)
                                click_ratio = 1.0f;

                        target_scroll = click_ratio * max_scroll_lines;
                        current_scroll = target_scroll;
                }

                target_scroll += term_mouse.wheel * 12.0f;
                if (target_scroll > max_scroll_lines)
                        target_scroll = max_scroll_lines;
                if (target_scroll < 0)
                        target_scroll = 0;

                if (!dragging_scroll)
                {
                        current_scroll += (target_scroll - current_scroll) * 0.3f;
                }
                scroll_offset = (int)current_scroll;

                ui_begin();
                ui_rect(0, 0, term_width, term_height, clr_bg);

                for (int i = 0; i < entry_count; i++)
                {
                        int r = (i / cols);
                        int c = (i % cols);

                        int screen_x = c * cell_w;
                        int screen_y = list_start_y + (r * cell_h) - scroll_offset;

                        if (screen_y + cell_h < 0 || screen_y >= term_height)
                                continue;

                        bool hovered = (!dragging_scroll &&
                                        term_mouse.x >= screen_x && term_mouse.x < screen_x + cell_w &&
                                        term_mouse.y >= screen_y && term_mouse.y < screen_y + cell_h &&
                                        term_mouse.y >= list_start_y && term_mouse.y < term_height - 1 &&
                                        term_mouse.x < term_width - 1);

                        bool is_selected = (i == selected_idx);
                        bool is_pressed = (hovered && term_mouse.left);

                        Color item_bg = is_pressed ? (Color){130, 130, 130} : ((hovered || is_selected) ? clr_hover : clr_bg);
                        Color icon_fg = entries[i].is_dir ? clr_folder : clr_text;

                        int y_off = is_pressed ? 1 : 0;

                        if (hovered || is_selected)
                                ui_rect(screen_x + 1, screen_y, cell_w - 2, cell_h, item_bg);

                        char display_name[256];
                        strncpy(display_name, entries[i].name, sizeof(display_name) - 1);
                        display_name[255] = '\0';

                        char ext_fmt[5] = ".   ";
                        int ext_len = 0;

                        if (!entries[i].is_dir)
                        {
                                char *last_dot = strrchr(display_name, '.');
                                if (last_dot && last_dot > display_name)
                                {
                                        int actual_ext_len = strlen(last_dot + 1);
                                        // Ensure it's short enough to look good in the icon (1-3 chars)
                                        if (actual_ext_len > 0 && actual_ext_len < 4)
                                        {
                                                ext_len = actual_ext_len;
                                                for (int j = 0; j < ext_len; j++)
                                                {
                                                        char c = last_dot[1 + j];
                                                        if (c >= 'a' && c <= 'z')
                                                                c -= 32;
                                                        ext_fmt[1 + j] = c;
                                                }
                                                *last_dot = '\0'; // Trim extension from display text below the icon
                                        }
                                }
                        }

                        if (entries[i].is_dir)
                        {
                                ui_text(screen_x + 2, screen_y + 0 + y_off, " ┌──┐___ ", icon_fg, item_bg, is_pressed, false);
                                ui_text(screen_x + 2, screen_y + 1 + y_off, " │  └───│ ", icon_fg, item_bg, is_pressed, false);
                                ui_text(screen_x + 2, screen_y + 2 + y_off, " │      │ ", icon_fg, item_bg, is_pressed, false);
                                ui_text(screen_x + 2, screen_y + 3 + y_off, " └──────┘ ", icon_fg, item_bg, is_pressed, false);
                        }
                        else
                        {
                                ui_text(screen_x + 2, screen_y + 0 + y_off, "  ┌──┐_ ", icon_fg, item_bg, is_pressed, false);
                                ui_text(screen_x + 2, screen_y + 1 + y_off, "  │  └─│", icon_fg, item_bg, is_pressed, false);
                                ui_text(screen_x + 2, screen_y + 2 + y_off, "  │    │", icon_fg, item_bg, is_pressed, false);
                                ui_text(screen_x + 2, screen_y + 3 + y_off, "  └────┘", icon_fg, item_bg, is_pressed, false);

                                if (ext_len > 0)
                                {
                                        // screen_x + 5 exactly aligns with the 4 inner spaces in the "  │    │" string
                                        ui_text(screen_x + 5, screen_y + 2 + y_off, ext_fmt, icon_fg, item_bg, is_pressed, true);
                                }
                        }

                        char line1[16] = {0};
                        char line2[16] = {0};
                        int max_chars = cell_w - 2;
                        int name_len = strlen(display_name);

                        if (name_len <= max_chars)
                        {
                                strncpy(line1, display_name, max_chars);
                        }
                        else
                        {
                                strncpy(line1, display_name, max_chars);
                                strncpy(line2, display_name + max_chars, max_chars);

                                if (name_len > max_chars * 2)
                                {
                                        line2[max_chars - 2] = '.';
                                        line2[max_chars - 1] = '.';
                                }
                        }

                        int pad1 = (cell_w - strlen(line1)) / 2;
                        ui_text(screen_x + pad1, screen_y + 4 + y_off, line1, clr_text, item_bg, is_pressed, false);

                        if (strlen(line2) > 0)
                        {
                                int pad2 = (cell_w - strlen(line2)) / 2;
                                ui_text(screen_x + pad2, screen_y + 5 + y_off, line2, clr_text, item_bg, is_pressed, false);
                        }

                        if (hovered && term_mouse.clicked)
                        {
                                selected_idx = i;
                                if (entries[i].is_dir)
                                {
                                        strncpy(next_dir, entries[i].name, sizeof(next_dir) - 1);
                                        next_dir[sizeof(next_dir) - 1] = '\0';
                                }
                        }
                }

                if (max_scroll_lines > 0)
                {
                        ui_rect(term_width - 1, track_y, 1, track_h, (Color){30, 30, 30});

                        float scroll_ratio = current_scroll / (float)max_scroll_lines;
                        int thumb_y = track_y + (int)(scroll_ratio * (track_h - thumb_h));

                        Color thumb_clr = dragging_scroll ? clr_text : clr_bar;
                        ui_rect(term_width - 1, thumb_y, 1, thumb_h, thumb_clr);
                }

                ui_rect(0, 0, term_width, list_start_y, clr_bg);
                ui_rect(0, 0, term_width, 1, clr_bar);

                char header[PATH_MAX + 20];
                snprintf(header, sizeof(header), " Directory: %s ", cwd);
                ui_text(1, 0, header, (Color){0, 0, 0}, clr_bar, false, false);

                ui_rect(0, term_height - 1, term_width, 1, clr_bar);
                ui_text(1, term_height - 1, " Arrows: Navigate | Click/Enter: Open | Backspace: Up | 'q'/ESC: Quit ", (Color){0, 0, 0}, clr_bar, false, false);

                ui_cursor();
                ui_end();

                if (next_dir[0] != '\0')
                {
                        load_dir(next_dir);
                        next_dir[0] = '\0';
                        first_frame = true;
                }
        }

        return 0;
}