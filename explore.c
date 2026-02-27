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

typedef struct
{
        FileEntry *entries;
        int capacity;
        int count;
        char cwd[PATH_MAX];
        char next_dir[PATH_MAX];
        UIListState list;
        bool quit;
        UIContextState context;
        int target_idx;
} AppState;

int cmp_entries(const void *a, const void *b)
{
        FileEntry *ea = (FileEntry *)a, *eb = (FileEntry *)b;
        return ea->is_dir != eb->is_dir ? eb->is_dir - ea->is_dir : strcmp(ea->name, eb->name);
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
        app->list.target_scroll = app->list.current_scroll = app->list.dragging_scroll = 0;
        app->list.selected_idx = -1;

        for (;;)
        {
                struct dirent *dir = readdir(d) orelse break;
                if (!strcmp(dir->d_name, "."))
                        continue;

                if (app->count >= app->capacity)
                {
                        app->capacity = app->capacity ? app->capacity * 2 : 256;
                        void *tmp = realloc(app->entries, app->capacity * sizeof(FileEntry)) orelse return;
                        app->entries = tmp;
                }

                struct stat st;
                (stat(dir->d_name, &st) == 0) orelse continue;
                strcpy(app->entries[app->count].name, dir->d_name);
                app->entries[app->count++].is_dir = S_ISDIR(st.st_mode);
        }
        qsort(app->entries, app->count, sizeof(FileEntry), cmp_entries);
}

void draw_item_grid(AppState *app, int i, int sx, int sy, bool hovered, bool pressed, int cell_w, int cell_h)
{
        bool is_sel = (i == app->list.selected_idx);
        Color item_bg = pressed ? (Color){130, 130, 130} : ((hovered || is_sel) ? clr_hover : clr_bg);
        Color icon_fg = app->entries[i].is_dir ? clr_folder : clr_text;

        if (hovered || is_sel)
                ui_rect(sx + 1, sy, cell_w - 2, cell_h, item_bg);

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
        int y_off = pressed ? 1 : 0;

        for (int j = 0; j < 4; j++)
                ui_text(sx + 2, sy + j + y_off, icon[j], icon_fg, item_bg, pressed, false);
        if (elen > 0)
                ui_text(sx + 5, sy + 2 + y_off, ext, icon_fg, item_bg, pressed, true);

        char l1[16], l2[16];
        int mw = cell_w - 2, len = strlen(disp);
        strncpy(l1, disp, mw);
        if (len > mw)
        {
                strncpy(l2, disp + mw, mw);
                if (len > mw * 2)
                        strcpy(l2 + mw - 2, "..");
        }
        ui_text_centered(sx + 1, sy + 4 + y_off, mw, l1, clr_text, item_bg, pressed, false);
        if (l2[0])
                ui_text_centered(sx + 1, sy + 5 + y_off, mw, l2, clr_text, item_bg, pressed, false);
}

void draw_item_list(AppState *app, int i, int sx, int sy, bool hovered, bool pressed, int item_w)
{
        bool is_sel = (i == app->list.selected_idx);
        Color item_bg = pressed ? (Color){130, 130, 130} : ((hovered || is_sel) ? clr_hover : clr_bg);
        Color icon_fg = app->entries[i].is_dir ? clr_folder : clr_text;

        ui_rect(sx, sy, item_w, 1, item_bg);
        ui_text(sx + 1, sy, app->entries[i].is_dir ? "[DIR]" : "[FILE]", icon_fg, item_bg, false, false);

        char l[256];
        strncpy(l, app->entries[i].name, item_w - 9);
        ui_text(sx + 8, sy, l, clr_text, item_bg, false, false);
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
                float diff = app.list.target_scroll - app.list.current_scroll;
                int timeout = (!app.list.dragging_scroll && (diff > 0.01f || diff < -0.01f) || app.list.dragging_scroll) ? 16 : 1000;
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

                bool item_was_clicked = false;

                for (int i = 0; i < app.count; i++)
                {
                        int sx, sy;
                        bool hovered, pressed;

                        if (ui_list_do_item(&app.list, &params, i, &sx, &sy, &hovered, &pressed))
                        {
                                if (app.context.active)
                                {
                                        pressed = false;
                                        if (app.context.w > 0 &&
                                            term_mouse.x >= app.context.x && term_mouse.x < app.context.x + app.context.w &&
                                            term_mouse.y >= app.context.y && term_mouse.y < app.context.y + app.context.h)
                                        {
                                                hovered = false;
                                        }
                                }

                                if (app.list.mode == UI_MODE_GRID)
                                {
                                        draw_item_grid(&app, i, sx, sy, hovered, pressed, params.cell_w, params.cell_h);
                                }
                                else
                                {
                                        draw_item_list(&app, i, sx, sy, hovered, pressed, params.w - 1);
                                }

                                if (hovered && term_mouse.clicked && !app.context.active)
                                {
                                        item_was_clicked = true;
                                        if (app.entries[i].is_dir)
                                                strcpy(app.next_dir, app.entries[i].name);
                                }
                        }

                        if (hovered && term_mouse.right_clicked)
                        {
                                app.context.active = true;
                                app.context.x = term_mouse.x;
                                app.context.y = term_mouse.y;
                                app.target_idx = i;
                        }
                }

                ui_list_end(&app.list, &params);

                if (term_mouse.clicked && !item_was_clicked && !app.list.dragging_scroll && !app.context.active)
                        app.list.selected_idx = -1;

                ui_rect(0, 0, term_width, params.y, clr_bg);
                ui_rect(0, 0, term_width, 1, clr_bar);
                raw char header[PATH_MAX + 20];
                snprintf(header, sizeof(header), " Directory: %s ", app.cwd);
                ui_text(1, 0, header, (Color){0, 0, 0}, clr_bar, false, false);

                ui_rect(0, term_height - 1, term_width, 1, clr_bar);
                ui_text(1, term_height - 1, " Arrows: Navigate | v: Toggle View | Backspace: Up | 'q': Quit ", (Color){0, 0, 0}, clr_bar, false, false);

                const char *menu_options[] = {"Open", "Rename", "Delete", "Cancel"};
                int selected_action = -1;

                if (ui_context_menu(&app.context, menu_options, 4, &selected_action))
                {
                        FileEntry *target = &app.entries[app.target_idx];

                        switch (selected_action)
                        {
                        case 0: // Open
                                if (target->is_dir)
                                        strcpy(app.next_dir, target->name);
                                break;
                        case 1: // Rename
                                // TODO: trigger a text input primitive
                                break;
                        case 2: // Delete
                                // TODO: unlink() or rmdir()
                                break;
                        }
                }

                ui_cursor();
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