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

FileEntry entries[1024];
int entry_count = 0;

float target_scroll = 0.0f;
float current_scroll = 0.0f;

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

        struct dirent *dir;
        while ((dir = readdir(d)) != NULL && entry_count < 1024)
        {
                if (!strcmp(dir->d_name, "."))
                        continue;
                struct stat st;
                stat(dir->d_name, &st);

                strncpy(entries[entry_count].name, dir->d_name, 255);
                entries[entry_count].is_dir = S_ISDIR(st.st_mode);
                entry_count++;
        }

        qsort(entries, entry_count, sizeof(FileEntry), cmp_entries);
}

int main(void)
{
        term_init() orelse return 1;
        defer term_restore();

        load_dir(".");

        while (1)
        {
                char key = term_poll(16);

                // Keyboard inputs
                if (key == 'q')
                        break;
                if (key == 'j')
                        term_mouse.wheel += 1; // Vim down
                if (key == 'k')
                        term_mouse.wheel -= 1; // Vim up

                int cell_w = 14;
                int cell_h = 6;
                int cols = term_width / cell_w;
                if (cols < 1)
                        cols = 1;
                int rows = (entry_count + cols - 1) / cols;

                int list_start_y = 2;

                int max_scroll_lines = (rows * cell_h) - (term_height - list_start_y - 1);
                if (max_scroll_lines < 0)
                        max_scroll_lines = 0;

                target_scroll += term_mouse.wheel * 4.0f;
                if (target_scroll > max_scroll_lines)
                        target_scroll = max_scroll_lines;
                if (target_scroll < 0)
                        target_scroll = 0;

                current_scroll += (target_scroll - current_scroll) * 0.3f;
                int scroll_offset = (int)current_scroll;

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

                        bool hovered = (term_mouse.x >= screen_x && term_mouse.x < screen_x + cell_w &&
                                        term_mouse.y >= screen_y && term_mouse.y < screen_y + cell_h &&
                                        term_mouse.y >= list_start_y && term_mouse.y < term_height - 1);

                        Color item_bg = hovered ? clr_hover : clr_bg;
                        Color icon_fg = entries[i].is_dir ? clr_folder : clr_text;

                        if (hovered)
                                ui_rect(screen_x + 1, screen_y, cell_w - 2, cell_h, item_bg);

                        if (entries[i].is_dir)
                        {
                                ui_text(screen_x + 3, screen_y + 0, " .---.__", icon_fg, item_bg);
                                ui_text(screen_x + 3, screen_y + 1, " |   |  |", icon_fg, item_bg);
                                ui_text(screen_x + 3, screen_y + 2, " |   |  |", icon_fg, item_bg);
                                ui_text(screen_x + 3, screen_y + 3, " !---|__|", icon_fg, item_bg);
                        }
                        else
                        {
                                ui_text(screen_x + 3, screen_y + 0, "  .- \\", icon_fg, item_bg);
                                ui_text(screen_x + 3, screen_y + 1, "  |   \\", icon_fg, item_bg);
                                ui_text(screen_x + 3, screen_y + 2, "  |   |", icon_fg, item_bg);
                                ui_text(screen_x + 3, screen_y + 3, "  `---`", icon_fg, item_bg);
                        }

                        char name_buf[16];
                        strncpy(name_buf, entries[i].name, cell_w - 2);
                        if (strlen(entries[i].name) > cell_w - 2)
                        {
                                name_buf[cell_w - 4] = '.';
                                name_buf[cell_w - 3] = '.';
                        }

                        int pad = (cell_w - strlen(name_buf)) / 2;
                        ui_text(screen_x + pad, screen_y + 4, name_buf, clr_text, item_bg);

                        if (hovered && term_mouse.clicked && entries[i].is_dir)
                        {
                                load_dir(entries[i].name);
                                break;
                        }
                }

                ui_rect(0, 0, term_width, list_start_y, clr_bg);
                ui_rect(0, 0, term_width, 1, clr_bar);

                char header[PATH_MAX + 20];
                snprintf(header, sizeof(header), " Directory: %s ", cwd);
                ui_text(1, 0, header, (Color){0, 0, 0}, clr_bar);

                ui_rect(0, term_height - 1, term_width, 1, clr_bar);
                ui_text(1, term_height - 1, " Double-click [DIR] to open | Scroll to navigate | 'q' to quit ", (Color){0, 0, 0}, clr_bar);

                ui_cursor();
                ui_end();
        }

        return 0;
}