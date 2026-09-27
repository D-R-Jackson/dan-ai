#include <gtk/gtk.h>
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <json-glib/json-glib.h>

typedef struct{
    char *id;
    char *title;
    char *model;
    GPtrArray *messages;
} Chat;
typedef struct{GtkEntry *prompt_entry;
            GtkDropDown *model_dropdown;
            GtkBox *conversation_box;
            GtkButton *generate_button;
            GtkScrolledWindow *response_scrolled;
            GPtrArray *chats;
            GtkBox *chat_list_box;
            Chat *current_chat;
            char *loaded_model;
            GtkBox *sidebar_box;
        } AppData;

typedef struct{
    GString *buffer;
    GString *full_response;
    AppData *app;
    GtkTextView *text_view;
} StreamData;
typedef struct{
    AppData *app;
    GtkTextView *text_view;
    char *text;
} ResponseUpdate;
typedef struct{
    AppData *app;
    char *model;
    char *prompt;
    GtkTextView *assistant_view;
} GenerateRequest;
typedef struct{
    char *role;
    char *content;
}Message;


static void free_message(Message *message){
    if(message == NULL){
        return;
    }
    g_free(message->role);
    g_free(message->content);
    g_free(message);
}

static void free_chat(Chat *chat){
    if(chat == NULL){
        return;
    }
    g_free(chat->id);
    g_free(chat->title);
    g_free(chat->model);\

    g_ptr_array_free(chat->messages, TRUE);
    g_free(chat);
}

static Chat *create_chat(const char *model){
    Chat *chat = g_new0(Chat, 1);

    chat->id = g_uuid_string_random();
    chat->title = g_strdup("New Chat");
    chat->model = g_strdup(model);
    chat->messages = g_ptr_array_new_with_free_func((GDestroyNotify)free_message);

    return chat;
}


static Chat *load_chat_file(const char *file_path){
    JsonParser *parser;
    JsonNode *root_node;
    JsonObject *root;
    JsonArray *messages_array;

    Chat *chat;
    parser = json_parser_new();
    if(!json_parser_load_from_file(parser, file_path, NULL)){
        g_object_unref(parser);
        return NULL;
    }
    root_node = json_parser_get_root(parser);
    root = json_node_get_object(root_node);

    chat = g_new0(Chat, 1);
    chat->id = g_strdup(json_object_get_string_member(root, "id"));
    chat->title = g_strdup(json_object_get_string_member(root, "title"));
    chat->model = g_strdup(json_object_get_string_member(root, "model"));
    chat->messages = g_ptr_array_new_with_free_func((GDestroyNotify)free_message);
    messages_array = json_object_get_array_member(root, "messages");
    for(guint i = 0; i<json_array_get_length(messages_array); i++){
        JsonObject *message_object = json_array_get_object_element(messages_array, i);
        Message *message = g_new0(Message, 1);
        message->role = g_strdup(json_object_get_string_member(message_object, "role"));
        message->content = g_strdup(json_object_get_string_member(message_object, "content"));
        g_ptr_array_add(chat->messages, message);
    }
    g_object_unref(parser);
    return chat;
}



static char *get_chats_dir(void){
    char *config_dir;
    char *chats_dir;

    config_dir = g_build_filename(g_get_user_config_dir(), "dan-ai", NULL);
    chats_dir = g_build_filename(config_dir, "chats", NULL);
    g_free(config_dir);
    g_mkdir_with_parents(chats_dir, 0700);
    return chats_dir;
}

static char *get_chat_file(Chat *chat){
    char *chats_dir;
    char *filename;
    char *path;

    chats_dir = get_chats_dir();
    filename = g_strdup_printf("%s.json", chat->id);
    path = g_build_filename(chats_dir, filename, NULL);
    g_free(chats_dir);
    g_free(filename);

    return path;
}

static void load_chats(AppData *data){
    char *chats_dir;
    GDir *dir;
    const char *filename;
    chats_dir = get_chats_dir();
    dir = g_dir_open(chats_dir, 0, NULL);
    if(dir == NULL){
        g_free(chats_dir);
        return;
    } 
    while((filename = g_dir_read_name(dir)) != NULL){
        if(!g_str_has_suffix(filename, ".json")){
            continue;
        }
        char *file_path = g_build_filename(chats_dir, filename, NULL);
        Chat *chat = load_chat_file(file_path);
        if(chat != NULL){
            g_ptr_array_add(data->chats, chat);
            printf("Loaded Chat -> %s\n", chat->id);
        }
        g_free(file_path);
    }
    g_dir_close(dir);
    g_free(chats_dir);
}

static gboolean save_chat(Chat *chat){
    JsonObject *root;
    JsonArray *messages_array;
    JsonNode *root_node;
    JsonGenerator *generator;

    char *json_data;
    char *file_path;

    root = json_object_new();
    json_object_set_string_member(root, "id", chat->id);
    json_object_set_string_member(root, "title", chat->title);
    json_object_set_string_member(root, "model", chat->model);

    messages_array = json_array_new();
    for(guint i = 0; i < chat->messages->len; i++){
        Message *message = g_ptr_array_index(chat->messages, i);
        JsonObject *message_object = json_object_new();
        json_object_set_string_member(message_object, "role", message->role);
        json_object_set_string_member(message_object, "content", message->content);
        JsonNode *message_node = json_node_new(JSON_NODE_OBJECT);

        json_node_set_object(message_node, message_object);
        json_array_add_element(messages_array, message_node);

        json_object_unref(message_object);
    }
    json_object_set_array_member(root, "messages", messages_array);
    root_node = json_node_new(JSON_NODE_OBJECT);
    json_node_set_object(root_node, root);
    generator = json_generator_new();
    json_generator_set_root(generator, root_node);
    json_data = json_generator_to_data(generator, NULL);
    file_path = get_chat_file(chat);
    gboolean success = g_file_set_contents(file_path, json_data, -1, NULL);
    if(success){
        printf("Saved Chat -> %s\n", file_path);
    }else{
        printf("Failed to save Chat -> %s\n", file_path);
    }
    g_free(file_path);
    g_free(json_data);

    g_object_unref(generator);
    json_node_unref(root_node);

    return success;
}

static gboolean resize_text_view(GtkWidget *widget, GdkFrameClock *clock, gpointer user_data){
    (void)clock;
    (void)user_data;

    GtkTextView *text_view = GTK_TEXT_VIEW(widget);
    int width = gtk_widget_get_width(widget);
    int height = gtk_widget_get_height(widget);
    
    printf("TICK | Textview %p | width=%d | height=%d\n", (void *)text_view, width, height);
    if(width<=0){
        return G_SOURCE_CONTINUE;
    }
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(text_view);
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(buffer, &end);

    GdkRectangle rectangle;

    gtk_text_view_get_iter_location(text_view, &end, &rectangle);

    int required_height = rectangle.y + rectangle.height;
    printf("Required Height -> %d\n", required_height);

    gtk_widget_set_size_request(widget, -1, required_height);
    return G_SOURCE_REMOVE;
}

static GtkTextView *add_message_view(AppData *app, const char *role, const char *content){
    GtkWidget *message_box;
    GtkWidget *label;
    GtkWidget *text_view;
    GtkTextBuffer *buffer;

    message_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_add_css_class(message_box, "message");
    if(g_strcmp0(role, "You") == 0){
        gtk_widget_add_css_class(message_box, "user-message");
    }else{
        gtk_widget_add_css_class(message_box, "assistant-message");
    }
    label = gtk_label_new(role);
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    gtk_widget_add_css_class(label, "message-role");
    gtk_box_append(GTK_BOX(message_box), label);
    text_view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(text_view), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(text_view), GTK_WRAP_WORD_CHAR);
    gtk_widget_set_hexpand(text_view, TRUE); 
    gtk_widget_add_css_class(text_view, "message-text");
    buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(text_view));
    if(content != NULL){
        gtk_text_buffer_set_text(buffer, content, -1);
    }
    gtk_box_append(GTK_BOX(message_box), text_view);
    gtk_box_append(app->conversation_box, message_box);
    gtk_widget_add_tick_callback(text_view, resize_text_view, NULL, NULL);
    return GTK_TEXT_VIEW(text_view);
}

static void clear_convorsation(AppData *data){
    GtkWidget *child;
    while((child = gtk_widget_get_first_child(GTK_WIDGET(data->conversation_box))) != NULL){
        gtk_box_remove(data->conversation_box, child);
    }
}
static void render_chat(AppData *data, Chat *chat){
    guint i;
    clear_convorsation(data);
    printf("Rendering Chat -> %s\n", chat->id);
    printf("%u Messages in Chat\n", chat->messages->len);
    for(i = 0; i< chat->messages->len; i++){
        Message *message = g_ptr_array_index(chat->messages, i);
        printf("Message %u | Role='%s' | content='%s'\n", i, message->role, message->content);
        const char *display_role;
        if(g_strcmp0(message->role, "user") == 0){
            display_role = "You";
        }else if(g_strcmp0(message->role, "assistant") == 0){
            display_role = "Dan-AI";
        }else{
            display_role = message->role;
        }
        add_message_view(data,display_role, message->content);
    }
    gtk_widget_queue_resize(GTK_WIDGET(data->conversation_box));
}



static void switch_chat(AppData *data, Chat *chat){
    if(chat == NULL){
        return;
    }
    data->current_chat = chat;
    render_chat(data,chat);
    printf("Switched Chat -> %s\n", chat->id);
}

static void chat_button_clicked(GtkButton *button, gpointer user_data){
    (void)button;

    Chat *chat = user_data;
    AppData *data = g_object_get_data(G_OBJECT(button), "app-data");
    switch_chat(data, chat);
}

static void refresh_chat_list(AppData *data){
    GtkWidget *child;
    while((child = gtk_widget_get_first_child(GTK_WIDGET(data->chat_list_box))) != NULL){
        gtk_box_remove(data->chat_list_box, child);
    }
    for(guint i = 0; i<data->chats->len; i++){
        Chat *chat = g_ptr_array_index(data->chats, i);
        GtkWidget *button = gtk_button_new_with_label(chat->title);
        g_object_set_data(G_OBJECT(button), "app-data", data);
        g_signal_connect(button, "clicked", G_CALLBACK(chat_button_clicked), chat);
        gtk_widget_set_hexpand(button,TRUE);
        gtk_box_append(data->chat_list_box, button);
    }
}







static void new_chat(GtkButton *button, gpointer user_data){
    (void)button;

    AppData *data = user_data;
    Chat *chat;
    if(data->current_chat != NULL && data->current_chat->messages->len <= 0){
        return;
    }
    chat = create_chat(data->loaded_model);
    g_ptr_array_add(data->chats, chat);
    data->current_chat = chat;
    clear_convorsation(data);
    refresh_chat_list(data);
    printf("Created New Chat -> %s\n", chat->id);
}







static GtkStringList * parse_models(const char *json){
    JsonParser *parser;
    JsonNode *root;
    JsonObject *object;
    JsonArray *models;
    GtkStringList *model_list;
    guint length;
    parser = json_parser_new();
    model_list = gtk_string_list_new(NULL);

    if(!json_parser_load_from_data(parser, json, -1, NULL)){
        fprintf(stderr, "Failed to parse JSON\n");
        g_object_unref(parser);
        return model_list;
    }

    root = json_parser_get_root(parser);
    object = json_node_get_object(root);
    models = json_object_get_array_member(object, "models");

    length = json_array_get_length(models);
    printf("Models Installed: %u\n", length);

    for(guint i = 0; i<length;i++){
        JsonObject *model;
        const char *name;

        model = json_array_get_object_element(models, i);
        name = json_object_get_string_member(model, "name");

        printf("-> %s\n", name);
        gtk_string_list_append(model_list, name);
    }
    g_object_unref(parser);
    return model_list;
}

static void free_generate_request(GenerateRequest *request){
    g_free(request->model);
    g_free(request->prompt);
    g_free(request);
}



static gboolean update_response(gpointer user_data){
    ResponseUpdate *update = user_data;
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(update->text_view);
    gtk_text_buffer_insert_at_cursor(buffer, update->text, -1);

    GtkAdjustment *adjustment = gtk_scrolled_window_get_vadjustment(update->app->response_scrolled);
    gtk_adjustment_set_value(adjustment, gtk_adjustment_get_upper(adjustment));

    g_free(update->text);
    g_free(update);

    return G_SOURCE_REMOVE;
}

static size_t write_callback(void *contents, size_t size, size_t nmemb, void *userp){
    size_t total_size = size * nmemb;
    GString *response = userp;
    g_string_append_len(response, contents, total_size);
    return total_size;
}

static size_t stream_callback(void *contents, size_t size, size_t nmemb, void *user_data){
    StreamData *stream = user_data;
    size_t total = size*nmemb;
    g_string_append_len(stream->buffer, contents, total);
    while(TRUE){
        char *new_line = strchr(stream->buffer->str, '\n');
        if(new_line == NULL){
            break;
        }

        size_t line_length = new_line - stream->buffer->str;
        if(line_length > 0){
            char *line = g_strndup(stream->buffer->str, line_length);
            JsonParser *parser = json_parser_new();
            GError *error = NULL;

            if(json_parser_load_from_data(parser, line, -1, &error)){
                JsonNode *root = json_parser_get_root(parser);
                JsonObject *object = json_node_get_object(root);

                if(json_object_has_member(object, "message")){
                    JsonObject *message_object = json_object_get_object_member(object, "message");
                    if(json_object_has_member(message_object, "content")){
                        const char *text = json_object_get_string_member(message_object, "content");
                        g_string_append(stream->full_response, text);
                        ResponseUpdate *update = g_new(ResponseUpdate, 1);
                        update->app = stream->app;
                        update->text_view = stream->text_view;
                        update->text = g_strdup(text);

                        g_main_context_invoke(NULL, update_response, update);
                    }
                }
            }
            if(error != NULL){
                g_error_free(error);
            }
            g_object_unref(parser);
            g_free(line);
        }
        g_string_erase(stream->buffer, 0, line_length+1);
    }
    return total;
}

static GtkStringList * get_models(void){
    CURL *curl;
    CURLcode result;
    GString *response;
    response = g_string_new(NULL);
    curl = curl_easy_init();

    if(curl == NULL){
        fprintf(stderr, "Failed to initialise curl\n");
        g_string_free(response, TRUE);
        return gtk_string_list_new(NULL);
    }

    curl_easy_setopt(curl, CURLOPT_URL, "http://localhost:11434/api/tags");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, response);
    result = curl_easy_perform(curl);
    if(result != CURLE_OK){
        fprintf(stderr, "HTTP request failed %s\n", curl_easy_strerror(result));
        curl_easy_cleanup(curl);
        g_string_free(response, TRUE);
        return gtk_string_list_new(NULL);
    }
    curl_easy_cleanup(curl);
    printf("Got %zu bytes\n", response->len);
    GtkStringList *models = parse_models(response->str);
    g_string_free(response, TRUE);
    return models;
}




static void send_prompt(AppData *app, const char *model, const char *prompt, GtkTextView *assistant_view){
    CURL *curl;
    CURLcode result;
    JsonObject *request_object;
    JsonNode *request_node;
    JsonGenerator *generator;
    gchar *json_body;
    
    StreamData stream;

    stream.buffer = g_string_new(NULL);
    stream.app = app;
    stream.full_response = g_string_new(NULL);
    stream.text_view = assistant_view;
    


    request_object = json_object_new();
    json_object_set_string_member(request_object, "model", model);
    json_object_set_boolean_member(request_object, "stream", TRUE);
    JsonArray *messages = json_array_new();
    for(guint i = 0; i<app->current_chat->messages->len; i++){
        Message *message = g_ptr_array_index(app->current_chat->messages, i);
        JsonObject *message_object = json_object_new();
        json_object_set_string_member(message_object, "role", message->role);
        json_object_set_string_member(message_object, "content", message->content);
        json_array_add_object_element(messages, message_object);
    }
    json_object_set_array_member(request_object, "messages", messages);

    request_node = json_node_new(JSON_NODE_OBJECT);
    json_node_set_object(request_node, request_object);

    generator = json_generator_new();
    json_generator_set_root(generator, request_node);

    json_body = json_generator_to_data(generator, NULL);
    



    curl = curl_easy_init();
    if(curl == NULL){
        fprintf(stderr, "Failed to initialise curl\n");

        g_free(json_body);
        g_object_unref(generator);
        json_node_free(request_node);

        return;
    }

    curl_easy_setopt(curl, CURLOPT_URL, "http://localhost:11434/api/chat");
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_body);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, stream_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &stream);

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    result = curl_easy_perform(curl);
    printf("FULL RESPONSE -> \n%s\n", stream.full_response->str);
    
    Message *message = g_new(Message, 1);
    message->role = g_strdup("assistant");
    message->content = g_strdup(stream.full_response->str);
    g_ptr_array_add(app->current_chat->messages, message);

    save_chat(app->current_chat);

    if(result != CURLE_OK){
        fprintf(stderr, "HTTP request failed: %s\n", curl_easy_strerror(result));
        return;
    }
    
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    g_free(json_body);
    g_object_unref(generator);
    json_node_free(request_node);
    g_string_free(stream.buffer, TRUE);
    g_string_free(stream.full_response, TRUE);

    return;
}

static void generate_task_thread(GTask *task, gpointer source_object, gpointer task_data, GCancellable *cancellable){
    (void)source_object;
    (void)cancellable;

    GenerateRequest *request = task_data;

    send_prompt(request->app, request->model, request->prompt, request->assistant_view);

    g_task_return_boolean(task,TRUE);
}

static void generate_task_finished(GObject *source_object, GAsyncResult *result, gpointer user_data){
    (void)source_object;

    AppData *data = user_data;
    GError *error = NULL;

    gboolean success = g_task_propagate_boolean(G_TASK(result), &error);

    if(!success){
        fprintf(stderr, "Generation Failed: %s\n", error->message);
        g_error_free(error);
    }

    gtk_widget_set_sensitive(GTK_WIDGET(data->generate_button), TRUE);
    gtk_button_set_label(data->generate_button, "Generate");
    gtk_widget_set_sensitive(GTK_WIDGET(data->model_dropdown), TRUE);
    gtk_widget_set_sensitive(GTK_WIDGET(data->chat_list_box), TRUE);
    gtk_widget_set_sensitive(GTK_WIDGET(data->sidebar_box), TRUE);

}

static void generate_response(GtkButton *button, gpointer user_data){
    (void)button;
    AppData *data = user_data;


    


    gtk_widget_set_sensitive(GTK_WIDGET(data->generate_button), FALSE);
    gtk_button_set_label(data->generate_button, "Generating...");
    gtk_widget_set_sensitive(GTK_WIDGET(data->model_dropdown), FALSE);
    gtk_widget_set_sensitive(GTK_WIDGET(data->chat_list_box), FALSE);
    gtk_widget_set_sensitive(GTK_WIDGET(data->sidebar_box), FALSE);

    const char *prompt = gtk_editable_get_text(GTK_EDITABLE(data->prompt_entry));
    
    if(data->current_chat != NULL && data->current_chat->messages->len <= 0){
        g_free(data->current_chat->title);
        data->current_chat->title = g_strndup(prompt, 25);
        refresh_chat_list(data);
    }
    add_message_view(data,"You", prompt);
    guint selected = gtk_drop_down_get_selected(data->model_dropdown);



    Message *message = g_new(Message, 1);
    message->role = g_strdup("user");
    message->content = g_strdup(prompt);
    g_ptr_array_add(data->current_chat->messages, message);

    const char *model = gtk_string_list_get_string(GTK_STRING_LIST(gtk_drop_down_get_model(data->model_dropdown)),selected);
    printf("Model -> %s\n", model);
    printf("Prompt -> %s\n", prompt);

    GenerateRequest *request = g_new(GenerateRequest, 1);
    request->app = data;
    request->model = g_strdup(model);
    request->prompt = g_strdup(prompt);

    GtkTextView *assistant_view = add_message_view(data, "Dan-AI", "");

    request->assistant_view = assistant_view;

    GTask *task = g_task_new(NULL,NULL,generate_task_finished, data);
    g_task_set_task_data(task, request, (GDestroyNotify)free_generate_request);
    g_task_run_in_thread(task, generate_task_thread);
    g_object_unref(task);
    gtk_editable_set_text(GTK_EDITABLE(data->prompt_entry), "");
    
    
}

static gboolean ollama_model_request(const char *model, const char *prompt, int keep_alive){
    CURL *curl;
    CURLcode result;
    GString *response;
    struct curl_slist *headers;

    JsonBuilder *builder;
    JsonNode *request_node;
    char *json_body;

    builder = json_builder_new();
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "model");
    json_builder_add_string_value(builder, model);
    json_builder_set_member_name(builder, "prompt");
    json_builder_add_string_value(builder, prompt);
    json_builder_set_member_name(builder, "stream");
    json_builder_add_boolean_value(builder, FALSE);
    json_builder_set_member_name(builder, "keep_alive");
    json_builder_add_int_value(builder, keep_alive);
    json_builder_end_object(builder);

    request_node = json_builder_get_root(builder);
    json_body = json_to_string(request_node, FALSE);

    curl = curl_easy_init();
    if(curl == NULL){
        json_node_free(request_node);
        g_object_unref(builder);
        g_free(json_body);
        return FALSE;
    }

    response = g_string_new(NULL);
    headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    curl_easy_setopt(curl,CURLOPT_URL,"http://localhost:11434/api/generate");
    curl_easy_setopt(curl,CURLOPT_HTTPHEADER,headers);
    curl_easy_setopt(curl,CURLOPT_POST,1L);
    curl_easy_setopt(curl,CURLOPT_POSTFIELDS, json_body);
    curl_easy_setopt(curl,CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl,CURLOPT_WRITEDATA,response);

    result = curl_easy_perform(curl);
    gboolean success = (result == CURLE_OK);
    if(!success){
        fprintf(stderr, "Ollama request failed: %s\n", curl_easy_strerror(result));
    }
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    g_string_free(response, TRUE);
    g_free(json_body);
    json_node_free(request_node);
    g_object_unref(builder);

    return success;
}

static gboolean unload_model(const char *model){
    printf("Unloading Model -> %s\n", model);
    return ollama_model_request(model, "", 0);
}
static gboolean preload_model(const char *model){
    printf("Loading Model -> %s\n", model);
    return ollama_model_request(model, "", -1);
}



static void model_switch_thread(GTask *task, gpointer source_object, gpointer task_data, GCancellable *cancellable){
    (void)source_object;
    (void)cancellable;

    char **models = task_data;

    const char *old_model = models[0];
    const char *new_model = models[1];

    if(old_model != NULL){
        if(!unload_model(old_model)){
            g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to unload model: %s", old_model);
            return;
        }
    }
    if(!preload_model(new_model)){
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to load model: %s", new_model);
        return;
    }
    g_task_return_pointer(task,g_strdup(new_model),g_free);

}
static char * get_config_file(void){
    const char *config_dir;

    char *app_config_dir;
    char *config_file;

    config_dir = g_get_user_config_dir();

    app_config_dir = g_build_filename(config_dir, "dan-ai", NULL);
    g_mkdir_with_parents(app_config_dir, 0755);
    config_file = g_build_filename(app_config_dir, "config", NULL);
    g_free(app_config_dir);
    return config_file;
}
static void save_model(const char *model){
    char *config_file;

    config_file = get_config_file();

    if(!g_file_set_contents(config_file, model, -1, NULL)){
        fprintf(stderr, "Failed to save model\n");
    }else{
        printf("Saved model -> %s\n", model);
    }
    g_free(config_file);
}
static void model_switch_finished(GObject *source_object, GAsyncResult *result, gpointer user_data){
    (void)source_object;

    AppData *data = user_data;
    GError *error = NULL;

    char *model = g_task_propagate_pointer(G_TASK(result), &error);
    if(error != NULL){
        fprintf(stderr, "Model switch failed: %s\n", error->message);
        g_error_free(error);
        gtk_widget_set_sensitive(GTK_WIDGET(data->model_dropdown), TRUE);
        gtk_widget_set_sensitive(GTK_WIDGET(data->generate_button), FALSE);
        return;
    }
    g_free(data->loaded_model);
    data->loaded_model = model;
    save_model(data->loaded_model);
    printf("%s Loaded Successfully\n", data->loaded_model);

    if(data->current_chat == NULL){
        data->current_chat = create_chat(data->loaded_model);
        g_ptr_array_add(data->chats, data->current_chat);
    }
    refresh_chat_list(data);
    printf("Current Chat -> %s\n", data->current_chat->id);
    gtk_widget_set_sensitive(GTK_WIDGET(data->model_dropdown), TRUE);
    gtk_widget_set_sensitive(GTK_WIDGET(data->generate_button), TRUE);
}

static void model_changed(GObject *object, GParamSpec *pspec, gpointer user_data){
    (void)object;
    (void)pspec;

    AppData *data = user_data;

    guint selected = gtk_drop_down_get_selected(data->model_dropdown);
    GtkStringList *models = GTK_STRING_LIST(gtk_drop_down_get_model(data->model_dropdown));
    const char *new_model = gtk_string_list_get_string(models, selected);

    printf("Selected model -> %s\n", new_model);
    if(data->loaded_model != NULL && g_strcmp0(data->loaded_model, new_model) == 0){
        printf("%s Already Loaded.\n", new_model);
        return;
    }
    
    printf("Switching from %s -> %s\n", data->loaded_model !=NULL ? data->loaded_model : "(none)", new_model);
    gtk_widget_set_sensitive(GTK_WIDGET(data->model_dropdown), FALSE);
    gtk_widget_set_sensitive(GTK_WIDGET(data->generate_button), FALSE);

    char **request = g_new(char *, 3);
    request[0] = data->loaded_model != NULL ? g_strdup(data->loaded_model) : NULL;
    request[1] = g_strdup(new_model);
    request[2] = NULL;

    GTask *task = g_task_new(G_OBJECT(data->model_dropdown),NULL,model_switch_finished, data);
    g_task_set_task_data(task, request, (GDestroyNotify)g_strfreev);
    g_task_run_in_thread(task, model_switch_thread);
    g_object_unref(task);
}

static char * load_saved_model(void){
    char *config_file;
    char *saved_model = NULL;

    config_file = get_config_file();

    if(g_file_get_contents(config_file, &saved_model, NULL, NULL)){
        g_strstrip(saved_model);
        printf("Saved model -> %s\n", saved_model);
    }
    g_free(config_file);
    return saved_model;
}


static void start_model_switch(AppData *data){
    guint selected;
    GtkStringList *models;

    const char *new_model;

    selected = gtk_drop_down_get_selected(data->model_dropdown);
    models = GTK_STRING_LIST(gtk_drop_down_get_model(data->model_dropdown));
    new_model = gtk_string_list_get_string(models, selected);

    printf("Initial Model -> %s\n", new_model);
    model_changed(G_OBJECT(data->model_dropdown), NULL, data);
}

static void load_css(void){
    GtkCssProvider *provider;
    provider = gtk_css_provider_new();
    gtk_css_provider_load_from_path(provider, "style.css");
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
}

static void activate(GtkApplication *app, gpointer user_data){
    
    load_css();

    GtkWidget *window;
    GtkWidget *main_box;
    GtkWidget *model_label;
    GtkWidget *model_dropdown;
    GtkWidget *prompt_entry;
    GtkWidget *generate_button;
    GtkWidget *new_chat_button;

    GtkWidget *header_box;
    GtkWidget *sidebar_box;
    GtkWidget *prompt_box;

    AppData *data;


    window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(window), "Dan-AI");
    gtk_window_set_default_size(GTK_WINDOW(window), 900, 700);

    //main vertical container;

    main_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_top(main_box, 15);
    gtk_widget_set_margin_bottom(main_box, 15);
    gtk_widget_set_margin_start(main_box, 15);
    gtk_widget_set_margin_end(main_box, 15);

    gtk_window_set_child(GTK_WINDOW(window), main_box);

    //model selector;
    GtkStringList *models;
    models = get_models();

    char *saved_model = load_saved_model();
    guint selected_index = 0;
    if(saved_model != NULL){
        guint model_count = g_list_model_get_n_items(G_LIST_MODEL(models));
        for(guint i = 0; i < model_count; i++){
            const char *name = gtk_string_list_get_string(models, i);
            if(g_strcmp0(name, saved_model) == 0){
                selected_index = i;
                break;
            }
        }
        g_free(saved_model);
    }

    header_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_box_append(GTK_BOX(main_box), header_box);
    model_label = gtk_label_new("Model ->");
    gtk_box_append(GTK_BOX(header_box), model_label);
    model_dropdown = gtk_drop_down_new(G_LIST_MODEL(models), NULL);  
    gtk_widget_set_hexpand(model_dropdown, TRUE);
    gtk_box_append(GTK_BOX(header_box), model_dropdown);
    //generate button;
    generate_button = gtk_button_new_with_label("Generate");

    data = g_new(AppData, 1);




    data->model_dropdown = GTK_DROP_DOWN(model_dropdown);

    data->chat_list_box = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 5));
    sidebar_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    data->sidebar_box = GTK_BOX(sidebar_box);
    gtk_widget_set_size_request(sidebar_box, 180, -1);
    GtkWidget *content_box;
    content_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_set_vexpand(content_box, TRUE);
    gtk_box_append(GTK_BOX(main_box), content_box);


    gtk_box_append(GTK_BOX(content_box), sidebar_box);
    

    data->conversation_box = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 10));
    data->response_scrolled = GTK_SCROLLED_WINDOW(gtk_scrolled_window_new());
    gtk_widget_set_vexpand(GTK_WIDGET(data->response_scrolled), TRUE);
    gtk_widget_set_hexpand(GTK_WIDGET(data->response_scrolled), TRUE);



    gtk_scrolled_window_set_policy(data->response_scrolled, GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(data->response_scrolled, GTK_WIDGET(data->conversation_box));

    gtk_box_append(GTK_BOX(content_box), GTK_WIDGET(data->response_scrolled));

        //prompt entry;
    prompt_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_box_append(GTK_BOX(main_box), prompt_box);
    prompt_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(prompt_entry), "Ask Something...");
    gtk_box_append(GTK_BOX(prompt_box), prompt_entry);
    data->prompt_entry = GTK_ENTRY(prompt_entry);
    gtk_widget_set_hexpand(prompt_entry, TRUE);


    data->generate_button = GTK_BUTTON(generate_button);
    data->loaded_model = NULL;
    data->chats = g_ptr_array_new_with_free_func((GDestroyNotify)free_chat);
    load_chats(data);
    data->current_chat = NULL;

    gtk_drop_down_set_selected(GTK_DROP_DOWN(model_dropdown), selected_index);
    start_model_switch(data);
    g_signal_connect(model_dropdown, "notify::selected", G_CALLBACK(model_changed), data);
    g_signal_connect(generate_button, "clicked", G_CALLBACK(generate_response), data);

    new_chat_button = gtk_button_new_with_label("New Chat");
    g_signal_connect(new_chat_button, "clicked", G_CALLBACK(new_chat), data);
    gtk_box_append(GTK_BOX(prompt_box), generate_button);
    gtk_box_append(GTK_BOX(sidebar_box), new_chat_button);
    gtk_box_append(GTK_BOX(sidebar_box), GTK_WIDGET(data->chat_list_box));

    gtk_widget_set_size_request(sidebar_box, 200, -1);
    gtk_widget_set_hexpand(sidebar_box, FALSE);


    gtk_widget_add_css_class(main_box, "main-container");
    gtk_widget_add_css_class(header_box, "header");
    gtk_widget_add_css_class(sidebar_box, "sidebar");
    gtk_widget_add_css_class(GTK_WIDGET(data->response_scrolled), "conversation");
    gtk_widget_add_css_class(prompt_box, "prompt-bar");
    gtk_widget_add_css_class(new_chat_button, "new-chat-button");
    gtk_widget_add_css_class(generate_button, "generate-button");
    gtk_widget_add_css_class(prompt_entry, "prompt-entry");
    gtk_widget_add_css_class(model_dropdown, "model-dropdown");

    gtk_window_present(GTK_WINDOW(window));

}

int main(int argc, char **argv){
    GtkApplication *app;
    int status;

    app = gtk_application_new("com.dan.localai", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app,"activate",G_CALLBACK(activate), NULL);
    status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}