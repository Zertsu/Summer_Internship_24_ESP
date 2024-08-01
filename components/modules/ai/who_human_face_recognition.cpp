#include "who_human_face_recognition.hpp"

#include "esp_log.h"
#include "esp_camera.h"

#include "dl_image.hpp"
#include "fb_gfx.h"

#include "human_face_detect_msr01.hpp"
#include "human_face_detect_mnp01.hpp"
#include "face_recognition_tool.hpp"

#if CONFIG_MFN_V1
#if CONFIG_S8
#include "face_recognition_112_v1_s8.hpp"
#elif CONFIG_S16
#include "face_recognition_112_v1_s16.hpp"
#endif
#endif

#include "who_ai_utils.hpp"

#include "dl_math.hpp"

using namespace std;
using namespace dl;

static const char *TAG = "human_face_recognition";

static QueueHandle_t xQueueFrameI = NULL;
static QueueHandle_t xQueueEvent = NULL;
static QueueHandle_t xQueueFrameO = NULL;
static QueueHandle_t xQueueResult = NULL;

static recognizer_state_t gEvent = DETECT;
static bool gReturnFB = true;
static face_info_t recognize_result;

static const bool *guardingMode;

SemaphoreHandle_t xMutex;

typedef enum
{
    SHOW_STATE_IDLE,
    SHOW_STATE_DELETE,
    SHOW_STATE_RECOGNIZE,
    SHOW_STATE_ENROLL,
} show_state_t;

#define RGB565_MASK_RED 0xF800
#define RGB565_MASK_GREEN 0x07E0
#define RGB565_MASK_BLUE 0x001F
#define FRAME_DELAY_NUM 16

static void rgb_print(camera_fb_t *fb, uint32_t color, const char *str)
{
    fb_gfx_print(fb, (fb->width - (strlen(str) * 14)) / 2, 10, color, str);
}

static int rgb_printf(camera_fb_t *fb, uint32_t color, const char *format, ...)
{
    char loc_buf[64];
    char *temp = loc_buf;
    int len;
    va_list arg;
    va_list copy;
    va_start(arg, format);
    va_copy(copy, arg);
    len = vsnprintf(loc_buf, sizeof(loc_buf), format, arg);
    va_end(copy);
    if (len >= sizeof(loc_buf))
    {
        temp = (char *)malloc(len + 1);
        if (temp == NULL)
        {
            return 0;
        }
    }
    vsnprintf(temp, len + 1, format, arg);
    va_end(arg);
    rgb_print(fb, color, temp);
    if (len > 64)
    {
        free(temp);
    }
    return len;
}

static void task_process_handler(void *arg)
{
    camera_fb_t *frame = NULL;
    HumanFaceDetectMSR01 detector(0.3F, 0.3F, 10, 0.3F);
    HumanFaceDetectMNP01 detector2(0.4F, 0.3F, 10);

#if CONFIG_MFN_V1
#if CONFIG_S8
    FaceRecognition112V1S8 *recognizer = new FaceRecognition112V1S8();
#elif CONFIG_S16
    FaceRecognition112V1S16 *recognizer = new FaceRecognition112V1S16();
#endif
#endif
    show_state_t frame_show_state = SHOW_STATE_IDLE;
    recognizer_state_t _gEvent;
    recognizer->set_partition(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "fr");
    int partition_result = recognizer->set_ids_from_flash();

    bool lastFrameHadFace = false;
    bool recoginzedFoe = false;
    bool recognizedFriend = false;
    recognizer_position_t positions_result;

    bool isFoe = false;
    
    while (true)
    {
        xSemaphoreTake(xMutex, portMAX_DELAY);
        _gEvent = gEvent;
        gEvent = DETECT;
        xSemaphoreGive(xMutex);

        if (_gEvent)
        {
            bool is_detected = false;

            if (xQueueReceive(xQueueFrameI, &frame, portMAX_DELAY))
            {
                std::list<dl::detect::result_t> &detect_candidates = detector.infer((uint16_t *)frame->buf, {(int)frame->height, (int)frame->width, 3});
                std::list<dl::detect::result_t> &detect_results = detector2.infer((uint16_t *)frame->buf, {(int)frame->height, (int)frame->width, 3}, detect_candidates);

                if (detect_results.size() == 1)
                    is_detected = true;

                if (is_detected)
                {
                    if (lastFrameHadFace == false && *guardingMode) {
                        _gEvent = RECOGNIZE;
                        lastFrameHadFace = true;
                    }
                    switch (_gEvent)
                    {
                    case ENROLL_FOE:
                    case ENROLL_FRIEND:
                        isFoe = _gEvent == ENROLL_FOE;
                        recognizer->enroll_id((uint16_t *)frame->buf, {(int)frame->height, (int)frame->width, 3}, detect_results.front().keypoint, isFoe ? "fo" : "fr", true);
                        ESP_LOGW("ENROLL", "ID %d is enrolled as %s", recognizer->get_enrolled_ids().back().id, isFoe ? "foe" : "friend");
                        frame_show_state = SHOW_STATE_ENROLL;
                        break;

                    case RECOGNIZE:
                        recognize_result = recognizer->recognize((uint16_t *)frame->buf, {(int)frame->height, (int)frame->width, 3}, detect_results.front().keypoint);
                        isFoe = recognize_result.name.compare("fo") == 0;
                        print_detection_result(detect_results);
                        if(recognize_result.id > 0) {
                            ESP_LOGI("RECOGNIZE", "Similarity: %f, Match ID: %d, %s", recognize_result.similarity, recognize_result.id, isFoe ? "foe" : "friend");
                            recoginzedFoe = isFoe;
                            recognizedFriend = recognize_result.name.compare("fr") == 0;
                        } else {
                            ESP_LOGE("RECOGNIZE", "Similarity: %f, Match ID: %d, %s", recognize_result.similarity, recognize_result.id, isFoe ? "foe" : "friend");
                        }
                        frame_show_state = SHOW_STATE_RECOGNIZE;
                        break;

                    case DELETE:
                        vTaskDelay(10);
                        recognizer->delete_id(true);
                        ESP_LOGE("DELETE", "% d IDs left", recognizer->get_enrolled_id_num());
                        frame_show_state = SHOW_STATE_DELETE;
                        break;

                    default:
                        break;
                    }
                } else {
                    lastFrameHadFace = false;
                    recoginzedFoe = false;
                    recognizedFriend = false;
                }


                if (frame_show_state != SHOW_STATE_IDLE)
                {
                    static int frame_count = 0;
                    switch (frame_show_state)
                    {
                    case SHOW_STATE_DELETE:
                        rgb_printf(frame, RGB565_MASK_RED, "%d IDs left", recognizer->get_enrolled_id_num());
                        break;

                    case SHOW_STATE_RECOGNIZE:
                        if (recognize_result.id > 0)
                            rgb_printf(frame, RGB565_MASK_GREEN, "ID %d, %s", recognize_result.id, isFoe ? "foe" : "friend");
                        else
                            rgb_print(frame, RGB565_MASK_RED, "who ?");
                        break;

                    case SHOW_STATE_ENROLL:
                        rgb_printf(frame, RGB565_MASK_BLUE, "Enroll %s: ID %d", isFoe ? "foe" : "friend", recognizer->get_enrolled_ids().back().id);
                        break;

                    default:
                        break;
                    }

                    if (++frame_count > FRAME_DELAY_NUM)
                    {
                        frame_count = 0;
                        frame_show_state = SHOW_STATE_IDLE;
                    }
                }

                if (detect_results.size())
                {
#if !CONFIG_IDF_TARGET_ESP32S3
                    print_detection_result(detect_results);
#endif
                    draw_detection_result((uint16_t *)frame->buf, frame->height, frame->width, detect_results);
                }

                if(recoginzedFoe || recognizedFriend) {
                    positions_result.valid    = recoginzedFoe ? 1 : 2;
                    positions_result.frameW   = (uint16_t)(frame->width);
                    positions_result.frameH   = (uint16_t)(frame->height);
                    positions_result.boxX     = (uint16_t)(detect_results.front().box[0]);
                    positions_result.boxY     = (uint16_t)(detect_results.front().box[1]);
                    positions_result.boxW     = (uint16_t)((detect_results.front().box[2] - detect_results.front().box[0]));
                    positions_result.boxH     = (uint16_t)((detect_results.front().box[3] - detect_results.front().box[1]));
                    positions_result.noseX    = (uint16_t)(detect_results.front().keypoint[4]);
                    positions_result.noseY    = (uint16_t)(detect_results.front().keypoint[5]);
                    positions_result.l_eyeX   = (uint16_t)(detect_results.front().keypoint[0]);
                    positions_result.l_eyeY   = (uint16_t)(detect_results.front().keypoint[1]);
                    positions_result.r_eyeX   = (uint16_t)(detect_results.front().keypoint[6]);
                    positions_result.r_eyeY   = (uint16_t)(detect_results.front().keypoint[7]);
                    positions_result.l_mouthX = (uint16_t)(detect_results.front().keypoint[2]);
                    positions_result.l_mouthY = (uint16_t)(detect_results.front().keypoint[3]);
                    positions_result.r_mouthX = (uint16_t)(detect_results.front().keypoint[8]);
                    positions_result.r_mouthY = (uint16_t)(detect_results.front().keypoint[9]);
                } else {
                    memset(&positions_result, 0, sizeof(positions_result));
                }
            }

            if (xQueueFrameO)
            {

                xQueueSend(xQueueFrameO, &frame, portMAX_DELAY);
            }
            else if (gReturnFB)
            {
                esp_camera_fb_return(frame);
            }
            else
            {
                free(frame);
            }

            if (xQueueResult && *guardingMode)
            {
                xQueueSend(xQueueResult, (void *)&positions_result, portMAX_DELAY);
            }
        }
    }
}

static void task_event_handler(void *arg)
{
    recognizer_state_t _gEvent;
    while (true)
    {
        xQueueReceive(xQueueEvent, &(_gEvent), portMAX_DELAY);
        xSemaphoreTake(xMutex, portMAX_DELAY);
        gEvent = _gEvent;
        xSemaphoreGive(xMutex);
    }
}

void register_human_face_recognition(const QueueHandle_t frame_i,
                                     const QueueHandle_t event,
                                     const QueueHandle_t result,
                                     const QueueHandle_t frame_o,
                                     const bool camera_fb_return,
                                     const bool *guardingModePointer)
{
    xQueueFrameI = frame_i;
    xQueueFrameO = frame_o;
    xQueueEvent = event;
    xQueueResult = result;
    gReturnFB = camera_fb_return;
    guardingMode = guardingModePointer;

    xMutex = xSemaphoreCreateMutex();

    xTaskCreatePinnedToCore(task_process_handler, TAG, 4 * 1024, NULL, 5, NULL, 0);
    if (xQueueEvent)
        xTaskCreatePinnedToCore(task_event_handler, TAG, 4 * 1024, NULL, 5, NULL, 1);
}
