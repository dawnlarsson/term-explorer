#include "terminal.c"
#include <dirent.h>
#include <sys/stat.h>
#include <limits.h>

Color clr_bg = {0, 0, 0}, clr_bar = {170, 170, 170}, clr_text = {255, 255, 255}, clr_folder = {255, 255, 85}, clr_hover = {170, 170, 170};

typedef struct
{
        char name[256];
        bool is_dir;
        off_t size;
} FileEntry;

typedef struct
{
        FileEntry *entries;
        int capacity;
        int count;
        char cwd[PATH_MAX];
        char next_dir[PATH_MAX];
        UIListState list;
        bool quit;
} AppState;

int cmp_entries(const void *a, const void *b)
{
        FileEntry *ea = (FileEntry *)a, *eb = (FileEntry *)b;
        if (ea->is_dir != eb->is_dir)
                return eb->is_dir - ea->is_dir;
        if (!strcmp(ea->name, ".."))
                return -1;
        if (!strcmp(eb->name, ".."))
                return 1;
        return strcmp(ea->name, eb->name);
}

void app_load_dir(AppState *app, const char *path)
{
        DIR *d = opendir(path) orelse
        {
                d = opendir(".");
                strcpy(app->cwd, ".");
        };
        defer closedir(d);

        (chdir(path) == 0) orelse return;
        getcwd(app->cwd, sizeof(app->cwd)) orelse return;

        app->count = 0;
        ui_list_reset(&app->list);

        for (;;)
        {
                struct dirent *dir = readdir(d) orelse break;
                if (!strcmp(dir->d_name, "."))
                        continue;

                if (app->count >= app->capacity)
                {
                        int new_cap = app->capacity ? app->capacity * 2 : 256;
                        void *tmp = realloc(app->entries, new_cap * sizeof(FileEntry)) orelse return;
                        app->capacity = new_cap;
                        app->entries = tmp;
                }

                struct stat st;
                (stat(dir->d_name, &st) == 0) orelse continue;
                strcpy(app->entries[app->count].name, dir->d_name);
                app->entries[app->count].size = st.st_size;
                app->entries[app->count++].is_dir = S_ISDIR(st.st_mode);
        }
        qsort(app->entries, app->count, sizeof(FileEntry), cmp_entries);

        if (app->count > 0)
                app->list.selected_idx = 0;
}

void draw_item_grid(AppState *app, int i, const UIItemResult *item)
{
        bool is_sel = (i == app->list.selected_idx);
        Color item_bg = item->pressed ? (Color){130, 130, 130} : ((item->hovered || is_sel) ? clr_hover : clr_bg);
        Color icon_fg = app->entries[i].is_dir ? clr_folder : clr_text;

        if (item->hovered || is_sel)
                ui_rect(item->x + 1, item->y, item->w - 2, item->h, item_bg);

        raw char disp[256];
        strcpy(disp, app->entries[i].name);
        char ext[5] = ".   ";
        int elen = 0;
        if (!app->entries[i].is_dir)
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
        const char **icon = app->entries[i].is_dir ? icon_dir : icon_file;
        int y_off = item->pressed ? 1 : 0;

        for (int j = 0; j < 4; j++)
                ui_text(item->x + 2, item->y + j + y_off, icon[j], icon_fg, item_bg, item->pressed, false);
        if (elen > 0)
                ui_text(item->x + 5, item->y + 2 + y_off, ext, icon_fg, item_bg, item->pressed, true);

        char l1[16], l2[16];
        int mw = item->w - 2, len = strlen(disp);
        strncpy(l1, disp, mw);
        if (len > mw)
        {
                strncpy(l2, disp + mw, mw);
                if (len > mw * 2)
                        strcpy(l2 + mw - 2, "..");
        }
        ui_text_centered(item->x + 1, item->y + 4 + y_off, mw, l1, clr_text, item_bg, item->pressed, false);
        if (l2[0])
                ui_text_centered(item->x + 1, item->y + 5 + y_off, mw, l2, clr_text, item_bg, item->pressed, false);
}

void draw_item_list(AppState *app, int i, const UIItemResult *item)
{
        bool is_sel = (i == app->list.selected_idx);
        Color item_bg = item->pressed ? (Color){130, 130, 130} : ((item->hovered || is_sel) ? clr_hover : clr_bg);
        Color icon_fg = app->entries[i].is_dir ? clr_folder : clr_text;

        ui_rect(item->x, item->y, item->w, 1, item_bg);
        ui_text(item->x + 1, item->y, app->entries[i].is_dir ? "[DIR]" : "[FILE]", icon_fg, item_bg, false, false);

        raw char l[256];
        int copy_len = item->w - 9 > 255 ? 255 : item->w - 9;
        strncpy(l, app->entries[i].name, copy_len);
        l[copy_len] = '\0';
        ui_text(item->x + 8, item->y, l, clr_text, item_bg, false, false);

        raw char size_str[32];
        if (!app->entries[i].is_dir)
        {
                if (app->entries[i].size < 1024)
                        snprintf(size_str, 32, "%lld B", (long long)app->entries[i].size);
                else if (app->entries[i].size < 1024 * 1024)
                        snprintf(size_str, 32, "%lld KB", (long long)(app->entries[i].size / 1024));
                else if (app->entries[i].size < 1024 * 1024 * 1024)
                        snprintf(size_str, 32, "%lld MB", (long long)(app->entries[i].size / (1024 * 1024)));
                else
                        snprintf(size_str, 32, "%lld GB", (long long)(app->entries[i].size / (1024 * 1024 * 1024)));
                ui_text(item->x + item->w - 10, item->y, size_str, clr_bar, item_bg, false, false);
        }
}

void handle_input(AppState *app, int key, const UIListParams *params)
{
        if (key == 'q' || key == KEY_ESC)
                app->quit = true;
        if (key == 'v')
                ui_list_set_mode(&app->list, params, !app->list.mode);
        if (key == KEY_BACKSPACE)
                strcpy(app->next_dir, "..");
        if (key == KEY_ENTER && app->count > 0 && app->list.selected_idx >= 0 && app->entries[app->list.selected_idx].is_dir)
                strcpy(app->next_dir, app->entries[app->list.selected_idx].name);
}

int main(void)
{
        term_init() orelse return 1;
        defer term_restore();

        AppState app;

        defer free(app.entries);
        app_load_dir(&app, ".");

        bool first_frame = true;

        while (!app.quit)
        {
                int timeout = ui_list_is_animating(&app.list) ? term_anim_timeout : 1000;
                if (first_frame)
                {
                        timeout = 0;
                        first_frame = false;
                }

                int key = term_poll(timeout);

                UIListParams params = {
                    .x = 0, .y = 1, .w = term_width, .h = term_height - 2 > 0 ? term_height - 2 : 1, .item_count = app.count, .cell_w = 14, .cell_h = 7, .bg = clr_bg, .scrollbar_bg = (Color){30, 30, 30}, .scrollbar_fg = clr_bar};

                handle_input(&app, key, &params);

                ui_begin();
                ui_clear(clr_bg);

                ui_list_begin(&app.list, &params, key);

                for (int i = 0; i < app.count; i++)
                {
                        UIItemResult item;
                        ui_list_do_item(&app.list, i, &item) orelse continue;

                        if (app.list.mode == UI_MODE_GRID)
                                draw_item_grid(&app, i, &item);
                        else
                                draw_item_list(&app, i, &item);

                        if (item.clicked && app.entries[i].is_dir)
                                strcpy(app.next_dir, app.entries[i].name);
                        if (item.right_clicked)
                                ui_context_open(i);
                }

                ui_list_end(&app.list);

                ui_rect(0, 0, term_width, params.y, clr_bg);
                ui_rect(0, 0, term_width, 1, clr_bar);
                raw char header[PATH_MAX + 20];
                snprintf(header, sizeof(header), "%s ", app.cwd);
                ui_text(1, 0, header, (Color){0, 0, 0}, clr_bar, false, false);

                ui_rect(0, term_height - 1, term_width, 1, clr_bar);
                ui_text(1, term_height - 1, " Arrows: Navigate | v: Toggle View | Backspace: Up | 'q': Quit ", (Color){0, 0, 0}, clr_bar, false, false);

                const char *menu_options[] = {"Open", "Rename", "Delete", "Cancel"};
                int selected_action = -1;

                if (ui_context_do(menu_options, 4, &selected_action))
                {
                        FileEntry *target = &app.entries[ui_context_target()];
                        switch (selected_action)
                        {
                        case 0:
                                if (target->is_dir)
                                        strcpy(app.next_dir, target->name);
                                break;
                        case 1:
                                break;
                        case 2:
                                break;
                        }
                }

                ui_end();

                if (app.next_dir[0] != '\0')
                {
                        app_load_dir(&app, app.next_dir);
                        app.next_dir[0] = '\0';
                        first_frame = true;
                }
        }
        return 0;
}