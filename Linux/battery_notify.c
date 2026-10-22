#include <errno.h>
#include <libnotify/notify.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

// #define EXIT_SUCCESS 0
// #define EXIT_FAILURE 1
#define ERR_INVALID_ARGS 1
#define BATTERY_PATH "/sys/class/power_supply/BAT0"
#define DEFAULT_MAX_CHARGE 80
#define DEFAULT_MIN_CHARGE 60

typedef enum { ALERT_NONE, ALERT_HIGH, ALERT_LOW } AlertState;

static volatile sig_atomic_t running = 1;

void handler(int sig)
{
    (void)sig;
    running = 0;
}

int get_battery_capacity()
{
    int cap = -1;
    char path[128];
    snprintf(path, sizeof(path), "%s/capacity", BATTERY_PATH);

    FILE *fp = fopen(path, "r");
    if (fp == NULL) {
        perror("Error opening capacity file");
        return -1;
    }

    if (fscanf(fp, "%d", &cap) != 1) {
        cap = -1;
    }
    fclose(fp);

    return cap;
}

void get_battery_status(char status[], size_t size)
{
    char path[128];
    snprintf(path, sizeof(path), "%s/status", BATTERY_PATH);

    FILE *fp = fopen(path, "r");
    if (fp == NULL) {
        perror("Error opening status file");
        status[0] = '\0';
        return;
    }

    if (fgets(status, size, fp) == NULL) {
        status[0] = '\0';
    } else {
        status[strcspn(status, "\n")] = '\0';
    }

    fclose(fp);
}

int get_value(char *num)
{
    char *end;
    errno = 0;

    long int value = strtol(num, &end, 10);

    if (num == end || *end != '\0' || errno == ERANGE || value < 0 ||
        value > 100) {
        fprintf(stderr, "Invalid battery threshold: '%s'\n", num);
        fprintf(stderr, "Usage:\n"
                        "  charge_notify --max <max_cap> --min <min_cap>\n"
                        "  charge_notify --min <min_cap> --max <max_cap>\n");
        fprintf(stderr, "  0 <= <min_cap> < <max_cap> <= 100\n");
        exit(ERR_INVALID_ARGS);
    }

    return (int)value;
}

int main(int argc, char *argv[])
{
    int min_cap = DEFAULT_MIN_CHARGE;
    int max_cap = DEFAULT_MAX_CHARGE;

    bool has_min = false;
    bool has_max = false;

    for (int i = 1; i < argc; i += 2) {
        if (i + 1 >= argc) {
            fprintf(stderr, "Missing value for argument: %s\n", argv[i]);
            goto invalid_args;
        }

        if (strcmp(argv[i], "--min") == 0) {
            if (has_min) {
                fprintf(stderr, "Duplicate argument: --min\n");
                goto invalid_args;
            }

            min_cap = get_value(argv[i + 1]);
            has_min = true;

        } else if (strcmp(argv[i], "--max") == 0) {
            if (has_max) {
                fprintf(stderr, "Duplicate argument: --max\n");
                goto invalid_args;
            }

            max_cap = get_value(argv[i + 1]);
            has_max = true;

        } else {
            fprintf(stderr, "Unknown argument: %s\n", argv[i]);
            goto invalid_args;
        }
    }

    if (min_cap >= max_cap) {
        fprintf(stderr, "Invalid limits: min_cap must be less than max_cap.\n");
        goto invalid_args;
    }

    goto args_done;

invalid_args:
    fprintf(stderr, "Usage:\n"
                    "  charge_notify --max <max_cap> --min <min_cap>\n"
                    "  charge_notify --min <min_cap> --max <max_cap>\n");
    fprintf(stderr, "  0 <= <min_cap> < <max_cap> <= 100\n");
    return ERR_INVALID_ARGS;

args_done:

    struct sigaction sa = {0};

    sa.sa_handler = handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    if (sigaction(SIGINT, &sa, NULL) == -1 ||
        sigaction(SIGTERM, &sa, NULL) == -1) {
        perror("sigaction");
        return EXIT_FAILURE;
    }

    if (!notify_init("Charge Notify")) {
        fprintf(stderr, "Failed to initialize notifications\n");
        return EXIT_FAILURE;
    }

    AlertState state = ALERT_NONE;

    while (running) {
        int capacity = get_battery_capacity();

        char status[32];
        get_battery_status(status, sizeof(status));

        if (capacity < 0 || capacity > 100 || status[0] == '\0') {
            sleep(30);
            continue;
        }

        char message[64];
        bool notify_now = false;
        static int count = 3;

        AlertState prev_state = state;
        state = ALERT_NONE;

        NotifyNotification *notification = NULL;

        if ((strcasecmp(status, "Full") == 0 ||
             strcasecmp(status, "Charging") == 0) &&
            capacity >= max_cap) {
            snprintf(message, sizeof(message),
                     "Battery Capacity is %d or more!\nPlug Out, Now!",
                     max_cap);

            notification = notify_notification_new("Battery Charge Alert",
                                                   message, "battery-caution");

            if (notification == NULL) {
                fprintf(stderr, "Failed to create notification\n");
                sleep(30);
                continue;
            }

            notify_now = true;
            state = ALERT_HIGH;
        } else if (strcasecmp(status, "Discharging") == 0 &&
                   capacity <= min_cap) {
            snprintf(message, sizeof(message),
                     "Battery Capacity is %d or less!\nPlug In, Now!", min_cap);

            notification = notify_notification_new("Battery Charge Alert",
                                                   message, "battery-caution");

            if (notification == NULL) {
                fprintf(stderr, "Failed to create notification\n");
                sleep(30);
                continue;
            }

            notify_now = true;
            state = ALERT_LOW;
        } else {
            count = 3;
        }

        if ((prev_state == ALERT_HIGH && state == ALERT_LOW) ||
            (prev_state == ALERT_LOW && state == ALERT_HIGH)) {
            count = 3;
        }

        if (notification != NULL) {
            if (notify_now && count) {
                notify_notification_set_timeout(notification, 3000);

                GError *error = NULL;

                if (!notify_notification_show(notification, &error)) {
                    fprintf(stderr, "Failed to show notification: %s\n",
                            error ? error->message : "unknown error");
                }

                if (error != NULL) {
                    g_error_free(error);
                }
            }

            g_object_unref(G_OBJECT(notification));
        }

        if (state == ALERT_HIGH || state == ALERT_LOW) {
            if (count > 0) {
                --count;
            }
        }

        sleep(30);
    }

    notify_uninit();
    return EXIT_SUCCESS;
}
