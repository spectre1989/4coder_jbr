#include <direct.h>
#include <Shlobj_core.h>
#pragma comment(lib, "Shell32")
#pragma comment(lib, "User32")

#include "4coder_default_include.cpp"

#if !defined(META_PASS)
#include "generated/managed_id_metadata.cpp"
#endif



static custom_hard_exit_type* g_default_hard_exit = 0;
static WINDOWPLACEMENT g_window_placement;
static bool g_window_placement_needs_restored;

static void jbr_hard_exit(Application_Links* app);
static void jbr_tick(Application_Links* app, Frame_Info frame_info);
static void write_panel_state(Application_Links* app, Scratch_Block* scratch, FILE* file, Panel_ID panel, String_u8* indent);
static void read_panel_state(Application_Links* app, Config_Compound* config_compound, Panel_ID panel);


void
custom_layer_init(Application_Links* app) {
    Thread_Context* tctx = get_thread_context(app);

    // NOTE(allen): setup for default framework
    default_framework_init(app);

    // NOTE(allen): default hooks and command maps
    set_all_default_hooks(app);
    mapping_init(tctx, &framework_mapping);
#if OS_MAC
    setup_mac_mapping(&framework_mapping, mapid_global, mapid_file, mapid_code);
#else
    setup_default_mapping(&framework_mapping, mapid_global, mapid_file, mapid_code);
#endif

    MappingScope();
    SelectMapping(&framework_mapping);

    SelectMap(mapid_global);
    BindCore(jbr_startup, CoreCode_Startup);
    set_custom_hook(app, HookID_Tick, jbr_tick);
    g_default_hard_exit = hard_exit;
    hard_exit = jbr_hard_exit;
}



CUSTOM_COMMAND_SIG(jbr_startup)
CUSTOM_DOC("")
{
    ProfileScope(app, "jbr_startup");
    default_startup(app);

    char path[MAX_PATH];
    HRESULT result = SHGetFolderPathA(
        0, // hwnd
        CSIDL_APPDATA, // csidl
        0, // hToken
        0, // flags
        path
    );
    if (result == S_OK)
    {
        size_t length = strlen(path);
        strncpy(&path[length], "\\4coder_jbr\\state.txt", MAX_PATH - length);
        
        Scratch_Block scratch(app);

        File_Name_Data file = dump_file(scratch, SCu8(path));
        if (file.data.size)
        {
            Token_Array token_array = token_array_from_text(app, scratch, SCu8(file.data));

            Config_Compound* root_panel = nullptr;

            Config_Parser parser = make_config_parser(scratch, file.file_name, SCu8(file.data), token_array);
            for (; !config_parser__recognize_token(&parser, TokenCppKind_EOF);) 
            {
                Config_Assignment* assignment = config_parser__assignment(&parser);
                if (assignment) 
                {
                    if (string_match(assignment->l->identifier, string_u8_litexpr("window_placement")))
                    {
                        if (assignment->r->type == ConfigRValueType_Compound)
                        {
                            g_window_placement = {};
                            g_window_placement.length = sizeof(g_window_placement);
                            g_window_placement.ptMaxPosition = { -1, -1 };
                            g_window_placement.ptMinPosition = { -1, -1 };

                            Config_Compound_Element* elem = assignment->r->compound->first;
                            while (elem)
                            {
                                if (string_match(elem->l.identifier, string_u8_litexpr("show_cmd")))
                                {
                                    if (elem->r->type == ConfigRValueType_Integer)
                                    {
                                        g_window_placement.showCmd = elem->r->uinteger;
                                        g_window_placement_needs_restored = true;
                                    }
                                }
                                else if (string_match(elem->l.identifier, string_u8_litexpr("left")))
                                {
                                    if (elem->r->type == ConfigRValueType_Integer)
                                    {
                                        g_window_placement.rcNormalPosition.left = elem->r->integer;
                                    }
                                }
                                else if (string_match(elem->l.identifier, string_u8_litexpr("right")))
                                {
                                    if (elem->r->type == ConfigRValueType_Integer)
                                    {
                                        g_window_placement.rcNormalPosition.right = elem->r->integer;
                                    }
                                }
                                else if (string_match(elem->l.identifier, string_u8_litexpr("top")))
                                {
                                    if (elem->r->type == ConfigRValueType_Integer)
                                    {
                                        g_window_placement.rcNormalPosition.top = elem->r->integer;
                                    }
                                }
                                else if (string_match(elem->l.identifier, string_u8_litexpr("bottom")))
                                {
                                    if (elem->r->type == ConfigRValueType_Integer)
                                    {
                                        g_window_placement.rcNormalPosition.bottom = elem->r->integer;
                                    }
                                }

                                elem = elem->next;
                            }
                        }
                    }
                    else if (string_match(assignment->l->identifier, string_u8_litexpr("loaded_files")))
                    {
                        if (assignment->r->type == ConfigRValueType_Compound)
                        {
                            Config_Compound_Element* elem = assignment->r->compound->first;
                            while (elem)
                            {
                                if (elem->r->type == ConfigRValueType_String)
                                {
                                    create_buffer(app, elem->r->string, BufferCreate_NeverNew | BufferCreate_MustAttachToFile);
                                }

                                elem = elem->next;
                            }
                        }
                    }
                    else if (string_match(assignment->l->identifier, string_u8_litexpr("root_panel")))
                    {
                        if (assignment->r->type == ConfigRValueType_Compound)
                        {
                            // cache this and restore panels later, want to load the buffers and restore
                            // state of scratch first, otherwise setting cursor pos won't work
                            root_panel = assignment->r->compound;
                        }
                    }
                    else if (string_match(assignment->l->identifier, string_u8_litexpr("scratch")))
                    {
                        if (assignment->r->type == ConfigRValueType_String)
                        {
                            Buffer_ID scratch_buffer = get_buffer_by_name(app, string_u8_litexpr("*scratch*"), Access_Always);
                            buffer_replace_range(app, scratch_buffer, Ii64((i64)0), assignment->r->string);
                        }
                    }
                }
            }

            if (root_panel)
            {
                // first close everything down to a single view
                View_ID view_iter = get_view_next(app, 0, Access_Always);
                while (view_iter)
                {
                    // 4ed seems to get angry and crash if you close the active view,
                    // so close all the others
                    if (view_iter != get_active_view(app, Access_Always))
                    {
                        view_close(app, view_iter);
                    }

                    view_iter = get_view_next(app, view_iter, Access_Always);
                }

                // restore panels
                read_panel_state(app, root_panel, panel_get_root(app));
            }
        }
    }
}

static void jbr_hard_exit(Application_Links* app)
{
    char path[MAX_PATH];
    HRESULT result = SHGetFolderPathA(
        0, // hwnd
        CSIDL_APPDATA, // csidl
        0, // hToken
        0, // flags
        path
    );
    if (result == S_OK)
    {
        size_t length = strlen(path);
        strncpy(&path[length], "\\4coder_jbr", MAX_PATH - length);
        mkdir(path);

        length = strlen(path);
        strncpy(&path[length], "\\state.txt", MAX_PATH - length);
        FILE* file = fopen(path, "w");
        if (file)
        {
            fprintf(file, "window_placement = \n{");
            fprintf(file, "\n\t.show_cmd = %u,", g_window_placement.showCmd);
            if (g_window_placement.showCmd != SW_MAXIMIZE)
            {
                fprintf(file, "\n\t.left = %u,", (u32)g_window_placement.rcNormalPosition.left);
                fprintf(file, "\n\t.right = %u,", (u32)g_window_placement.rcNormalPosition.right);
                fprintf(file, "\n\t.top = %u,", (u32)g_window_placement.rcNormalPosition.top);
                fprintf(file, "\n\t.bottom = %u,", (u32)g_window_placement.rcNormalPosition.bottom);
            }
            fprintf(file, "\n};\n\n");

            Scratch_Block scratch(app);

            fprintf(file, "loaded_files = \n{");

            for (Buffer_ID buffer = get_buffer_next(app, 0, Access_Always);
                buffer != 0;
                buffer = get_buffer_next(app, buffer, Access_Always))
            {
                String_Const_u8 file_name = push_buffer_file_name(app, scratch, buffer);
                if (file_name.size)
                {
                    fprintf(file, "\n\t\"%s\",", file_name.str);
                }
            }
            fprintf(file, "\n};\n\n");

            fprintf(file, "root_panel = \n");
            String_u8 indent = push_string_u8(scratch, 1);
            indent.str[0] = 0;
            write_panel_state(app, &scratch, file, panel_get_root(app), &indent);
            fprintf(file, ";\n\n");

            Buffer_ID scratch_buffer_id = get_buffer_by_name(app, string_u8_litexpr("*scratch*"), Access_Read);
            if (scratch_buffer_id)
            {
                i64 size = buffer_get_size(app, scratch_buffer_id);
                String_Const_u8 scratch_contents = push_string_u8(scratch, size + 1).string;
                buffer_read_range(app, scratch_buffer_id, {0, size}, scratch_contents.str);
                scratch_contents.str[size] = 0;
                scratch_contents.size = size;
                scratch_contents = string_replace(scratch, scratch_contents, string_u8_litexpr("\n"), string_u8_litexpr("\\n"));
                fprintf(file, "scratch = \"%s\";", scratch_contents.str);
            }

            fclose(file);
        }
    }

    g_default_hard_exit(app);
}

static void jbr_tick(Application_Links* app, Frame_Info frame_info) 
{
    default_tick(app, frame_info);

    HWND hwnd = GetActiveWindow();
    if (hwnd)
    {
        if (g_window_placement_needs_restored)
        {
            g_window_placement_needs_restored = false;

            if (g_window_placement.showCmd == SW_MAXIMIZE)
            {
                ShowWindow(hwnd, SW_MAXIMIZE);
            }
            else
            {
                SetWindowPlacement(hwnd, &g_window_placement);
            }
        }

        GetWindowPlacement(hwnd, &g_window_placement);
    }
}



static Rect_f32 panel_get_screen_rect(Application_Links* app, Panel_ID panel)
{
    if (panel_is_split(app, panel))
    {
        Rect_f32 min = panel_get_screen_rect(app, panel_get_child(app, panel, Side_Min));
        Rect_f32 max = panel_get_screen_rect(app, panel_get_child(app, panel, Side_Max));
        Rect_f32 rect;
        rect.x0 = min.x0;
        rect.y0 = min.y0;
        rect.x1 = max.x1;
        rect.y1 = max.y1;
        return rect;
    }
    else
    {
        return view_get_screen_rect(app, panel_get_view(app, panel, Access_Always));
    }
}

static void write_panel_state(Application_Links* app, Scratch_Block* scratch, FILE* file, Panel_ID panel, String_u8* indent)
{
    fprintf(file, "%s{\n", indent->str);

    if (panel_is_split(app, panel))
    {
        Panel_ID min_panel = panel_get_child(app, panel, Side_Min);
        Rect_f32 min_rect = panel_get_screen_rect(app, min_panel);
        Panel_ID max_panel = panel_get_child(app, panel, Side_Max);
        Rect_f32 max_rect = panel_get_screen_rect(app, max_panel);

        const char* split = 0;
        float t = 0.0f;
        if (min_rect.x0 == max_rect.x0)
        {
            split = "hsplit";
            t = min_rect.y1 / (max_rect.y1 - min_rect.y0);
        }
        else
        {
            split = "vsplit";
            t = min_rect.x1 / (max_rect.x1 - min_rect.x0);
        }

        fprintf(file, "%s\t.split = \"%s\",\n", indent->str, split);
        fprintf(file, "%s\t.t = %d,\n", indent->str, (int)(t * 1000.0f));

        bool can_increase_indent = (indent->cap - indent->size) > 1;
        if (can_increase_indent)
        {
            indent->str[indent->size++] = '\t';
            indent->str[indent->size] = 0;
        }
        else
        {
            String_u8 new_indent = push_string_u8(*scratch, indent->cap * 2);
            string_append(&new_indent, indent->string);
            new_indent.str[new_indent.size++] = '\t';
            new_indent.str[new_indent.size] = 0;
            *indent = new_indent;
        }

        fprintf(file, "%s.min = \n", indent->str);
        write_panel_state(app, scratch, file, min_panel, indent);
        fprintf(file, ",\n");

        fprintf(file, "%s.max = \n", indent->str);
        write_panel_state(app, scratch, file, max_panel, indent);
        fprintf(file, ",\n");

        --indent->size;
        indent->str[indent->size] = 0;
    }
    else
    {
        fprintf(file, "%s\t.split = \"none\",\n", indent->str);

        View_ID view = panel_get_view(app, panel, Access_Always);
        Buffer_ID buffer = view_get_buffer(app, view, Access_Always);
        String_Const_u8 file_name = push_buffer_file_name(app, *scratch, buffer);
        if (file_name.size)
        {
            fprintf(file, "%s\t.is_file = 1,\n", indent->str);
            fprintf(file, "%s\t.name = \"%s\",\n", indent->str, file_name.str);
        }
        else
        {
            String_Const_u8 unique_name = push_buffer_unique_name(app, *scratch, buffer);

            fprintf(file, "%s\t.is_file = 0,\n", indent->str);
            fprintf(file, "%s\t.name = \"%s\",\n", indent->str, unique_name.str);
        }
        
        fprintf(file, "%s\t.cursor_pos = %lld,\n", indent->str, view_get_cursor_pos(app, view));

        if (get_active_view(app, Access_Always) == view)
        {
            fprintf(file, "%s\t.is_active = 1,\n", indent->str);
            fprintf(file, "%s\t.mark_pos = %lld,\n", indent->str, view_get_mark_pos(app, view));
        }
        else
        {
            fprintf(file, "%s\t.is_active = 0,\n", indent->str);
        }
    }

    fprintf(file, "%s}", indent->str);
}

static void read_panel_state(Application_Links* app, Config_Compound* config_compound, Panel_ID panel)
{
    String_Const_u8 split = {};
    float t = 0.0f;
    i32 is_file = 0;
    String_Const_u8 name = {};
    Config_Compound* min = nullptr;
    Config_Compound* max = nullptr;
    i64 mark_pos = 0;
    i64 cursor_pos = 0;
    i32 is_active = 0;

    Config_Compound_Element* elem = config_compound->first;
    while (elem)
    {
        if (string_match(elem->l.identifier, string_u8_litexpr("split")))
        {
            if (elem->r->type == ConfigRValueType_String)
            {
                split = elem->r->string;
            }
        }
        else if (string_match(elem->l.identifier, string_u8_litexpr("t")))
        {
            if (elem->r->type == ConfigRValueType_Integer)
            {
                t = elem->r->integer / 1000.0f;
            }
        }
        else if (string_match(elem->l.identifier, string_u8_litexpr("min")))
        {
            if (elem->r->type == ConfigRValueType_Compound)
            {
                min = elem->r->compound;
            }
        }
        else if (string_match(elem->l.identifier, string_u8_litexpr("max")))
        {
            if (elem->r->type == ConfigRValueType_Compound)
            {
                max = elem->r->compound;
            }
        }
        else if (string_match(elem->l.identifier, string_u8_litexpr("is_file")))
        {
            if (elem->r->type == ConfigRValueType_Integer)
            {
                is_file = elem->r->integer;
            }
        }
        else if (string_match(elem->l.identifier, string_u8_litexpr("name")))
        {
            if (elem->r->type == ConfigRValueType_String)
            {
                name = elem->r->string;
            }
        }
        else if (string_match(elem->l.identifier, string_u8_litexpr("mark_pos")))
        {
            if (elem->r->type == ConfigRValueType_Integer)
            {
                mark_pos = elem->r->integer;
            }
        }
        else if (string_match(elem->l.identifier, string_u8_litexpr("cursor_pos")))
        {
            if (elem->r->type == ConfigRValueType_Integer)
            {
                cursor_pos = elem->r->integer;
            }
        }
        else if (string_match(elem->l.identifier, string_u8_litexpr("is_active")))
        {
            if (elem->r->type == ConfigRValueType_Integer)
            {
                is_active = elem->r->integer;
            }
        }

        elem = elem->next;
    }

    View_Split_Position view_split_position = -1;
    if (string_match(split, string_u8_litexpr("hsplit")))
    {
        view_split_position = ViewSplit_Bottom;
    }
    else if (string_match(split, string_u8_litexpr("vsplit")))
    {
        view_split_position = ViewSplit_Right;
    }

    if (view_split_position != -1)
    {
        View_ID view = panel_get_view(app, panel, Access_Always);
        View_ID new_view = open_view(app, view, view_split_position);
        new_view_settings(app, new_view);

        panel_set_split(app, panel, PanelSplitKind_Ratio_Min, t);

        Panel_ID panel_min = panel_get_child(app, panel, Side_Min);
        Panel_ID panel_max = panel_get_child(app, panel, Side_Max);

        read_panel_state(app, min, panel_min);
        read_panel_state(app, max, panel_max);
    }
    else
    {
        Buffer_ID buffer_id = 0;
        if (is_file)
        {
            buffer_id = get_buffer_by_file_name(app, name, Access_Always);
        }
        else
        {
            buffer_id = get_buffer_by_name(app, name, Access_Always);
        }

        View_ID view = panel_get_view(app, panel, Access_Always);
        view_set_buffer(app, view, buffer_id, 0);
        if (is_active)
        {
            view_set_active(app, view);
            view_set_mark(app, view, seek_pos(mark_pos));
            view_set_cursor(app, view, seek_pos(cursor_pos));
        }
        else
        {
            view_set_mark(app, view, seek_pos(cursor_pos));
            view_set_cursor(app, view, seek_pos(cursor_pos));
        }
    }
}