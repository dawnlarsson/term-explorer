#include "terminal.c"
#include <dirent.h>
#include <sys/stat.h>
#include <limits.h>

Color clr_bg = {0, 0, 0}, clr_bar = {170, 170, 170}, clr_text = {255, 255, 255}, clr_folder = {255, 255, 85}, clr_hover = {170, 170, 170};

typedef struct
{
        char name[256];
        bool is_dir;
} FileEntry;

FileEntry *entries;
int entry_capacity, entry_count, selected_idx = -1;
bool dragging_scroll;
float target_scroll, current_scroll;
char cwd[PATH_MAX];

int cmp_entries(const void *a, const void *b)
{
        FileEntry *ea = (FileEntry *)a, *eb = (FileEntry *)b;
        return ea->is_dir != eb->is_dir ? eb->is_dir - ea->is_dir : strcmp(ea->name, eb->name);
}

void load_dir(const char *path)
{
        DIR *d = opendir(path) orelse return;
        defer closedir(d);

        (chdir(path) == 0) orelse return;
        getcwd(cwd, sizeof(cwd)) orelse return;

        entry_count = target_scroll = current_scroll = dragging_scroll = 0;
        selected_idx = -1;

        for (;;)
        {
                struct dirent *dir = readdir(d) orelse break;
                if (!strcmp(dir->d_name, "."))
                        continue;

                if (entry_count >= entry_capacity)
                {
                        int new_cap = entry_capacity == 0 ? 256 : entry_capacity * 2;
                        entries = realloc(entries, new_cap * sizeof(FileEntry)) orelse return;
                        entry_capacity = new_cap;
                }

                struct stat st;
                stat(dir->d_name, &st);
                strcpy(entries[entry_count].name, dir->d_name);
                entries[entry_count++].is_dir = S_ISDIR(st.st_mode);
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
        raw char next_dir[PATH_MAX];
        next_dir[0] = '\0';

        while (1)
        {
                float diff = target_scroll - current_scroll;
                int timeout = (!dragging_scroll && (diff > 0.01f || diff < -0.01f) || dragging_scroll) ? 16 : 1000;
                if (first_frame)
                {
                        timeout = 0;
                        first_frame = false;
                }

                int key = term_poll(timeout);
                int cell_w = 14, cell_h = 7, list_start_y = 2;
                int cols = (term_width - 1) / cell_w > 0 ? (term_width - 1) / cell_w : 1;
                int rows = (entry_count + cols - 1) / cols;
                int track_y = list_start_y, track_h = term_height - 1 - list_start_y > 0 ? term_height - 1 - list_start_y : 1;

                if (key == 'q' || key == KEY_ESC)
                        break;
                if (key == 'j')
                        term_mouse.wheel += 1;
                if (key == 'k')
                        term_mouse.wheel -= 1;
                if (key == KEY_BACKSPACE)
                        strcpy(next_dir, "..");
                if (key == KEY_ENTER && entry_count > 0 && selected_idx >= 0 && entries[selected_idx].is_dir)
                        strcpy(next_dir, entries[selected_idx].name);

                int old_idx = selected_idx;
                if (key == KEY_RIGHT && selected_idx < entry_count - 1)
                        selected_idx++;
                if (key == KEY_LEFT && selected_idx > 0)
                        selected_idx--;
                if (key == KEY_DOWN && selected_idx + cols < entry_count)
                        selected_idx += cols;
                if (key == KEY_UP && selected_idx >= cols)
                        selected_idx -= cols;
                if (selected_idx == -1 && entry_count > 0 && (key == KEY_RIGHT || key == KEY_LEFT || key == KEY_DOWN || key == KEY_UP))
                        selected_idx = 0;

                int scroll_offset = (int)current_scroll;
                if (old_idx != selected_idx && selected_idx >= 0)
                {
                        int sel_y = list_start_y + (selected_idx / cols * cell_h) - scroll_offset;
                        if (sel_y < list_start_y)
                                target_scroll -= (list_start_y - sel_y);
                        else if (sel_y + cell_h > term_height - 1)
                                target_scroll += (sel_y + cell_h - (term_height - 1));
                }

                int max_scroll = rows * cell_h > track_h ? rows * cell_h - track_h : 0;
                int thumb_h = track_h * track_h / (rows * cell_h > track_h ? rows * cell_h : track_h);
                if (thumb_h < 1)
                        thumb_h = 1;

                if (term_mouse.clicked && term_mouse.x == term_width - 1 && term_mouse.y >= track_y && term_mouse.y < track_y + track_h)
                        dragging_scroll = true;
                if (!term_mouse.left)
                        dragging_scroll = false;

                if (dragging_scroll && max_scroll > 0)
                {
                        float cr = (float)(term_mouse.y - track_y - (thumb_h / 2)) / (track_h - thumb_h > 0 ? track_h - thumb_h : 1);
                        target_scroll = current_scroll = (cr < 0 ? 0 : cr > 1 ? 1
                                                                              : cr) *
                                                         max_scroll;
                }

                target_scroll += term_mouse.wheel * 12.0f;
                target_scroll = target_scroll > max_scroll ? max_scroll : target_scroll < 0 ? 0
                                                                                            : target_scroll;
                if (!dragging_scroll)
                        current_scroll += (target_scroll - current_scroll) * 0.3f;
                scroll_offset = (int)current_scroll;

                ui_begin();
                ui_clear(clr_bg);

                for (int i = 0; i < entry_count; i++)
                {
                        int screen_x = (i % cols) * cell_w;
                        int screen_y = list_start_y + (i / cols * cell_h) - scroll_offset;
                        if (screen_y + cell_h < 0 || screen_y >= term_height)
                                continue;

                        bool hovered = (!dragging_scroll && term_mouse.x >= screen_x && term_mouse.x < screen_x + cell_w &&
                                        term_mouse.y >= screen_y && term_mouse.y < screen_y + cell_h &&
                                        term_mouse.y >= list_start_y && term_mouse.y < term_height - 1 && term_mouse.x < term_width - 1);

                        bool is_sel = (i == selected_idx), is_pressed = (hovered && term_mouse.left);
                        Color item_bg = is_pressed ? (Color){130, 130, 130} : ((hovered || is_sel) ? clr_hover : clr_bg);
                        Color icon_fg = entries[i].is_dir ? clr_folder : clr_text;
                        int y_off = is_pressed ? 1 : 0;

                        if (hovered || is_sel)
                                ui_rect(screen_x + 1, screen_y, cell_w - 2, cell_h, item_bg);

                        raw char disp[256];
                        strcpy(disp, entries[i].name);
                        char ext[5] = ".   ";
                        int elen = 0;
                        if (!entries[i].is_dir)
                        {
                                char *dot = strrchr(disp, '.');
                                if (dot && dot > disp && strlen(dot + 1) < 4)
                                {
                                        elen = strlen(dot + 1);
                                        for (int j = 0; j < elen; j++)
                                                ext[1 + j] = (dot[1 + j] >= 'a' && dot[1 + j] <= 'z') ? dot[1 + j] - 32 : dot[1 + j];
                                        *dot = '\0';
                                }
                        }

                        const char *icon_dir[] = {" ┌──┐___ ", " │  └───│ ", " │      │ ", " └──────┘ "};
                        const char *icon_file[] = {"  ┌──┐_ ", "  │  └─│", "  │    │", "  └────┘"};
                        const char **icon = entries[i].is_dir ? icon_dir : icon_file;
                        for (int j = 0; j < 4; j++)
                                ui_text(screen_x + 2, screen_y + j + y_off, icon[j], icon_fg, item_bg, is_pressed, false);
                        if (elen > 0)
                                ui_text(screen_x + 5, screen_y + 2 + y_off, ext, icon_fg, item_bg, is_pressed, true);

                        char l1[16], l2[16];
                        int mw = cell_w - 2, len = strlen(disp);
                        strncpy(l1, disp, mw);
                        if (len > mw)
                        {
                                strncpy(l2, disp + mw, mw);
                                if (len > mw * 2)
                                        strcpy(l2 + mw - 2, "..");
                        }
                        ui_text_centered(screen_x + 1, screen_y + 4 + y_off, mw, l1, clr_text, item_bg, is_pressed, false);
                        if (l2[0])
                                ui_text_centered(screen_x + 1, screen_y + 5 + y_off, mw, l2, clr_text, item_bg, is_pressed, false);

                        if (hovered && term_mouse.clicked)
                        {
                                selected_idx = i;
                                if (entries[i].is_dir)
                                        strcpy(next_dir, entries[i].name);
                        }
                }

                if (max_scroll > 0)
                {
                        ui_rect(term_width - 1, track_y, 1, track_h, (Color){30, 30, 30});
                        ui_rect(term_width - 1, track_y + (int)((current_scroll / max_scroll) * (track_h - thumb_h)), 1, thumb_h, dragging_scroll ? clr_text : clr_bar);
                }

                ui_rect(0, 0, term_width, list_start_y, clr_bg);
                ui_rect(0, 0, term_width, 1, clr_bar);
                raw char header[PATH_MAX + 20];
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