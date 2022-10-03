#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "driver/gpio.h"
#include "driver/i2c.h"
#include "driver/pwm.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_err.h"

#include "esp_wifi.h"
#include "nvs_flash.h"
#include <sys/param.h>
#include "esp_event.h"
#include "esp_now.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/timers.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
//
// main/Kconfig.projbuild controls and retrieves 
// global variables
// 
#include "sdkconfig.h"  // generated by "make menuconfig"
#include "errno.h"

//#include "lwip/err.h"
//#include "lwip/sys.h"

#include "ssd1366.h"
#include "fonts.h"
#include "light_remote.h"

#define SDA_PIN   14
#define SCL_PIN   2

static const char *TAG = "trLightRemote";
//
//
//
char *trOperation[] = {"ALL", "RED", "GREEN", "BLUE", "ON", "OFF"};
char trCursor               = 0x7f;
uint16_t cursor_position[]  = {24, 1, 8, 12, 48, 12, 88, 12};
uint16_t rgb_position[]     = {16, 12, 56, 12, 96, 12};
uint16_t op_position[]      = {32, 1};
volatile uint32_t           ulIdleCycleCount;

volatile uint32_t           timer_intr_counter;

#if 0
typedef struct
{
  _DIR_t    dir;
  FontDef_t font;
  int       x_position;
  int       y_position;
  int       color_idx;
  int       opers_idx;
} _INPUT_t;
#endif

static xQueueHandle gpio_evt_queue_a      = NULL;
static xQueueHandle gpio_evt_queue_b      = NULL;
static xQueueHandle gpio_evt_queue_sw     = NULL;
static xQueueHandle gpio_evt_queue_pwm    = NULL;
static xQueueHandle encoder_evt_queue     = NULL;
static xQueueHandle tr_espnow_send_queue  = NULL;
static xQueueHandle tr_espnow_recv_queue  = NULL;
static TaskHandle_t tr_recv_h             = NULL;
static SemaphoreHandle_t tr_recv_sem      = NULL;

//static uint8_t tr_kitchen_light_mac[ESP_NOW_ETH_ALEN] = {0x7c, 0xdf, 0xa1, 0x1a, 0x2c, 0x96};
static uint8_t tr_kitchen_light_mac[ESP_NOW_ETH_ALEN] = {0x7c, 0xdf, 0xa1, 0x1a, 0x2d, 0x16};


static uint8_t tr_ping[4] = { 'p','i','n','g' };
static uint8_t tr_status[4] = { 's','t','a','t' };

Buffer_t test_buffer = 
{
  .CmdBufferIndex = 0,
  .CmdBuffer = {_CONTROL_CMD_STREAM, 0},
  .DataBuffer = {_CONTROL_DATA_STREAM, 0}
};

//
// subroutines:
//
//
//
// because there is only one isr handler available for
// all the gpio pins, all the interrupts are processed
// through this one lone function
// sad...
//
static void gpio_isr_handler(void *arg)
{
  uint32_t gpio_num = (uint32_t) arg;
  
  if(gpio_num == GPIO_ENCODER_A)
    xQueueSendFromISR(gpio_evt_queue_a, &gpio_num, NULL);
  else if(gpio_num == GPIO_ENCODER_B)
    xQueueSendFromISR(gpio_evt_queue_b, &gpio_num, NULL);
  else if(gpio_num == GPIO_PWM_INPUT)
    xQueueSendFromISR(gpio_evt_queue_pwm, &gpio_num, NULL);
  else if(gpio_num == GPIO_ENCODER_SW)
    xQueueSendFromISR(gpio_evt_queue_sw, &gpio_num, NULL);
  else
  {}
  
}
//
//
//
static void gpio_task_a(void *arg)
{
  uint32_t io_num;
  uint32_t level_a, level_b;
  REMOTE_t *remote = (REMOTE_t *)arg;
  
  for (;;) {
    // xQueueReceive pops an item from the queue
    // and adjusts automagically
    xQueueReceive(gpio_evt_queue_a, &io_num, portMAX_DELAY);
    //xQueueReset(gpio_evt_queue_a);
    
    timer_intr_counter = 1;
    
    // here the edge of the interrupt is sorted out
    // since this is triggered by A, the B input
    // contains the history of the last event and 
    // knowing this is interrupt A and the current op
    // with that information, the direction of rotation 
    // can be determined
    level_a = gpio_get_level(GPIO_ENCODER_A);
    level_b = gpio_get_level(GPIO_ENCODER_B);
#if 0
    if(level_a == 1 && level_b == 1)
      // ccw
    else if(level_a == 1 && level_b == 0)
      // cw
    else if(level_a == 0 && level_b == 1)
      // cw
    else
      // ccw
#endif
    uint32_t dir = ~(level_a ^ level_b);
    dir &= 0x0001;
    remote->encoder->dir = (_DIR_t)dir;
    remote->encoder->swtch = ENCODER_PULSE;
    xQueueSendToBack(encoder_evt_queue, arg, 10);
    
    //ESP_LOGI(TAG, "DIR : 0x%04x", remote->encoder->dir);    
  }
}
//
//
//
static void gpio_task_b(void *arg)
{
  uint32_t io_num;
  uint32_t level_a, level_b;
  REMOTE_t *remote = (REMOTE_t *)arg;
  
  for (;;) {
    xQueueReceive(gpio_evt_queue_b, &io_num, portMAX_DELAY);
    //xQueueReset(gpio_evt_queue_b);
    
    timer_intr_counter = 1;
    
    level_a = gpio_get_level(GPIO_ENCODER_A);
    level_b = gpio_get_level(GPIO_ENCODER_B);
    
    uint32_t dir = (level_a ^ level_b) & 0x0001;
    remote->encoder->dir = (_DIR_t)dir;
    remote->encoder->swtch = ENCODER_PULSE;
    xQueueSendToBack(encoder_evt_queue, arg, 10);

    //ESP_LOGI(TAG, "DIR : 0x%04x", remote->encoder->dir);
  }
}
//
//
//
static void gpio_task_sw(void *arg)
{
  uint32_t io_num;
  REMOTE_t *remote = (REMOTE_t *)arg;
  
  for (;;) {
    xQueueReceive(gpio_evt_queue_sw, &io_num, portMAX_DELAY);
    //xQueueReset(gpio_evt_queue_sw);
    
    timer_intr_counter = 1;
    
    vTaskDelay(20/portTICK_PERIOD_MS);
    if(!(gpio_get_level(GPIO_ENCODER_SW)))
    {
      ESP_LOGI(TAG, "sw");
      remote->encoder->swtch = ENCODER_SWITCH;
      xQueueSendToBack(encoder_evt_queue, arg, 10);

    }
  }
}
//
//
//
static void kill_timeout(REMOTE_t *remote)
{
  pwm_stop(0);
  timer_intr_counter = 1;
  DISABLE_ENCODER
  remote->op_index = op_SNOOZE;
  tr_espnow_deinit();
  // go to sleep...
  
}
//
//
//
#define TIMEOUT 50
static void gpio_task_pwm(void *arg)
{
  uint32_t io_num;
  REMOTE_t *remote = (REMOTE_t *)arg;
  
  for(;;)
  {
    xQueueReceive(gpio_evt_queue_pwm, &io_num, portMAX_DELAY);
    //xQueueReset(gpio_evt_queue_pwm);
    timer_intr_counter++;
    //ESP_LOGI(TAG, "callback");
    if(0 == (timer_intr_counter % TIMEOUT))
    {
      _Clear();
      _Screen_Update();
      kill_timeout(remote);
    }
  }
}
//
//
//
static OPERATION_t tr_next_operation(OPERATION_t current_op_index)
{
  OPERATION_t next_op_index;
  
  switch(current_op_index)
  {
    case op_ALL:
      next_op_index = op_RED;
    break;
    case op_RED:
      next_op_index = op_GREEN;
    break;
    case op_GREEN:
      next_op_index = op_BLUE;
    break;
    case op_BLUE:
      next_op_index = op_ON;
    break;
    case op_ON:
    default:
      next_op_index = op_ALL;
    break;
  }
  return next_op_index;
}
//
//
//
static void tr_dispatcher(void *arg)
{
  REMOTE_t *remote = (REMOTE_t *)arg;
  OPERATION_t current_op_index = op_ALL;
  tr_espnow_data_t update;
            
  memcpy(update.mac_addr, tr_kitchen_light_mac, ESP_NOW_ETH_ALEN);
  update.data_len = sizeof(tr_data_t);
  // start up the receive task
  xTaskCreate(tr_espnow_receive_task, "tr_receive_task", 4096, (void *)&update, 4, NULL);
  //
  // remote->light->pixel must be current with update->data
  // pixel_t <=> tr_data_t
  // this is due to the receiving task taking only tr_espnow_data_t
  // and to update the display, font info is required
  //
  // the system is coming out of reset
  // from a deep sleep
  // this task starts
  // op_SNOOZE is happens first before the loop
  // 
  printf("wake up\n");
  // wake up
  pwm_start();
  // get status
  update.data.cmd = (uint8_t)op_STATUS;
 // clear semaphore
  xSemaphoreTake(tr_recv_sem, 0);
  tr_espnow_send_task(&update);
  // wait for response
  // update will contain the light status
  // i.e. active plus rgb
  // should probably figure out a better delay
  if(xSemaphoreTake(tr_recv_sem, portMAX_DELAY) != pdPASS)
    printf("AH CRAP!!\n");

  _Clear();
  // cursor
  SET_DISP_XY(cursor_position[0], cursor_position[1]);
  _WriteChar(remote->display, trCursor, _WHITE, _OVERRIDE);
  remote->light->active = update.data.cmd;
  if(update.data.cmd == op_ON)
  {
    // lights are on
    // enable encoder
    ENABLE_ENCODER
    // set up display for op_ON
    remote->op_index = op_ON;
    SET_DISP_XY(op_position[0], op_position[1]);
    _WriteString(remote->display, trOperation[remote->op_index], _WHITE, _OVERRIDE);
  }
  else
  {
    // set up display for op_OFF
    remote->op_index = op_OFF;
    SET_DISP_XY(op_position[0], op_position[1]);
    _WriteString(remote->display, trOperation[remote->op_index], _WHITE, _OVERRIDE);
  }
  _Screen_Update();

  
  while(1)
  {
    xQueueReceive(encoder_evt_queue, remote, portMAX_DELAY);
    //current_op_index = remote->op_index;
printf("op_index : %d\n", current_op_index);
    switch(remote->op_index)
    {
      case op_OFF:
        if(remote->encoder->swtch == ENCODER_SWITCH)
        {
          // turn lights on
          update.data.cmd = (uint8_t)op_ON;
          //update.data_len = sizeof(tr_data_t);
          // the cmd is important
          // the server will do the command
          // and turn the lights on
          // rgb is ignored
          tr_espnow_send_task(&update);
          // enable encoder
          ENABLE_ENCODER
          // set up display for op_ALL
          remote->op_index = op_ALL;
          SET_DISP_XY(op_position[0], op_position[1]);
          _Clear_String(remote->display, 8, _OVERRIDE);
          _WriteString(remote->display, trOperation[remote->op_index], _WHITE, _OVERRIDE);
          _Screen_Update();
        }
        
      break;
      case op_ON:
        if(remote->encoder->swtch == ENCODER_SWITCH)
        {
          // turn lights off
          update.data.cmd = (uint8_t)op_OFF;
          //update.data_len = sizeof(tr_data_t);
          tr_espnow_send_task(&update);
          // disable encoder
          DISABLE_ENCODER
          remote->op_index = op_SNOOZE;
          _Clear();
          _Screen_Update();
        }
        else
        {
          // set up display for op_ALL
          remote->op_index = op_ALL;
          SET_DISP_XY(op_position[0], op_position[1]);
          _Clear_String(remote->display, 8, _OVERRIDE);
          _WriteString(remote->display, trOperation[remote->op_index], _WHITE, _OVERRIDE);
          _Screen_Update();
        }
      break;
      case op_ALL:
      case op_RED:
      case op_GREEN:
      case op_BLUE:
        current_op_index = remote->op_index;
        if(remote->encoder->swtch == ENCODER_SWITCH)
        {
          // adjust
      printf("current : %d\n", current_op_index);
          // need the most recent pixel data
          update.data.cmd = (uint8_t)op_STATUS;
          // clear semaphore
          xSemaphoreTake(tr_recv_sem, 0);
          tr_espnow_send_task(&update);
          if(xSemaphoreTake(tr_recv_sem, portMAX_DELAY) != pdPASS)
            printf("AH WTF!!\n");

          
          remote->op_index = op_ADJUST;
        }
        else // pulse
        {
          remote->op_index = tr_next_operation(remote->op_index);
          SET_DISP_XY(op_position[0], op_position[1]);
          _Clear_String(remote->display, 8, _OVERRIDE);
          _WriteString(remote->display, trOperation[remote->op_index], _WHITE, _OVERRIDE);
          _Screen_Update();
        }
        
      break;
      case op_ADJUST:
        // done adjusting
        if(remote->encoder->swtch == ENCODER_SWITCH)
        {
          remote->op_index = tr_next_operation(current_op_index);
          SET_DISP_XY(op_position[0], op_position[1]);
          _Clear_String(remote->display, 8, _OVERRIDE);
          _WriteString(remote->display, trOperation[remote->op_index], _WHITE, _OVERRIDE);
          _Screen_Update();
        }
        else // pulse => adjust
        {
          // [current_op_index]++|--
          // operations allowed : ALL RED GREEN BLUE
          // current_op_index is the color | all being adjusted
          ESP_LOGI("current op_index", "%d", current_op_index);
          update.data.val = 0;
          update.data.cmd = op_ADJUST;
          switch(current_op_index)
          {
            case op_ALL:
              if(remote->encoder->dir == CW)
              {
                update.data.red = PLUS;
                update.data.green = PLUS;
                update.data.blue = PLUS;
              }
              else
              {
                update.data.red = MINUS;
                update.data.green = MINUS;
                update.data.blue = MINUS;              
              }
              tr_espnow_send_task(&update);
            break;
            case op_RED:
              if(remote->encoder->dir == CW)
              {
                update.data.red = PLUS;
              }
              else
              {
                update.data.red = MINUS;
              }
              ESP_LOGI("T4t0nk4", "red : %d", update.data.red);
              tr_espnow_send_task(&update);
            break;
            case op_GREEN:
              if(remote->encoder->dir == CW)
              {
                update.data.green = PLUS;
              }
              else
              {
                update.data.green = MINUS;
              }
              tr_espnow_send_task(&update);
            break;
            case op_BLUE:
              if(remote->encoder->dir == CW)
              {
                update.data.blue = PLUS;
              }
              else
              {
                update.data.blue = MINUS;
              }
              tr_espnow_send_task(&update);
            break;
            default:
            break;
          }
        }
        
      break;
      default:
      break;
    }// switch
  }// while
}
//
// encoder gpio 
//
static void gpio_init(REMOTE_t *remote)
{
  gpio_config_t io_conf;

  // encoder switch input
  // interrupt on low going edge
  io_conf.intr_type = GPIO_INTR_DISABLE;
  // set as input mode
  io_conf.mode = GPIO_MODE_INPUT;
  //bit mask of the input pin
  io_conf.pin_bit_mask = GPIO_ENCODER_SW_PIN;
  //disable pull-down mode
  io_conf.pull_down_en = 0;
  //disable pull-up mode
  // input is pulled high externally
  io_conf.pull_up_en = 0;
  //configure GPIO with the given settings
  gpio_config(&io_conf);
  
  
  // pwm input
  // interrupt on neg edge
  io_conf.intr_type = GPIO_INTR_NEGEDGE;
  // bit mask of the pins, use GPIO4/5 here
  io_conf.pin_bit_mask = GPIO_PWM_INPUT_PIN;
  // set as input mode
  io_conf.mode = GPIO_MODE_INPUT;
  // disable pull-up mode
  io_conf.pull_up_en = 0;
  gpio_config(&io_conf);

  ESP_LOGI(TAG, "PWM pins %d", GPIO_PWM_INPUT);

  ESP_LOGI(TAG, "encoder switch pin %d", GPIO_ENCODER_SW);
  
  // encoder ab inputs
  // encoder gpio is setup last to leave the 
  // configuration for a/b encoder inputs
  // start up with encoder disabled
  // enabling starts with the push button
  // interrupt on any edge
  io_conf.intr_type = GPIO_INTR_DISABLE;
  // bit mask of the pins, use GPIO4/5 here
  io_conf.pin_bit_mask = GPIO_ENCODER_AB_PINS;
  // set as input mode
  io_conf.mode = GPIO_MODE_INPUT;
  // disable pull-up mode
  io_conf.pull_up_en = 0;
  gpio_config(&io_conf);

  ESP_LOGI(TAG, "encoder ab pins %d/%d", GPIO_ENCODER_A, GPIO_ENCODER_B);
  
  // create a queue to handle gpio event from isr
  gpio_evt_queue_a   = xQueueCreate(1, sizeof(uint32_t));
  gpio_evt_queue_b   = xQueueCreate(1, sizeof(uint32_t));
  gpio_evt_queue_sw  = xQueueCreate(1, sizeof(uint32_t));
  gpio_evt_queue_pwm = xQueueCreate(1, sizeof(uint32_t));
  ESP_LOGI(TAG, "queue");

// start gpio task
  xTaskCreate(gpio_task_a,  "gpio_task_a",  2048, (void *)remote, 10, NULL);
  xTaskCreate(gpio_task_b,  "gpio_task_b",  2048, (void *)remote, 10, NULL);
  xTaskCreate(gpio_task_sw,  "gpio_task_sw", 2048, (void *)remote, 10, NULL);
  xTaskCreate(gpio_task_pwm, "gpio_task_pwm", 2048, (void *)remote, 4, NULL);
    ESP_LOGI(TAG, "create");

  // install gpio isr service
  gpio_install_isr_service(0);
  // hook isr handler to gpio pin 4
  gpio_isr_handler_add(GPIO_ENCODER_B, gpio_isr_handler, (void *) GPIO_ENCODER_B);
  // hook isr handler to gpio pin 5
  gpio_isr_handler_add(GPIO_ENCODER_A, gpio_isr_handler, (void *) GPIO_ENCODER_A);
  // hook isr handler to gpio pin 16
  gpio_isr_handler_add(GPIO_ENCODER_SW, gpio_isr_handler, (void *) GPIO_ENCODER_SW);
  //
  gpio_isr_handler_add(GPIO_PWM_INPUT, gpio_isr_handler, (void *) GPIO_PWM_INPUT);
  ENABLE_ENC_SW
  ESP_LOGI(TAG, "gpio done");
  
}
//
// encoder event initialization
//
static void remote_queue_init(REMOTE_t *remote)
{
  //encoder_evt_queue  = xQueueCreate(1, sizeof(uint32_t));
  encoder_evt_queue  = xQueueCreate(1, sizeof(REMOTE_t));
  
}
//
//
//
/* ESPNOW sending or receiving callback function is called in WiFi task.
 * Users should not do lengthy operations from this task. Instead, post
 * necessary data to a queue and handle it from a lower priority task. */
//
// note: 
// when this callback fires, the mac_addr and status are passed in 
// i.e. some transmit buffer emptied and triggered some interrupt
//
static void tr_espnow_send_cb(const uint8_t *mac_addr, esp_now_send_status_t status)
{
  tr_espnow_status_t send_status, check;
  BaseType_t xHigherPriorityTaskWoken;
  xHigherPriorityTaskWoken = pdFALSE;
  
  if (mac_addr == NULL) {
      ESP_LOGE(TAG, "Send cb arg error");
      return;
  }
printf("called\n");
  // should already know mac_addr, but.....
  memcpy(send_status.mac_addr, mac_addr, ESP_NOW_ETH_ALEN);
  send_status.status = status;
  //if (xQueueSendToBackFromISR(tr_espnow_send_queue, &send_status, portMAX_DELAY) != pdTRUE) 
  if (xQueueSendToBackFromISR(tr_espnow_send_queue, &send_status, &xHigherPriorityTaskWoken) != pdTRUE) 
  {
    ESP_LOGW(TAG, "Send send queue full?");
  }
  //if(xQueuePeek(tr_espnow_send_queue, (void *)&check , portMAX_DELAY) != pdTRUE)
  //{
  //  printf("send q empty\n");
  //}
printf("hangup\n");
}
//
// note: 
// when this callback fires, the mac_addr, data and data_len are passed in 
// i.e. some receive buffer filled and triggered some interrupt
//      now deal with it
//
static void tr_espnow_recv_cb(const uint8_t *mac_addr, const uint8_t *data, int len)
{
  tr_espnow_data_t incoming;
//printf("recv_cb\n");  
  // test to be sure there are params to work on...
  if (mac_addr == NULL || data == NULL || len <= 0) 
  {
    ESP_LOGE(TAG, "Receive cb arg error");
    return;
  }
  // copy the mac
  memcpy(incoming.mac_addr, mac_addr, ESP_NOW_ETH_ALEN);
  
  memcpy(&incoming.data.val, data, len);
  incoming.data_len = len;
  // call the receive task to process the incoming data
  if (xQueueSend(tr_espnow_recv_queue, &incoming, portMAX_DELAY) != pdTRUE) 
  {
    ESP_LOGW(TAG, "Send receive queue fail");
  }
//printf("incoming data : 0x%x\n", incoming.data.val);
    //for(int i=0; i<6; i++)
    //  printf("mac[%d] : 0x%02x\n", i, incoming.mac_addr[i]);
//printf("recv_cb all good\n"); 
}
//
//
//
static void tr_espnow_send_task(tr_espnow_data_t *send)
{
  tr_espnow_status_t send_status;
  //
  // &send->data.val throws a warning
  // but it seems to work
  //
printf("sent val : 0x%08x\n", send->data.val);
  esp_err_t send_err = esp_now_send(send->mac_addr, (uint8_t *)&send->data.val, send->data_len);
  if (send_err != ESP_OK) 
  {
    ESP_LOGI(TAG, "data : 0x%x", send->data.val);
    ESP_LOGI(TAG, "len : %d", send->data_len);
    ESP_LOGE(TAG, "Send error : %s", esp_err_to_name(send_err));
    //tr_espnow_deinit();
  }
  //
  // wait for send resp
  //
//printf("waiting....\n");
  if(xQueueReceive(tr_espnow_send_queue, &send_status, 1000 / portTICK_RATE_MS) != pdPASS)
  {
    // fail
    ESP_LOGE(TAG, "xQueueReceive fail");
    printf("send status : %d\n", send_status.status);  
  }
  else
    printf("sent okay\n");
}
//
//
//
static void tr_espnow_receive_task(void * arg)
{
  tr_espnow_data_t *receive =(tr_espnow_data_t *)arg;
  tr_espnow_status_t receive_status;
  //

  while(xQueueReceive(tr_espnow_recv_queue, receive, portMAX_DELAY)== pdPASS)
  {
    //
    // unpack the data 
    // update the display
    // may consider a queue for op_snooze
    // op_snooze needs feedback posthaste
    // to select the state of the lights
    //
    // update is passed in as (void *)arg
    // 
    printf("receive okay : 0x%08x\n", receive->data.val);
    //
    xSemaphoreGive(tr_recv_sem);
  }
}
//
//
//
static esp_err_t tr_espnow_init(void)
{
  tr_espnow_data_t incoming;

    tr_espnow_send_queue = xQueueCreate(1, sizeof(tr_espnow_status_t));
    if (tr_espnow_send_queue == NULL) {
        ESP_LOGE(TAG, "Create mutex fail");
        return ESP_FAIL;
    }
    tr_espnow_recv_queue = xQueueCreate(1, sizeof(tr_espnow_data_t));
    if (tr_espnow_recv_queue == NULL) {
        ESP_LOGE(TAG, "Create mutex fail");
        vSemaphoreDelete(tr_espnow_send_queue);
        return ESP_FAIL;
    }
    tr_recv_sem = xSemaphoreCreateBinary();
    if (tr_recv_sem == NULL) {
        ESP_LOGE(TAG, "Create semaphore fail");
        vSemaphoreDelete(tr_espnow_send_queue);
        vSemaphoreDelete(tr_espnow_recv_queue);
        return ESP_FAIL;
    }
    
    /* Initialize ESPNOW and register sending and receiving callback function. */
    ESP_ERROR_CHECK( esp_now_init() );
    ESP_ERROR_CHECK( esp_now_register_send_cb(tr_espnow_send_cb) );
    ESP_ERROR_CHECK( esp_now_register_recv_cb(tr_espnow_recv_cb) );

    /* Set primary master key. */
    ESP_ERROR_CHECK( esp_now_set_pmk((uint8_t *)CONFIG_ESPNOW_PMK) );

    /* Add broadcast peer information to peer list. */
    esp_now_peer_info_t *peer = malloc(sizeof(esp_now_peer_info_t));
    if (peer == NULL) {
      ESP_LOGE(TAG, "Malloc peer information fail");
      vSemaphoreDelete(tr_espnow_send_queue);
      vSemaphoreDelete(tr_espnow_recv_queue);
      vSemaphoreDelete(tr_recv_sem);
      esp_now_deinit();
      return ESP_FAIL;
    }
    memset(peer, 0, sizeof(esp_now_peer_info_t));
    peer->channel = CONFIG_ESPNOW_CHANNEL;
    peer->ifidx = ESPNOW_WIFI_IF;
    peer->encrypt = false;
    //
    // set the initial peer mac 
    //
    memcpy(peer->peer_addr, tr_kitchen_light_mac, ESP_NOW_ETH_ALEN);
    ESP_ERROR_CHECK( esp_now_add_peer(peer) );
    free(peer);

    //xTaskCreate(tr_espnow_task, "tr_espnow_task", 2048, (void *)&incoming, 4, &tr_recv_h);

    return ESP_OK;
}
//
//
//
/* WiFi should start before using ESPNOW */
static void tr_wifi_init(void)
{
    tcpip_adapter_init();

    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK( esp_wifi_init(&cfg) );
    ESP_ERROR_CHECK( esp_wifi_set_storage(WIFI_STORAGE_RAM) );
    ESP_ERROR_CHECK( esp_wifi_set_mode(ESPNOW_WIFI_MODE) );
    ESP_ERROR_CHECK( esp_wifi_start());

    /* In order to simplify example, channel is set after WiFi started.
     * This is not necessary in real application if the two devices have
     * been already on the same channel.
     */
    ESP_ERROR_CHECK( esp_wifi_set_channel(CONFIG_ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE) );
}
//
//
//
static void tr_espnow_deinit(void)
{
  vSemaphoreDelete(tr_espnow_send_queue);
  vSemaphoreDelete(tr_espnow_recv_queue);
  vSemaphoreDelete(tr_recv_sem);
  esp_now_deinit();
}
//
// display
//
static void i2c_master_init(void)
{
	i2c_config_t i2c_config = {
		.mode = I2C_MODE_MASTER,
		.sda_io_num = SDA_PIN,
		.scl_io_num = SCL_PIN,
		.sda_pullup_en = GPIO_PULLUP_ENABLE,
		.scl_pullup_en = GPIO_PULLUP_ENABLE,
    .clk_stretch_tick = 300
	};
	i2c_driver_install(I2C_NUM_0, I2C_MODE_MASTER);
	i2c_param_config(I2C_NUM_0, &i2c_config);
}
//OLED_CONTROL_BYTE_DATA_STREAM
//
//
void _Screen_Update(void) 
{
	esp_err_t espRc;

	i2c_cmd_handle_t cmd = i2c_cmd_link_create();

  i2c_master_start(cmd);
	i2c_master_write_byte(cmd, (OLED_I2C_ADDRESS << 1) | I2C_MASTER_WRITE, true);
	i2c_master_write_byte(cmd, OLED_CONTROL_BYTE_CMD_STREAM, true);
  
  i2c_master_write_byte(cmd, OLED_CMD_SET_COLUMN_RANGE, true); 
  i2c_master_write_byte(cmd, 0, true);  
  i2c_master_write_byte(cmd, _WIDTH-1, true);  
  i2c_master_write_byte(cmd, OLED_CMD_SET_PAGE_RANGE, true); 
  i2c_master_write_byte(cmd, 0, true);  
  i2c_master_write_byte(cmd, (_HEIGHT/8) -1, true);  
	i2c_master_write_byte(cmd, OLED_CMD_DISPLAY_ON, true);           // turn display on

  espRc = i2c_master_cmd_begin(I2C_NUM_0, cmd, 100/portTICK_PERIOD_MS);
  if(ESP_OK == espRc)
    ESP_LOGI(TAG, "display update successful");
  else
    ESP_LOGI(TAG, "display update failed  code: 0x%.2X", espRc);

 	i2c_cmd_link_delete(cmd);
  cmd = i2c_cmd_link_create();
  
  
  i2c_master_start(cmd);
	i2c_master_write_byte(cmd, (OLED_I2C_ADDRESS << 1) | I2C_MASTER_WRITE, true);

  i2c_master_write(cmd, &test_buffer.DataBuffer[0], _BUFFER_DATA_MAX +1, true);
  i2c_master_stop(cmd);
  espRc = i2c_master_cmd_begin(I2C_NUM_0, cmd, 100/portTICK_PERIOD_MS);
  if(ESP_OK == espRc)
    ESP_LOGI(TAG, "display data update successful");
  else
    ESP_LOGI(TAG, "display data update failed  code: 0x%.2X", espRc);
	
  i2c_cmd_link_delete(cmd);

}
//
//
//
void _Fill(_COLOR_t color) 
{
  memset(test_buffer.DataBuffer + 1,
         (color == _BLACK) ? 0x00 : 0xFF,
         _BUFFER_DATA_MAX);
}
//
//
//
void _Clear(void) 
{
  _Fill(_BLACK);
}
//
//
//
void _DrawPixel(int16_t x, int16_t y, _COLOR_t color) 
{
  if(color == _WHITE) 
  {
    test_buffer.DataBuffer[1+ x + (y >> 3) * _WIDTH] |= (1 << (y % 8));
  } 
  else 
  {
    test_buffer.DataBuffer[1+ x + (y >> 3) * _WIDTH] &= ~(1 << (y % 8));
  }

}
//
//
//
void _WriteChar(DISPLAY_t *display, char ch, _COLOR_t color, _DRAW_t mode)
{
    int16_t x0, y0, b;

    // Translate font to screen buffer
    for (y0 = 0; y0 < display->font.height; y0++)
    {
      b = display->font.data[(ch - 32) * display->font.height + y0];
      for (x0 = 0; x0 < display->font.width; x0++)
      {
        if ((b << x0) & 0x8000)
        {
          _DrawPixel(display->x + x0, display->y + y0, (_COLOR_t) color);
        }
        else if (mode == _OVERRIDE)
        {
          _DrawPixel(display->x + x0, display->y + y0, (_COLOR_t)!color);
        }
      }
    }
}
//
//
//
void _WriteString(DISPLAY_t *display, char *str, _COLOR_t color, _DRAW_t mode)
{
  int16_t l = strlen(str);
  int X = display->x;
  int Y = display->y;
  
  if (
      (display->x + l*display->font.width < display->MaskX1) ||
      (display->MaskX2 < display->x) ||
      (display->y + display->font.height < display->MaskY1) ||
      (display->MaskY2 < display->y)
  )
  {
    ESP_LOGE(TAG, "writestring init test");
    return;
  }

  int16_t fx = (display->MaskX1 - display->x) / display->font.width;
  int16_t rx = (display->x - display->MaskX2) / display->font.width;
  char* estr = str + l;
  


  // cut off characters which are out of masking box
  if (fx > 0) {
      str += fx;
      display->x += fx*display->font.width;
  }

  if (rx > 0) {
    estr -= rx;
  }



    // Write until null-byte or the first cutoff char
    int tmp_x = display->x;
    //display->y = 0;
    int16_t n = 0;
    while (*str && str < estr)
    {
      display->x = tmp_x + n * display->font.width;
      _WriteChar(display, *str, color, mode);
      n++;
      str++;
    }
  // restore x,y
  display->x = X;
  display->y = Y;

}
//
//
//
void _Clear_String(DISPLAY_t *display, int16_t string_length, _DRAW_t mode)
{
  int16_t x_tmp = display->x;
  int16_t y_tmp = display->y;

  for(int i=0; i<string_length; i++)
  {
    _WriteChar(display, 0x20, _WHITE, mode);
    display->x += display->font.width;
  }

  display->x = x_tmp;
  display->y = y_tmp;
}
//
//
//
static void tr_ssd1306_init(void) 
{
	esp_err_t espRc;

	i2c_cmd_handle_t cmd = i2c_cmd_link_create();

	i2c_master_start(cmd);
	i2c_master_write_byte(cmd, (OLED_I2C_ADDRESS << 1) | I2C_MASTER_WRITE, true);
	i2c_master_write_byte(cmd, OLED_CONTROL_BYTE_CMD_STREAM, true);
	i2c_master_write_byte(cmd, OLED_CMD_DISPLAY_OFF, true);           // turn display off
  // OSC default=0x81
  i2c_master_write_byte(cmd, OLED_CMD_SET_DISPLAY_CLK_DIV, true);   // set up oscillator
  i2c_master_write_byte(cmd, 0x81, true);
  // Brightness in range 0~255, default=0x7F
  i2c_master_write_byte(cmd, OLED_CMD_SET_CONTRAST, true);          // contrast
  i2c_master_write_byte(cmd, 0x7f, true);
  // Memory Address Mode: Horizontal=0, Vertical=1, Page=default=2
  i2c_master_write_byte(cmd, OLED_CMD_SET_MEMORY_ADDR_MODE, true);  // horiz
  i2c_master_write_byte(cmd, 0x00, true);
  // Set Display Offset in range 0~63
  i2c_master_write_byte(cmd, OLED_CMD_SET_DISPLAY_OFFSET, true);
  i2c_master_write_byte(cmd, 0x00, true);
  // Set Display start line in range 0x40~0x7F
  i2c_master_write_byte(cmd, OLED_CMD_SET_DISPLAY_START_LINE, true);
  // Set multiplex number (activated rows): rows=height-1, default=63
  // 128 x 32 display
  i2c_master_write_byte(cmd, OLED_CMD_SET_MUX_RATIO, true);
  i2c_master_write_byte(cmd, 0x31, true); 
  // Reduce a half of height??
  // set com pins - bits 5 / 4
  // bit 4 => 0 sequential com pin config
  // bit 5 => 0 disable com left/right remap
  i2c_master_write_byte(cmd, OLED_CMD_SET_COM_PIN_MAP, true);
  i2c_master_write_byte(cmd, 0x02, true);
  
  // Segment (Column) normal mode, Inverse=0xA1
  // set segment remap
  i2c_master_write_byte(cmd, OLED_CMD_SET_SEGMENT_NORMAL_MODE, true);
  // Common (Row) normal mode, inverse=0xC8
  // set com output scan direction
  i2c_master_write_byte(cmd, OLED_CMD_SET_COM_NORMAL_MODE, true);
  
  // disable entire display on
  i2c_master_write_byte(cmd, OLED_CMD_DISPLAY_RAM, true);
  
  // Display normal mode, inverse=0xA7
  i2c_master_write_byte(cmd, OLED_CMD_DISPLAY_NORMAL, true);
  // Disable Scroll
  i2c_master_write_byte(cmd, OLED_CMD_SET_DISABLE_SCROLL, true);
  // Pre-charge period, default=2
  i2c_master_write_byte(cmd, OLED_CMD_SET_PRECHARGE, true);
  i2c_master_write_byte(cmd, 0x02, true);
  // enable charge pump 0x14 => ON  0x10 => OFF
  i2c_master_write_byte(cmd, OLED_CMD_SET_CHARGE_PUMP, true);
	i2c_master_write_byte(cmd, OLED_CHARGE_PUMP_ON, true);

	i2c_master_write_byte(cmd, OLED_CMD_DISPLAY_ON, true);
	i2c_master_stop(cmd);

	espRc = i2c_master_cmd_begin(I2C_NUM_0, cmd, 10/portTICK_PERIOD_MS);
	if (espRc == ESP_OK) {
		ESP_LOGI(TAG, "OLED configured successfully");
	} else {
		ESP_LOGE(TAG, "OLED configuration failed. code: 0x%.2X", espRc);
	}
	i2c_cmd_link_delete(cmd);
  
}
//
//
//
void app_main(void)
{
  // pwm pin number
  const uint32_t pin_num = PWM_0_OUT_IO_NUM;
  // duties table, real_duty = duties[x]/PERIOD
  uint32_t duties = PWM_PERIOD / 2;
  float phase = 0;

  
  DISPLAY_t display =
  {
    .font       = Font_8x8,
    .x          = 8,
    .y          = 0,
    .MaskX1     = 0,
    .MaskY1     = 0,
    .MaskX2     = _WIDTH,
    .MaskY2     = _HEIGHT,
    .cursor_pos = cursor_position,
    .rgb_pos    = rgb_position
  };
  
  ENCODER_t encoder =
  {
    .dir        = CCW,
    .active     = false
  };
  
  LIGHT_t light =
  {
    .pixel.val        = 0,
    .active           = false
  };
  
  REMOTE_t remote =
  {
    .encoder        = &encoder,
    .display        = &display,
    .light          = &light,    // contains rgb:val and bool light op on/off
    .op_index       = op_SNOOZE,

  };
  
  ESP_ERROR_CHECK(nvs_flash_init());
  tr_wifi_init();

 
	i2c_master_init();
	tr_ssd1306_init();
  gpio_init(&remote);
  pwm_init(PWM_PERIOD, &duties, 1, &pin_num);
  pwm_set_phases(&phase);
  pwm_stop(0);
  remote_queue_init(&remote);
  tr_espnow_init();
  
  _Clear();
  _DrawPixel(0, 0, _WHITE);

  ESP_LOGI(TAG, "Let's Rock");
  
  xTaskCreate(tr_dispatcher, "tr_dispatcher", 4096, (void *)&remote, 4, NULL);
  
	while(1)
  {
    vTaskDelay(100/portTICK_PERIOD_MS);
  }

    
  
}
