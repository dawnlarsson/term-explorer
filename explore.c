#include "terminal.c"
#include <dirent.h>
#include <sys/stat.h>
#include <limits.h>

Color clr_bg = {0, 0, 0};          // Classic Blue
Color clr_bar = {170, 170, 170};   // Light Gray
Color clr_text = {255, 255, 255};  // White
Color clr_folder = {255, 255, 85}; // Yellow
Color clr_hover = {170, 170, 170}; // Cyan

typedef struct
{
        char name[256];
        bool is_dir;
} FileEntry;

FileEntry entries[1024]; // Prism implicitly zero-initializes this
int entry_count = 0;
int scroll_y = 0; // In rows, not items
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
        scroll_y = 0;

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
                if (key == 'q')
                        break;

                // Grid Configuration
                int cell_w = 14;
                int cell_h = 6;
                int cols = term_width / cell_w;
                if (cols < 1)
                        cols = 1;
                int rows = (entry_count + cols - 1) / cols;

                // Handle Row Scrolling
                scroll_y += term_mouse.wheel;
                int max_visible_rows = (term_height - 3) / cell_h;
                int max_scroll = rows - max_visible_rows;
                if (max_scroll < 0)
                        max_scroll = 0;
                if (scroll_y > max_scroll)
                        scroll_y = max_scroll;
                if (scroll_y < 0)
                        scroll_y = 0;

                ui_begin();

                // 1. Draw Background & Top Bar
                ui_rect(0, 0, term_width, term_height, clr_bg);
                ui_rect(0, 0, term_width, 1, clr_bar);

                char header[PATH_MAX + 20];
                snprintf(header, sizeof(header), " Directory: %s ", cwd);
                ui_text(1, 0, header, (Color){0, 0, 0}, clr_bar);

                // 2. Draw File Grid
                int list_start_y = 2;

                for (int i = 0; i < entry_count; i++)
                {
                        int r = (i / cols) - scroll_y;
                        int c = (i % cols);

                        int screen_x = c * cell_w;
                        int screen_y = list_start_y + (r * cell_h);

                        // Skip items that are scrolled off-screen
                        if (screen_y < list_start_y || screen_y + cell_h >= term_height - 1)
                                continue;

                        // Check bounding box for mouse hover
                        bool hovered = (term_mouse.x >= screen_x && term_mouse.x < screen_x + cell_w &&
                                        term_mouse.y >= screen_y && term_mouse.y < screen_y + cell_h);

                        Color item_bg = hovered ? clr_hover : clr_bg;
                        Color icon_fg = entries[i].is_dir ? clr_folder : clr_text;

                        // Draw Selection Highlight Box
                        if (hovered)
                                ui_rect(screen_x + 1, screen_y, cell_w - 2, cell_h, item_bg);

                        // Draw ASCII Icon
                        if (entries[i].is_dir)
                        {
                                ui_text(screen_x + 3, screen_y + 0, " .---.__", icon_fg, item_bg);
                                ui_text(screen_x + 3, screen_y + 1, " |   |  |", icon_fg, item_bg);
                                ui_text(screen_x + 3, screen_y + 2, " |   |  |", icon_fg, item_bg);
                                ui_text(screen_x + 3, screen_y + 3, " !---|__|`", icon_fg, item_bg);
                        }
                        else
                        {
                                ui_text(screen_x + 3, screen_y + 0, "  .- \\", icon_fg, item_bg);
                                ui_text(screen_x + 3, screen_y + 1, "  |   \\", icon_fg, item_bg);
                                ui_text(screen_x + 3, screen_y + 2, "  |   |", icon_fg, item_bg);
                                ui_text(screen_x + 3, screen_y + 3, "  `---`", icon_fg, item_bg);
                        }

                        // Truncate name and center it
                        // 🌟 PRISM MAGIC: name_buf is implicitly zero-initialized!
                        char name_buf[16];
                        strncpy(name_buf, entries[i].name, cell_w - 2);
                        if (strlen(entries[i].name) > cell_w - 2)
                        {
                                name_buf[cell_w - 4] = '.';
                                name_buf[cell_w - 3] = '.';
                        }

                        int pad = (cell_w - strlen(name_buf)) / 2;
                        ui_text(screen_x + pad, screen_y + 4, name_buf, clr_text, item_bg);

                        // Handle Clicks
                        if (hovered && term_mouse.clicked && entries[i].is_dir)
                        {
                                load_dir(entries[i].name);
                                break; // Break loop to rebuild grid next frame
                        }
                }

                // 3. Draw Bottom Bar
                ui_rect(0, term_height - 1, term_width, 1, clr_bar);
                ui_text(1, term_height - 1, " Double-click [DIR] to open | Scroll to navigate | 'q' to quit ", (Color){0, 0, 0}, clr_bar);

                ui_cursor();
                ui_end();
        }

        return 0;
}