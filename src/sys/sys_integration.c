/**
 * @file sys_integration.c
 * @brief System-level integrations and OS interop.
 *
 * Provides hooks for platform-specific features such as
 * power management, clipboard access, or system event handling.
 */

#include "sys_integration.h"

#include "common/common.h"

#include "mpris.h"
#include "notifications.h"

#include "utils/file.h"
#include "utils/term.h"
#include "utils/utils.h"

#include "gio/gio.h"
#include "math.h"
#include <stdio.h>
#include <sys/wait.h>

static GDBusConnection *connection = NULL;
static GMainContext *global_main_context = NULL;

void set_g_main_context(GMainContext *val)
{
        global_main_context = val;
}

void *get_g_main_context(void)
{
        return global_main_context;
}

GDBusConnection *get_gd_bus_connection(void)
{
        return connection;
}

void set_gd_bus_connection(GDBusConnection *val)
{
        connection = val;
}

void process_d_bus_events(void)
{
        while (g_main_context_pending(get_g_main_context())) {
                g_main_context_iteration(get_g_main_context(), FALSE);
        }
}

void resize(UIState *uis)
{
        alarm(1); // Timer
        while (uis->resizeFlag) {
                uis->resizeFlag = 0;
                c_sleep(100);
        }
        alarm(0); // Cancel timer
        printf("\033[1;1H");
        clear_screen();
        trigger_refresh();
}

void emit_string_property_changed(const gchar *property_name, const gchar *new_value)
{
#ifndef __APPLE__
        GVariantBuilder changed_properties_builder;
        g_variant_builder_init(&changed_properties_builder, G_VARIANT_TYPE("a{sv}"));

        GVariant *value_variant = g_variant_new_string(new_value);
        g_variant_builder_add(&changed_properties_builder, "{sv}", property_name, value_variant);

        GVariant *signal_variant =
            g_variant_new("(sa{sv}as)", "org.mpris.MediaPlayer2.Player", &changed_properties_builder, NULL);

        g_variant_ref_sink(signal_variant);

        g_dbus_connection_emit_signal(
            connection, NULL, "/org/mpris/MediaPlayer2",
            "org.freedesktop.DBus.Properties", "PropertiesChanged",
            signal_variant, NULL);

        g_variant_builder_clear(&changed_properties_builder);
        g_variant_unref(signal_variant);
#else
        (void)property_name;
        (void)new_value;
#endif
}

void update_playback_position(double elapsed_seconds)
{
#ifndef __APPLE__
        if (elapsed_seconds < 0.0)
                elapsed_seconds = 0.0;

        // Max safe seconds to avoid overflow when multiplied by 1,000,000
        const double max_seconds = (double)(LLONG_MAX / G_USEC_PER_SEC);

        if (elapsed_seconds > max_seconds)
                elapsed_seconds = max_seconds;

        GVariantBuilder changedPropertiesBuilder;
        g_variant_builder_init(&changedPropertiesBuilder,
                               G_VARIANT_TYPE_DICTIONARY);
        g_variant_builder_add(
            &changedPropertiesBuilder, "{sv}", "Position",
            g_variant_new_int64(llround(elapsed_seconds * G_USEC_PER_SEC)));

        GVariant *parameters =
            g_variant_new("(sa{sv}as)", "org.mpris.MediaPlayer2.Player",
                          &changedPropertiesBuilder, NULL);

        g_dbus_connection_emit_signal(connection, NULL,
                                      "/org/mpris/MediaPlayer2",
                                      "org.freedesktop.DBus.Properties",
                                      "PropertiesChanged", parameters, NULL);
#else
        (void)elapsed_seconds;
#endif
}

void emit_seeked_signal(double new_position_seconds)
{
#ifndef __APPLE__
        if (new_position_seconds < 0.0)
                new_position_seconds = 0.0;

        const double max_seconds = (double)(LLONG_MAX / G_USEC_PER_SEC);
        if (new_position_seconds > max_seconds)
                new_position_seconds = max_seconds;

        gint64 newPositionMicroseconds =
            llround(new_position_seconds * G_USEC_PER_SEC);

        GVariant *parameters = g_variant_new("(x)", newPositionMicroseconds);

        g_dbus_connection_emit_signal(
            connection, NULL, "/org/mpris/MediaPlayer2",
            "org.mpris.MediaPlayer2.Player", "Seeked", parameters, NULL);
#else
        (void)new_position_seconds;
#endif
}

void emit_boolean_property_changed(const gchar *property_name, gboolean new_value)
{
#ifndef __APPLE__
        GVariantBuilder changed_properties_builder;
        g_variant_builder_init(&changed_properties_builder,
                               G_VARIANT_TYPE("a{sv}"));

        GVariant *value_variant = g_variant_new_boolean(new_value);
        if (value_variant == NULL) {
                fprintf(stderr,
                        "Failed to allocate GVariant for boolean property\n");
                return;
        }

        g_variant_builder_add(&changed_properties_builder, "{sv}", property_name,
                              value_variant);

        GVariant *signal_variant =
            g_variant_new("(sa{sv}as)", "org.mpris.MediaPlayer2.Player",
                          &changed_properties_builder, NULL);
        if (signal_variant == NULL) {
                fprintf(stderr, "Failed to allocate GVariant for "
                                "PropertiesChanged signal\n");
                g_variant_builder_clear(&changed_properties_builder);
                return;
        }

        g_dbus_connection_emit_signal(
            connection, NULL, "/org/mpris/MediaPlayer2",
            "org.freedesktop.DBus.Properties", "PropertiesChanged",
            signal_variant, NULL);

        g_variant_builder_clear(&changed_properties_builder);
#else
        (void)property_name;
        (void)new_value;
#endif
}

void notify_mpris_switch(SongData *current_song_data)
{
        if (current_song_data == NULL)
                return;

        gint64 length = get_length_in_micro_sec(current_song_data->duration);

        // Update mpris
        emit_metadata_changed(
            current_song_data->metadata->title, current_song_data->metadata->artist,
            current_song_data->metadata->album, current_song_data->cover_art_path,
            current_song_data->track_id != NULL ? current_song_data->track_id : "",
            get_current_song(), length);
}

void notify_song_switch(SongData *current_song_data)
{
        AppState *state = get_app_state();
        UISettings *ui = &(state->uiSettings);
        if (current_song_data != NULL && current_song_data->hasErrors == 0 &&
            current_song_data->metadata &&
            strnlen(current_song_data->metadata->title, 10) > 0) {
#ifdef USE_DBUS
                display_song_notification(current_song_data->metadata->artist,
                                          current_song_data->metadata->title,
                                          current_song_data->cover_art_path, ui);
#else
                (void)ui;
#endif

                notify_mpris_switch(current_song_data);

                Node *current = get_current_song();

                if (current != NULL)
                        state->uiState.lastNotifiedId = current->id;
        }
}

int is_process_running(pid_t pid)
{
        if (pid <= 0) {
                return 0; // Invalid PID
        }

        // Send signal 0 to check if the process exists
        if (kill(pid, 0) == 0) {
                return 1; // Process exists
        }

        // Check errno for detailed status
        if (errno == ESRCH) {
                return 0; // No such process
        } else if (errno == EPERM) {
                return 1; // Process exists but we don't have permission
        }

        return 0; // Other errors
}

int is_kew_process(pid_t pid)
{
        char comm_path[64];
        char process_name[256];
        FILE *file;

        // First check /proc/[pid]/comm for the process name
        snprintf(comm_path, sizeof(comm_path), "/proc/%d/comm", pid);
        file = fopen(comm_path, "r");
        if (file != NULL) {
                if (fgets(process_name, sizeof(process_name), file)) {
                        fclose(file);
                        // Remove trailing newline
                        process_name[strcspn(process_name, "\n")] = 0;

                        // Check if it's kew (process name might be truncated to
                        // 15 chars)
                        if (strstr(process_name, "kew") != NULL) {
                                return 1; // It's likely kew
                        }
                } else {
                        fclose(file);
                }
        }

        return 0; // Not kew or couldn't determine
}

void delete_pid_file()
{
        char pidfile_path[MAXPATHLEN];
        const char *temp_dir = get_temp_dir();

        snprintf(pidfile_path, sizeof(pidfile_path), "%s/kew_%d.pid", temp_dir,
                 getuid());

        FILE *pidfile;

        pidfile = fopen(pidfile_path, "r");

        if (pidfile != NULL) {
                fclose(pidfile);
                unlink(pidfile_path);
        }
}

pid_t read_pid_file()
{
        char pidfile_path[MAXPATHLEN];
        const char *temp_dir = get_temp_dir();

        snprintf(pidfile_path, sizeof(pidfile_path), "%s/kew_%d.pid", temp_dir,
                 getuid());

        FILE *pidfile;
        pid_t pid;

        pidfile = fopen(pidfile_path, "r");
        if (pidfile != NULL) {
                if (fscanf(pidfile, "%d", &pid) == 1) {
                        fclose(pidfile);
                }
        } else {
                {
                        pid = -1;
                        unlink(pidfile_path);
                }
        }

        return pid;
}

void create_pid_file()
{
        char pidfile_path[MAXPATHLEN];
        const char *temp_dir = get_temp_dir();

        snprintf(pidfile_path, sizeof(pidfile_path), "%s/kew_%d.pid", temp_dir,
                 getuid());

        FILE *pidfile = fopen(pidfile_path, "w");
        if (pidfile == NULL) {
                perror("Unable to create PID file");
                exit(1);
        }

        fprintf(pidfile, "%d\n", getpid());
        fclose(pidfile);
}

void restart_kew(char *argv[])
{
        pid_t oldpid = read_pid_file();
        if (oldpid > 0) {
                if (kill(oldpid, SIGUSR1) != 0) {
                        if (errno == ESRCH) {
                                fprintf(stderr, "No running kew process found.\n");
                                delete_pid_file();
                        } else {
                                fprintf(stderr, "Failed to stop old kew (pid %d): %s\n",
                                        oldpid, strerror(errno));
                        }
                } else {
                        int status;
                        if (waitpid(oldpid, &status, 0) == -1 && errno != ECHILD) {
                                perror("waitpid");
                        }
                        delete_pid_file();
                }
        }

        execvp("kew", argv);

        fprintf(stderr, "Failed to restart kew via execvp: %s\n", strerror(errno));
        exit(1);
}

void handle_shutdown(int sig)
{
        (void)sig;
        exit(0); // runs all atexit handlers
}

// Ensures only a single instance of kew can run at a time for the current user.
void restart_if_already_running(char *argv[])
{
        signal(SIGUSR1, handle_shutdown);

        pid_t pid = read_pid_file();

#ifdef __ANDROID__
        if (is_process_running(pid) && is_kew_process(pid))
#else
        if (is_process_running(pid))
#endif
        {
                restart_kew(argv);
        } else {
                delete_pid_file();
        }

        create_pid_file();
}

void handle_resize(int sig)
{
        (void)sig;
        AppState *state = get_app_state();
        state->uiState.resizeFlag = 1;
}

void reset_resize_flag(int sig)
{
        (void)sig;
        AppState *state = get_app_state();
        state->uiState.resizeFlag = 0;
}

void init_resize(void)
{
        signal(SIGWINCH, handle_resize);

        struct sigaction sa;
        sa.sa_handler = reset_resize_flag;
        sigemptyset(&(sa.sa_mask));
        sa.sa_flags = 0;
        sigaction(SIGALRM, &sa, NULL);
}

void quit(void)
{
        exit(0);
}
