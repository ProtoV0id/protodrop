/*
 * ProtoDrop Flipper App
 *
 * Target:
 * Flipper Zero
 *
 * Purpose:
 * - Control the ProtoDrop ESP32-S2 Wi-Fi dev board over UART.
 * - Keep the main screen simple.
 * - Open latest messages on their own screen.
 *
 * Current controls:
 *
 * MENU SCREEN:
 * OK    = STATUS
 * UP    = COUNT
 * DOWN  = HELP
 * LEFT  = WIPE
 * RIGHT = LATEST MESSAGE SCREEN
 * BACK  = EXIT APP
 *
 * MESSAGE SCREEN:
 * BACK  = RETURN TO MENU
 */

#include <furi.h>
#include <gui/gui.h>
#include <input/input.h>

#include <furi_hal_serial.h>
#include <furi_hal_serial_control.h>

#define UART_BAUD_RATE 115200

/*
 * ScreenMode tells the app which screen to draw.
 *
 * SCREEN_MENU:
 * Shows the basic command tester screen.
 *
 * SCREEN_MESSAGE:
 * Shows the latest message on its own screen.
 */
typedef enum {
    SCREEN_MENU,
    SCREEN_PICK_MESSAGE,
    SCREEN_MESSAGE,
} ScreenMode;
/*
 * Main app state.
 *
 * This structure holds everything the app needs while running.
 */
typedef struct {
    ViewPort* view_port;
    Gui* gui;
    bool running;

    FuriHalSerialHandle* serial_handle;

    ScreenMode screen;

    /*
     * uart_text stores the latest response from the ESP32.
     *
     * Examples:
     * STATUS: ProtoDrop running
     * COUNT: 3
     * MSG: hello world
     */
    char uart_text[512];

    size_t uart_index;
    bool response_ready;

    /*
     * last_command stores which command was most recently sent.
     *
     * We use this so the app knows whether a UART response should stay
     * on the menu screen or open the message screen.
     */
    char last_command[16];
    
    /*
     * Message picker state.
     */
    int selected_message;
    int message_count;
} ProtoDropApp;

/*
 * Draws text split into multiple short lines.
 *
 * The Flipper screen is only 128x64 pixels, so long text must be broken
 * into smaller pieces.
 *
 * x/y:
 * Starting position.
 *
 * max_chars:
 * Rough number of characters per line.
 *
 * max_lines:
 * Maximum number of lines to draw.
 */
int selected_message;
int message_count;

/*
 * Draws long text across multiple lines.
 *
 * This does simple fixed-width wrapping.
 * It will not show the full message if the message is huge,
 * but it will show the first visible chunk safely.
 */
static void draw_wrapped_text(
    Canvas* canvas,
    int x,
    int y,
    const char* text,
    size_t max_chars,
    size_t max_lines) {
    char line[32];

    size_t offset = 0;
    size_t text_len = strlen(text);

    for(size_t line_num = 0; line_num < max_lines; line_num++) {
        if(offset >= text_len) {
            break;
        }

        memset(line, 0, sizeof(line));

        size_t remaining = text_len - offset;
        size_t copy_len = remaining < max_chars ? remaining : max_chars;

        memcpy(line, text + offset, copy_len);
        line[copy_len] = '\0';

        canvas_draw_str(canvas, x, y + (line_num * 10), line);

        offset += copy_len;
    }
}

/*
 * Draws the message picker screen.
 *
 * This lets the user choose which stored message number to request.
 */
static void draw_pick_message_screen(Canvas* canvas, ProtoDropApp* app) {
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 5, 12, "Pick Message");
    canvas_draw_line(canvas, 0, 16, 127, 16);

    canvas_set_font(canvas, FontSecondary);

    char selected_text[32];
    snprintf(selected_text, sizeof(selected_text), "Message #: %d", app->selected_message);

    canvas_draw_str(canvas, 5, 30, selected_text);
    canvas_draw_str(canvas, 5, 44, "UP/DN Change");
    canvas_draw_str(canvas, 5, 56, "OK View  BK Menu");
}

/*
 * Draws the main ProtoDrop command screen.
 */
static void draw_menu_screen(Canvas* canvas, ProtoDropApp* app) {
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 5, 12, "ProtoDrop");
    canvas_draw_line(canvas, 0, 16, 127, 16);

    canvas_set_font(canvas, FontSecondary);

    /*
     * Left-side command list.
     */
    canvas_draw_str(canvas, 5, 26, "OK Status");
    canvas_draw_str(canvas, 5, 36, "UP Count");
    canvas_draw_str(canvas, 5, 46, "RT Latest");
    canvas_draw_str(canvas, 5, 56, "LT Wipe");

    /*
     * Right-side short response area.
     */
    canvas_draw_str(canvas, 70, 26, "Reply:");

    draw_wrapped_text(canvas, 70, 38, app->uart_text, 9, 2);
}

/*
 * Draws the full latest-message screen.
 */
static void draw_message_screen(Canvas* canvas, ProtoDropApp* app) {
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 5, 12, "Latest Msg");
    canvas_draw_line(canvas, 0, 16, 127, 16);

    canvas_set_font(canvas, FontSecondary);

    /*
     * If the ESP32 response begins with "MSG: ", skip that prefix
     * so the screen shows only the message content.
     */
    const char* message_text = app->uart_text;

    if(strncmp(app->uart_text, "MSG: ", 5) == 0) {
        message_text = app->uart_text + 5;
    }

    /*
     * Draw the message across most of the screen.
     */
    draw_wrapped_text(canvas, 5, 28, message_text, 25, 3);

    canvas_draw_str(canvas, 5, 62, "BACK = Menu");
}

/*
 * Main draw callback.
 *
 * This runs whenever the screen needs to redraw.
 */
static void draw_callback(Canvas* canvas, void* context) {
    ProtoDropApp* app = context;

    canvas_clear(canvas);

    if(app->screen == SCREEN_MENU) {
    draw_menu_screen(canvas, app);
} else if(app->screen == SCREEN_PICK_MESSAGE) {
    draw_pick_message_screen(canvas, app);
} else if(app->screen == SCREEN_MESSAGE) {
    draw_message_screen(canvas, app);
}
}

/*
 * Sends a command over UART to the ESP32.
 */
static void uart_send_command(ProtoDropApp* app, const char* command) {
    if(app->serial_handle == NULL) {
        strcpy(app->uart_text, "UART unavailable");
        view_port_update(app->view_port);
        return;
    }

    /*
     * Save the command name.
     */
    strncpy(app->last_command, command, sizeof(app->last_command) - 1);
    app->last_command[sizeof(app->last_command) - 1] = '\0';

    /*
     * Clear the receive buffer before sending a new command.
     */
    app->uart_index = 0;
    app->response_ready = false;
    memset(app->uart_text, 0, sizeof(app->uart_text));

    /*
     * Show feedback immediately.
     */
    strcpy(app->uart_text, "Waiting...");
    view_port_update(app->view_port);

    /*
     * Send command plus newline.
     *
     * The ESP32 command parser expects complete commands ending in newline.
     */
    furi_hal_serial_tx(app->serial_handle, (const uint8_t*)command, strlen(command));
    furi_hal_serial_tx(app->serial_handle, (const uint8_t*)"\n", 1);
    furi_hal_serial_tx_wait_complete(app->serial_handle);
}

/*
 * Handles Flipper button input.
 */
static void input_callback(InputEvent* input_event, void* context) {
    ProtoDropApp* app = context;

    if(input_event->type != InputTypePress) {
        return;
    }

    /*
     * BACK:
     * - From message screen or picker screen: return to menu
     * - From menu screen: exit app
     */
    if(input_event->key == InputKeyBack) {
        if(app->screen == SCREEN_MESSAGE || app->screen == SCREEN_PICK_MESSAGE) {
            app->screen = SCREEN_MENU;
            view_port_update(app->view_port);
        } else {
            app->running = false;
        }

        return;
    }

    /*
     * Message picker screen controls.
     */
    if(app->screen == SCREEN_PICK_MESSAGE) {
        if(input_event->key == InputKeyUp) {
            if(app->selected_message < app->message_count) {
                app->selected_message++;
            }
            view_port_update(app->view_port);
        } else if(input_event->key == InputKeyDown) {
            if(app->selected_message > 1) {
                app->selected_message--;
            }
            view_port_update(app->view_port);
        } else if(input_event->key == InputKeyOk) {
            if(app->message_count > 0) {
                char command[16];
                snprintf(command, sizeof(command), "GET %d", app->selected_message);
                uart_send_command(app, command);
            } else {
                strcpy(app->uart_text, "No messages");
                view_port_update(app->view_port);
            }
        }

        return;
    }

    /*
     * Ignore command buttons unless we are on the main menu.
     */
    if(app->screen != SCREEN_MENU) {
        return;
    }

    /*
     * Main menu controls.
     */
    if(input_event->key == InputKeyOk) {
        uart_send_command(app, "STATUS");
    } else if(input_event->key == InputKeyUp) {
        uart_send_command(app, "COUNT");
    } else if(input_event->key == InputKeyDown) {
        uart_send_command(app, "HELP");
    } else if(input_event->key == InputKeyLeft) {
        app->screen = SCREEN_MENU;
        strcpy(app->uart_text, "Wiping...");
        app->selected_message = 1;
        app->message_count = 0;
        view_port_update(app->view_port);

        uart_send_command(app, "WIPE");
    } else if(input_event->key == InputKeyRight) {
        app->screen = SCREEN_PICK_MESSAGE;
        uart_send_command(app, "COUNT");
        view_port_update(app->view_port);
    }
}


/*
 * UART receive callback.
 *
 * Reads ESP32 response data into uart_text.
 * If the message is too long, it safely truncates instead of failing.
 */
static void uart_rx_callback(
    FuriHalSerialHandle* handle,
    FuriHalSerialRxEvent event,
    void* context) {
    ProtoDropApp* app = context;

    if(event & FuriHalSerialRxEventData) {
        while(furi_hal_serial_async_rx_available(handle)) {
            uint8_t byte = furi_hal_serial_async_rx(handle);

            if(byte == '\n' || byte == '\r') {
                app->uart_text[app->uart_index] = '\0';
                app->response_ready = true;
                app->uart_index = 0;
                break;
            }

            if(app->uart_index < sizeof(app->uart_text) - 1) {
                app->uart_text[app->uart_index++] = (char)byte;
            } else {
                /*
                 * Buffer full.
                 * End the string safely and show what we have.
                 */
                app->uart_text[sizeof(app->uart_text) - 1] = '\0';
                app->response_ready = true;
                app->uart_index = 0;
                break;
            }
        }
    }
}

/*
 * App entry point.
 *
 * This function name must match entry_point in application.fam:
 *
 * entry_point="protodrop_app"
 */
int32_t protodrop_app(void* p) {
    UNUSED(p);

    ProtoDropApp* app = malloc(sizeof(ProtoDropApp));

    app->running = true;
    app->screen = SCREEN_MENU;
    app->selected_message = 1;
    app->message_count = 0;
    app->uart_index = 0;
    app->response_ready = false;

    strcpy(app->uart_text, "Ready");
    strcpy(app->last_command, "");

    /*
     * Set up the Flipper screen.
     */
    app->view_port = view_port_alloc();
    view_port_draw_callback_set(app->view_port, draw_callback, app);
    view_port_input_callback_set(app->view_port, input_callback, app);

    app->gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(app->gui, app->view_port, GuiLayerFullscreen);

    /*
     * Acquire Flipper USART.
     * This is the correct serial interface for the Wi-Fi dev board.
     */
    app->serial_handle = furi_hal_serial_control_acquire(FuriHalSerialIdUsart);

    if(app->serial_handle == NULL) {
        strcpy(app->uart_text, "UART busy");
        view_port_update(app->view_port);
    } else {
        furi_hal_serial_init(app->serial_handle, UART_BAUD_RATE);

        furi_hal_serial_async_rx_start(
            app->serial_handle,
            uart_rx_callback,
            app,
            false
        );

        strcpy(app->uart_text, "UART ready");
        view_port_update(app->view_port);
    }

    /*
     * Main app loop.
     *
     * This loop watches for UART responses and updates the screen.
     */
    while(app->running) {
        if(app->response_ready) {
            app->response_ready = false;

            /*
             * If COUNT was received, update the app's known message count.
             */
            if(strcmp(app->last_command, "COUNT") == 0) {
                int count = 0;
                sscanf(app->uart_text, "COUNT: %d", &count);

                app->message_count = count;

                if(app->message_count < 1) {
                    app->message_count = 0;
                    app->selected_message = 1;
                }

                if(app->selected_message > app->message_count && app->message_count > 0) {
                    app->selected_message = app->message_count;
                }
            }

            /*
             * If WIPE was received, reset the picker state.
             */
            if(strcmp(app->last_command, "WIPE") == 0) {
                app->message_count = 0;
                app->selected_message = 1;
            }

            /*
             * If a message was requested, open the message screen.
             */
            if(strcmp(app->last_command, "LATEST") == 0 ||
               strncmp(app->last_command, "GET ", 4) == 0) {
                app->screen = SCREEN_MESSAGE;
            }

            view_port_update(app->view_port);
        }

        furi_delay_ms(50);
    }

    /*
     * Cleanup before exiting.
     */
    if(app->serial_handle != NULL) {
        furi_hal_serial_async_rx_stop(app->serial_handle);
        furi_hal_serial_deinit(app->serial_handle);
        furi_hal_serial_control_release(app->serial_handle);
    }

    gui_remove_view_port(app->gui, app->view_port);
    furi_record_close(RECORD_GUI);
    view_port_free(app->view_port);
    free(app);

    return 0;
}