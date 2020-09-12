#include <Arduino.h>
#ifdef ESP32
#include <AsyncTCP.h>
#include <WiFi.h>
#elif defined(ESP8266)
#include <ESP8266WiFi.h>
#include <ESPAsyncTCP.h>
#endif
#include <ESPAsyncWebServer.h>
#include <WebSocketsServer.h>

// gzipped html content
#include "camera_index.h"
#include "camera_wrap.h"

#include "string/strcopy.h"


#define MIN(x, y) (((x) <= (y)) ? (x) : (y))
#define MAX(x, y) (((x) >= (y)) ? (x) : (y))
#define ABS(x) (((x) >= 0) ? (x) : -(x))

//
// control pins for the L9110S motor controller
//
#include "rover/rover.h"
const int A1_A_PIN = 15;    // left forward input pin
const int A1_B_PIN = 13;    // left reverse input pin
const int B1_B_PIN = 14;    // right forward input pin
const int B1_A_PIN = 2;     // right reverse input pin

//
// wheel encoders use same pins as the serial port,
// so if we are using encoders, we must disable serial output/input
//
// #define USE_WHEEL_ENCODERS
#ifdef USE_WHEEL_ENCODERS
    #define SERIAL_DISABLE  // disable serial if we are using encodes; they use same pins
    #include "./encoders.h"
    const int LEFT_ENCODER_PIN = 1;   // left LM393 wheel encoder input pin
    const int RIGHT_ENCODER_PIN = 3;  // right LM393 wheel encoder input pin
#endif

//
// Include _after_ encoder stuff so SERIAL_DISABLE is correctly set
//
// leave LOG_LEVEL undefined to turn off all logging
// or define as one of ERROR_LEVEL, WARNING_LEVEL, INFO_LEVEL, DEBUG_LEVEL
//
#define LOG_LEVEL INFO_LEVEL
#include "./log.h"

//
// put ssid and password in wifi_credentials.h
// and do NOT check that into source control.
//
#include "wifi_credentials.h"
//const char* ssid = "******";
//const char* password = "******";

//
// camera stuff
//

void statusHandler(AsyncWebServerRequest *request);
void configHandler(AsyncWebServerRequest *request);
void captureHandler(AsyncWebServerRequest *request);
void roverHandler(AsyncWebServerRequest *request);
void roverTask(void *params);
TaskHandle_t roverTaskHandle;

void videoHandler(AsyncWebServerRequest *request);

// health endpoint
void healthHandler(AsyncWebServerRequest *request);


// create the http server and websocket server
AsyncWebServer server(80);
WebSocketsServer wsStream = WebSocketsServer(81);
WebSocketsServer wsCommand = WebSocketsServer(82);

// 404 handler
void notFound(AsyncWebServerRequest *request)
{
    request->send(404, "text/plain", "Not found");
}

// websocket message handler
void wsStreamEvent(uint8_t clientNum, WStype_t type, uint8_t * payload, size_t length);
void wsCommandEvent(uint8_t clientNum, WStype_t type, uint8_t * payload, size_t length);

// wheel encoders
void attachWheelEncoders(int leftInputPin, int rightInputPin);
void detachWheelEncoders(int leftInputPin, int rightInputPin);
unsigned int readLeftWheelEncoder();
unsigned int readRightWheelEncoder();
void logWheelEncoders();

void setup()
{
    // 
    // init serial monitor
    //
    SERIAL_BEGIN(115200);
    SERIAL_DEBUG(true);
    SERIAL_PRINTLN();

    LOG_INFO("Setting up...");

    //
    // initialize motor output pins
    //
    roverInit(A1_A_PIN, A1_B_PIN, B1_B_PIN, B1_A_PIN);
    LOG_INFO("...Rover Initialized...");

    //
    // initialize wheel encoder input pin
    //
    // attachWheelEncoders(LEFT_ENCODER_PIN, RIGHT_ENCODER_PIN);

    // 
    // init wifi
    //
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);
    if (WiFi.waitForConnectResult() != WL_CONNECTED)
    {
        LOG_ERROR("WiFi Failed!\n");
        return;
    }

    SERIAL_PRINT("...Wifi initialized, running on IP Address: ");
    SERIAL_PRINTLN(WiFi.localIP().toString());
    SERIAL_PRINT("ESP Board MAC Address:  ");
    SERIAL_PRINTLN(WiFi.macAddress());

    //
    // init web server
    //

    // endpoints to return the compressed html/css/javascript for running the rover
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
        LOG_INFO("handling " + request->url());
        AsyncWebServerResponse *response = request->beginResponse_P(200, "text/html", index_html_gz, sizeof(index_html_gz));
        response->addHeader("Content-Encoding", "gzip");
        request->send(response);
    });
    server.on("/bundle.css", HTTP_GET, [](AsyncWebServerRequest *request) {
        LOG_INFO("handling " + request->url());
        AsyncWebServerResponse *response = request->beginResponse_P(200, "text/css", bundle_css_gz, sizeof(bundle_css_gz));
        response->addHeader("Content-Encoding", "gzip");
        request->send(response);
    });
    server.on("/bundle.js", HTTP_GET, [](AsyncWebServerRequest *request) {
        LOG_INFO("handling " + request->url());
        AsyncWebServerResponse *response = request->beginResponse_P(200, "text/javascript", bundle_js_gz, sizeof(bundle_js_gz));
        response->addHeader("Content-Encoding", "gzip");
        request->send(response);
    });

    server.on("/health", HTTP_GET, healthHandler);

    // endpoint for sending rover commands
    server.on("/rover", HTTP_GET, roverHandler);

    // endpoint for streaming video from camera
    server.on("/control", HTTP_GET, configHandler);
    server.on("/status", HTTP_GET, statusHandler);
    server.on("/capture", HTTP_GET, captureHandler);
    server.on("/stream", HTTP_GET, notFound /*videoHandler*/);

    // return 404 for unhandled urls
    server.onNotFound(notFound);

    // start listening for requests
    server.begin();
    LOG_INFO("... http server intialized ...");

    //
    // init websockets
    //
    wsStream.begin();
    wsStream.onEvent(wsStreamEvent);
    wsCommand.begin();
    wsCommand.onEvent(wsCommandEvent);

    LOG_INFO("... websockets server intialized ...");

    //
    // create background task to execute queued rover tasks
    //
    // xTaskCreate(roverTask, "roverTask", 1024, NULL, 1, &roverTaskHandle);

    //
    // initialize the camera
    //
    initCamera();
}

void healthHandler(AsyncWebServerRequest *request)
{
    LOG_INFO("handling " + request->url());

    // TODO: determine if camera and rover are healty
    request->send(200, "application/json", "{\"health\": \"ok\"}");
}

//
// handle /capture
//
void captureHandler(AsyncWebServerRequest *request)
{
    LOG_INFO("handling " + request->url());

    //
    // 1. create buffer to hold image
    // 2. capture a camera image
    // 3. create response and send it
    // 4. free the image buffer
    //
 
 
    // 1. create buffer to hold frames
    uint8_t* jpgBuff = new uint8_t[68123];  // TODO: should be based on image dimensions
    size_t   jpgLength = 0;

    // 2. capture a camera image
    esp_err_t result = grabImage(jpgLength, jpgBuff);

    // 3. create and send response
    if(result == ESP_OK){
        request->send_P(200, "image/jpeg", jpgBuff, jpgLength);
    }
    else
    {
        request->send(500, "text/plain", "Error capturing image from camera");
    }

    //
    // 4. free the image buffer
    //
    delete jpgBuff;
}

//
// handle /stream
// start video stream background task
//
void videoHandler(AsyncWebServerRequest *request)
{
    LOG_INFO("handling " + request->url());
    request->send(501, "text/plain", "not implemented");
}

int cameraClientId = -1;        // websocket client id for camera streaming
bool isCameraStreamOn = false;  // true if streaming, false if not

int commandClientId = -1;       // websocket client id for rover commands
bool isCommandSocketOn = false; // true if command socket is ready

//
// send the given image buffer down the websocket
//
int sendImage(uint8_t *imageBuffer, size_t bufferSize) {
    if (wsStream.sendBIN(cameraClientId, imageBuffer, bufferSize)) {
        return SUCCESS;
    }
    return FAILURE;
}

//
// get a camera image and send it down websocket
//
void streamCameraImage() {
    if (isCameraStreamOn && (cameraClientId >= 0)) {
        //
        // grab and image and call sendImage on it
        //
        esp_err_t result = processImage(sendImage);
        if (SUCCESS != result) {
            LOG_ERROR("Failure grabbing and sending image.");
        }
    }
}


/******************************************************/
/*************** main loop ****************************/
/******************************************************/
void loop()
{
    wsCommand.loop();
    TankCommand command;
    if (SUCCESS == dequeueRoverCommand(&command)) {
        LOG_INFO("Executing RoveR Command");
        executeRoverCommand(command);
    }
    wsCommand.loop();

    // send image to clients via websocket
    streamCameraImage();
    wsStream.loop();
    wsCommand.loop();

    logWheelEncoders();
}

/******************************************************/
/*************** rover control ************************/
/******************************************************/

//
// handle '/rover' endpoint.
// optional query params
// - speed: 0..255
// - direction: stop|forward|reverse|left|right
//
void roverHandler(AsyncWebServerRequest *request)
{
    LOG_INFO("handling " + request->url());

    String directionParam = "";
    if (request->hasParam("direction", false))
    {
        directionParam = request->getParam("direction", false)->value();
    }
    
    String speedParam = "";
    if (request->hasParam("speed", false))
    {
        speedParam = request->getParam("speed", false)->value();
    }

    //
    // submit the command to a queue and return
    //
    if((NULL == directionParam) 
        || (NULL == speedParam)
        || (SUCCESS != submitTurtleCommand(directionParam.c_str(), speedParam.c_str())))
    {
        request->send(400, "text/plain", "bad_request");
    }

    request->send(200, "text/plain", directionParam + "," + speedParam);
}


//
// background task to process queue commands 
// as they appear.
//
void roverTask(void *params) {
    //
    // read next task from the command queue and execute it
    //
    TankCommand command;

    for(;;) 
    {
        if (SUCCESS == dequeueRoverCommand(&command)) {
            LOG_INFO("Executing RoveR Command");
            executeRoverCommand(command);
            taskYIELD();    // give web server some time
        }
    }
}


/*
 * Handle /status web service endpoint;
 * - response body is is json payload with
 *   all camera properties and values like;
 *   `{"framesize":0,"quality":10,"brightness":0,...,"special_effect":0}`
 */
void statusHandler(AsyncWebServerRequest *request) 
{
    LOG_INFO("handling " + request->url());

    const String json = getCameraPropertiesJson();
    request->send_P(200, "application/json", (uint8_t *)json.c_str(), json.length());
}


//
// handle /control
//
void configHandler(AsyncWebServerRequest *request) {
    LOG_INFO("handling " + request->url());

    //
    // validate parameters
    //
    String varParam = "";
    if (request->hasParam("var", false))
    {
        varParam = request->getParam("var", false)->value();
    }
    
    String valParam = "";
    if (request->hasParam("val", false))
    {
        valParam = request->getParam("val", false)->value();
    }

    // we must have values for each parameter
    if (varParam.isEmpty() || valParam.isEmpty()) 
    {
        request->send(400, "text/plain", "bad request; both the var and val params must be present.");
    }
    else
    {
        const int status = setCameraProperty(varParam, valParam);
        if(SUCCESS != status) {
            LOG_ERROR("Failure setting camera property");
        }
        request->send((SUCCESS == status) ? 200: 500);
    }
}


//////////////////////////////////////
///////// websocket server ///////////
//////////////////////////////////////
void logWsEvent(
    const char *event,  // IN : name of event as null terminated string
    const int id)       // IN : client id to copy
{
    #ifdef LOG_LEVEL
        #if (LOG_LEVEL >= INFO_LEVEL)
            char msg[128];
            int offset = strCopy(msg, sizeof(msg), event);
            offset = strCopyAt(msg, sizeof(msg), offset, ", clientId: ");
            strCopyIntAt(msg, sizeof(msg), offset, id);
            LOG_INFO(msg);
        #endif
    #endif
}

void wsStreamEvent(uint8_t clientNum, WStype_t type, uint8_t * payload, size_t length) {
    switch(type) {
        case WStype_CONNECTED: {
            logWsEvent("wsStreamEvent.WS_EVT_CONNECT", clientNum);
            wsStream.sendPing(clientNum, (uint8_t *)"ping", sizeof("ping"));
            return;
        }
        case WStype_DISCONNECTED: {
            // os_printf("wsStream[%s][%u] disconnect: %u\n", server->url(), client->id());
            logWsEvent("wsStreamEvent.WS_EVT_DISCONNECT", clientNum);
            if (cameraClientId == clientNum) {
                cameraClientId = -1;
                isCameraStreamOn = false;
            }
            return;
        } 
        case WStype_PONG: {
            logWsEvent("wsStreamEvent.WStype_PONG", clientNum);
            cameraClientId = clientNum;
            isCameraStreamOn = true;
            return;
        }
        case WStype_BIN: {
            logWsEvent("wsStreamEvent.WStype_BIN", clientNum);
            return;
        }
        case WStype_TEXT: {
            char buffer[128];
            int offset = strCopy(buffer, sizeof(buffer), "wsStreamEvent.WStype_TEXT: ");
            strCopySizeAt(buffer, sizeof(buffer), offset, (char *)payload, length);
            logWsEvent(buffer, clientNum);
            return;
        }
        default: {
            logWsEvent("wsStreamEvent.UNHANDLED EVENT: ", clientNum);
            return;
        }
    }
}


void wsCommandEvent(uint8_t clientNum, WStype_t type, uint8_t * payload, size_t length) {
    switch(type) {
        case WStype_CONNECTED: {
            logWsEvent("wsCommandEvent.WS_EVT_CONNECT", clientNum);
            wsStream.sendPing(clientNum, (uint8_t *)"ping", sizeof("ping"));
            return;
        }
        case WStype_DISCONNECTED: {
            // os_printf("wsStream[%s][%u] disconnect: %u\n", server->url(), client->id());
            logWsEvent("wsCommandEvent.WS_EVT_DISCONNECT", clientNum);
            if (commandClientId == clientNum) {
                commandClientId = -1;
                isCommandSocketOn = false;
            }
            return;
        } 
        case WStype_PONG: {
            logWsEvent("wsCommandEvent.WStype_PONG", clientNum);
            commandClientId = clientNum;
            isCommandSocketOn = true;
            return;
        }
        case WStype_BIN: {
            logWsEvent("wsCommandEvent.WStype_BIN", clientNum);
            return;
        }
        case WStype_TEXT: {
            // log the command
            char buffer[128];
            #ifdef LOG_LEVEL
                #if (LOG_LEVEL >= INFO_LEVEL)
                    const int offset = strCopy(buffer, sizeof(buffer), "wsCommandEvent.WStype_TEXT: ");
                    strCopySizeAt(buffer, sizeof(buffer), offset, (const char *)payload, length);
                    logWsEvent(buffer, clientNum);
                #endif
            #endif

            // submit the command for execution
            strCopySize(buffer, sizeof(buffer), (const char *)payload, (int)length);
            const SubmitTankCommandResult result = submitTankCommand(buffer, 0);
            if(SUCCESS == result.status) {
                //
                // ack the command by sending it back
                //
                wsCommand.sendTXT(clientNum, (const char *)payload, length);
            } else {
                //
                // nack the command with status
                //
                wsCommand.sendTXT(clientNum, String("nack(") + String(result.status) + String(")"));
            }
            return;
        }
        default: {
            logWsEvent("wsCommandEvent.UNHANDLED EVENT: ", clientNum);
            return;
        }
    }
}

