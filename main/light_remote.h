
#ifndef _TR_LIGHT_REMOTE_H_
#define _TR_LIGHT_REMOTE_H_

#ifdef __cplusplus
extern "C" {
#endif

/* ESPNOW can work in both station and softap mode. It is configured in menuconfig. */
#if CONFIG_STATION_MODE
#define ESPNOW_WIFI_MODE WIFI_MODE_STA
#define ESPNOW_WIFI_IF   ESP_IF_WIFI_STA
#else
#define ESPNOW_WIFI_MODE WIFI_MODE_AP
#define ESPNOW_WIFI_IF   ESP_IF_WIFI_AP
#endif

#define ESPNOW_QUEUE_SIZE           6
//
// tr
//
typedef struct
{
  union
  {
    struct
    {
    // {d c b a} 
      uint32_t cmd   :8; // LSB << available for error or special data/reqs
      uint32_t blue  :8; //      blue
      uint32_t green :8; //      green
      uint32_t red   :8; // MSB  red
    };
    uint32_t val;
  };
}tr_data_t;
//
typedef struct {
    uint8_t mac_addr[ESP_NOW_ETH_ALEN];
    tr_data_t data;
    int data_len;
} tr_espnow_data_t;
//
typedef struct {
    uint8_t mac_addr[ESP_NOW_ETH_ALEN];
    esp_now_send_status_t status;
} tr_espnow_status_t;
//
// DISPLAY
//
#define DISP_OP_POS           remote->display->x = 32; \
                              remote->display->y = 1;            
#define DISP_CURSOR_OP_POS    remote->display->x = 24; \
                              remote->display->y = 1;            

#define DISP_CURSOR_RED_POS   remote->display->x = 32; \
                              remote->display->y = 12;            
#define DISP_CURSOR_GREEN_POS remote->display->x = 32; \
                              remote->display->y = 12;            
#define DISP_CURSOR_BLUE_POS  remote->display->x = 32; \
                              remote->display->y = 12;            

#define SET_DISP_XY(a, b) remote->display->x = a; \
                          remote->display->y = b;

typedef struct
{
  // _INPUT_t
  FontDef_t font;
  int       x;
  int       y;
  // Disp_t
  int16_t   MaskX1;
  int16_t   MaskY1;
  int16_t   MaskX2;
  int16_t   MaskY2;
  // new
  char      cursor;
  uint16_t  *cursor_pos;  // {{x,y}, {x,y}, ...}

  uint8_t   rgb[3];       // [0-255][][]
  uint16_t  *rgb_pos;     // {{x,y}, {x,y}, ...}

} DISPLAY_t;

typedef enum
{
  RED   = 0,
  GREEN,
  BLUE
} LED_color_t;
//
// structures borrowed from addressable_led
//
typedef struct
{
  union
  {
    struct
    {
    // {0 b g r}  
      uint32_t blue   :8;
      uint32_t green  :8;
      uint32_t red    :8;
    };
    uint32_t val;
  };
}pixel_t;

#define OLED_CMD_SET_SEGMENT_NORMAL_MODE  0xA0
#define OLED_CMD_SET_SEGMENT_INVERSE_MODE 0xA1
#define OLED_CMD_SET_COM_NORMAL_MODE      0xC0
#define OLED_CMD_SET_COM_REMAP_MODE       0xC8
#define OLED_CMD_SET_DISABLE_SCROLL       0x2E
#define OLED_CHARGE_PUMP_ON               0x14
#define OLED_CHARGE_PUMP_OFF              0x10

#define OLED_CMD_SET_MEMORY_ADDR_MODE     0x20    // follow with 0x00 = HORZ mode = Behave like a KS108 graphic LCD
#define OLED_CMD_SET_COLUMN_RANGE         0x21    // can be used only in HORZ/VERT mode - follow with 0x00 and 0x7F = COL127
#define OLED_CMD_SET_PAGE_RANGE           0x22    // can be used only in HORZ/VERT mode - follow with 0x00 and 0x07 = PAGE7

typedef enum 
{
  _OVERRIDE = 0x00,
  _TRANSPARENT = 0x01
} _DRAW_t;

typedef enum 
{
  _BLACK = 0x00,
  _WHITE = 0x01
} _COLOR_t;

#define _WIDTH     128
#define _HEIGHT    32

#define _BUFFER_CMD_MAX    64
#define _BUFFER_DATA_MAX   (_WIDTH * _HEIGHT / 8)

#define _CONTROL_CMD_SINGLE    0x80
#define _CONTROL_CMD_STREAM    0x00
#define _CONTROL_DATA_SINGLE   0xC0
#define _CONTROL_DATA_STREAM   0x40

typedef struct 
{
  uint8_t CmdBufferIndex;
  uint8_t CmdBuffer[_BUFFER_CMD_MAX];
  uint8_t DataBuffer[_BUFFER_DATA_MAX+1];
} Buffer_t;
//
// LIGHT
//
typedef struct
{
  pixel_t   pixel;
  bool      active;
 
} LIGHT_t;
//
// encoder
//
typedef enum
{
  CW = 0,
  CCW
} _DIR_t;

typedef enum
{
  INDEX = 0,
  ADJUST
} _ACTION_t;

typedef enum
{
  ENCODER_PULSE = 0,
  ENCODER_SWITCH
}_ENCODER_INPUT;

typedef struct
{
  volatile _DIR_t         dir;
  volatile _ENCODER_INPUT swtch;  // 0 = encoder pulse : 1 = switch
  bool                    active;
} ENCODER_t;

#define GPIO_ENCODER_SW       13
#define GPIO_ENCODER_SW_PIN   (1ULL<<GPIO_ENCODER_SW)
#define GPIO_PWM_INPUT        15
#define GPIO_PWM_INPUT_PIN    (1ULL<<GPIO_PWM_INPUT)
#define GPIO_ENCODER_B        4
#define GPIO_ENCODER_A        5
#define GPIO_ENCODER_AB_PINS  ((1ULL<<GPIO_ENCODER_B) | (1ULL<<GPIO_ENCODER_A))
#define GPIO_ENCODER_PINS     (GPIO_ENCODER_AB_PINS | (1ULL<<GPIO_ENCODER_SW))

#define ENABLE_ENCODER  gpio_set_intr_type(GPIO_ENCODER_A, GPIO_INTR_ANYEDGE); \
                        gpio_set_intr_type(GPIO_ENCODER_B, GPIO_INTR_ANYEDGE);
#define DISABLE_ENCODER gpio_set_intr_type(GPIO_ENCODER_A, GPIO_INTR_DISABLE); \
                        gpio_set_intr_type(GPIO_ENCODER_B, GPIO_INTR_DISABLE);
#define ENABLE_ENC_SW   gpio_set_intr_type(GPIO_ENCODER_SW, GPIO_INTR_NEGEDGE);
#define DISABLE_ENC_SW  gpio_set_intr_type(GPIO_ENCODER_SW, GPIO_INTR_DISABLE);
//
// DISPLAY TIMER
// the display timer is active whenever the
// encoder is active
// this is such a hack, but it works!
// the pwm output gpio 12 is directed to
// gpio 15 
// gpio 15 input will trigger a low going 
// edge interrupt, count it. when > some
// terminal count, shut down, i.e. 
// stop entry, encoder.active false, clear display
// finally reset to state 0
//
// PWM period, unit: us. (micro seconds)
// For 1KHz PWM, period is 1000 us. 
// Do not set the period below 20us.
//
// pwm_period = 100000x10-6 
// pwm_freq   = 10 Hz i.e. 10 interrupts per second
//
#define PWM_PERIOD    (100000)
#define PWM_0_OUT_IO_NUM   12
//
// REMOTE
//
#define PLUS            0x1
#define MINUS           0x2
#define ENCODER_PULSE   false
#define SWTCH           true
typedef enum
{
  op_ALL      = 0,
  op_RED,
  op_GREEN,
  op_BLUE,
  op_OFF,    
  op_ON,
  op_STATUS,
  op_SNOOZE,
  op_WAKE_UP,
  op_ADJUST,
  op_END      = 90
  
} OPERATION_t;

typedef struct
{
	ENCODER_t     *encoder;
	DISPLAY_t     *display;
  LIGHT_t       *light;
  OPERATION_t   op_index;
    
} REMOTE_t;

//
// prototypes
//
void _Clear_String(DISPLAY_t *display, int16_t string_length, _DRAW_t mode);
void _WriteString(DISPLAY_t *display, char *str, _COLOR_t color, _DRAW_t mode);
void _WriteChar(DISPLAY_t *display, char ch, _COLOR_t color, _DRAW_t mode);
void _Screen_Update(void);
void _DrawPixel(int16_t x, int16_t y, _COLOR_t color);
void _Clear(void);
void _Fill(_COLOR_t color);

static void gpio_task_b(void *arg);
static void gpio_task_a(void *arg);
static void gpio_task_sw(void *arg);
static void gpio_task_pwm(void *arg);
static void kill_timeout(REMOTE_t *remote);

static void tr_espnow_send_task(tr_espnow_data_t *send);
static void tr_espnow_deinit(void);

static void tr_dispatcher(void *arg);
static void tr_espnow_receive_task(void * arg);
static OPERATION_t tr_next_operation(OPERATION_t current_op_index);

#ifdef __cplusplus
}
#endif

#endif
