#include <furi.h>
#include <furi_hal_console.h>
#include <furi_hal_gpio.h>
#include <furi_hal_power.h>
#include <furi_hal_uart.h>
#include <gui/canvas_i.h>
#include <gui/gui.h>
#include <input/input.h>
//#include <m-string.h>
//#include <math.h>
//#include <notification/notification.h>
//#include <notification/notification_messages.h>
//#include <stdlib.h>
#include <stream_buffer.h>
#include <u8g2.h>

#include "FlipperZeroWiFiDeauthModuleDefines.h"

#define DEAUTH_APP_DEBUG 0

#if DEAUTH_APP_DEBUG
#define APP_NAME_TAG "WiFi_Scanner"
#define DEAUTH_APP_LOG_I(format, ...) FURI_LOG_I(APP_NAME_TAG, format, ##__VA_ARGS__)
#define DEAUTH_APP_LOG_D(format, ...) FURI_LOG_D(APP_NAME_TAG, format, ##__VA_ARGS__)
#define DEAUTH_APP_LOG_E(format, ...) FURI_LOG_E(APP_NAME_TAG, format, ##__VA_ARGS__)
#else
#define DEAUTH_APP_LOG_I(format, ...)
#define DEAUTH_APP_LOG_D(format, ...)
#define DEAUTH_APP_LOG_E(format, ...)
#endif // WIFI_APP_DEBUG

#define DISABLE_CONSOLE !DEAUTH_APP_DEBUG
#define ENABLE_MODULE_POWER 1
#define ENABLE_MODULE_DETECTION 0

typedef enum EChunkArrayData
{
    EChunkArrayData_Context = 0,
    EChunkArrayData_ENUM_MAX
} EChunkArrayData;

typedef enum EEventType // app internally defined event types
{
    EventTypeKey // flipper input.h type
} EEventType; 

typedef struct SPluginEvent
{
    EEventType m_type;
    InputEvent m_input;
} SPluginEvent;

typedef enum EAppContext
{
    Undefined,
    WaitingForModule,
    Initializing,
    ScanMode,
    MonitorMode,
    ScanAnimation,
    MonitorAnimation
} EAppContext;

typedef enum EWorkerEventFlags
{
    WorkerEventReserved = (1 << 0), // Reserved for StreamBuffer internal event
    WorkerEventStop = (1 << 1),
    WorkerEventRx = (1 << 2),
} EWorkerEventFlags;


typedef struct SWiFiDeauthApp 
{
    Gui* m_gui;
    FuriThread* m_worker_thread;
    //NotificationApp* m_notification;
    StreamBufferHandle_t m_rx_stream;

    bool m_wifiDeauthModuleInitialized;
    bool m_wifiDeauthModuleAttached;

    EAppContext m_context;

    uint8_t m_backBuffer[128 * 8 * 8]; // 128 * 64 * 8
    uint8_t m_renderBuffer[128 * 8 * 8];

    uint8_t* m_backBufferPtr;
    uint8_t* m_m_renderBufferPtr;

    uint8_t* m_originalBuffer;
    uint8_t** m_originalBufferLocation;
    size_t m_canvasSize;

    bool m_needUpdateGUI;
} SWiFiDeauthApp; 

/////// INIT STATE /////// 
static void esp8266_deauth_app_init(SWiFiDeauthApp* const app)
{
    app->m_context = Undefined;

    app->m_canvasSize = 128 * 8 * 8;
    memset(app->m_backBuffer, 0xFF, app->m_canvasSize);
    memset(app->m_renderBuffer, 0xFF, app->m_canvasSize);

    app->m_originalBuffer = NULL;
    app->m_originalBufferLocation = NULL;

    app->m_m_renderBufferPtr = app->m_renderBuffer;
    app->m_backBufferPtr = app->m_backBuffer;

    app->m_needUpdateGUI = false;

#if ENABLE_MODULE_POWER
    app->m_wifiDeauthModuleInitialized = false;
#else
    app->m_wifiDeauthModuleInitialized = true;
#endif // ENABLE_MODULE_POWER

#if ENABLE_MODULE_DETECTION
    app->m_wifiDeauthModuleAttached = false;
#else
    app->m_wifiDeauthModuleAttached = true;
#endif
}

static void wifi_module_render_callback(Canvas* const canvas, void* ctx)
{
    SWiFiDeauthApp* app = acquire_mutex((ValueMutex*)ctx, 25);
    if (app == NULL)
    {
        return;
    }

    //if(app->m_needUpdateGUI)
    //{
    //    app->m_needUpdateGUI = false;

    //    //app->m_canvasSize = canvas_get_buffer_size(canvas);
    //    //app->m_originalBuffer = canvas_get_buffer(canvas);
    //    //app->m_originalBufferLocation = &u8g2_GetBufferPtr(&canvas->fb);
    //    //u8g2_GetBufferPtr(&canvas->fb) = app->m_m_renderBufferPtr;
    //}

    //uint8_t* exchangeBuffers = app->m_m_renderBufferPtr;
    //app->m_m_renderBufferPtr = app->m_backBufferPtr;
    //app->m_backBufferPtr = exchangeBuffers;

    if(app->m_needUpdateGUI)
    { 
        //memcpy(app->m_renderBuffer, app->m_backBuffer, app->m_canvasSize);
        app->m_needUpdateGUI = false;
    }

    uint8_t* buffer = canvas_get_buffer(canvas);
    app->m_canvasSize = canvas_get_buffer_size(canvas);
    memcpy(buffer, app->m_backBuffer, app->m_canvasSize);

    release_mutex((ValueMutex*)ctx, app);
}

static void wifi_module_input_callback(InputEvent* input_event, FuriMessageQueue* event_queue) {
    furi_assert(event_queue); 

    SPluginEvent event = {.m_type = EventTypeKey, .m_input = *input_event};
    furi_message_queue_put(event_queue, &event, FuriWaitForever);
}

static void uart_on_irq_cb(UartIrqEvent ev, uint8_t data, void* context) {
    furi_assert(context);
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;

    SWiFiDeauthApp* app = context;

    DEAUTH_APP_LOG_I("uart_echo_on_irq_cb");

    if(ev == UartIrqEventRXNE) {
        DEAUTH_APP_LOG_I("ev == UartIrqEventRXNE");
        xStreamBufferSendFromISR(app->m_rx_stream, &data, 1, &xHigherPriorityTaskWoken);
        furi_thread_flags_set(furi_thread_get_id(app->m_worker_thread), WorkerEventRx);
        portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
    }

    
}

static int32_t uart_worker(void* context) {
    furi_assert(context);

    while(true) 
    {
        uint32_t events = furi_thread_flags_wait(WorkerEventStop | WorkerEventRx, FuriFlagWaitAny, FuriWaitForever);
        furi_check((events & FuriFlagError) == 0);

        if(events & WorkerEventStop) break;
        if(events & WorkerEventRx) 
        {

            SWiFiDeauthApp* app = acquire_mutex((ValueMutex*)context, 25);
            if (app == NULL)
            {
                return 1;
            }
            size_t length = 0;
            int index = 0;
            do 
            {
                uint8_t data[64];
                length = xStreamBufferReceive(app->m_rx_stream, data, 64, 25);
                if(length > 0) 
                {
                    memcpy(app->m_backBuffer + index, data, length);
                    index += length;
                }
            } while(length > 0);

            app->m_needUpdateGUI = true;

            release_mutex((ValueMutex*)context, app);
        }
    }

    return 0;
}

int32_t esp8266_deauth_app(void* p)
{ 
    UNUSED(p);

    DEAUTH_APP_LOG_I("Init");
    
    // FuriTimer* timer = furi_timer_alloc(blink_test_update, FuriTimerTypePeriodic, event_queue);
    // furi_timer_start(timer, furi_kernel_get_tick_frequency());

    FuriMessageQueue* event_queue = furi_message_queue_alloc(8, sizeof(SPluginEvent)); 

    SWiFiDeauthApp* app = malloc(sizeof(SWiFiDeauthApp));

    esp8266_deauth_app_init(app);

    furi_hal_gpio_init_simple(&gpio_ext_pc3, GpioModeOutputPushPull);
    furi_hal_gpio_init_simple(&gpio_ext_pb2, GpioModeOutputPushPull);
    furi_hal_gpio_init_simple(&gpio_ext_pb3, GpioModeOutputPushPull);
    furi_hal_gpio_init_simple(&gpio_ext_pa4, GpioModeOutputPushPull);

    furi_hal_gpio_write(&gpio_ext_pc3, true);
    furi_hal_gpio_write(&gpio_ext_pb2, true);
    furi_hal_gpio_write(&gpio_ext_pb3, true);
    furi_hal_gpio_write(&gpio_ext_pa4, true);

#if ENABLE_MODULE_DETECTION    
    furi_hal_gpio_init(&gpio_ext_pc0, GpioModeInput, GpioPullUp, GpioSpeedLow); // Connect to the Flipper's ground just to be sure 
    //furi_hal_gpio_add_int_callback(pinD0, input_isr_d0, this);
    app->m_context = WaitingForModule;
#else
#if ENABLE_MODULE_POWER
    app->m_context = Initializing;
    furi_hal_power_enable_otg();
#endif
#endif // ENABLE_MODULE_DETECTION

    ValueMutex app_data_mutex; 
    if (!init_mutex(&app_data_mutex, app, sizeof(SWiFiDeauthApp))) {
        DEAUTH_APP_LOG_E("cannot create mutex\r\n");
        free(app); 
        return 255;
    }

    DEAUTH_APP_LOG_I("Mutex created");

    app->m_rx_stream = xStreamBufferCreate(1 * 1024, 1);

    //app->m_notification = furi_record_open("notification");

    ViewPort* view_port = view_port_alloc(); 
    view_port_draw_callback_set(view_port, wifi_module_render_callback, &app_data_mutex);
    view_port_input_callback_set(view_port, wifi_module_input_callback, event_queue);

    // Open GUI and register view_port
    Gui* gui = furi_record_open("gui"); 
    gui_add_view_port(gui, view_port, GuiLayerFullscreen); 

    //notification_message(app->notification, &sequence_set_only_blue_255);

    // Enable uart listener
#if DISABLE_CONSOLE    
    furi_hal_console_disable();
#endif
    furi_hal_uart_set_br(FuriHalUartIdUSART1, 230400); // 115200 921600
    furi_hal_uart_set_irq_cb(FuriHalUartIdUSART1, uart_on_irq_cb, app);
    DEAUTH_APP_LOG_I("UART Listener created");

    app->m_worker_thread = furi_thread_alloc();
    furi_thread_set_name(app->m_worker_thread, "WiFiModuleUARTWorker");
    furi_thread_set_stack_size(app->m_worker_thread, 1024);
    furi_thread_set_context(app->m_worker_thread, &app_data_mutex);
    furi_thread_set_callback(app->m_worker_thread, uart_worker);
    furi_thread_start(app->m_worker_thread);
    DEAUTH_APP_LOG_I("UART thread allocated");

    SPluginEvent event; 
    for(bool processing = true; processing;) 
    {
        FuriStatus event_status = furi_message_queue_get(event_queue, &event, 100);
        SWiFiDeauthApp* app = (SWiFiDeauthApp*)acquire_mutex_block(&app_data_mutex);

#if ENABLE_MODULE_DETECTION
        if(!app->m_wifiDeauthModuleAttached)
        {
            if(furi_hal_gpio_read(&gpio_ext_pc0) == false)
            {
                DEAUTH_APP_LOG_I("Module Attached");
                app->m_wifiDeauthModuleAttached = true;
#if ENABLE_MODULE_POWER
                app->m_context = Initializing;
                furi_hal_power_enable_otg();
#endif
            }
        }
#endif // ENABLE_MODULE_DETECTION

        if(event_status == FuriStatusOk) 
        {
            if(event.m_type == EventTypeKey) 
            {
                //if (app->m_wifiModuleInitialized)
                {
                    //if (app->m_context == Initializing)
                    {
                        switch (event.m_input.key)
                        {
                            case InputKeyUp:
                                if (event.m_input.type == InputTypePress)
                                {
                                    DEAUTH_APP_LOG_I("Previous Press");
                                    furi_hal_gpio_write(&gpio_ext_pb2, false);
                                }
                                else if (event.m_input.type == InputTypeRelease)
                                {
                                    DEAUTH_APP_LOG_I("Previous Release");
                                    furi_hal_gpio_write(&gpio_ext_pb2, true);
                                }
                                break;
                            case InputKeyDown:
                                if (event.m_input.type == InputTypePress)
                                {
                                    DEAUTH_APP_LOG_I("Next Press");
                                    furi_hal_gpio_write(&gpio_ext_pc3, false);
                                }
                                else if (event.m_input.type == InputTypeRelease)
                                {
                                    DEAUTH_APP_LOG_I("Next Release");
                                    furi_hal_gpio_write(&gpio_ext_pc3, true);
                                }
                                break;
                            case InputKeyOk:
                                if (event.m_input.type == InputTypePress)
                                {
                                    DEAUTH_APP_LOG_I("OK Press");
                                    furi_hal_gpio_write(&gpio_ext_pb3, false);
                                }
                                else if (event.m_input.type == InputTypeRelease)
                                {
                                    DEAUTH_APP_LOG_I("OK Release");
                                    furi_hal_gpio_write(&gpio_ext_pb3, true);
                                }
                                break;
                            case InputKeyBack:
                                if (event.m_input.type == InputTypePress)
                                {
                                    DEAUTH_APP_LOG_I("Back Press");
                                    furi_hal_gpio_write(&gpio_ext_pa4, false);
                                }
                                else if (event.m_input.type == InputTypeRelease)
                                {
                                    DEAUTH_APP_LOG_I("Back Release");
                                    furi_hal_gpio_write(&gpio_ext_pa4, true);
                                }
                                else if (event.m_input.type == InputTypeLong)
                                {
                                    DEAUTH_APP_LOG_I("Back Long");
                                    processing = false;
                                }
                                break;
                            default:
                                break;
                        }
                    }
                }
                //else
                //{
                //    if(event.m_input.key == InputKeyBack)
                //    {
                //        if(event.m_input.type == InputTypeShort || event.m_input.type == InputTypeLong) //event.input.type == InputTypePress)
                //        {
                //            processing = false;
                //        }
                //    }
                //}
            } 
        } 
        else 
        {
            DEAUTH_APP_LOG_D("osMessageQueue: event timeout");
        }

#if ENABLE_MODULE_DETECTION
        if(app->m_wifiDeauthModuleAttached && furi_hal_gpio_read(&gpio_ext_pc0) == true)
        {
            DEAUTH_APP_LOG_D("Module Disconnected - Exit");
            processing = false;
            app->m_wifiDeauthModuleAttached = false;
            app->m_wifiDeauthModuleInitialized = false;
        }
#endif

        //if(app->m_needUpdateGUI)
        //{
            view_port_update(view_port);
        //    app->m_needUpdateGUI = false;
        //}
        release_mutex(&app_data_mutex, app);

        //view_port_update(view_port);
    }

    DEAUTH_APP_LOG_I("Start exit app");

    furi_thread_flags_set(furi_thread_get_id(app->m_worker_thread), WorkerEventStop);
    furi_thread_join(app->m_worker_thread);
    furi_thread_free(app->m_worker_thread);

    DEAUTH_APP_LOG_I("Thread Deleted");

#if DISABLE_CONSOLE
    furi_hal_console_enable();
#endif

    //*app->m_originalBufferLocation = app->m_originalBuffer;

    view_port_enabled_set(view_port, false);

    gui_remove_view_port(gui, view_port);

    // Close gui record
    furi_record_close("gui");
    furi_record_close("notification");
    app->m_gui = NULL;

    view_port_free(view_port);

    furi_message_queue_free(event_queue);  

    vStreamBufferDelete(app->m_rx_stream);

    delete_mutex(&app_data_mutex);

    

    // Free rest
    free(app);

    DEAUTH_APP_LOG_I("App freed");

#if ENABLE_MODULE_POWER
    furi_hal_power_disable_otg();
#endif

    return 0;
}
