#include <glib/gstdio.h>
#include <gio/gio.h>
#include <gio/gunixsocketaddress.h>
#include <json.h>
#include "controller.h"
#include "common.h"

#define PCAT_CONTROLLER_SOCKET_FILE "/tmp/pcat-manager.sock"

typedef struct _PCatControllerConnectionData
{
    GSocketConnection *connection;
    GInputStream *input_stream;
    GOutputStream *output_stream;
    GSource *input_stream_source;
    GSource *output_stream_source;
    GByteArray *input_buffer;
    GByteArray *output_buffer;
}PCatControllerConnectionData;

typedef struct _PCatControllerData
{
    gboolean initialized;
    GSocketService *control_socket_service;
    GHashTable *control_connection_table;
}PCatControllerData;

static PCatControllerData g_pcat_controller_data = {0};

static void pcat_controller_connection_data_free(
    PCatControllerConnectionData *data)
{
    if(data==NULL)
    {
        return;
    }

    if(data->output_stream_source!=NULL)
    {
        g_source_destroy(data->output_stream_source);
        g_source_unref(data->output_stream_source);
    }
    if(data->input_stream_source!=NULL)
    {
        g_source_destroy(data->input_stream_source);
        g_source_unref(data->input_stream_source);
    }

    if(data->output_buffer!=NULL)
    {
        g_byte_array_unref(data->output_buffer);
    }
    if(data->input_buffer!=NULL)
    {
        g_byte_array_unref(data->input_buffer);
    }

    if(data->connection!=NULL)
    {
        g_object_unref(data->connection);
    }

    g_free(data);
}

static gboolean pcat_controller_unix_socket_output_watch_func(
    GObject *stream, gpointer user_data)
{
    PCatControllerData *ctrl_data = &g_pcat_controller_data;
    PCatControllerConnectionData *connection_data =
        (PCatControllerConnectionData *)user_data;
    gssize wsize;
    gsize total_write_size = 0, remaining_size;
    GError *error = NULL;
    gboolean ret = FALSE;
    gboolean need_close = FALSE;

    do
    {
        if(total_write_size >= connection_data->output_buffer->len)
        {
            break;
        }

        remaining_size = connection_data->output_buffer->len -
            total_write_size;

        wsize = g_pollable_output_stream_write_nonblocking(
            G_POLLABLE_OUTPUT_STREAM(stream),
            connection_data->output_buffer->data + total_write_size,
            remaining_size > 4096 ? 4096 : remaining_size, NULL, &error);
        if(wsize > 0)
        {
            total_write_size += wsize;
        }
    }
    while(wsize > 0);

    if(total_write_size > 0)
    {
        g_byte_array_remove_range(connection_data->output_buffer, 0,
            total_write_size);
    }

    if(error!=NULL)
    {
        if(g_error_matches(error, G_IO_ERROR, G_IO_ERROR_AGAIN))
        {
            ret = TRUE;
        }
        else if(g_error_matches(error, G_IO_ERROR,
            G_IO_ERROR_CONNECTION_CLOSED))
        {
            ret = FALSE;
            need_close = TRUE;

            g_message("A Unix socket connection closed.");
        }
        else
        {
            ret = FALSE;
            need_close = TRUE;

            g_warning("A Unix socket connection broke with error %s!",
                error->message);
        }

        g_clear_error(&error);
    }

    if(need_close)
    {
        g_hash_table_remove(ctrl_data->control_connection_table,
            connection_data->connection);
    }
    else if(!ret)
    {
        g_source_destroy(connection_data->output_stream_source);
        g_source_unref(connection_data->output_stream_source);
        connection_data->output_stream_source = NULL;
    }

    return ret;
}

static void pcat_controller_unix_socket_input_parse(
    PCatControllerData *ctrl_data,
    PCatControllerConnectionData *connection_data)
{
    gsize i;
    gsize used_size = 0;
    const gchar *start = (const gchar *)connection_data->input_buffer->data;

    for(i=0;i<connection_data->input_buffer->len;i++)
    {
        if(connection_data->input_buffer->data[i]==0)
        {


            used_size = i + 1;
            start = (const gchar *)connection_data->input_buffer->data +
                i + 1;
        }
    }

    if(used_size > 0)
    {
        g_byte_array_remove_range(connection_data->input_buffer, 0, used_size);
    }
}

static gboolean pcat_controller_unix_socket_input_watch_func(
    GObject *stream, gpointer user_data)
{
    PCatControllerData *ctrl_data = &g_pcat_controller_data;
    PCatControllerConnectionData *connection_data =
        (PCatControllerConnectionData *)user_data;
    guint8 buffer[4096];
    gssize rsize;
    GError *error = NULL;
    gboolean ret = TRUE;

    while((rsize=g_pollable_input_stream_read_nonblocking(
        G_POLLABLE_INPUT_STREAM(stream), buffer, 4096, NULL, &error))>0)
    {
        if(connection_data->input_buffer->len > 2097152)
        {
            g_byte_array_remove_range(connection_data->input_buffer, 0,
                1048576);
        }
        g_byte_array_append(connection_data->input_buffer, buffer, rsize);

        pcat_controller_unix_socket_input_parse(ctrl_data, connection_data);
    }

    if(error!=NULL)
    {
        if(g_error_matches(error, G_IO_ERROR, G_IO_ERROR_AGAIN))
        {

        }
        else if(g_error_matches(error, G_IO_ERROR,
            G_IO_ERROR_CONNECTION_CLOSED))
        {
            ret = FALSE;

            g_message("A Unix socket connection closed.");
        }
        else
        {
            ret = FALSE;

            g_warning("A Unix socket connection broke with error %s!",
                error->message);
        }

        g_clear_error(&error);
    }

    if(!ret)
    {
        g_hash_table_remove(ctrl_data->control_connection_table,
            connection_data->connection);
    }

    return ret;
}


static gboolean pcat_controller_unix_socket_incoming_func(
    GSocketService *service, GSocketConnection *connection,
    GObject *source_object, gpointer user_data)
{
    PCatControllerData *ctrl_data = (PCatControllerData *)user_data;
    PCatControllerConnectionData *connection_data;

    connection_data = g_new0(PCatControllerConnectionData, 1);
    connection_data->connection = g_object_ref(connection);
    connection_data->input_stream = g_io_stream_get_input_stream(
        G_IO_STREAM(connection_data->connection));
    connection_data->output_stream = g_io_stream_get_output_stream(
        G_IO_STREAM(connection_data->connection));
    connection_data->input_buffer = g_byte_array_new();
    connection_data->output_buffer = g_byte_array_new();

    connection_data->input_stream_source =
        g_pollable_input_stream_create_source(
        G_POLLABLE_INPUT_STREAM(connection_data->input_stream), NULL);
    g_source_set_callback(connection_data->input_stream_source,
        G_SOURCE_FUNC(pcat_controller_unix_socket_input_watch_func),
        connection_data, NULL);
    g_source_attach(connection_data->input_stream_source, NULL);

    g_hash_table_replace(ctrl_data->control_connection_table,
        connection_data->connection, connection_data);

    return TRUE;
}

static gboolean pcat_controller_unix_socket_open(
    PCatControllerData *ctrl_data)
{
    GSocketAddress *address;
    GSocketService *service;
    GError *error = NULL;

    if(ctrl_data->control_socket_service!=NULL)
    {
        return TRUE;
    }

    g_remove(PCAT_CONTROLLER_SOCKET_FILE);

    address = g_unix_socket_address_new(PCAT_CONTROLLER_SOCKET_FILE);
    if(address==NULL)
    {
        g_warning("Failed to create socket address for unix socket %s!",
            PCAT_CONTROLLER_SOCKET_FILE);

        return FALSE;
    }

    service = g_socket_service_new();
    if(service==NULL)
    {
        g_warning("Failed to create socket service for unix socket %s!",
            PCAT_CONTROLLER_SOCKET_FILE);
        g_object_unref(address);

        return FALSE;
    }

    if(!g_socket_listener_add_address(G_SOCKET_LISTENER(service),
        address, G_SOCKET_TYPE_STREAM, G_SOCKET_PROTOCOL_DEFAULT,
        NULL, NULL, &error))
    {
        g_warning("Failed to listen to unix socket %s: %s",
            PCAT_CONTROLLER_SOCKET_FILE, error!=NULL ?
            error->message : "Unknown");

        g_clear_error(&error);
        g_object_unref(service);
        g_object_unref(address);

        return FALSE;
    }

    g_object_unref(address);

    ctrl_data->control_socket_service = service;
    ctrl_data->control_connection_table = g_hash_table_new_full(
        g_direct_hash, g_direct_equal, NULL, (GDestroyNotify)
        pcat_controller_connection_data_free);
    g_socket_service_start(ctrl_data->control_socket_service);
    g_signal_connect(service, "incoming",
        G_CALLBACK(pcat_controller_unix_socket_incoming_func), ctrl_data);


    return TRUE;
}

static void pcat_controller_unix_socket_close(
    PCatControllerData *ctrl_data)
{
    if(ctrl_data->control_connection_table!=NULL)
    {
        g_hash_table_unref(ctrl_data->control_connection_table);
        ctrl_data->control_connection_table = NULL;
    }

    if(ctrl_data->control_socket_service!=NULL)
    {
        g_object_unref(ctrl_data->control_socket_service);
        ctrl_data->control_socket_service = NULL;
    }

    g_remove(PCAT_CONTROLLER_SOCKET_FILE);
}

gboolean pcat_controller_init()
{
    if(g_pcat_controller_data.initialized)
    {
        return TRUE;
    }

    if(!pcat_controller_unix_socket_open(&g_pcat_controller_data))
    {
        g_warning("Failed to open controller socket!");

        return FALSE;
    }

    g_pcat_controller_data.initialized = TRUE;

    return TRUE;
}

void pcat_controller_uninit()
{
    if(!g_pcat_controller_data.initialized)
    {
        return;
    }

    pcat_controller_unix_socket_close(&g_pcat_controller_data);

    g_pcat_controller_data.initialized = FALSE;
}

